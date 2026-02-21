#include "ops_arith_const.h"

int vm_handle_op_sub_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b)));
    } else {
        Value method = get_metamethod(vm, a, "__sub");
        if (IS_NIL(method)) method = get_metamethod(vm, b, "__sub");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

int vm_handle_op_mul_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b)));
    } else {
        Value method = get_metamethod(vm, a, "__mul");
        if (IS_NIL(method)) method = get_metamethod(vm, b, "__mul");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

int vm_handle_op_div_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
    } else {
        Value method = get_metamethod(vm, a, "__div");
        if (IS_NIL(method)) method = get_metamethod(vm, b, "__div");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

void vm_handle_op_i_add(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a + b));
}

void vm_handle_op_i_sub(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a - b));
}

void vm_handle_op_i_mul(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a * b));
}

void vm_handle_op_i_div(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a / b));
}
