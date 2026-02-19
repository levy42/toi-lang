#include "ops_arith_const.h"

int vmHandleOpSubConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b)));
    } else {
        Value method = getMetamethod(vm, a, "__sub");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__sub");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

int vmHandleOpMulConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b)));
    } else {
        Value method = getMetamethod(vm, a, "__mul");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__mul");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

int vmHandleOpDivConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
    } else {
        Value method = getMetamethod(vm, a, "__div");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__div");
        if (IS_NIL(method)) return 0;
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

void vmHandleOpIAdd(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a + b));
}

void vmHandleOpISub(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a - b));
}

void vmHandleOpIMul(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a * b));
}

void vmHandleOpIDiv(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a / b));
}
