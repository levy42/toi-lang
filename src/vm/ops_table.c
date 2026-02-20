#include <stdio.h>

#include "../lib/libs.h"
#include "ops_table.h"

static int is_self_callable_local(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->is_self;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->is_self;
    return 0;
}

static Value maybe_bind_self_local(Value receiver, Value result) {
    if (IS_BOUND_METHOD(result)) return result;
    if (IS_TABLE(receiver) && AS_TABLE(receiver)->is_module) {
        return result;
    }
    if (is_self_callable_local(result)) {
        return OBJ_VAL(new_bound_method(receiver, AS_OBJ(result)));
    }
    return result;
}

static int handle_index_metamethod_local(
    VM* vm, ObjTable* t, Value table_val, Value key, Value* result, CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value idx_val = NIL_VAL;
    if (!table_get(&t->metatable->table, vm->mm_index, &idx_val)) return 0;

    if (IS_CLOSURE(idx_val) || IS_NATIVE(idx_val)) {
        push(vm, idx_val);
        push(vm, table_val);
        push(vm, key);
        if (!call_value(vm, idx_val, 2, frame, ip)) return -1;
        *result = pop(vm);
        return 1;
    }
    if (IS_TABLE(idx_val)) {
        *result = NIL_VAL;
        if (IS_STRING(key)) {
            table_get(&AS_TABLE(idx_val)->table, AS_STRING(key), result);
        } else if (IS_NUMBER(key)) {
            ObjString* n_key = number_key_string(AS_NUMBER(key));
            table_get(&AS_TABLE(idx_val)->table, n_key, result);
        }
        return 1;
    }
    return 0;
}

static int append_to_table_local(ObjTable* table, Value value) {
    int index = table->table.array_max + 1;
    if (!table_set_array(&table->table, index, value)) {
        ObjString* key = number_key_string((double)index);
        table_set(&table->table, key, value);
    }
    return index;
}

int vm_handle_op_append(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);

    Value method = get_metamethod(vm, a, "__append");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call_value(vm, method, 2, frame, ip)) return 0;
        return 1;
    }

    if (IS_TABLE(a)) {
        int index = append_to_table_local(AS_TABLE(a), b);
        push(vm, NUMBER_VAL((double)index));
        return 1;
    }

    method = get_metamethod(vm, b, "__append");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!call_value(vm, method, 2, frame, ip)) return 0;
        return 1;
    }

    vm_runtime_error(vm, "Left operand must be a table or define __append.");
    return 0;
}

int vm_handle_op_get_table(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value key = pop(vm);
    Value table = pop(vm);
    Value result = NIL_VAL;

    if (IS_TABLE(table)) {
        ObjTable* t = AS_TABLE(table);
        if (IS_STRING(key)) {
            if (table_get(&t->table, AS_STRING(key), &result)) {
                result = maybe_bind_self_local(table, result);
            } else if (t->metatable) {
                int handled = handle_index_metamethod_local(vm, t, table, key, &result, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) result = NIL_VAL;
                result = maybe_bind_self_local(table, result);
            }
        } else if (IS_NUMBER(key)) {
            double num_key = AS_NUMBER(key);
            int idx = (int)num_key;
            if (num_key == (double)idx) {
                if (idx < 0) {
                    int len = 0;
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!table_get_array(&t->table, i, &val) || IS_NIL(val)) {
                            len = i - 1;
                            break;
                        }
                    }
                    idx = len + idx + 1;
                }
                if (table_get_array(&t->table, idx, &result)) {
                    result = maybe_bind_self_local(table, result);
                    push(vm, result);
                    maybe_collect_garbage(vm);
                    return 1;
                }
            }

            ObjString* n_key = number_key_string(num_key);
            if (table_get(&t->table, n_key, &result)) {
                result = maybe_bind_self_local(table, result);
                push(vm, result);
            } else if (t->metatable) {
                int handled = handle_index_metamethod_local(vm, t, table, key, &result, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) result = NIL_VAL;
                result = maybe_bind_self_local(table, result);
                push(vm, result);
            } else {
                push(vm, NIL_VAL);
            }
            maybe_collect_garbage(vm);
            return 1;
        }
    } else if (IS_USERDATA(table)) {
        ObjUserdata* udata = AS_USERDATA(table);
        if (udata->metatable) {
            Value idx = NIL_VAL;
            ObjString* idx_name = vm->mm_index;
            if (table_get(&udata->metatable->table, idx_name, &idx)) {
                if (IS_CLOSURE(idx) || IS_NATIVE(idx)) {
                    push(vm, idx);
                    push(vm, table);
                    push(vm, key);
                    if (!call_value(vm, idx, 2, frame, ip)) return 0;
                    result = pop(vm);
                } else if (IS_TABLE(idx)) {
                    if (IS_STRING(key)) {
                        table_get(&AS_TABLE(idx)->table, AS_STRING(key), &result);
                    }
                }
                result = maybe_bind_self_local(table, result);
            }
        }
    } else if (IS_STRING(table)) {
        if (IS_STRING(key)) {
            Value string_module = NIL_VAL;
            ObjString* string_name = copy_string("string", 6);
            push(vm, OBJ_VAL(string_name));
            if ((!table_get(&vm->globals, string_name, &string_module) || !IS_TABLE(string_module)) &&
                load_native_module(vm, "string")) {
                string_module = peek(vm, 0);
                pop(vm);
            }
            if (IS_TABLE(string_module)) {
                if (table_get(&AS_TABLE(string_module)->table, AS_STRING(key), &result)) {
                    result = maybe_bind_self_local(table, result);
                } else {
                    result = NIL_VAL;
                }
            }
            pop(vm);
        } else if (IS_NUMBER(key)) {
            double num_key = AS_NUMBER(key);
            int idx = (int)num_key;
            if (num_key == (double)idx) {
                ObjString* s = AS_STRING(table);
                if (idx < 0) idx = s->length + idx + 1;
                if (idx >= 1 && idx <= s->length) {
                    char buf[2];
                    buf[0] = s->chars[idx - 1];
                    buf[1] = '\0';
                    push(vm, OBJ_VAL(copy_string(buf, 1)));
                    maybe_collect_garbage(vm);
                    return 1;
                }
            }
            push(vm, NIL_VAL);
            maybe_collect_garbage(vm);
            return 1;
        }
    } else {
        if (IS_OBJ(table)) {
            printf("DEBUG: Attempt to index non-table object type: %d\n", OBJ_TYPE(table));
        } else {
            printf("DEBUG: Attempt to index non-table value type: %d\n", table.type);
        }
        vm_runtime_error(vm, "Attempt to index non-table.");
        return 0;
    }
    push(vm, result);
    maybe_collect_garbage(vm);
    return 1;
}

