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
static int gilInitialized = 0;
static int gilDisabled = 0;  // Set via MYLANG_NO_GIL env var

// Thread data structure
typedef struct {
    pthread_t pthread;
    VM* vm;
    ObjClosure* closure;
    Value* args;
    int argCount;
    Value result;
    int resultCount;
    int done;
    int error;
    char errorMsg[256];
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
    pthread_cond_t notEmpty;
    pthread_cond_t notFull;
    ChannelNode* head;
    ChannelNode* tail;
    int count;
    int capacity; // 0 = unbounded
    int closed;
} ChannelData;

static void acquireGIL(void) {
    if (!gilDisabled) {
        pthread_mutex_lock(&gil);
    }
}

static void releaseGIL(void) {
    if (!gilDisabled) {
        pthread_mutex_unlock(&gil);
    }
}

// Thread entry point
static void* threadRunner(void* arg) {
    ThreadData* data = (ThreadData*)arg;

    acquireGIL();

    // Save the main thread
    ObjThread* mainThread = data->vm->currentThread;

    // Create a new thread like coroutine.create does
    ObjThread* workerThread = newThread();
    workerThread->vm = data->vm;

    // Push closure onto the worker thread's stack
    workerThread->stack[0] = OBJ_VAL(data->closure);
    workerThread->stackTop = workerThread->stack + 1;

    // Push arguments
    for (int i = 0; i < data->argCount; i++) {
        *workerThread->stackTop = data->args[i];
        workerThread->stackTop++;
    }

    // Set up the call frame
    CallFrame* frame = &workerThread->frames[0];
    frame->closure = data->closure;
    frame->ip = data->closure->function->chunk.code;
    frame->slots = workerThread->stack;
    workerThread->frameCount = 1;

    // Switch to worker thread and run
    data->vm->currentThread = workerThread;

    // Set REPL mode to keep result on stack after vmRun
    int savedREPL = data->vm->isREPL;
    data->vm->isREPL = 1;

    InterpretResult result = vmRun(data->vm, 0);

    data->vm->isREPL = savedREPL;

    if (result != INTERPRET_OK) {
        data->error = 1;
        snprintf(data->errorMsg, sizeof(data->errorMsg), "Thread execution error");
    } else {
        // After vmRun completes, the result is on top of the stack
        ObjThread* t = data->vm->currentThread;
        if (t->stackTop > t->stack) {
            data->result = t->stackTop[-1];
            data->resultCount = 1;
        } else {
            data->result = NIL_VAL;
            data->resultCount = 0;
        }
    }

    // Restore main thread
    data->vm->currentThread = mainThread;
    data->done = 1;
    releaseGIL();

    return NULL;
}

