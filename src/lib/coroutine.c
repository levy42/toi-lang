#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int coroutine_create(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    if (!IS_CLOSURE(args[0])) { RETURN_NIL; }

    ObjClosure* closure = AS_CLOSURE(args[0]);
    ObjThread* thread = new_thread();
    
    thread->stack[0] = args[0]; // Push closure itself onto thread's stack
    thread->stack_top = thread->stack + 1;
    
    CallFrame* frame = &thread->frames[0];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = thread->stack;
    thread->frame_count = 1;
    
    RETURN_OBJ(thread);
}

static int coroutine_resume(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_THREAD(0);
    
    ObjThread* thread = GET_THREAD(0); // GET_USERDATA can be used for ObjThread* too.
    
    if (thread->frame_count == 0) {
         push(vm, BOOL_VAL(0));
         RETURN_STRING("cannot resume dead coroutine", 28);
    }
    
    thread->caller = vm->current_thread;
    if (thread->is_generator) {
        thread->generator_mode = 0;
    }
    
    int passed_args = arg_count - 1;
    for (int i = 0; i < passed_args; i++) {
        *thread->stack_top = args[1 + i];
        thread->stack_top++;
    }
    
    vm->current_thread = thread;
    return 1; 
}

static int coroutine_yield(VM* vm, int arg_count, Value* args) {
    ObjThread* caller = vm->current_thread->caller;
    if (caller == NULL) {
        push(vm, BOOL_VAL(0));
        RETURN_STRING("attempt to yield from outside a coroutine", 41);
    }

    if (vm->current_thread->is_generator && vm->current_thread->generator_mode) {
        vm->current_thread->generator_mode = 0;
        vm->current_thread->generator_index++;
        *caller->stack_top = NUMBER_VAL((double)vm->current_thread->generator_index);
        caller->stack_top++;
        *caller->stack_top = (arg_count > 0) ? args[0] : NIL_VAL;
        caller->stack_top++;
    } else {
        *caller->stack_top = BOOL_VAL(1); // Push status (true)
        caller->stack_top++;

        for (int i = 0; i < arg_count; i++) {
            *caller->stack_top = args[i]; // Push all yield arguments
            caller->stack_top++;
        }
    }

    vm->current_thread = caller;
    vm->current_thread->caller = NULL; // Unlink (the caller will relink if it resumes this thread)

    return 1;
}

static int coroutine_status(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_THREAD(0);

    ObjThread* thread = GET_THREAD(0);
    if (thread->frame_count == 0) {
        RETURN_STRING("dead", 4);
    } else if (thread->caller != NULL) {
        RETURN_STRING("normal", 6); // normal means it has a caller, is running or yielded to its caller.
    } else if (thread == vm->current_thread) {
        RETURN_STRING("running", 7); // The currently executing thread is running.
    } else {
        RETURN_STRING("suspended", 9); // Suspended (created but not run, or yielded).
    }
}

void register_coroutine(VM* vm) {
    const NativeReg co_funcs[] = {
        {"create", coroutine_create},
        {"resume", coroutine_resume},
        {"yield", coroutine_yield},
        {"status", coroutine_status},
        {NULL, NULL}
    };
    register_module(vm, "coroutine", co_funcs);
    pop(vm); // Pop coroutine module from stack
}
