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

static int pushPendingSetLocalLocal(VM* vm, int frameIndex, int slot) {
    if (vm->pendingSetLocalCount >= 8) {
        vmRuntimeError(vm, "Pending set-local stack overflow.");
        return 0;
    }
    int idx = vm->pendingSetLocalCount++;
    vm->pendingSetLocalFrames[idx] = frameIndex;
    vm->pendingSetLocalSlots[idx] = slot;
    return 1;
}

int vmHandleOpIncLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    Value v = (*frame)->slots[slot];
    Value c = (*frame)->closure->function->chunk.constants.values[constant];
    if (!IS_NUMBER(v) || !IS_NUMBER(c)) {
        vmRuntimeError(vm, "Operands must be two numbers.");
        return 0;
    }
    Value out = NUMBER_VAL(AS_NUMBER(v) + AS_NUMBER(c));
    (*frame)->slots[slot] = out;
    push(vm, out);
    return 1;
}

static int binaryLocalConst(
    VM* vm, CallFrame** frame, uint8_t** ip, const char* mmName, uint8_t slot, uint8_t constant, int op) {
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

    Value method = getMetamethod(vm, a, mmName);
    if (IS_NIL(method)) method = getMetamethod(vm, b, mmName);
    if (IS_NIL(method)) return 0;
    push(vm, method);
    push(vm, a);
    push(vm, b);
    if (!pushPendingSetLocalLocal(vm, vm->currentThread->frameCount - 1, slot)) {
        return 0;
    }
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
    *ip = (*frame)->ip;
    return 1;
}

int vmHandleOpSubLocalConst(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binaryLocalConst(vm, frame, ip, "__sub", slot, constant, 0);
}

int vmHandleOpMulLocalConst(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binaryLocalConst(vm, frame, ip, "__mul", slot, constant, 1);
}

int vmHandleOpDivLocalConst(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binaryLocalConst(vm, frame, ip, "__div", slot, constant, 2);
}

int vmHandleOpModLocalConst(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    uint8_t constant = *(*ip)++;
    return binaryLocalConst(vm, frame, ip, "__mod", slot, constant, 3);
}
