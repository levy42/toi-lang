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

static int push_pending_set_local_local(VM* vm, int frame_index, int slot) {
    ObjThread* thread = vm_current_thread(vm);
    if (thread->pending_set_local_count >= 8) {
        vm_runtime_error(vm, "Pending set-local stack overflow.");
        return 0;
    }
    int idx = thread->pending_set_local_count++;
    thread->pending_set_local_frames[idx] = frame_index;
    thread->pending_set_local_slots[idx] = slot;
    return 1;
}

static void concatenate_local(VM* vm) {
    ObjString* b = AS_STRING(pop(vm));
    ObjString* a = AS_STRING(pop(vm));

    int length = a->length + b->length;
    char* chars = (char*)malloc((size_t)length + 1);
    memcpy(chars, a->chars, (size_t)a->length);
    memcpy(chars + a->length, b->chars, (size_t)b->length);
    chars[length] = '\0';

    push(vm, OBJ_VAL(take_string(chars, length)));
}

static void table_add_local(ObjTable* ta, ObjTable* tb, ObjTable* result) {
    int len_a = 0, len_b = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&ta->table, i, &val) || IS_NIL(val)) {
            len_a = i - 1;
            break;
        }
    }
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&tb->table, i, &val) || IS_NIL(val)) {
            len_b = i - 1;
            break;
        }
    }

    for (int i = 1; i <= len_a; i++) {
        Value val;
        table_get_array(&ta->table, i, &val);
        table_set_array(&result->table, i, val);
    }
    for (int i = 1; i <= len_b; i++) {
        Value val;
        table_get_array(&tb->table, i, &val);
        table_set_array(&result->table, len_a + i, val);
    }

    table_add_all(&ta->table, &result->table);
    table_add_all(&tb->table, &result->table);
}

int vm_handle_op_add_set_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    Value b = pop(vm);
    Value a = pop(vm);
    if (IS_STRING(a) && IS_STRING(b)) {
        push(vm, a);
        push(vm, b);
        concatenate_local(vm);
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
        ObjTable* result = new_table();
        push(vm, OBJ_VAL(result));
        table_add_local(ta, tb, result);
        (*frame)->slots[slot] = peek(vm, 0);
        return 1;
    }

    Value method = get_metamethod(vm, a, "__add");
    if (IS_NIL(method)) method = get_metamethod(vm, b, "__add");
    if (IS_NIL(method)) {
        vm_runtime_error(vm, "Operands must be two numbers or two strings.");
        return 0;
    }
    push(vm, method);
    push(vm, a);
    push(vm, b);
    if (!push_pending_set_local_local(vm, vm_current_thread(vm)->frame_count - 1, slot)) return 0;
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    *ip = (*frame)->ip;
    return 1;
}

static int binary_set_local(
    VM* vm, CallFrame** frame, uint8_t** ip, const char* mm_name, uint8_t slot, int op) {
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

    Value method = get_metamethod(vm, a, mm_name);
    if (IS_NIL(method)) method = get_metamethod(vm, b, mm_name);
    if (IS_NIL(method)) return 0;
    push(vm, method);
    push(vm, a);
    push(vm, b);
    if (!push_pending_set_local_local(vm, vm_current_thread(vm)->frame_count - 1, slot)) return 0;
    (*frame)->ip = *ip;
    if (!call(vm, AS_CLOSURE(method), 2)) return 0;
    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    *ip = (*frame)->ip;
    return 1;
}

int vm_handle_op_sub_set_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binary_set_local(vm, frame, ip, "__sub", slot, 0);
}

int vm_handle_op_mul_set_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binary_set_local(vm, frame, ip, "__mul", slot, 1);
}

int vm_handle_op_div_set_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binary_set_local(vm, frame, ip, "__div", slot, 2);
}

int vm_handle_op_mod_set_local(VM* vm, CallFrame** frame, uint8_t** ip) {
    uint8_t slot = *(*ip)++;
    return binary_set_local(vm, frame, ip, "__mod", slot, 3);
}