static int handle_new_index_metamethod_local(
    VM* vm, ObjTable* t, Value table_val, Value key, Value value, CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value ni = NIL_VAL;
    if (!table_get(&t->metatable->table, vm->mm_newindex, &ni)) return 0;

    if (IS_CLOSURE(ni) || IS_NATIVE(ni)) {
        push(vm, ni);
        push(vm, table_val);
        push(vm, key);
        push(vm, value);
        if (!call_value(vm, ni, 3, frame, ip)) return -1;
        return 1;
    }
    if (IS_TABLE(ni)) {
        if (IS_STRING(key)) {
            table_set(&AS_TABLE(ni)->table, AS_STRING(key), value);
        } else if (IS_NUMBER(key)) {
            ObjString* n_key = number_key_string(AS_NUMBER(key));
            table_set(&AS_TABLE(ni)->table, n_key, value);
        }
        return 1;
    }
    return 0;
}

int vm_handle_op_set_table(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value value = pop(vm);
    Value key = pop(vm);
    Value table = pop(vm);

    if (!IS_TABLE(table)) {
        vm_runtime_error(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        Value dummy;
        if (table_get(&t->table, AS_STRING(key), &dummy)) {
            table_set(&t->table, AS_STRING(key), value);
        } else if (t->metatable) {
            int handled = handle_new_index_metamethod_local(vm, t, table, key, value, frame, ip);
            if (handled < 0) return 0;
            if (handled == 0) table_set(&t->table, AS_STRING(key), value);
        } else {
            table_set(&t->table, AS_STRING(key), value);
        }
    } else if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        int is_array = 0;
        if (num_key == (double)idx) {
            if (idx < 0) {
                int len = 0;
                for (int i = 1; ; i++) {
                    Value val;
                    if (!table_get_array(&t->table, i, &val) || IS_NIL(val)) {
                        len = i - 1;
                        break;
                    }
                }
                idx = len + idx + 1;
            }
            if (table_set_array(&t->table, idx, value)) {
                is_array = 1;
            }
        }

        if (!is_array) {
            ObjString* n_key = number_key_string(num_key);
            Value dummy;
            if (table_get(&t->table, n_key, &dummy)) {
                table_set(&t->table, n_key, value);
            } else if (t->metatable) {
                int handled = handle_new_index_metamethod_local(vm, t, table, key, value, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) table_set(&t->table, n_key, value);
            } else {
                table_set(&t->table, n_key, value);
            }
        }
    }

    push(vm, value);
    maybe_collect_garbage(vm);
    return 1;
}

int vm_handle_op_delete_table(VM* vm) {
    Value key = pop(vm);
    Value table = pop(vm);

    if (!IS_TABLE(table)) {
        vm_runtime_error(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        if (!table_delete(&t->table, AS_STRING(key))) {
            vm_runtime_error(vm, "Key not found.");
            return 0;
        }
        return 1;
    }

    if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        if (num_key == (double)idx) {
            if (idx < 0) {
                int len = 0;
                for (int i = 1; ; i++) {
                    Value val;
                    if (!table_get_array(&t->table, i, &val) || IS_NIL(val)) {
                        len = i - 1;
                        break;
                    }
                }
                idx = len + idx + 1;
            }
            if (idx >= 1 && idx <= t->table.array_capacity) {
                if (!IS_NIL(t->table.array[idx - 1])) {
                    table_set_array(&t->table, idx, NIL_VAL);
                    return 1;
                }
            }
        }

        ObjString* n_key = number_key_string(num_key);
        if (!table_delete(&t->table, n_key)) {
            vm_runtime_error(vm, "Key not found.");
            return 0;
        }
        return 1;
    }

    vm_runtime_error(vm, "Invalid key type for deletion.");
    return 0;
}
