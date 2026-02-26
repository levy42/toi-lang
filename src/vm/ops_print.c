#include <stdio.h>

#include "ops_print.h"

static int vm_print_value(VM* vm, CallFrame** frame, uint8_t** ip, Value v, InterpretResult* out_result) {

    ObjTable* metatable = NULL;
    if (IS_TABLE(v)) {
        metatable = AS_TABLE(v)->metatable;
    } else if (IS_USERDATA(v)) {
        metatable = AS_USERDATA(v)->metatable;
    }

    if (metatable != NULL) {
        Value str_method;
        ObjString* str_key = vm->mm_str;
        if (table_get(&metatable->table, str_key, &str_method) &&
            (IS_CLOSURE(str_method) || IS_NATIVE(str_method))) {
            int saved_frame_count = vm_current_thread(vm)->frame_count;

            push(vm, str_method);
            push(vm, v);

            (*frame)->ip = *ip;
            if (call_value(vm, str_method, 1, frame, ip)) {
                if (IS_CLOSURE(str_method)) {
                    InterpretResult result = vm_run(vm, saved_frame_count);
                    if (result != INTERPRET_OK) {
                        *out_result = result;
                        return 0;
                    }
                }

                Value str_result = pop(vm);
                if (IS_STRING(str_result)) {
                    ObjString* s = AS_STRING(str_result);
                    fwrite(s->chars, 1, (size_t)s->length, stdout);
                } else {
                    print_value(str_result);
                }
                return 1;
            }
            return -1;
        }
    }

    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
    } else {
        print_value(v);
    }
    return 1;
}

int vm_handle_op_print(
    VM* vm,
    CallFrame** frame,
    uint8_t** ip,
    uint8_t arg_count,
    InterpretResult* out_result
) {
    if (arg_count == 0) {
        printf("\n");
        return 1;
    }

    Value values[UINT8_MAX];
    for (int i = (int)arg_count - 1; i >= 0; i--) {
        values[i] = pop(vm);
    }

    for (uint8_t i = 0; i < arg_count; i++) {
        int print_status = vm_print_value(vm, frame, ip, values[i], out_result);
        if (print_status <= 0) return print_status;
        if (i + 1 < arg_count) {
            printf(" ");
        }
    }
    printf("\n");
    return 1;
}
