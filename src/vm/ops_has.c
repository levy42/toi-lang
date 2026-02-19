#include <string.h>

#include "ops_has.h"

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

static int stringContainsLocal(ObjString* haystack, ObjString* needle) {
    if (needle->length == 0) return 1;
    if (needle->length > haystack->length) return 0;
    int last = haystack->length - needle->length;
    for (int i = 0; i <= last; i++) {
        if (haystack->chars[i] == needle->chars[0] &&
            memcmp(haystack->chars + i, needle->chars, needle->length) == 0) {
            return 1;
        }
    }
    return 0;
}

int vmHandleOpHas(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    Value method = getMetamethod(vm, a, "__has");
    if (IS_NIL(method)) method = getMetamethod(vm, b, "__has");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
        return 1;
    }
    if (IS_STRING(a)) {
        if (!IS_STRING(b)) {
            vmRuntimeError(vm, "Right operand of 'has' must be a string.");
            return 0;
        }
        push(vm, BOOL_VAL(stringContainsLocal(AS_STRING(a), AS_STRING(b))));
        return 1;
    }
    if (IS_TABLE(a)) {
        ObjTable* t = AS_TABLE(a);
        int found = 0;
        int max = t->table.arrayMax;
        if (max > t->table.arrayCapacity) max = t->table.arrayCapacity;
        for (int i = 0; i < max; i++) {
            Value v = t->table.array[i];
            if (!IS_NIL(v) && valuesEqualSimpleLocal(v, b)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < t->table.capacity; i++) {
                Entry* entry = &t->table.entries[i];
                if (entry->key != NULL && valuesEqualSimpleLocal(entry->value, b)) {
                    found = 1;
                    break;
                }
            }
        }
        push(vm, BOOL_VAL(found));
        return 1;
    }

    vmRuntimeError(vm, "Left operand of 'has' must be a string or table.");
    return 0;
}
