#include "ops_state.h"

void vm_handle_op_constant(VM* vm, CallFrame* frame, uint8_t** ip) {
    Value constant = frame->closure->function->chunk.constants.values[*(*ip)++];
    push(vm, constant);
    maybe_collect_garbage(vm);
}

int vm_handle_op_get_global(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    Value value;
    if (!table_get(&vm->globals, name, &value)) {
        vm_runtime_error(vm, "Undefined variable '%s'.", name->chars);
        return 0;
    }
    push(vm, value);
    return 1;
}

void vm_handle_op_define_global(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    table_set(&vm->globals, name, peek(vm, 0));
    pop(vm);
    maybe_collect_garbage(vm);
}

void vm_handle_op_set_global(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    table_set(&vm->globals, name, peek(vm, 0));
    maybe_collect_garbage(vm);
}

int vm_handle_op_delete_global(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    if (!table_delete(&vm->globals, name)) {
        vm_runtime_error(vm, "Undefined variable '%s'.", name->chars);
        return 0;
    }
    return 1;
}

void vm_handle_op_get_local(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    push(vm, frame->slots[slot]);
}

void vm_handle_op_set_local(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    frame->slots[slot] = peek(vm, 0);
}

void vm_handle_op_get_upvalue(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    push(vm, *frame->closure->upvalues[slot]->location);
}

void vm_handle_op_set_upvalue(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    *frame->closure->upvalues[slot]->location = peek(vm, 0);
}
