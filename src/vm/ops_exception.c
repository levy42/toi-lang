#include "ops_exception.h"

static uint16_t read_short_local(uint8_t** ip) {
    *ip += 2;
    return (uint16_t)(((*ip)[-2] << 8) | (*ip)[-1]);
}

int vm_handle_op_try(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t depth = *(*ip)++;
    uint8_t flags = *(*ip)++;
    uint16_t ex_jump = read_short_local(ip);
    uint16_t fin_jump = read_short_local(ip);
    ObjThread* thread = vm->current_thread;
    if (thread->handler_count >= thread->handler_capacity) {
        vm_runtime_error(vm, "Too many nested try blocks.");
        return 0;
    }

    ExceptionHandler* handler = &thread->handlers[thread->handler_count++];
    handler->frame_count = thread->frame_count;
    handler->stack_top = frame->slots + depth;
    handler->has_except = (flags & 0x1) != 0;
    handler->has_finally = (flags & 0x2) != 0;
    handler->in_except = 0;
    handler->except_ip = handler->has_except ? *ip + ex_jump : NULL;
    handler->finally_ip = handler->has_finally ? *ip + fin_jump : NULL;
    return 1;
}

void vm_handle_op_end_try(VM* vm) {
    ObjThread* thread = vm->current_thread;
    if (thread->handler_count > 0) thread->handler_count--;
}

int vm_handle_op_end_finally(VM* vm) {
    return !vm->has_exception;
}

void vm_handle_op_throw(VM* vm) {
    Value ex = pop(vm);
    vm->has_exception = 1;
    vm->exception = ex;
}
