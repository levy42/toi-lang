#include <math.h>
#include <stdint.h>

#include "ops_mod.h"

static inline int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

void vm_handle_op_i_mod(VM* vm) {
    double bd = AS_NUMBER(pop(vm));
    double ad = AS_NUMBER(pop(vm));
    int64_t ia, ib;
    if (to_int64_local(ad, &ia) && to_int64_local(bd, &ib) && ib != 0) {
        push(vm, NUMBER_VAL((double)(ia % ib)));
    } else {
        push(vm, NUMBER_VAL(fmod(ad, bd)));
    }
}

int vm_handle_op_mod_const(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        double ad = AS_NUMBER(a);
        double bd = AS_NUMBER(b);
        int64_t ia, ib;
        if (to_int64_local(ad, &ia) && to_int64_local(bd, &ib) && ib != 0) {
            push(vm, NUMBER_VAL((double)(ia % ib)));
        } else {
            push(vm, NUMBER_VAL(fmod(ad, bd)));
        }
        return 1;
    }

    Value method = get_metamethod(vm, a, "__mod");
    if (IS_NIL(method)) method = get_metamethod(vm, b, "__mod");
    if (IS_NIL(method)) return 0;
    push(vm, method);
    push(vm, a);
    push(vm, b);
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    *ip = (*frame)->ip;
    return 1;
}
