#include "ops_state.h"

void vmHandleOpConstant(VM* vm, CallFrame* frame, uint8_t** ip) {
    Value constant = frame->closure->function->chunk.constants.values[*(*ip)++];
    push(vm, constant);
    maybeCollectGarbage(vm);
}

int vmHandleOpGetGlobal(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    Value value;
    if (!tableGet(&vm->globals, name, &value)) {
        vmRuntimeError(vm, "Undefined variable '%s'.", name->chars);
        return 0;
    }
    push(vm, value);
    return 1;
}

void vmHandleOpDefineGlobal(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    tableSet(&vm->globals, name, peek(vm, 0));
    pop(vm);
    maybeCollectGarbage(vm);
}

void vmHandleOpSetGlobal(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    tableSet(&vm->globals, name, peek(vm, 0));
    maybeCollectGarbage(vm);
}

int vmHandleOpDeleteGlobal(VM* vm, CallFrame* frame, uint8_t** ip) {
    ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[*(*ip)++]);
    if (!tableDelete(&vm->globals, name)) {
        vmRuntimeError(vm, "Undefined variable '%s'.", name->chars);
        return 0;
    }
    return 1;
}

void vmHandleOpGetLocal(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    push(vm, frame->slots[slot]);
}

void vmHandleOpSetLocal(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    frame->slots[slot] = peek(vm, 0);
}

void vmHandleOpGetUpvalue(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    push(vm, *frame->closure->upvalues[slot]->location);
}

void vmHandleOpSetUpvalue(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    *frame->closure->upvalues[slot]->location = peek(vm, 0);
}
