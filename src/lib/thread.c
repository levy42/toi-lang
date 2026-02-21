#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// Global Interpreter Lock
static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;
static int gil_initialized = 0;
static int no_gil_enabled = 0;

// Thread data structure
typedef struct {
    pthread_t pthread;
    VM* vm;
    ObjThread* caller_thread;
    ObjClosure* closure;
    Value* args;
    int arg_count;
    Value result;
    int result_count;
    int done;
    int error;
    char error_msg[256];
} ThreadData;

// Mutex userdata
typedef struct {
    pthread_mutex_t mutex;
    int locked;
} MutexData;

// Channel for thread communication
typedef struct ChannelNode {
    Value value;
    struct ChannelNode* next;
} ChannelNode;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    ChannelNode* head;
    ChannelNode* tail;
    int count;
    int capacity; // 0 = unbounded
    int closed;
} ChannelData;

static void thread_handle_mark(void* ptr) {
    ThreadData* data = (ThreadData*)ptr;
    if (data == NULL) return;
    if (data->closure != NULL) {
        mark_object((struct Obj*)data->closure);
    }
    for (int i = 0; i < data->arg_count; i++) {
        mark_value(data->args[i]);
    }
    if (data->result_count > 0) {
        mark_value(data->result);
    }
}

static void channel_mark(void* ptr) {
    ChannelData* data = (ChannelData*)ptr;
    if (data == NULL) return;
    pthread_mutex_lock(&data->mutex);
    for (ChannelNode* node = data->head; node != NULL; node = node->next) {
        mark_value(node->value);
    }
    pthread_mutex_unlock(&data->mutex);
}

static void acquire_gil(void) {
    if (no_gil_enabled) return;
    pthread_mutex_lock(&gil);
}

static void release_gil(void) {
    if (no_gil_enabled) return;
    pthread_mutex_unlock(&gil);
}

static void park_vm_thread(VM* vm, ObjThread* thread) {
    if (thread == NULL) return;
    if (thread->gc_park_count == 0) {
        thread->gc_park_next = vm->gc_parked_threads;
        vm->gc_parked_threads = thread;
    }
    thread->gc_park_count++;
}

