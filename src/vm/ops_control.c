#include "ops_control.h"

static uint16_t read_short_local(uint8_t** ip) {
    *ip += 2;
    return (uint16_t)(((*ip)[-2] << 8) | (*ip)[-1]);
}

static int is_falsey_local(Value v) {
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.array_capacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

void vm_handle_op_jump(uint8_t** ip) {
    uint16_t offset = read_short_local(ip);
    *ip += offset;
}

void vm_handle_op_jump_if_false(VM* vm, uint8_t** ip) {
    uint16_t offset = read_short_local(ip);
    Value value = peek(vm, 0);
    if (is_falsey_local(value)) {
        *ip += offset;
    }
}

void vm_handle_op_jump_if_true(VM* vm, uint8_t** ip) {
    uint16_t offset = read_short_local(ip);
    Value value = peek(vm, 0);
    if (!is_falsey_local(value)) {
        *ip += offset;
    }
}

void vm_handle_op_loop(uint8_t** ip) {
    uint16_t offset = read_short_local(ip);
    *ip -= offset;
}

int vm_handle_op_for_prep(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t var_slot = *(*ip)++;
    uint8_t end_slot = *(*ip)++;
    uint16_t offset = read_short_local(ip);
    Value v = frame->slots[var_slot];
    Value end = frame->slots[end_slot];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
        vm_runtime_error(vm, "for range requires numeric bounds.");
        return 0;
    }
    if (AS_NUMBER(v) > AS_NUMBER(end)) {
        *ip += offset;
    }
    return 1;
}

int vm_handle_op_for_loop(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t var_slot = *(*ip)++;
    uint8_t end_slot = *(*ip)++;
    uint16_t offset = read_short_local(ip);
    Value v = frame->slots[var_slot];
    Value end = frame->slots[end_slot];
    if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
        vm_runtime_error(vm, "for range requires numeric bounds.");
        return 0;
    }
    double next = AS_NUMBER(v) + 1.0;
    frame->slots[var_slot] = NUMBER_VAL(next);
    if (next <= AS_NUMBER(end)) {
        *ip -= offset;
    }
    return 1;
}
