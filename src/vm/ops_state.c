#include "ops_state.h"

void vm_handle_op_constant(VM* vm, CallFrame* frame, uint8_t** ip) {
    Value constant = frame->closure->function->chunk.constants.values[*(*ip)++];
    push(vm, constant);
    maybe_collect_garbage(vm);
}

int vm_handle_op_get_global(VM* vm, CallFrame* frame, uint8_t** ip) {
    Chunk* chunk = &frame->closure->function->chunk;
    int operand_offset = (int)(*ip - chunk->code);
    uint8_t constant_index = *(*ip)++;
    int opcode_offset = operand_offset - 1;
    ObjString* name = AS_STRING(chunk->constants.values[constant_index]);

    if (opcode_offset >= 0 && opcode_offset < chunk->capacity &&
        chunk->global_ic_names != NULL &&
        chunk->global_ic_names[opcode_offset] == name &&
        chunk->global_ic_versions[opcode_offset] == vm->globals.version) {
        push(vm, chunk->global_ic_values[opcode_offset]);
        return 1;
    }

    Value value;
    if (!table_get(&vm->globals, name, &value)) {
        vm_runtime_error(vm, "Undefined variable '%s'.", name->chars);
        return 0;
    }

    if (opcode_offset >= 0 && opcode_offset < chunk->capacity &&
        chunk->global_ic_names != NULL) {
        chunk->global_ic_names[opcode_offset] = name;
        chunk->global_ic_versions[opcode_offset] = vm->globals.version;
        chunk->global_ic_values[opcode_offset] = value;
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
