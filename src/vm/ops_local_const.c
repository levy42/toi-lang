#include <math.h>
#include <stdint.h>

#include "ops_local_const.h"

static inline int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

static int push_pending_set_local_local(VM* vm, int frame_index, int slot) {
    if (vm->pending_set_local_count >= 8) {
        vm_runtime_error(vm, "Pending set-local stack overflow.");
        return 0;
    }
    int idx = vm->pending_set_local_count++;
    vm->pending_set_local_frames[idx] = frame_index;
    vm->pending_set_local_slots[idx] = slot;
    return 1;
}

int vm_handle_op_inc_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    Value v = (*frame)->slots[slot];
    Value c = (*frame)->closure->function->chunk.constants.values[constant];
    if (!IS_NUMBER(v) || !IS_NUMBER(c)) {
        vm_runtime_error(vm, "Operands must be two numbers.");
        return 0;
    }
    Value out = NUMBER_VAL(AS_NUMBER(v) + AS_NUMBER(c));
    (*frame)->slots[slot] = out;
    push(vm, out);
    return 1;
}

static int binary_local_const(
    VM* vm, CallFrame** frame, uint8_t** ip, const char* mm_name, uint8_t slot, uint8_t constant, int op) {
    Value a = (*frame)->slots[slot];
    Value b = (*frame)->closure->function->chunk.constants.values[constant];
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        Value out;
        switch (op) {
            case 0: out = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b)); break;
            case 1: out = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b)); break;
            case 2: out = NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b)); break;
            case 3: {
                double ad = AS_NUMBER(a);
                double bd = AS_NUMBER(b);
                int64_t ia, ib;
                if (to_int64_local(ad, &ia) && to_int64_local(bd, &ib) && ib != 0) {
                    out = NUMBER_VAL((double)(ia % ib));
                } else {
                    out = NUMBER_VAL(fmod(ad, bd));
                }
                break;
            }
            default:
                return 0;
        }
        (*frame)->slots[slot] = out;
        push(vm, out);
        return 1;
    }

    Value method = get_metamethod(vm, a, mm_name);
    if (IS_NIL(method)) method = get_metamethod(vm, b, mm_name);
    if (IS_NIL(method)) return 0;
    push(vm, method);
    push(vm, a);
    push(vm, b);
    if (!push_pending_set_local_local(vm, vm->current_thread->frame_count - 1, slot)) {
        return 0;
    }
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm->current_thread->frames[vm->current_thread->frame_count - 1];
    *ip = (*frame)->ip;
    return 1;
}

int vm_handle_op_sub_local_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binary_local_const(vm, frame, ip, "__sub", slot, constant, 0);
}

int vm_handle_op_mul_local_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binary_local_const(vm, frame, ip, "__mul", slot, constant, 1);
}

int vm_handle_op_div_local_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binary_local_const(vm, frame, ip, "__div", slot, constant, 2);
}

int vm_handle_op_mod_local_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binary_local_const(vm, frame, ip, "__mod", slot, constant, 3);
}
