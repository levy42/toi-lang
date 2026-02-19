#include "ops_exception.h"

static uint16_t readShortLocal(uint8_t** ip) {
    *ip += 2;
    return (uint16_t)(((*ip)[-2] << 8) | (*ip)[-1]);
}

int vmHandleOpTry(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t depth = *(*ip)++;
    uint8_t flags = *(*ip)++;
    uint16_t exJump = readShortLocal(ip);
    uint16_t finJump = readShortLocal(ip);
    ObjThread* thread = vm->currentThread;
    if (thread->handlerCount >= 64) {
        vmRuntimeError(vm, "Too many nested try blocks.");
        return 0;
    }

    ExceptionHandler* handler = &thread->handlers[thread->handlerCount++];
    handler->frameCount = thread->frameCount;
    handler->stackTop = frame->slots + depth;
    handler->hasExcept = (flags & 0x1) != 0;
    handler->hasFinally = (flags & 0x2) != 0;
    handler->inExcept = 0;
    handler->except_ip = handler->hasExcept ? *ip + exJump : NULL;
    handler->finally_ip = handler->hasFinally ? *ip + finJump : NULL;
    return 1;
}

void vmHandleOpEndTry(VM* vm) {
    ObjThread* thread = vm->currentThread;
    if (thread->handlerCount > 0) thread->handlerCount--;
}

int vmHandleOpEndFinally(VM* vm) {
    return !vm->hasException;
}

void vmHandleOpThrow(VM* vm) {
    Value ex = pop(vm);
    vm->hasException = 1;
    vm->exception = ex;
}
