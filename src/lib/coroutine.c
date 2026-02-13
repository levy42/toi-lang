#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int coroutineCreate(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    if (!IS_CLOSURE(args[0])) { RETURN_NIL; }

    ObjClosure* closure = AS_CLOSURE(args[0]);
    ObjThread* thread = newThread();
    
    thread->stack[0] = args[0]; // Push closure itself onto thread's stack
    thread->stackTop = thread->stack + 1;
    
    CallFrame* frame = &thread->frames[0];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = thread->stack;
    thread->frameCount = 1;
    
    RETURN_OBJ(thread);
}

static int coroutineResume(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_THREAD(0);
    
    ObjThread* thread = GET_THREAD(0); // GET_USERDATA can be used for ObjThread* too.
    
    if (thread->frameCount == 0) {
         push(vm, BOOL_VAL(0));
         RETURN_STRING("cannot resume dead coroutine", 28);
    }
    
    thread->caller = vm->currentThread;
    
    int passedArgs = argCount - 1;
    for (int i = 0; i < passedArgs; i++) {
        *thread->stackTop = args[1 + i];
        thread->stackTop++;
    }
    
    vm->currentThread = thread;
    return 1; 
}

static int coroutineYield(VM* vm, int argCount, Value* args) {
    ObjThread* caller = vm->currentThread->caller;
    if (caller == NULL) {
        push(vm, BOOL_VAL(0));
        RETURN_STRING("attempt to yield from outside a coroutine", 41);
    }
    
    *caller->stackTop = BOOL_VAL(1); // Push status (true)
    caller->stackTop++;
    
    for (int i = 0; i < argCount; i++) {
        *caller->stackTop = args[i]; // Push all yield arguments
        caller->stackTop++;
    }
    
    vm->currentThread = caller;
    vm->currentThread->caller = NULL; // Unlink (the caller will relink if it resumes this thread)
    
    return 1;
}

static int coroutineStatus(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_THREAD(0);

    ObjThread* thread = GET_THREAD(0);
    if (thread->frameCount == 0) {
        RETURN_STRING("dead", 4);
    } else if (thread->caller != NULL) {
        RETURN_STRING("normal", 6); // normal means it has a caller, is running or yielded to its caller.
    } else if (thread == vm->currentThread) {
        RETURN_STRING("running", 7); // The currently executing thread is running.
    } else {
        RETURN_STRING("suspended", 9); // Suspended (created but not run, or yielded).
    }
}

void registerCoroutine(VM* vm) {
    const NativeReg coFuncs[] = {
        {"create", coroutineCreate},
        {"resume", coroutineResume},
        {"yield", coroutineYield},
        {"status", coroutineStatus},
        {NULL, NULL}
    };
    registerModule(vm, "coroutine", coFuncs);
    pop(vm); // Pop coroutine module from stack
}