static void unpark_vm_thread(VM* vm, ObjThread* thread) {
    if (thread == NULL) return;
    if (thread->gc_park_count <= 0) return;
    thread->gc_park_count--;
    if (thread->gc_park_count > 0) return;

    ObjThread* prev = NULL;
    ObjThread* cur = vm->gc_parked_threads;
    while (cur != NULL) {
        if (cur == thread) {
            if (prev != NULL) {
                prev->gc_park_next = cur->gc_park_next;
            } else {
                vm->gc_parked_threads = cur->gc_park_next;
            }
            thread->gc_park_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->gc_park_next;
    }
}

// Preserve and restore the caller VM thread across unlock/relock windows.
// Without this, vm_current_thread(vm) can be clobbered by another OS thread.
static ObjThread* suspend_vm_thread(VM* vm) {
    ObjThread* caller = vm_current_thread(vm);
    if (!no_gil_enabled) {
        park_vm_thread(vm, caller);
    }
    release_gil();
    return caller;
}

static void resume_vm_thread(VM* vm, ObjThread* caller) {
    if (!no_gil_enabled) {
        sched_yield();
    }
    acquire_gil();
    if (!no_gil_enabled) {
        unpark_vm_thread(vm, caller);
    }
    vm_set_current_thread(vm, caller);
}

// Thread entry point
static void* thread_runner(void* arg) {
    ThreadData* data = (ThreadData*)arg;

    acquire_gil();

    // Restore to the spawning VM thread, not whatever happens to be current.
    ObjThread* main_thread = data->caller_thread;
    if (main_thread == NULL) {
        main_thread = vm_current_thread(data->vm);
    }

    // Create a new thread like coroutine.create does
    ObjThread* worker_thread = new_thread();
    worker_thread->vm = data->vm;

    // Switch to worker thread and run
    vm_set_current_thread(data->vm, worker_thread);

    // Build a normal call frame to preserve VM invariants.
    push(data->vm, OBJ_VAL(data->closure));
    for (int i = 0; i < data->arg_count; i++) {
        push(data->vm, data->args[i]);
    }
    if (!call(data->vm, data->closure, data->arg_count)) {
        data->error = 1;
        if (worker_thread->has_exception && IS_STRING(worker_thread->last_error)) {
            ObjString* msg = AS_STRING(worker_thread->last_error);
            snprintf(data->error_msg, sizeof(data->error_msg), "%.*s", msg->length, msg->chars);
        } else {
            snprintf(data->error_msg, sizeof(data->error_msg), "Thread setup error");
        }
        worker_thread->last_error = NIL_VAL;
        vm_set_current_thread(data->vm, main_thread);
        data->done = 1;
        release_gil();
        return NULL;
    }

    // Run until this worker frame returns; min_frame_count=1 keeps the
    // return value on the worker stack without touching global REPL mode.
    InterpretResult result = vm_run(data->vm, 1);

    if (result != INTERPRET_OK) {
        data->error = 1;
        ObjThread* t = vm_current_thread(data->vm);
        if (t != NULL && IS_STRING(t->last_error)) {
            ObjString* msg = AS_STRING(t->last_error);
            snprintf(data->error_msg, sizeof(data->error_msg), "%.*s", msg->length, msg->chars);
        } else {
            snprintf(data->error_msg, sizeof(data->error_msg), "Thread execution error");
        }
        if (t != NULL) {
            t->last_error = NIL_VAL;
        }
    } else {
        // After vm_run completes, the result is on top of the stack
        ObjThread* t = vm_current_thread(data->vm);
        if (t->stack_top > t->stack) {
            data->result = t->stack_top[-1];
            data->result_count = 1;
        } else {
            data->result = NIL_VAL;
            data->result_count = 0;
        }
    }

    // Restore main thread
    vm_set_current_thread(data->vm, main_thread);
    data->done = 1;
    release_gil();

    return NULL;
}

// thread.spawn(fn, ...) - create and start a new thread
static int thread_spawn(VM* vm, int arg_count, Value* args) {
    vm_enable_thread_tls(vm);
    if (arg_count < 1 || !IS_CLOSURE(args[0])) {
        vm_runtime_error(vm, "thread.spawn requires a function as first argument");
        return 0;
    }

    ThreadData* data = (ThreadData*)malloc(sizeof(ThreadData));
    if (!data) {
        RETURN_NIL;
    }

    data->vm = vm;
    data->caller_thread = vm_current_thread(vm);
    data->closure = AS_CLOSURE(args[0]);
    data->arg_count = arg_count - 1;
    data->args = NULL;
    data->result = NIL_VAL;
    data->result_count = 0;
    data->done = 0;
    data->error = 0;
    data->error_msg[0] = '\0';

    // Copy arguments
    if (data->arg_count > 0) {
        data->args = (Value*)malloc(sizeof(Value) * data->arg_count);
        for (int i = 0; i < data->arg_count; i++) {
            data->args[i] = args[i + 1];
        }
    }

    // Release lock before creating OS thread.
    ObjThread* caller = suspend_vm_thread(vm);

    // Create the thread
    int thread_err = pthread_create(&data->pthread, NULL, thread_runner, data);

    // Reacquire lock and restore caller VM thread.
    resume_vm_thread(vm, caller);

    if (thread_err != 0) {
        free(data->args);
        free(data);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("Failed to create thread", 23)));
        return 2;
    }

    ObjUserdata* udata = new_userdata_with_hooks(data, NULL, thread_handle_mark);

    // Set metatable
    Value thread_val;
    ObjString* thread_name = copy_string("thread", 6);
    if (table_get(&vm->globals, thread_name, &thread_val) && IS_TABLE(thread_val)) {
        Value mt;
        ObjString* mt_name = copy_string("_thread_mt", 10);
        if (table_get(&AS_TABLE(thread_val)->table, mt_name, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// thread.join(t) - wait for thread to complete
static int thread_join(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    ThreadData* data = (ThreadData*)udata->data;

    if (!data) {
        RETURN_NIL;
    }

    // Release lock while waiting.
    ObjThread* caller = suspend_vm_thread(vm);

    pthread_join(data->pthread, NULL);

    // Reacquire lock and restore caller VM thread.
    resume_vm_thread(vm, caller);

    if (data->error) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(data->error_msg, strlen(data->error_msg))));

        // Cleanup
        if (data->args) free(data->args);
        free(data);
        udata->data = NULL;

        return 2;
    }

    Value result = data->result;

    // Cleanup
    if (data->args) free(data->args);
    free(data);
    udata->data = NULL;

    RETURN_VAL(result);
}

// thread.yield() - release GIL momentarily to let other threads run
static int thread_yield_native(VM* vm, int arg_count, Value* args) {
    (void)vm; (void)arg_count; (void)args;

    ObjThread* caller = suspend_vm_thread(vm);
    sched_yield(); // Let other threads run
    resume_vm_thread(vm, caller);

    RETURN_NIL;
}

// thread.sleep(seconds) - sleep and release GIL
static int thread_sleep(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_NUMBER(0);

    double seconds = GET_NUMBER(0);

    ObjThread* caller = suspend_vm_thread(vm);
    usleep((unsigned int)(seconds * 1000000));
    resume_vm_thread(vm, caller);

    RETURN_NIL;
}

