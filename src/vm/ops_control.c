#include "ops_control.h"

static uint16_t readShortLocal(uint8_t** ip) {
    *ip += 2;
    return (uint16_t)(((*ip)[-2] << 8) | (*ip)[-1]);
}

static int isFalseyLocal(Value v) {
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.arrayCapacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

void vmHandleOpJump(uint8_t** ip) {
    uint16_t offset = readShortLocal(ip);
    *ip += offset;
}

void vmHandleOpJumpIfFalse(VM* vm, uint8_t** ip) {
    uint16_t offset = readShortLocal(ip);
    Value value = peek(vm, 0);
    if (isFalseyLocal(value)) {
        *ip += offset;
    }
}

void vmHandleOpJumpIfTrue(VM* vm, uint8_t** ip) {
    uint16_t offset = readShortLocal(ip);
    Value value = peek(vm, 0);
    if (!isFalseyLocal(value)) {
        *ip += offset;
    }
}

void vmHandleOpLoop(uint8_t** ip) {
    uint16_t offset = readShortLocal(ip);
    *ip -= offset;
}

int vmHandleOpForPrep(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t varSlot = *(*ip)++;
    uint8_t endSlot = *(*ip)++;
    uint16_t offset = readShortLocal(ip);
    Value v = frame->slots[varSlot];
    Value end = frame->slots[endSlot];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
        vmRuntimeError(vm, "for range requires numeric bounds.");
        return 0;
    }
    if (AS_NUMBER(v) > AS_NUMBER(end)) {
        *ip += offset;
    }
    return 1;
}

int vmHandleOpForLoop(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t varSlot = *(*ip)++;
    uint8_t endSlot = *(*ip)++;
    uint16_t offset = readShortLocal(ip);
    Value v = frame->slots[varSlot];
    Value end = frame->slots[endSlot];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
        vmRuntimeError(vm, "for range requires numeric bounds.");
        return 0;
    }
    double next = AS_NUMBER(v) + 1.0;
    frame->slots[varSlot] = NUMBER_VAL(next);
    if (next <= AS_NUMBER(end)) {
        *ip -= offset;
    }
    return 1;
}
