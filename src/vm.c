#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#ifndef TOI_WASM
#include <pthread.h>
#endif

#include "common.h"
#include "table.h"
#include "vm.h"
#include "vm/build_string.h"
#include "vm/ops_arith.h"
#include "vm/ops_arith_const.h"
#include "vm/ops_compare.h"
#include "vm/ops_control.h"
#include "vm/ops_exception.h"
#include "vm/ops_float.h"
#include "vm/ops_has.h"
#include "vm/ops_import.h"
#include "vm/ops_import_star.h"
#include "vm/ops_iter.h"
#include "vm/ops_local_const.h"
#include "vm/ops_local_set.h"
#include "vm/ops_meta.h"
#include "vm/ops_mod.h"
#include "vm/ops_power.h"
#include "vm/ops_print.h"
#include "vm/ops_state.h"
#include "vm/ops_table.h"
#include "vm/ops_unary.h"
#include "lib/libs.h" // For register_libs
#include "object.h" // For free_object

// #define DEBUG_TABLE_KEYS

// Forward declaration for compile function
ObjFunction* compile(const char* source);
static void close_upvalues(VM* vm, Value* last);

static inline int to_int64(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

// Returns 1 when handled, 0 when not handled, -1 on runtime error.
static int try_fast_native_call(VM* vm, ObjNative* native, int arg_count, Value* args) {
    switch ((NativeFastKind)native->fast_kind) {
        case NATIVE_FAST_NONE:
            return 0;
        case NATIVE_FAST_MATH_SIN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(sin(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_COS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(cos(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_TAN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(tan(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ASIN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(asin(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ACOS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(acos(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ATAN:
            if (arg_count == 1 && IS_NUMBER(args[0])) {
                push(vm, NUMBER_VAL(atan(AS_NUMBER(args[0]))));
                return 1;
            }
            if (arg_count == 2 && IS_NUMBER(args[0]) && IS_NUMBER(args[1])) {
                push(vm, NUMBER_VAL(atan2(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
                return 1;
            }
            goto fast_arg_error;
        case NATIVE_FAST_MATH_SQRT:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(sqrt(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_FLOOR:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(floor(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_CEIL:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(ceil(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ABS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(fabs(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_EXP:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(exp(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_LOG:
            if (arg_count == 1 && IS_NUMBER(args[0])) {
                push(vm, NUMBER_VAL(log(AS_NUMBER(args[0]))));
                return 1;
            }
            if (arg_count == 2 && IS_NUMBER(args[0]) && IS_NUMBER(args[1])) {
                push(vm, NUMBER_VAL(log(AS_NUMBER(args[0])) / log(AS_NUMBER(args[1]))));
                return 1;
            }
            goto fast_arg_error;
        case NATIVE_FAST_MATH_POW:
            if (arg_count != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) goto fast_arg_error;
            push(vm, NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
            return 1;
        case NATIVE_FAST_MATH_FMOD:
            if (arg_count != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) goto fast_arg_error;
            push(vm, NUMBER_VAL(fmod(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
            return 1;
        case NATIVE_FAST_MATH_DEG:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(AS_NUMBER(args[0]) * (180.0 / 3.14159265358979323846)));
            return 1;
        case NATIVE_FAST_MATH_RAD:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(AS_NUMBER(args[0]) * (3.14159265358979323846 / 180.0)));
            return 1;
    }
    return 0;

fast_arg_error:
    // Fall back to native implementation for exact error text/semantics.
    return 0;
}

static int value_matches_type(Value v, uint8_t type) {
    switch (type) {
        case TYPEHINT_ANY:
            return 1;
        case TYPEHINT_INT: {
            if (!IS_NUMBER(v)) return 0;
            int64_t tmp;
            return to_int64(AS_NUMBER(v), &tmp);
        }
        case TYPEHINT_FLOAT:
            return IS_NUMBER(v);
        case TYPEHINT_BOOL:
            return IS_BOOL(v);
        case TYPEHINT_STR:
            return IS_STRING(v);
        case TYPEHINT_TABLE:
            return IS_TABLE(v);
        default:
            return 0;
    }
}

static void apply_pending_set_local(VM* vm) {
    ObjThread* thread = vm_current_thread(vm);
    if (thread->pending_set_local_count == 0) return;
    int top = thread->pending_set_local_count - 1;
    int frame_index = thread->pending_set_local_frames[top];
    if (frame_index != vm_current_thread(vm)->frame_count - 1) return;
    CallFrame* target = &vm_current_thread(vm)->frames[frame_index];
    target->slots[thread->pending_set_local_slots[top]] = peek(vm, 0);
    thread->pending_set_local_count--;
}

static int vm_handle_op_unpack(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t base_depth = *(*ip)++;
    uint8_t target_count = *(*ip)++;
    Value* base = frame->slots + base_depth;
    ptrdiff_t available = vm_current_thread(vm)->stack_top - base;
    if (available < 0) {
        vm_runtime_error(vm, "Internal error: invalid stack state in unpack.");
        return 0;
    }

    if (available == 1 && IS_TABLE(base[0])) {
        ObjTable* values = AS_TABLE(base[0]);
        for (int i = 0; i < target_count; i++) {
            Value element = NIL_VAL;
            table_get_array(&values->table, i + 1, &element);
            base[i] = element;
        }
        vm_current_thread(vm)->stack_top = base + target_count;
        return 1;
    }

    int have = (int)available;
    if (have < target_count) {
        for (int i = have; i < target_count; i++) {
            base[i] = NIL_VAL;
        }
    }
    vm_current_thread(vm)->stack_top = base + (have > target_count ? have : target_count);
    return 1;
}

extern void register_libs(VM* vm);
extern void free_object(struct Obj* object);
void collect_garbage(VM* vm); // Defined later in this file

static void reset_stack(VM* vm) {
    if (vm_current_thread(vm) != NULL) {
        vm_current_thread(vm)->stack_top = vm_current_thread(vm)->stack;
        vm_current_thread(vm)->frame_count = 0;
        vm_current_thread(vm)->open_upvalues = NULL;
    }
}

void vm_runtime_error(VM* vm, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    vm_current_thread(vm)->has_exception = 1;
    vm_current_thread(vm)->exception = OBJ_VAL(copy_string(buffer, (int)strlen(buffer)));
    vm_current_thread(vm)->last_error = vm_current_thread(vm)->exception;
}

static void report_exception(VM* vm) {
    if (!vm_current_thread(vm)->has_exception) return;
    Value ex = vm_current_thread(vm)->exception;
    if (IS_STRING(ex)) {
        fprintf(stderr, COLOR_RED "Error: " COLOR_RESET "%s\n", AS_CSTRING(ex));
    } else if (IS_TABLE(ex)) {
        ObjTable* t = AS_TABLE(ex);
        Value msg = NIL_VAL;
        Value type = NIL_VAL;
        ObjString* msg_key = copy_string("msg", 3);
        ObjString* type_key = copy_string("type", 4);
        table_get(&t->table, msg_key, &msg);
        table_get(&t->table, type_key, &type);

        const char* msg_s = IS_STRING(msg) ? AS_CSTRING(msg) : "<exception>";
        const char* type_s = IS_STRING(type) ? AS_CSTRING(type) : "Error";
        fprintf(stderr, COLOR_RED "%s: " COLOR_RESET "%s\n", type_s, msg_s);
    } else {
        fprintf(stderr, COLOR_RED "Error: " COLOR_RESET "<exception>\n");
    }

    for (int i = vm_current_thread(vm)->frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm_current_thread(vm)->frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
}

Value peek(VM* vm, int distance) {
    return vm_current_thread(vm)->stack_top[-1 - distance];
}

static int handle_exception(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (!vm_current_thread(vm)->has_exception) return 0;
    ObjThread* thread = vm_current_thread(vm);
    while (thread->handler_count > 0) {
        ExceptionHandler* handler = &thread->handlers[thread->handler_count - 1];

        while (thread->frame_count > handler->frame_count) {
            CallFrame* f = &thread->frames[thread->frame_count - 1];
            close_upvalues(vm, f->slots);
            thread->frame_count--;
        }

        if (thread->frame_count == 0) {
            report_exception(vm);
            reset_stack(vm);
            thread->has_exception = 0;
            thread->exception = NIL_VAL;
            return 0;
        }

        *frame = &thread->frames[thread->frame_count - 1];
        thread->stack_top = handler->stack_top;

        if (handler->has_except && !handler->in_except) {
            handler->in_except = 1;
            *ip = handler->except_ip;
            push(vm, thread->exception);
            thread->has_exception = 0;
            thread->exception = NIL_VAL;
            return 1;
        }

        if (handler->has_finally && handler->finally_ip != NULL) {
            thread->handler_count--;
            *ip = handler->finally_ip;
            return 1;
        }

        thread->handler_count--;
    }

    report_exception(vm);
    reset_stack(vm);
    thread->has_exception = 0;
    thread->exception = NIL_VAL;
    return 0;
}

static volatile sig_atomic_t interrupt_requested = 0;

#ifndef TOI_WASM
static pthread_key_t current_thread_key;
static pthread_once_t current_thread_key_once = PTHREAD_ONCE_INIT;

static void make_current_thread_key(void) {
    pthread_key_create(&current_thread_key, NULL);
}

ObjThread* vm_current_thread(VM* vm) {
    if (!vm->use_thread_tls) {
        return vm->current_thread;
    }
    pthread_once(&current_thread_key_once, make_current_thread_key);
    ObjThread* t = (ObjThread*)pthread_getspecific(current_thread_key);
    if (t != NULL) return t;
    return vm->current_thread;
}

void vm_set_current_thread(VM* vm, ObjThread* thread) {
    if (!vm->use_thread_tls) {
        vm->current_thread = thread;
        return;
    }
    pthread_once(&current_thread_key_once, make_current_thread_key);
    pthread_setspecific(current_thread_key, thread);
    if (thread == NULL || thread == vm->current_thread) return;
    if (vm->current_thread == NULL) {
        vm->current_thread = thread;
    }
}

void vm_enable_thread_tls(VM* vm) {
    if (vm->use_thread_tls) return;
    vm->use_thread_tls = 1;
    pthread_once(&current_thread_key_once, make_current_thread_key);
    pthread_setspecific(current_thread_key, vm->current_thread);
}
#else
ObjThread* vm_current_thread(VM* vm) {
    return vm->current_thread;
}

void vm_set_current_thread(VM* vm, ObjThread* thread) {
    vm->current_thread = thread;
}

void vm_enable_thread_tls(VM* vm) {
    (void)vm;
}
#endif

void vm_request_interrupt(void) {
    interrupt_requested = 1;
}

void push(VM* vm, Value value) {
    *vm_current_thread(vm)->stack_top = value;
    vm_current_thread(vm)->stack_top++;
}

Value pop(VM* vm) {
    vm_current_thread(vm)->stack_top--;
    return *vm_current_thread(vm)->stack_top;
}

static Value get_metamethod_cached(VM* vm, Value val, ObjString* name);
static int call_named(VM* vm, ObjClosure* closure, int arg_count);

static int finish_closure_call(VM* vm, ObjClosure* closure, int arg_count) {
    ObjFunction* function = closure->function;

    if (function->param_types_count > 0) {
        int check_count = function->param_types_count < function->arity ? function->param_types_count : function->arity;
        Value* args = vm_current_thread(vm)->stack_top - arg_count;
        for (int i = 0; i < check_count; i++) {
            uint8_t type = function->param_types[i];
            if (type == TYPEHINT_ANY) continue;
            if (!value_matches_type(args[i], type)) {
                vm_runtime_error(vm, "Type mismatch for parameter %d.", i + 1);
                return 0;
            }
        }
    }

    if (vm_current_thread(vm)->frame_count == vm_current_thread(vm)->frame_capacity) {
        vm_runtime_error(vm, "Stack overflow.");
        return 0;
    }

    CallFrame* frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count++];
    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = vm_current_thread(vm)->stack_top - arg_count - 1;
    frame->restore_module_context = 0;
    frame->cache_module_result = 0;
    frame->had_prev_module_name = 0;
    frame->had_prev_module_file = 0;
    frame->had_prev_module_main = 0;
    frame->module_cache_name = NIL_VAL;
    frame->prev_module_name = NIL_VAL;
    frame->prev_module_file = NIL_VAL;
    frame->prev_module_main = NIL_VAL;
    return 1;
}

int call_value(VM* vm, Value callee, int arg_count, CallFrame** frame, uint8_t** ip) {
    if (IS_NATIVE(callee)) {
        ObjNative* native_obj = AS_NATIVE_OBJ(callee);
        NativeFn native = native_obj->function;
        Value* args = vm_current_thread(vm)->stack_top - arg_count;
        vm_current_thread(vm)->stack_top -= arg_count + 1; // Pop args and callee

        (*frame)->ip = *ip;
        int fast = try_fast_native_call(vm, native_obj, arg_count, args);
        if (fast > 0) {
            return 1;
        }
        ObjThread* current = vm_current_thread(vm);
        if (!native(vm, arg_count, args)) {
            return 0;
        }

        if (vm_current_thread(vm) != current) {
            *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
            *ip = (*frame)->ip;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(callee), arg_count)) {
            return 0;
        }
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    return 0;
}

static int create_generator_positional(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* caller = vm_current_thread(vm);
    Value* args_start = caller->stack_top - arg_count;

    ObjThread* gen = new_thread_with_caps(GEN_STACK_MAX, GEN_FRAMES_MAX, GEN_HANDLERS_MAX);
    gen->vm = vm;
    gen->is_generator = 1;
    gen->generator_mode = 0;
    gen->generator_index = 0;

    vm_set_current_thread(vm, gen);
    push(vm, OBJ_VAL(closure));
    for (int i = 0; i < arg_count; i++) {
        push(vm, args_start[i]);
    }
    if (!call(vm, closure, arg_count)) {
        vm_set_current_thread(vm, caller);
        return 0;
    }
    vm_set_current_thread(vm, caller);

    caller->stack_top -= arg_count + 1;
    push(vm, OBJ_VAL(gen));
    return 1;
}

static int create_generator_named(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* caller = vm_current_thread(vm);
    Value* args_start = caller->stack_top - arg_count;

    ObjThread* gen = new_thread_with_caps(GEN_STACK_MAX, GEN_FRAMES_MAX, GEN_HANDLERS_MAX);
    gen->vm = vm;
    gen->is_generator = 1;
    gen->generator_mode = 0;
    gen->generator_index = 0;

    vm_set_current_thread(vm, gen);
    push(vm, OBJ_VAL(closure));
    for (int i = 0; i < arg_count; i++) {
        push(vm, args_start[i]);
    }
    if (!call_named(vm, closure, arg_count)) {
        vm_set_current_thread(vm, caller);
        return 0;
    }
    vm_set_current_thread(vm, caller);

    caller->stack_top -= arg_count + 1;
    push(vm, OBJ_VAL(gen));
    return 1;
}

static int invoke_call_with_arg_count(VM* vm, int arg_count, CallFrame** frame, uint8_t** ip) {
    Value callee = peek(vm, arg_count);
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);

        // Stack: [callee, arg1, ..., arg_n]
        // We want: [method, receiver, arg1, ..., arg_n]
        for (int i = 0; i < arg_count; i++) {
            vm_current_thread(vm)->stack_top[0 - i] = vm_current_thread(vm)->stack_top[-1 - i];
        }
        vm_current_thread(vm)->stack_top[-arg_count] = bound->receiver;
        vm_current_thread(vm)->stack_top[-arg_count - 1] = method_val;
        vm_current_thread(vm)->stack_top++;
        arg_count++;
        callee = method_val;
    }

    if (IS_CLOSURE(callee) && AS_CLOSURE(callee)->function->is_generator) {
        if (!create_generator_positional(vm, AS_CLOSURE(callee), arg_count)) {
            return 0;
        }
        return 1;
    }

    if (IS_NATIVE(callee) || IS_CLOSURE(callee)) {
        if (!call_value(vm, callee, arg_count, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_TABLE(callee)) {
        // __call metamethod: __call(table, ...)
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            // Stack: [callee, arg1, ..., arg_n]
            // We want: [mm, callee, arg1, ..., arg_n]
            for (int i = 0; i < arg_count; i++) {
                vm_current_thread(vm)->stack_top[0 - i] = vm_current_thread(vm)->stack_top[-1 - i];
            }

            vm_current_thread(vm)->stack_top[-arg_count] = callee;      // Insert table as first arg
            vm_current_thread(vm)->stack_top[-arg_count - 1] = mm;      // Replace callee slot with callable
            vm_current_thread(vm)->stack_top++;                        // Increase stack size

            arg_count++;
            if (!call_value(vm, mm, arg_count, frame, ip)) {
                return 0;
            }
            return 1;
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static int invoke_call_with_named_arg_count(VM* vm, int arg_count, CallFrame** frame, uint8_t** ip) {
    Value callee = peek(vm, arg_count);
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);

        for (int i = 0; i < arg_count; i++) {
            vm_current_thread(vm)->stack_top[0 - i] = vm_current_thread(vm)->stack_top[-1 - i];
        }
        vm_current_thread(vm)->stack_top[-arg_count] = bound->receiver;
        vm_current_thread(vm)->stack_top[-arg_count - 1] = method_val;
        vm_current_thread(vm)->stack_top++;
        arg_count++;
        callee = method_val;
    }

    if (IS_NATIVE(callee)) {
        if (!call_value(vm, callee, arg_count, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        if (AS_CLOSURE(callee)->function->is_generator) {
            if (!create_generator_named(vm, AS_CLOSURE(callee), arg_count)) {
                return 0;
            }
            return 1;
        }
        (*frame)->ip = *ip;
        if (!call_named(vm, AS_CLOSURE(callee), arg_count)) {
            return 0;
        }
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_TABLE(callee)) {
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            for (int i = 0; i < arg_count; i++) {
                vm_current_thread(vm)->stack_top[0 - i] = vm_current_thread(vm)->stack_top[-1 - i];
            }

            vm_current_thread(vm)->stack_top[-arg_count] = callee;
            vm_current_thread(vm)->stack_top[-arg_count - 1] = mm;
            vm_current_thread(vm)->stack_top++;

            arg_count++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!call_named(vm, AS_CLOSURE(mm), arg_count)) {
                    return 0;
                }
                *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                *ip = (*frame)->ip;
            } else {
                if (!call_value(vm, mm, arg_count, frame, ip)) {
                    return 0;
                }
            }
            return 1;
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static void discard_handlers_for_frame_return(ObjThread* thread) {
    int current_frame_count = thread->frame_count;
    while (thread->handler_count > 0 &&
           thread->handlers[thread->handler_count - 1].frame_count >= current_frame_count) {
        thread->handler_count--;
    }
}

static Value get_metamethod_cached(VM* vm, Value val, ObjString* name) {
    (void)vm;
    Value method = NIL_VAL;
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            table_get(&table->metatable->table, name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            table_get(&udata->metatable->table, name, &method);
        }
    }
    return method;
}

static ObjUpvalue* capture_upvalue(VM* vm, Value* local) {
    ObjUpvalue* prev_upvalue = NULL;
    ObjUpvalue* upvalue = vm_current_thread(vm)->open_upvalues;

    // Find existing upvalue or position to insert
    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    // If we found an existing upvalue for this slot, reuse it
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    // Create new upvalue
    ObjUpvalue* created_upvalue = new_upvalue(local);
    created_upvalue->next = upvalue;

    if (prev_upvalue == NULL) {
        vm_current_thread(vm)->open_upvalues = created_upvalue;
    } else {
        prev_upvalue->next = created_upvalue;
    }

    return created_upvalue;
}

static void close_upvalues(VM* vm, Value* last) {

    while (vm_current_thread(vm)->open_upvalues != NULL &&

           vm_current_thread(vm)->open_upvalues->location >= last) {

        ObjUpvalue* upvalue = vm_current_thread(vm)->open_upvalues;

        upvalue->closed = *upvalue->location;

        upvalue->location = &upvalue->closed;

        vm_current_thread(vm)->open_upvalues = upvalue->next;

    }

}

int call(VM* vm, ObjClosure* closure, int arg_count) {
    ObjFunction* function = closure->function;

    // Handle variadic functions
    if (function->is_variadic) {
        int required_args = function->arity - 1;  // Minus the varargs parameter

        if (arg_count < required_args) {
            vm_runtime_error(vm, "Expected at least %d arguments but got %d.", required_args, arg_count);
            return 0;
        }

        // Collect extra arguments into a table
        int extra_args = arg_count - required_args;
        ObjTable* varargs = new_table();

        // Pop extra args and put them in the varargs array part so
        // call-spread (`fn(*args)`) can iterate them via table_get_array.
        for (int i = 0; i < extra_args; i++) {
            Value arg = vm_current_thread(vm)->stack_top[-extra_args + i];
            table_set_array(&varargs->table, i + 1, arg);
            #ifdef DEBUG_VARIADIC
            printf("Vararg[%d] = ", i + 1);
            print_value(arg);
            printf("\n");
            #endif
        }

        // Remove the extra args from the stack
        vm_current_thread(vm)->stack_top -= extra_args;

        // Push the varargs table as the last argument
        push(vm, OBJ_VAL(varargs));

        #ifdef DEBUG_VARIADIC
        printf("Created varargs table with %d args\n", extra_args);
        #endif
        arg_count = function->arity;
    } else {
        // Non-variadic function handling
        if (arg_count > function->arity) {
            vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, arg_count);
            return 0;
        }

        if (arg_count < function->arity) {
            if (function->defaults_count == 0) {
                vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, arg_count);
                return 0;
            }

            int default_start = function->arity - function->defaults_count;
            if (arg_count < default_start) {
                vm_runtime_error(vm, "Expected at least %d arguments (non-default parameters) but got %d.", default_start, arg_count);
                return 0;
            }

            for (int i = arg_count; i < function->arity; i++) {
                push(vm, function->defaults[i - default_start]);
            }
            arg_count = function->arity;
        }
    }

    return finish_closure_call(vm, closure, arg_count);
}

static int find_named_param_index(ObjFunction* function, ObjString* key, int non_variadic_arity) {
    if (function->param_names == NULL) return -1;
    int limit = function->param_names_count < non_variadic_arity ? function->param_names_count : non_variadic_arity;
    for (int i = 0; i < limit; i++) {
        ObjString* name = function->param_names[i];
        if (name == NULL) continue;
        if (name == key) return i;
        if (name->hash == key->hash &&
            name->length == key->length &&
            memcmp(name->chars, key->chars, name->length) == 0) {
            return i;
        }
    }
    return -1;
}

static int is_options_param_name(ObjString* name) {
    if (name == NULL) return 0;
    if (name->length == 4 && memcmp(name->chars, "opts", 4) == 0) return 1;
    if (name->length == 7 && memcmp(name->chars, "options", 7) == 0) return 1;
    if (name->length == 6 && memcmp(name->chars, "kwargs", 6) == 0) return 1;
    return 0;
}

static int call_named(VM* vm, ObjClosure* closure, int arg_count) {
    ObjFunction* function = closure->function;
    if (arg_count < 1) {
        vm_runtime_error(vm, "Named call requires a named-arguments table.");
        return 0;
    }

    Value named_value = vm_current_thread(vm)->stack_top[-1];
    if (!IS_TABLE(named_value)) {
        vm_runtime_error(vm, "Named call requires a table as final argument.");
        return 0;
    }

    ObjTable* named_args = AS_TABLE(named_value);
    int positional_count = arg_count - 1;
    int non_variadic_arity = function->is_variadic ? (function->arity - 1) : function->arity;
    if (non_variadic_arity < 0) non_variadic_arity = 0;

    Value* incoming = vm_current_thread(vm)->stack_top - arg_count;
    Value bound_args[256];
    uint8_t assigned[256];
    memset(assigned, 0, sizeof(assigned));

    ObjTable* varargs = NULL;
    int vararg_pos = 0;
    if (function->is_variadic) {
        varargs = new_table();
    }
    ObjTable* legacy_options = NULL;
    ObjString* first_unexpected = NULL;

    int positional_to_bind = positional_count < non_variadic_arity ? positional_count : non_variadic_arity;
    for (int i = 0; i < positional_to_bind; i++) {
        bound_args[i] = incoming[i];
        assigned[i] = 1;
    }

    if (positional_count > non_variadic_arity) {
        if (!function->is_variadic) {
            vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, positional_count);
            return 0;
        }
        for (int i = non_variadic_arity; i < positional_count; i++) {
            table_set_array(&varargs->table, ++vararg_pos, incoming[i]);
        }
    }

    for (int i = 0; i < named_args->table.capacity; i++) {
        Entry* entry = &named_args->table.entries[i];
        if (entry->key == NULL) continue;

        int index = find_named_param_index(function, entry->key, non_variadic_arity);
        if (index >= 0) {
            if (assigned[index]) {
                vm_runtime_error(vm, "Multiple values for argument '%s'.", entry->key->chars);
                return 0;
            }
            bound_args[index] = entry->value;
            assigned[index] = 1;
            continue;
        }

        if (function->is_variadic) {
            table_set(&varargs->table, entry->key, entry->value);
            continue;
        }

        if (legacy_options == NULL) {
            legacy_options = new_table();
            first_unexpected = entry->key;
        }
        table_set(&legacy_options->table, entry->key, entry->value);
    }

    if (legacy_options != NULL) {
        int target = -1;
        for (int i = 0; i < non_variadic_arity; i++) {
            if (assigned[i]) continue;
            if (function->param_names != NULL && i < function->param_names_count &&
                is_options_param_name(function->param_names[i])) {
                target = i;
                break;
            }
        }
        if (target >= 0) {
            bound_args[target] = OBJ_VAL(legacy_options);
            assigned[target] = 1;
        } else {
            vm_runtime_error(vm, "Unexpected named argument '%s'.", first_unexpected->chars);
            return 0;
        }
    }

    int default_start = non_variadic_arity - function->defaults_count;
    for (int i = 0; i < non_variadic_arity; i++) {
        if (assigned[i]) continue;
        if (i >= default_start) {
            bound_args[i] = function->defaults[i - default_start];
            assigned[i] = 1;
            continue;
        }
        if (function->param_names != NULL && i < function->param_names_count && function->param_names[i] != NULL) {
            vm_runtime_error(vm, "Missing required argument '%s'.", function->param_names[i]->chars);
        } else {
            vm_runtime_error(vm, "Missing required argument %d.", i + 1);
        }
        return 0;
    }

    if (function->is_variadic) {
        bound_args[non_variadic_arity] = OBJ_VAL(varargs);
    }

    vm_current_thread(vm)->stack_top -= arg_count;
    for (int i = 0; i < function->arity; i++) {
        push(vm, bound_args[i]);
    }

    return finish_closure_call(vm, closure, function->arity);
}

static void mark_roots(VM* vm) {
    if (vm_current_thread(vm) != NULL) {
        mark_object((struct Obj*)vm_current_thread(vm));
    }
    for (ObjThread* parked = vm->gc_parked_threads; parked != NULL; parked = parked->gc_park_next) {
        mark_object((struct Obj*)parked);
    }
    if (vm->mm_index != NULL) mark_object((struct Obj*)vm->mm_index);
    if (vm->mm_newindex != NULL) mark_object((struct Obj*)vm->mm_newindex);
    if (vm->mm_str != NULL) mark_object((struct Obj*)vm->mm_str);
    if (vm->mm_call != NULL) mark_object((struct Obj*)vm->mm_call);
    if (vm->mm_new != NULL) mark_object((struct Obj*)vm->mm_new);
    if (vm->mm_append != NULL) mark_object((struct Obj*)vm->mm_append);
    if (vm->mm_next != NULL) mark_object((struct Obj*)vm->mm_next);
    if (vm->mm_slice != NULL) mark_object((struct Obj*)vm->mm_slice);
    if (vm->str_module_name != NULL) mark_object((struct Obj*)vm->str_module_name);
    if (vm->str_upper_name != NULL) mark_object((struct Obj*)vm->str_upper_name);
    if (vm->str_lower_name != NULL) mark_object((struct Obj*)vm->str_lower_name);
    if (vm->slice_name != NULL) mark_object((struct Obj*)vm->slice_name);
    if (vm->module_name_key != NULL) mark_object((struct Obj*)vm->module_name_key);
    if (vm->module_file_key != NULL) mark_object((struct Obj*)vm->module_file_key);
    if (vm->module_main_key != NULL) mark_object((struct Obj*)vm->module_main_key);
    if (!IS_NIL(vm->str_upper_fn)) mark_value(vm->str_upper_fn);
    if (!IS_NIL(vm->str_lower_fn)) mark_value(vm->str_lower_fn);
    // Mark globals
    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL) {
            mark_object((struct Obj*)entry->key);
            mark_value(entry->value);
        }
    }
    for (int i = 0; i < vm->modules.capacity; i++) {
        Entry* entry = &vm->modules.entries[i];
        if (entry->key != NULL) {
            mark_object((struct Obj*)entry->key);
            mark_value(entry->value);
        }
    }
}

void init_vm(VM* vm) {
   vm->current_thread = new_thread(); // Main thread root/fallback
   vm_set_current_thread(vm, vm->current_thread);
   vm_current_thread(vm)->vm = vm;
   vm->gc_parked_threads = NULL;
   vm->use_thread_tls = 0;
   vm->disable_gc = 0;
   vm->is_repl = 0;
   vm->mm_index = NULL;
   vm->mm_newindex = NULL;
   vm->mm_str = NULL;
   vm->mm_call = NULL;
   vm->mm_new = NULL;
   vm->mm_append = NULL;
   vm->mm_next = NULL;
   vm->mm_slice = NULL;
   vm->str_module_name = NULL;
   vm->str_upper_name = NULL;
   vm->str_lower_name = NULL;
   vm->slice_name = NULL;
   vm->module_name_key = NULL;
   vm->module_file_key = NULL;
   vm->module_main_key = NULL;
   vm->str_upper_fn = NIL_VAL;
   vm->str_lower_fn = NIL_VAL;

    init_table(&vm->globals);
    init_table(&vm->modules);
    vm->cli_argc = 0;
    vm->cli_argv = NULL;
   vm_current_thread(vm)->open_upvalues = NULL;

    vm->mm_index = copy_string("__index", 7);
    vm->mm_newindex = copy_string("__newindex", 10);
    vm->mm_str = copy_string("__str", 5);
    vm->mm_call = copy_string("__call", 6);
    vm->mm_new = copy_string("__new", 5);
    vm->mm_append = copy_string("__append", 8);
    vm->mm_next = copy_string("__next", 6);
    vm->mm_slice = copy_string("__slice", 7);
    vm->str_module_name = copy_string("string", 6);
    vm->str_upper_name = copy_string("upper", 5);
    vm->str_lower_name = copy_string("lower", 5);
    vm->slice_name = copy_string("slice", 5);
    vm->module_name_key = copy_string("__name", 6);
    vm->module_file_key = copy_string("__file", 6);
    vm->module_main_key = copy_string("__main", 6);

    // Register built-in native functions (from libs module)
    register_libs(vm);
}

void free_vm(VM* vm) {
   free_table(&vm->globals);
   free_table(&vm->modules);
   // The current_thread will be freed as part of GC if it's reachable.
    // We manually free the main thread if it's not part of GC collection.
    // Since we explicitly control it, we can free it here.
    // free_object((struct Obj*)vm_current_thread(vm));
    vm_set_current_thread(vm, NULL);

    collect_garbage(vm); // Final garbage collection
#ifdef DEBUG_LOG_GC
    printf("-- GC DONE --\n");
#endif
}

void collect_garbage(VM* vm) {
    mark_roots(vm);
    sweep_objects();
}

void maybe_collect_garbage(VM* vm) {
    if (vm->disable_gc) return;  // Skip GC if disabled

    extern size_t bytes_allocated;
    extern size_t next_gc;
    if (bytes_allocated > next_gc) {
        collect_garbage(vm);
    }
}

void define_native(VM* vm, const char* name, NativeFn function) {
    ObjString* name_str = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(name_str));
    push(vm, OBJ_VAL(new_native(function, name_str)));
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // Native function.
    pop(vm); // Native name.
}

ObjString* number_key_string(double num) {
    if (num == 0) num = 0.0; // normalize -0 to 0
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%.17g", num);
    char buffer[2 + 32];
    buffer[0] = '\x1F';
    buffer[1] = 'n';
    size_t len = strlen(numbuf);
    memcpy(buffer + 2, numbuf, len);
    return copy_string(buffer, (int)(2 + len));
}

Value get_metamethod(VM* vm, Value val, const char* name) {
    ObjString* method_name = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(method_name)); // Protect from GC
    Value method = NIL_VAL;

    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            table_get(&table->metatable->table, method_name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            table_get(&udata->metatable->table, method_name, &method);
        }
    }
    pop(vm); // Pop method_name
    return method;
}

static ObjThread* run_stop_thread = NULL;

static void set_global_value(VM* vm, ObjString* key, Value value) {
    push(vm, OBJ_VAL(key));
    push(vm, value);
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
}

static void restore_module_context(VM* vm, CallFrame* frame) {
    if (!frame->restore_module_context) return;

    if (frame->had_prev_module_name) {
        set_global_value(vm, vm->module_name_key, frame->prev_module_name);
    } else {
        table_delete(&vm->globals, vm->module_name_key);
    }

    if (frame->had_prev_module_file) {
        set_global_value(vm, vm->module_file_key, frame->prev_module_file);
    } else {
        table_delete(&vm->globals, vm->module_file_key);
    }

    if (frame->had_prev_module_main) {
        set_global_value(vm, vm->module_main_key, frame->prev_module_main);
    } else {
        table_delete(&vm->globals, vm->module_main_key);
    }
}

InterpretResult vm_run(VM* vm, int min_frame_count) {
    CallFrame* frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    uint8_t* ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_SHORT() \
    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

    for (;;) {
        if (run_stop_thread != NULL && vm_current_thread(vm) == run_stop_thread) {
            return INTERPRET_OK;
        }
        if (interrupt_requested) {
            interrupt_requested = 0;
            vm_runtime_error(vm, "Interrupted.");
            goto runtime_error;
        }
#ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value* slot = vm_current_thread(vm)->stack; slot < vm_current_thread(vm)->stack_top; slot++) {
            printf("[ ");
            print_value(*slot);
            printf(" ]");
        }
        printf("\n");
        disassemble_instruction(&frame->closure->function->chunk, (int)(ip - frame->closure->function->chunk.code));
#endif

        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_TRY: {
                if (!vm_handle_op_try(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_END_TRY: {
                vm_handle_op_end_try(vm);
                break;
            }
            case OP_END_FINALLY: {
                if (!vm_handle_op_end_finally(vm)) goto runtime_error;
                break;
            }
            case OP_THROW: {
                vm_handle_op_throw(vm);
                goto runtime_error;
            }
            case OP_CONSTANT: {
                vm_handle_op_constant(vm, frame, &ip);
                break;
            }
            case OP_BUILD_STRING: {
                uint8_t part_count = READ_BYTE();
                if (!vm_build_string(vm, part_count)) goto runtime_error;
                break;
            }
            case OP_NIL: push(vm, NIL_VAL); break;
            case OP_TRUE: push(vm, BOOL_VAL(1)); break;
            case OP_FALSE: push(vm, BOOL_VAL(0)); break;
            case OP_POP: pop(vm); break;
            case OP_GET_GLOBAL: {
                if (!vm_handle_op_get_global(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DEFINE_GLOBAL: {
                vm_handle_op_define_global(vm, frame, &ip);
                break;
            }
            case OP_SET_GLOBAL: {
                vm_handle_op_set_global(vm, frame, &ip);
                break;
            }
            case OP_DELETE_GLOBAL: {
                if (!vm_handle_op_delete_global(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GET_LOCAL: {
                vm_handle_op_get_local(vm, frame, &ip);
                break;
            }
            case OP_SET_LOCAL: {
                vm_handle_op_set_local(vm, frame, &ip);
                break;
            }
            case OP_ADD_SET_LOCAL: {
                if (!vm_handle_op_add_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SUB_SET_LOCAL: {
                if (!vm_handle_op_sub_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MUL_SET_LOCAL: {
                if (!vm_handle_op_mul_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DIV_SET_LOCAL: {
                if (!vm_handle_op_div_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MOD_SET_LOCAL: {
                if (!vm_handle_op_mod_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_INC_LOCAL: {
                if (!vm_handle_op_inc_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SUB_LOCAL_CONST: {
                if (!vm_handle_op_sub_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MUL_LOCAL_CONST: {
                if (!vm_handle_op_mul_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DIV_LOCAL_CONST: {
                if (!vm_handle_op_div_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MOD_LOCAL_CONST: {
                if (!vm_handle_op_mod_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GET_UPVALUE: {
                vm_handle_op_get_upvalue(vm, frame, &ip);
                break;
            }
            case OP_SET_UPVALUE: {
                vm_handle_op_set_upvalue(vm, frame, &ip);
                break;
            }
            case OP_CLOSE_UPVALUE: {
                close_upvalues(vm, vm_current_thread(vm)->stack_top - 1);
                pop(vm);
                break;
            }
            case OP_NEW_TABLE: {
                if (!vm_handle_op_new_table(vm)) goto runtime_error;
                break;
            }
            case OP_SET_METATABLE: {
                if (!vm_handle_op_set_metatable(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DUP: push(vm, peek(vm, 0)); break;
            case OP_GET_TABLE: {
                if (!vm_handle_op_get_table(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GET_META_TABLE: {
                if (!vm_handle_op_get_meta_table(vm)) goto runtime_error;
                break;
            }
            case OP_SET_TABLE: {
                if (!vm_handle_op_set_table(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DELETE_TABLE: {
                if (!vm_handle_op_delete_table(vm)) goto runtime_error;
                break;
            }
            case OP_PRINT: {
                uint8_t arg_count = READ_BYTE();
                InterpretResult print_result = INTERPRET_OK;
                int print_status = vm_handle_op_print(vm, &frame, &ip, arg_count, &print_result);
                if (print_status < 0) goto runtime_error;
                if (print_status == 0) return print_result;
                break;
            }
            case OP_JUMP: {
                vm_handle_op_jump(&ip);
                break;
            }
            case OP_JUMP_IF_FALSE: {
                vm_handle_op_jump_if_false(vm, &ip);
                break;
            }
            case OP_JUMP_IF_TRUE: {
                vm_handle_op_jump_if_true(vm, &ip);
                break;
            }
            case OP_LOOP: {
                vm_handle_op_loop(&ip);
                break;
            }
            case OP_CALL: {
                int arg_count = READ_BYTE();
                if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL0: {
                if (!invoke_call_with_arg_count(vm, 0, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL1: {
                if (!invoke_call_with_arg_count(vm, 1, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL2: {
                if (!invoke_call_with_arg_count(vm, 2, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL_NAMED: {
                int arg_count = READ_BYTE();
                if (!invoke_call_with_named_arg_count(vm, arg_count, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL_EXPAND: {
                int fixed_arg_count = READ_BYTE();
                Value spread = peek(vm, 0);
                if (!IS_TABLE(spread)) {
                    vm_runtime_error(vm, "Spread argument must be a table.");
                    goto runtime_error;
                }

                ObjTable* spread_table = AS_TABLE(spread);
                int spread_count = 0;
                int has_named = spread_table->table.count > 0;
                for (int i = 1; ; i++) {
                    Value val = NIL_VAL;
                    if (!table_get_array(&spread_table->table, i, &val) || IS_NIL(val)) {
                        break;
                    }
                    spread_count++;
                    if (fixed_arg_count + spread_count > 255) {
                        vm_runtime_error(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                }

                pop(vm); // Remove spread table
                for (int i = 1; i <= spread_count; i++) {
                    Value val = NIL_VAL;
                    table_get_array(&spread_table->table, i, &val);
                    push(vm, val);
                }

                int arg_count = fixed_arg_count + spread_count;
                if (has_named) {
                    ObjTable* named = new_table();
                    table_add_all(&spread_table->table, &named->table);
                    push(vm, OBJ_VAL(named));
                    if (arg_count + 1 > 255) {
                        vm_runtime_error(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                    if (!invoke_call_with_named_arg_count(vm, arg_count + 1, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else {
                    if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                        goto runtime_error;
                    }
                }
                break;
            }
            case OP_ITER_PREP: {
                if (!vm_handle_op_iter_prep(vm)) goto runtime_error;
                break;
            }
            case OP_ITER_PREP_IPAIRS: {
                if (!vm_handle_op_iter_prep_i_pairs(vm)) goto runtime_error;
                break;
            }
            case OP_RANGE: {
                if (!vm_handle_op_range(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_FOR_PREP: {
                if (!vm_handle_op_for_prep(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_FOR_LOOP: {
                if (!vm_handle_op_for_loop(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SLICE: {
                if (!vm_handle_op_slice(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = new_closure(function);
                push(vm, OBJ_VAL(closure));

                // Read upvalue information
                for (int i = 0; i < function->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        closure->upvalues[i] = capture_upvalue(vm, frame->slots + index);
                    }
                    else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                closure->upvalue_count = function->upvalue_count;
                break;
            }
            case OP_RETURN: {
                Value result = pop(vm);
                if (frame->cache_module_result && IS_STRING(frame->module_cache_name)) {
                    table_set(&vm->modules, AS_STRING(frame->module_cache_name), result);
                }
                close_upvalues(vm, frame->slots);
                discard_handlers_for_frame_return(vm_current_thread(vm));
                restore_module_context(vm, frame);
                vm_current_thread(vm)->frame_count--;

                // Restore stack and push result
                vm_current_thread(vm)->stack_top = frame->slots;
                push(vm, result);
                apply_pending_set_local(vm);

                if (vm_current_thread(vm)->frame_count <= min_frame_count) {
                    if (vm_current_thread(vm)->caller != NULL) {
                        ObjThread* caller = vm_current_thread(vm)->caller;
                        vm_current_thread(vm)->caller = NULL;

                        // Check stack overflow
                        if (caller->stack_top + 2 >= caller->stack + caller->stack_capacity) {
                            vm_runtime_error(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        if (vm_current_thread(vm)->is_generator && vm_current_thread(vm)->generator_mode) {
                            vm_current_thread(vm)->generator_mode = 0;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                        } else {
                            *caller->stack_top = BOOL_VAL(1);
                            caller->stack_top++;
                            *caller->stack_top = result;
                            caller->stack_top++;
                        }

                        vm_set_current_thread(vm, caller);
                        frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                        ip = frame->ip;
                        break;
                    }

                    // In REPL mode, leave the result on stack so it can be printed
                    // In normal mode, pop the script closure
                    if (min_frame_count == 0 && !vm->is_repl) {
                        pop(vm);  // Pop the script closure when completely done
                    }
                    return INTERPRET_OK;
                }

                frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                ip = frame->ip;
                break;
            }
            case OP_RETURN_N: {
                uint8_t count = READ_BYTE();
                Value* results = vm_current_thread(vm)->stack_top - count;
                if (frame->cache_module_result && IS_STRING(frame->module_cache_name)) {
                    Value module_result = count > 0 ? results[0] : NIL_VAL;
                    table_set(&vm->modules, AS_STRING(frame->module_cache_name), module_result);
                }
                close_upvalues(vm, frame->slots);
                discard_handlers_for_frame_return(vm_current_thread(vm));
                restore_module_context(vm, frame);
                vm_current_thread(vm)->frame_count--;

                // Copy results to where the function was called (frame->slots)
                // This replaces the function + args with the return values
                Value* dest = frame->slots;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_RETURN_N: count=%d, dest offset=%ld\n", count, dest - vm_current_thread(vm)->stack);
                for (int i = 0; i < count; i++) {
                    printf("  result[%d] = ", i);
                    print_value(results[i]);
                    printf("\n");
                }
                #endif
                for (int i = 0; i < count; i++) {
                    dest[i] = results[i];
                }
                vm_current_thread(vm)->stack_top = dest + count;
                apply_pending_set_local(vm);

                if (vm_current_thread(vm)->frame_count <= min_frame_count) {
                    if (vm_current_thread(vm)->caller != NULL) {
                        ObjThread* caller = vm_current_thread(vm)->caller;
                        vm_current_thread(vm)->caller = NULL;

                        if (caller->stack_top + 1 + count >= caller->stack + caller->stack_capacity) {
                            vm_runtime_error(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        if (vm_current_thread(vm)->is_generator && vm_current_thread(vm)->generator_mode) {
                            vm_current_thread(vm)->generator_mode = 0;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                        } else {
                            *caller->stack_top = BOOL_VAL(1);
                            caller->stack_top++;

                            Value* results = vm_current_thread(vm)->stack_top - count;
                            for (int i = 0; i < count; i++) {
                                *caller->stack_top = results[i];
                                caller->stack_top++;
                            }
                        }

                        vm_set_current_thread(vm, caller);
                        frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                        ip = frame->ip;
                        break;
                    }

                    if (min_frame_count == 0) {
                        vm_current_thread(vm)->stack_top -= count;
                    }
                    return INTERPRET_OK;
                }

                frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                ip = frame->ip;
                break;
            }
            case OP_ADJUST_STACK: {
                uint8_t target_depth = READ_BYTE();
                vm_current_thread(vm)->stack_top = frame->slots + target_depth;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_ADJUST_STACK: target depth=%d, new stack_top offset=%ld\n",
                       target_depth, vm_current_thread(vm)->stack_top - vm_current_thread(vm)->stack);
                #endif
                break;
            }
            case OP_UNPACK: {
                if (!vm_handle_op_unpack(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_ADD_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_add_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_ADD:
                if (!vm_handle_op_add(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_ADD_INPLACE:
                if (!vm_handle_op_add_inplace(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_SUBTRACT:
                if (!vm_handle_op_subtract(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_MULTIPLY:
                if (!vm_handle_op_multiply(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_DIVIDE:
                if (!vm_handle_op_divide(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_MODULO:
                if (!vm_handle_op_modulo(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_APPEND:
                if (!vm_handle_op_append(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_IADD:
                vm_handle_op_i_add(vm);
                break;
            case OP_SUB_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_sub_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_ISUB:
                vm_handle_op_i_sub(vm);
                break;
            case OP_MUL_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_mul_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_IMUL:
                vm_handle_op_i_mul(vm);
                break;
            case OP_DIV_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_div_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_IDIV:
                vm_handle_op_i_div(vm);
                break;
            case OP_NEGATE:
                vm_handle_op_negate(vm);
                break;
            case OP_NOT:
                vm_handle_op_not(vm);
                break;
            case OP_LENGTH:
                if (!vm_handle_op_length(vm)) goto runtime_error;
                break;
            case OP_EQUAL: {
                if (!vm_handle_op_equal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GREATER: {
                if (!vm_handle_op_greater(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_LESS: {
                if (!vm_handle_op_less(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_HAS: {
                if (!vm_handle_op_has(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_IN: {
                if (!vm_handle_op_in(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_POWER: {
                if (!vm_handle_op_power(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_INT_DIV: {
                if (!vm_handle_op_int_div(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_IMOD: {
                vm_handle_op_i_mod(vm);
                break;
            }
            case OP_FADD: {
                vm_handle_op_f_add(vm);
                break;
            }
            case OP_FSUB: {
                vm_handle_op_f_sub(vm);
                break;
            }
            case OP_FMUL: {
                vm_handle_op_f_mul(vm);
                break;
            }
            case OP_FDIV: {
                vm_handle_op_f_div(vm);
                break;
            }
            case OP_FMOD: {
                vm_handle_op_f_mod(vm);
                break;
            }
            case OP_MOD_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_mod_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_GC: {
                collect_garbage(vm);
                break;
            }
            case OP_IMPORT: {
               ObjString* module_name = READ_STRING();
               InterpretResult import_result = vm_handle_op_import(vm, module_name, &frame, &ip);
               if (import_result == INTERPRET_RUNTIME_ERROR) goto runtime_error;
               if (import_result == INTERPRET_COMPILE_ERROR) return INTERPRET_COMPILE_ERROR;
                break;
            }
            case OP_IMPORT_STAR: {
                if (!vm_handle_op_import_star(vm)) goto runtime_error;
                break;
            }
        }
        continue;
runtime_error:
        if (handle_exception(vm, &frame, &ip)) {
            continue;
        }
        return INTERPRET_RUNTIME_ERROR;
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
}

InterpretResult vm_run_until_thread(VM* vm, int min_frame_count, ObjThread* stop_thread) {
    ObjThread* saved_stop_thread = run_stop_thread;
    run_stop_thread = stop_thread;
    InterpretResult result = vm_run(vm, min_frame_count);
    run_stop_thread = saved_stop_thread;
    return result;
}

InterpretResult interpret(VM* vm, ObjFunction* function) {
    ObjClosure* closure = new_closure(function);
    push(vm, OBJ_VAL(closure));
    call(vm, closure, 0);
    return vm_run(vm, 0);  // Run until all frames complete
}