// thread.mutex() - create a mutex
static int thread_mutex(VM* vm, int arg_count, Value* args) {
    (void)arg_count; (void)args;

    MutexData* data = (MutexData*)malloc(sizeof(MutexData));
    pthread_mutex_init(&data->mutex, NULL);
    data->locked = 0;

    ObjUserdata* udata = new_userdata(data);

    // Set metatable
    Value thread_val;
    ObjString* thread_name = copy_string("thread", 6);
    if (table_get(&vm->globals, thread_name, &thread_val) && IS_TABLE(thread_val)) {
        Value mt;
        ObjString* mt_name = copy_string("_mutex_mt", 9);
        if (table_get(&AS_TABLE(thread_val)->table, mt_name, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// mutex:lock()
static int mutex_lock(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MutexData* data = (MutexData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    // Release lock while waiting for mutex.
    ObjThread* caller = suspend_vm_thread(vm);
    pthread_mutex_lock(&data->mutex);
    resume_vm_thread(vm, caller);

    data->locked = 1;
    RETURN_TRUE;
}

// mutex:unlock()
static int mutex_unlock(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MutexData* data = (MutexData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    data->locked = 0;
    pthread_mutex_unlock(&data->mutex);

    RETURN_TRUE;
}

// mutex:trylock()
static int mutex_trylock(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MutexData* data = (MutexData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    if (pthread_mutex_trylock(&data->mutex) == 0) {
        data->locked = 1;
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

// thread.channel(capacity?) - create a channel for thread communication
static int thread_channel(VM* vm, int arg_count, Value* args) {
    int capacity = 0; // unbounded by default
    if (arg_count >= 1 && IS_NUMBER(args[0])) {
        capacity = (int)AS_NUMBER(args[0]);
    }

    ChannelData* data = (ChannelData*)malloc(sizeof(ChannelData));
    pthread_mutex_init(&data->mutex, NULL);
    pthread_cond_init(&data->not_empty, NULL);
    pthread_cond_init(&data->not_full, NULL);
    data->head = NULL;
    data->tail = NULL;
    data->count = 0;
    data->capacity = capacity;
    data->closed = 0;

    ObjUserdata* udata = new_userdata_with_hooks(data, NULL, channel_mark);

    // Set metatable
    Value thread_val;
    ObjString* thread_name = copy_string("thread", 6);
    if (table_get(&vm->globals, thread_name, &thread_val) && IS_TABLE(thread_val)) {
        Value mt;
        ObjString* mt_name = copy_string("_channel_mt", 11);
        if (table_get(&AS_TABLE(thread_val)->table, mt_name, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// channel:send(value)
static int channel_send(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data || data->closed) { RETURN_FALSE; }

    Value value = args[1];

    // Release lock while waiting on channel mutex/condition.
    ObjThread* caller = suspend_vm_thread(vm);
    pthread_mutex_lock(&data->mutex);

    // Wait if channel is full (bounded)
    while (data->capacity > 0 && data->count >= data->capacity && !data->closed) {
        pthread_cond_wait(&data->not_full, &data->mutex);
    }

    if (data->closed) {
        pthread_mutex_unlock(&data->mutex);
        resume_vm_thread(vm, caller);
        RETURN_FALSE;
    }

    // Add to queue
    ChannelNode* node = (ChannelNode*)malloc(sizeof(ChannelNode));
    node->value = value;
    node->next = NULL;

    if (data->tail) {
        data->tail->next = node;
    } else {
        data->head = node;
    }
    data->tail = node;
    data->count++;

    pthread_cond_signal(&data->not_empty);
    pthread_mutex_unlock(&data->mutex);

    resume_vm_thread(vm, caller);
    RETURN_TRUE;
}

// channel:recv()
static int channel_recv(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_NIL; }

    // Release lock while waiting on channel mutex/condition.
    ObjThread* caller = suspend_vm_thread(vm);
    pthread_mutex_lock(&data->mutex);

    // Wait for data
    while (data->count == 0 && !data->closed) {
        pthread_cond_wait(&data->not_empty, &data->mutex);
    }

    if (data->count == 0 && data->closed) {
        pthread_mutex_unlock(&data->mutex);
        resume_vm_thread(vm, caller);
        RETURN_NIL;
    }

    // Get from queue
    ChannelNode* node = data->head;
    Value value = node->value;
    data->head = node->next;
    if (!data->head) data->tail = NULL;
    data->count--;
    free(node);

    pthread_cond_signal(&data->not_full);
    pthread_mutex_unlock(&data->mutex);

    resume_vm_thread(vm, caller);
    RETURN_VAL(value);
}

// channel:close()
static int channel_close(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    pthread_mutex_lock(&data->mutex);
    data->closed = 1;
    pthread_cond_broadcast(&data->not_empty);
    pthread_cond_broadcast(&data->not_full);
    pthread_mutex_unlock(&data->mutex);

    RETURN_TRUE;
}

// channel:tryrecv()
static int channel_tryrecv(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data) {
        push(vm, NIL_VAL);
        push(vm, BOOL_VAL(0));
        return 2;
    }

    pthread_mutex_lock(&data->mutex);

    if (data->count == 0) {
        pthread_mutex_unlock(&data->mutex);
        push(vm, NIL_VAL);
        push(vm, BOOL_VAL(0));
        return 2;
    }

    ChannelNode* node = data->head;
    Value value = node->value;
    data->head = node->next;
    if (!data->head) data->tail = NULL;
    data->count--;
    free(node);

    pthread_cond_signal(&data->not_full);
    pthread_mutex_unlock(&data->mutex);

    push(vm, value);
    push(vm, BOOL_VAL(1));
    return 2;
}

void register_thread(VM* vm) {
    // Initialize VM-state lock once.
    if (!gil_initialized) {
        gil_initialized = 1;
        const char* no_gil = getenv("PUA_NO_GIL");
        if (no_gil && (no_gil[0] == '1' || no_gil[0] == 'y' || no_gil[0] == 'Y')) {
            no_gil_enabled = 1;
            fprintf(stderr, "WARNING: PUA_NO_GIL enabled (experimental/unsafe).\n");
            fprintf(stderr, "         Shared-VM execution may race or crash.\n");
        }
        if (!no_gil_enabled) {
            acquire_gil();
        }
    }

    const NativeReg thread_funcs[] = {
        {"spawn", thread_spawn},
        {"join", thread_join},
        {"yield", thread_yield_native},
        {"runtime_yield", thread_yield_native},
        {"sleep", thread_sleep},
        {"mutex", thread_mutex},
        {"channel", thread_channel},
        {NULL, NULL}
    };
    register_module(vm, "thread", thread_funcs);
    ObjTable* thread_module = AS_TABLE(peek(vm, 0));

    // Thread handle metatable
    ObjTable* thread_mt = new_table();
    push(vm, OBJ_VAL(thread_mt));

    const NativeReg thread_methods[] = {
        {"join", thread_join},
        {NULL, NULL}
    };

    for (int i = 0; thread_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(thread_methods[i].name, (int)strlen(thread_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        push(vm, OBJ_VAL(new_native(thread_methods[i].function, name_str)));
        table_set(&thread_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copy_string("__index", 7)));
    push(vm, OBJ_VAL(thread_mt));
    table_set(&thread_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("thread.handle", 13)));
    table_set(&thread_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("_thread_mt", 10)));
    push(vm, OBJ_VAL(thread_mt));
    table_set(&thread_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // thread_mt

    // Mutex metatable
    ObjTable* mutex_mt = new_table();
    push(vm, OBJ_VAL(mutex_mt));

    const NativeReg mutex_methods[] = {
        {"lock", mutex_lock},
        {"unlock", mutex_unlock},
        {"trylock", mutex_trylock},
        {NULL, NULL}
    };

    for (int i = 0; mutex_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(mutex_methods[i].name, (int)strlen(mutex_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        push(vm, OBJ_VAL(new_native(mutex_methods[i].function, name_str)));
        table_set(&mutex_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copy_string("__index", 7)));
    push(vm, OBJ_VAL(mutex_mt));
    table_set(&mutex_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("thread.mutex", 12)));
    table_set(&mutex_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("_mutex_mt", 9)));
    push(vm, OBJ_VAL(mutex_mt));
    table_set(&thread_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // mutex_mt

    // Channel metatable
    ObjTable* channel_mt = new_table();
    push(vm, OBJ_VAL(channel_mt));

    const NativeReg channel_methods[] = {
        {"send", channel_send},
        {"recv", channel_recv},
        {"tryrecv", channel_tryrecv},
        {"close", channel_close},
        {NULL, NULL}
    };

    for (int i = 0; channel_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(channel_methods[i].name, (int)strlen(channel_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        push(vm, OBJ_VAL(new_native(channel_methods[i].function, name_str)));
        table_set(&channel_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copy_string("__index", 7)));
    push(vm, OBJ_VAL(channel_mt));
    table_set(&channel_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("thread.channel", 14)));
    table_set(&channel_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copy_string("_channel_mt", 11)));
    push(vm, OBJ_VAL(channel_mt));
    table_set(&thread_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // channel_mt

    pop(vm); // thread_module
}
