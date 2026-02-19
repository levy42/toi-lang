#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ops_arith.h"

static inline int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
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

int vmHandleOpAddConst(VM* vm, CallFrame** frame, uint8_t** ip, Value b) {
    Value a = peek(vm, 0);
    if (IS_STRING(a) && IS_STRING(b)) {
        push(vm, b);
        concatenateLocal(vm);
    } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
        pop(vm);
        push(vm, NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
    } else if (IS_TABLE(a) && IS_TABLE(b)) {
        ObjTable* tb = AS_TABLE(b);
        ObjTable* ta = AS_TABLE(pop(vm));
        ObjTable* result = newTable();
        push(vm, OBJ_VAL(result));
        tableAddLocal(ta, tb, result);
    } else {
        Value aPop = pop(vm);
        Value method = getMetamethod(vm, aPop, "__add");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
        if (IS_NIL(method)) {
            vmRuntimeError(vm, "Operands must be two numbers or two strings.");
            return 0;
        }
        push(vm, method);
        push(vm, aPop);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

int vmHandleOpAdd(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
        concatenateLocal(vm);
    } else if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
        double b = AS_NUMBER(pop(vm));
        double a = AS_NUMBER(pop(vm));
        push(vm, NUMBER_VAL(a + b));
    } else if (IS_TABLE(peek(vm, 0)) && IS_TABLE(peek(vm, 1))) {
        ObjTable* tb = AS_TABLE(pop(vm));
        ObjTable* ta = AS_TABLE(pop(vm));
        ObjTable* result = newTable();
        push(vm, OBJ_VAL(result));
        tableAddLocal(ta, tb, result);
    } else {
        Value b = pop(vm);
        Value a = pop(vm);
        Value method = getMetamethod(vm, a, "__add");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
        if (IS_NIL(method)) {
            vmRuntimeError(vm, "Operands must be two numbers or two strings.");
            return 0;
        }
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

int vmHandleOpSubtract(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
        double b = AS_NUMBER(pop(vm));
        double a = AS_NUMBER(pop(vm));
        push(vm, NUMBER_VAL(a - b));
    } else {
        Value b = pop(vm);
        Value a = pop(vm);
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

int vmHandleOpMultiply(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
        double b = AS_NUMBER(pop(vm));
        double a = AS_NUMBER(pop(vm));
        push(vm, NUMBER_VAL(a * b));
    } else {
        Value b = pop(vm);
        Value a = pop(vm);
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

int vmHandleOpDivide(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
        double b = AS_NUMBER(pop(vm));
        double a = AS_NUMBER(pop(vm));
        push(vm, NUMBER_VAL(a / b));
    } else {
        Value b = pop(vm);
        Value a = pop(vm);
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

int vmHandleOpModulo(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
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
    }
    else {
        Value method = getMetamethod(vm, a, "__mod");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__mod");
        if (IS_NIL(method)) return 0;
        push(vm, method); push(vm, a); push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}