// thread.spawn(fn, ...) - create and start a new thread
static int thread_spawn(VM* vm, int argCount, Value* args) {
    if (argCount < 1 || !IS_CLOSURE(args[0])) {
        vmRuntimeError(vm, "thread.spawn requires a function as first argument");
        return 0;
    }

    ThreadData* data = (ThreadData*)malloc(sizeof(ThreadData));
    if (!data) {
        RETURN_NIL;
    }

    data->vm = vm;
    data->closure = AS_CLOSURE(args[0]);
    data->argCount = argCount - 1;
    data->args = NULL;
    data->result = NIL_VAL;
    data->resultCount = 0;
    data->done = 0;
    data->error = 0;
    data->errorMsg[0] = '\0';

    // Copy arguments
    if (data->argCount > 0) {
        data->args = (Value*)malloc(sizeof(Value) * data->argCount);
        for (int i = 0; i < data->argCount; i++) {
            data->args[i] = args[i + 1];
        }
    }

    // Release GIL before creating thread
    releaseGIL();

    // Create the thread
    int err = pthread_create(&data->pthread, NULL, threadRunner, data);

    // Reacquire GIL
    acquireGIL();

    if (err != 0) {
        free(data->args);
        free(data);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString("Failed to create thread", 23)));
        return 2;
    }

    ObjUserdata* udata = newUserdata(data);

    // Set metatable
    Value threadVal;
    ObjString* threadName = copyString("thread", 6);
    if (tableGet(&vm->globals, threadName, &threadVal) && IS_TABLE(threadVal)) {
        Value mt;
        ObjString* mtName = copyString("_thread_mt", 10);
        if (tableGet(&AS_TABLE(threadVal)->table, mtName, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// thread.join(t) - wait for thread to complete
static int thread_join(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    ThreadData* data = (ThreadData*)udata->data;

    if (!data) {
        RETURN_NIL;
    }

    // Release GIL while waiting
    releaseGIL();

    pthread_join(data->pthread, NULL);

    // Reacquire GIL
    acquireGIL();

    if (data->error) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(data->errorMsg, strlen(data->errorMsg))));

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
static int thread_yield_native(VM* vm, int argCount, Value* args) {
    (void)vm; (void)argCount; (void)args;

    releaseGIL();
    sched_yield(); // Let other threads run
    acquireGIL();

    RETURN_NIL;
}

// thread.sleep(seconds) - sleep and release GIL
static int thread_sleep(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_NUMBER(0);

    double seconds = GET_NUMBER(0);

    releaseGIL();
    usleep((unsigned int)(seconds * 1000000));
    acquireGIL();

    RETURN_NIL;
}

// thread.mutex() - create a mutex
static int thread_mutex(VM* vm, int argCount, Value* args) {
    (void)argCount; (void)args;

    MutexData* data = (MutexData*)malloc(sizeof(MutexData));
    pthread_mutex_init(&data->mutex, NULL);
    data->locked = 0;

    ObjUserdata* udata = newUserdata(data);

    // Set metatable
    Value threadVal;
    ObjString* threadName = copyString("thread", 6);
    if (tableGet(&vm->globals, threadName, &threadVal) && IS_TABLE(threadVal)) {
        Value mt;
        ObjString* mtName = copyString("_mutex_mt", 9);
        if (tableGet(&AS_TABLE(threadVal)->table, mtName, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// mutex:lock()
static int mutex_lock(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MutexData* data = (MutexData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    // Release GIL while waiting for mutex
    releaseGIL();
    pthread_mutex_lock(&data->mutex);
    acquireGIL();

    data->locked = 1;
    RETURN_TRUE;
}

// mutex:unlock()
static int mutex_unlock(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MutexData* data = (MutexData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    data->locked = 0;
    pthread_mutex_unlock(&data->mutex);

    RETURN_TRUE;
}

// mutex:trylock()
static int mutex_trylock(VM* vm, int argCount, Value* args) {
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
static int thread_channel(VM* vm, int argCount, Value* args) {
    int capacity = 0; // unbounded by default
    if (argCount >= 1 && IS_NUMBER(args[0])) {
        capacity = (int)AS_NUMBER(args[0]);
    }

    ChannelData* data = (ChannelData*)malloc(sizeof(ChannelData));
    pthread_mutex_init(&data->mutex, NULL);
    pthread_cond_init(&data->notEmpty, NULL);
    pthread_cond_init(&data->notFull, NULL);
    data->head = NULL;
    data->tail = NULL;
    data->count = 0;
    data->capacity = capacity;
    data->closed = 0;

    ObjUserdata* udata = newUserdata(data);

    // Set metatable
    Value threadVal;
    ObjString* threadName = copyString("thread", 6);
    if (tableGet(&vm->globals, threadName, &threadVal) && IS_TABLE(threadVal)) {
        Value mt;
        ObjString* mtName = copyString("_channel_mt", 11);
        if (tableGet(&AS_TABLE(threadVal)->table, mtName, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

// channel:send(value)
static int channel_send(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data || data->closed) { RETURN_FALSE; }

    Value value = args[1];

    // Release GIL, acquire channel mutex
    releaseGIL();
    pthread_mutex_lock(&data->mutex);

    // Wait if channel is full (bounded)
    while (data->capacity > 0 && data->count >= data->capacity && !data->closed) {
        pthread_cond_wait(&data->notFull, &data->mutex);
    }

    if (data->closed) {
        pthread_mutex_unlock(&data->mutex);
        acquireGIL();
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

    pthread_cond_signal(&data->notEmpty);
    pthread_mutex_unlock(&data->mutex);

    acquireGIL();
    RETURN_TRUE;
}

// channel:recv()
static int channel_recv(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_NIL; }

    // Release GIL, acquire channel mutex
    releaseGIL();
    pthread_mutex_lock(&data->mutex);

    // Wait for data
    while (data->count == 0 && !data->closed) {
        pthread_cond_wait(&data->notEmpty, &data->mutex);
    }

    if (data->count == 0 && data->closed) {
        pthread_mutex_unlock(&data->mutex);
        acquireGIL();
        RETURN_NIL;
    }

    // Get from queue
    ChannelNode* node = data->head;
    Value value = node->value;
    data->head = node->next;
    if (!data->head) data->tail = NULL;
    data->count--;
    free(node);

    pthread_cond_signal(&data->notFull);
    pthread_mutex_unlock(&data->mutex);

    acquireGIL();
    RETURN_VAL(value);
}

// channel:close()
static int channel_close(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ChannelData* data = (ChannelData*)GET_USERDATA(0)->data;
    if (!data) { RETURN_FALSE; }

    pthread_mutex_lock(&data->mutex);
    data->closed = 1;
    pthread_cond_broadcast(&data->notEmpty);
    pthread_cond_broadcast(&data->notFull);
    pthread_mutex_unlock(&data->mutex);

    RETURN_TRUE;
}

// channel:tryrecv()
static int channel_tryrecv(VM* vm, int argCount, Value* args) {
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

    pthread_cond_signal(&data->notFull);
    pthread_mutex_unlock(&data->mutex);

    push(vm, value);
    push(vm, BOOL_VAL(1));
    return 2;
}

void registerThread(VM* vm) {
    // Check for GIL disable option
    if (!gilInitialized) {
        gilInitialized = 1;
        const char* noGil = getenv("MYLANG_NO_GIL");
        if (noGil && (noGil[0] == '1' || noGil[0] == 'y' || noGil[0] == 'Y')) {
            gilDisabled = 1;
            fprintf(stderr, "WARNING: GIL disabled. VM is not thread-safe - expect crashes!\n");
            fprintf(stderr, "         Only safe for threads with completely isolated data.\n");
        } else {
            acquireGIL();
        }
    }

    const NativeReg threadFuncs[] = {
        {"spawn", thread_spawn},
        {"join", thread_join},
        {"yield", thread_yield_native},
        {"sleep", thread_sleep},
        {"mutex", thread_mutex},
        {"channel", thread_channel},
        {NULL, NULL}
    };
    registerModule(vm, "thread", threadFuncs);
    ObjTable* threadModule = AS_TABLE(peek(vm, 0));

    // Thread handle metatable
    ObjTable* threadMT = newTable();
    push(vm, OBJ_VAL(threadMT));

    const NativeReg threadMethods[] = {
        {"join", thread_join},
        {NULL, NULL}
    };

    for (int i = 0; threadMethods[i].name != NULL; i++) {
        ObjString* nameStr = copyString(threadMethods[i].name, (int)strlen(threadMethods[i].name));
        push(vm, OBJ_VAL(nameStr));
        push(vm, OBJ_VAL(newNative(threadMethods[i].function, nameStr)));
        tableSet(&threadMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copyString("__index", 7)));
    push(vm, OBJ_VAL(threadMT));
    tableSet(&threadMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copyString("_thread_mt", 10)));
    push(vm, OBJ_VAL(threadMT));
    tableSet(&threadModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // threadMT

    // Mutex metatable
    ObjTable* mutexMT = newTable();
    push(vm, OBJ_VAL(mutexMT));

    const NativeReg mutexMethods[] = {
        {"lock", mutex_lock},
        {"unlock", mutex_unlock},
        {"trylock", mutex_trylock},
        {NULL, NULL}
    };

    for (int i = 0; mutexMethods[i].name != NULL; i++) {
        ObjString* nameStr = copyString(mutexMethods[i].name, (int)strlen(mutexMethods[i].name));
        push(vm, OBJ_VAL(nameStr));
        push(vm, OBJ_VAL(newNative(mutexMethods[i].function, nameStr)));
        tableSet(&mutexMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copyString("__index", 7)));
    push(vm, OBJ_VAL(mutexMT));
    tableSet(&mutexMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copyString("_mutex_mt", 9)));
    push(vm, OBJ_VAL(mutexMT));
    tableSet(&threadModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // mutexMT

    // Channel metatable
    ObjTable* channelMT = newTable();
    push(vm, OBJ_VAL(channelMT));

    const NativeReg channelMethods[] = {
        {"send", channel_send},
        {"recv", channel_recv},
        {"tryrecv", channel_tryrecv},
        {"close", channel_close},
        {NULL, NULL}
    };

    for (int i = 0; channelMethods[i].name != NULL; i++) {
        ObjString* nameStr = copyString(channelMethods[i].name, (int)strlen(channelMethods[i].name));
        push(vm, OBJ_VAL(nameStr));
        push(vm, OBJ_VAL(newNative(channelMethods[i].function, nameStr)));
        tableSet(&channelMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copyString("__index", 7)));
    push(vm, OBJ_VAL(channelMT));
    tableSet(&channelMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);

    push(vm, OBJ_VAL(copyString("_channel_mt", 11)));
    push(vm, OBJ_VAL(channelMT));
    tableSet(&threadModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); pop(vm);
    pop(vm); // channelMT

    pop(vm); // threadModule
}
