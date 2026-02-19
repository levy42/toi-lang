#include <string.h>

#include "ops_compare.h"

static int valuesEqualSimpleLocal(Value a, Value b) {
    if (IS_NUMBER(a) && IS_NUMBER(b)) return AS_NUMBER(a) == AS_NUMBER(b);
    if (IS_BOOL(a) && IS_BOOL(b)) return AS_BOOL(a) == AS_BOOL(b);
    if (IS_NIL(a) && IS_NIL(b)) return 1;
    if (IS_OBJ(a) && IS_OBJ(b)) {
        if (AS_OBJ(a) == AS_OBJ(b)) return 1;
        if (IS_STRING(a) && IS_STRING(b)) {
            ObjString* sa = AS_STRING(a);
            ObjString* sb = AS_STRING(b);
            return (sa->hash == sb->hash &&
                    sa->length == sb->length &&
                    memcmp(sa->chars, sb->chars, sa->length) == 0);
        }
    }
    return 0;
}

int vmHandleOpEqual(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (valuesEqualSimpleLocal(a, b)) {
        push(vm, BOOL_VAL(1));
    } else if (IS_OBJ(a) && IS_OBJ(b)) {
        if (AS_OBJ(a) == AS_OBJ(b)) {
            push(vm, BOOL_VAL(1));
        }
        else if (IS_STRING(a) && IS_STRING(b)) {
            ObjString* sa = AS_STRING(a);
            ObjString* sb = AS_STRING(b);
            int equal = (sa->hash == sb->hash &&
                         sa->length == sb->length &&
                         memcmp(sa->chars, sb->chars, sa->length) == 0);
            push(vm, BOOL_VAL(equal));
        }
        else {
            Value method = getMetamethod(vm, a, "__eq");
            if (IS_NIL(method)) method = getMetamethod(vm, b, "__eq");
            if (!IS_NIL(method)) {
                push(vm, method);
                push(vm, a);
                push(vm, b);
                (*frame)->ip = *ip;
                if (!call(vm, AS_CLOSURE(method), 2)) return 0;
                *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                *ip = (*frame)->ip;
            } else {
                push(vm, BOOL_VAL(0));
            }
        }
    }
    else if (IS_NIL(a) && IS_NIL(b)) {
        push(vm, BOOL_VAL(1));
    }
    else {
        push(vm, BOOL_VAL(0));
    }
    return 1;
}

int vmHandleOpGreater(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b)));
    }
    else {
        Value method = getMetamethod(vm, a, "__lt");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__lt");
        if (!IS_NIL(method)) {
            push(vm, method);
            push(vm, b);
            push(vm, a);
            (*frame)->ip = *ip;
            if (!call(vm, AS_CLOSURE(method), 2)) return 0;
            *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
            *ip = (*frame)->ip;
        }
        else {
            push(vm, BOOL_VAL(0));
        }
    }
    return 1;
}

int vmHandleOpLess(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        push(vm, BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b)));
    }
    else {
        Value method = getMetamethod(vm, a, "__lt");
        if (IS_NIL(method)) method = getMetamethod(vm, b, "__lt");
        if (!IS_NIL(method)) {
            push(vm, method);
            push(vm, a);
            push(vm, b);
            (*frame)->ip = *ip;
            if (!call(vm, AS_CLOSURE(method), 2)) return 0;
            *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
            *ip = (*frame)->ip;
        }
        else {
            push(vm, BOOL_VAL(0));
        }
    }
    return 1;
}
