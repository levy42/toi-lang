#include <math.h>

#include "ops_power.h"

int vmHandleOpPower(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(pow(AS_NUMBER(a), AS_NUMBER(b))));
    } else {
        Value method = getMetamethod(vm, a, "__pow");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__pow");
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

int vmHandleOpIntDiv(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(floor(AS_NUMBER(a) / AS_NUMBER(b))));
    } else {
        Value method = getMetamethod(vm, a, "__int_div");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__int_div");
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
