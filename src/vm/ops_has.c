#include <string.h>

#include "ops_has.h"

static int values_equal_simple_local(Value a, Value b) {
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

static int string_contains_local(ObjString* haystack, ObjString* needle) {
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

int vm_handle_op_has(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);
    Value method = get_metamethod(vm, a, "__has");
    if (IS_NIL(method)) method = get_metamethod(vm, b, "__has");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(method), 2)) return 0;
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }
    if (IS_STRING(a)) {
        if (!IS_STRING(b)) {
            vm_runtime_error(vm, "Right operand of 'has' must be a string.");
            return 0;
        }
        push(vm, BOOL_VAL(string_contains_local(AS_STRING(a), AS_STRING(b))));
        return 1;
    }
    if (IS_TABLE(a)) {
        ObjTable* t = AS_TABLE(a);
        int found = 0;
        int max = t->table.array_max;
        if (max > t->table.array_capacity) max = t->table.array_capacity;
        for (int i = 0; i < max; i++) {
            Value v = t->table.array[i];
            if (!IS_NIL(v) && values_equal_simple_local(v, b)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < t->table.capacity; i++) {
                Entry* entry = &t->table.entries[i];
                if (entry->key != NULL && values_equal_simple_local(entry->value, b)) {
                    found = 1;
                    break;
                }
            }
        }
        push(vm, BOOL_VAL(found));
        return 1;
    }

    vm_runtime_error(vm, "Left operand of 'has' must be a string or table.");
    return 0;
}
