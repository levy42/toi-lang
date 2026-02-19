#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ops_local_set.h"

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

static void concatenateLocal(VM* vm) {
    ObjString* b = AS_STRING(pop(vm));
    ObjString* a = AS_STRING(pop(vm));

    int length = a->length + b->length;
    char* chars = (char*)malloc((size_t)length + 1);
    memcpy(chars, a->chars, (size_t)a->length);
    memcpy(chars + a->length, b->chars, (size_t)b->length);
    chars[length] = '\0';

    push(vm, OBJ_VAL(takeString(chars, length)));
}

static void tableAddLocal(ObjTable* ta, ObjTable* tb, ObjTable* result) {
    int lenA = 0, lenB = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&ta->table, i, &val) || IS_NIL(val)) {
            lenA = i - 1;
            break;
        }
    }
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&tb->table, i, &val) || IS_NIL(val)) {
            lenB = i - 1;
            break;
        }
    }

    for (int i = 1; i <= lenA; i++) {
        Value val;
        tableGetArray(&ta->table, i, &val);
        tableSetArray(&result->table, i, val);
    }
    for (int i = 1; i <= lenB; i++) {
        Value val;
        tableGetArray(&tb->table, i, &val);
        tableSetArray(&result->table, lenA + i, val);
    }

    tableAddAll(&ta->table, &result->table);
    tableAddAll(&tb->table, &result->table);
}

int vmHandleOpAddSetLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_STRING(a) && IS_STRING(b)) {
        push(vm, a);
        push(vm, b);
        concatenateLocal(vm);
        (*frame)->slots[slot] = peek(vm, 0);
        return 1;
    }
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        Value out = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
        (*frame)->slots[slot] = out;
        push(vm, out);
        return 1;
    }
    if (IS_TABLE(a) && IS_TABLE(b)) {
        ObjTable* tb = AS_TABLE(b);
        ObjTable* ta = AS_TABLE(a);
        ObjTable* result = newTable();
        push(vm, OBJ_VAL(result));
        tableAddLocal(ta, tb, result);
        (*frame)->slots[slot] = peek(vm, 0);
        return 1;
    }

    Value method = getMetamethod(vm, a, "__add");
    if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
    if (IS_NIL(method)) {
        vmRuntimeError(vm, "Operands must be two numbers or two strings.");
        return 0;
    }
    push(vm, method);
    push(vm, a);
    push(vm, b);
    if (!pushPendingSetLocalLocal(vm, vm->currentThread->frameCount - 1, slot)) return 0;
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
    *ip = (*frame)->ip;
    return 1;
}

static int binarySetLocal(
    VM* vm, CallFrame** frame, uint8_t** ip, const char* mmName, uint8_t slot, int op) {
    Value b = pop(vm);
    Value a = pop(vm);
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
    if (!pushPendingSetLocalLocal(vm, vm->currentThread->frameCount - 1, slot)) return 0;
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
    *ip = (*frame)->ip;
    return 1;
}

int vmHandleOpSubSetLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binarySetLocal(vm, frame, ip, "__sub", slot, 0);
}

int vmHandleOpMulSetLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binarySetLocal(vm, frame, ip, "__mul", slot, 1);
}

int vmHandleOpDivSetLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binarySetLocal(vm, frame, ip, "__div", slot, 2);
}

int vmHandleOpModSetLocal(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binarySetLocal(vm, frame, ip, "__mod", slot, 3);
}
