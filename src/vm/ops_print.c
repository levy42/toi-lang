#include <stdio.h>

#include "ops_print.h"

int vm_handle_op_print(VM* vm, CallFrame** frame, uint8_t** ip, InterpretResult* out_result) {
    Value v = pop(vm);

    if (IS_TABLE(v)) {
        ObjTable* table = AS_TABLE(v);
        if (table->metatable != NULL) {
            Value str_method;
            ObjString* str_key = vm->mm_str;
            if (table_get(&table->metatable->table, str_key, &str_method) &&
                (IS_CLOSURE(str_method) || IS_NATIVE(str_method))) {
                int saved_frame_count = vm->current_thread->frame_count;

                push(vm, str_method);
                push(vm, v);

                (*frame)->ip = *ip;
                if (call_value(vm, str_method, 1, frame, ip)) {
                    InterpretResult result = vm_run(vm, saved_frame_count);
                    if (result == INTERPRET_OK) {
                        Value str_result = pop(vm);
                        if (IS_STRING(str_result)) {
                            ObjString* s = AS_STRING(str_result);
                            fwrite(s->chars, 1, (size_t)s->length, stdout);
                        } else {
                            print_value(str_result);
                        }
                        printf("\n");
                        return 1;
                    }
                    *out_result = result;
                    return 0;
                }
                return -1;
            }
        }
    }

    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
    } else {
        print_value(v);
    }
    printf("\n");
    return 1;
}
