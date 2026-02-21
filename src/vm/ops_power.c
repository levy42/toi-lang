#include <math.h>

#include "ops_power.h"

int vm_handle_op_power(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(pow(AS_NUMBER(a), AS_NUMBER(b))));
    } else {
        Value method = get_metamethod(vm, a, "__pow");
        if (IS_NIL(method)) method = get_metamethod(vm, b, "__pow");
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

int vm_handle_op_int_div(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, NUMBER_VAL(floor(AS_NUMBER(a) / AS_NUMBER(b))));
    } else {
        Value method = get_metamethod(vm, a, "__int_div");
        if (IS_NIL(method)) method = get_metamethod(vm, b, "__int_div");
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
