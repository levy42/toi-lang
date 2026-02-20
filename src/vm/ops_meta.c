#include "ops_meta.h"

static int find_property_local(VM* vm, ObjTable* table, ObjString* name, Value* result) {
    if (table_get(&table->table, name, result)) return 1;

    ObjTable* current = table;
    int depth = 0;
    ObjString* idx_name = vm->mm_index;

    while (current->metatable && depth < 10) {
        Value idx_val = NIL_VAL;
        if (table_get(&current->metatable->table, idx_name, &idx_val)) {
            if (IS_TABLE(idx_val)) {
                current = AS_TABLE(idx_val);
                if (table_get(&current->table, name, result)) {
                    return 1;
                }
                depth++;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return 0;
}

int vm_handle_op_new_table(VM* vm) {
    push(vm, OBJ_VAL(new_table()));
    maybe_collect_garbage(vm);
    return 1;
}

int vm_handle_op_set_metatable(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value table = peek(vm, 0);
    Value metatable = peek(vm, 1);
    if (!IS_TABLE(table) || (!IS_TABLE(metatable) && !IS_NIL(metatable))) {
        vm_runtime_error(vm, "Invalid arguments to setmetatable.");
        return 0;
    }
    AS_TABLE(table)->metatable = IS_NIL(metatable) ? NULL : AS_TABLE(metatable);

    int constructor_called = 0;
    if (!IS_NIL(metatable)) {
        Value init_method = NIL_VAL;
        ObjString* new_str = vm->mm_new;

        int found = find_property_local(vm, AS_TABLE(table), new_str, &init_method);
        if (!found) {
            if (table_get(&AS_TABLE(metatable)->table, new_str, &init_method)) {
                found = 1;
            }
        }

        if (found && (IS_CLOSURE(init_method) || IS_NATIVE(init_method))) {
            pop(vm);
            pop(vm);

            push(vm, init_method);
            push(vm, metatable);
            push(vm, table);

            constructor_called = 1;
            if (!call_value(vm, init_method, 2, frame, ip)) {
                return 0;
            }
        }
    }

    if (!constructor_called) {
        pop(vm);
        pop(vm);
        push(vm, table);
    }
    return 1;
}
