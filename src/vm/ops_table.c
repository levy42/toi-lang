#include <stdio.h>
#include <string.h>

#include "../lib/libs.h"
#include "ops_table.h"

static int is_self_callable_local(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->is_self;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->is_self;
    return 0;
}

static int objstring_equals_local(ObjString* a, ObjString* b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (a->length != b->length) return 0;
    if (objstring_hash(a) != objstring_hash(b)) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
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

static Value bind_self_for_meta_local(Value receiver, Value result) {
    if (IS_BOUND_METHOD(result)) return result;
    if (IS_CLOSURE(result) || IS_NATIVE(result)) {
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
        if (vm->table_prof_enabled) vm->table_prof_get_meta_index_count++;
        push(vm, idx_val);
        push(vm, table_val);
        push(vm, key);
        if (!call_value(vm, idx_val, 2, frame, ip)) return -1;
        *result = pop(vm);
        return 1;
    }
    if (IS_TABLE(idx_val)) {
        if (vm->table_prof_enabled) vm->table_prof_get_meta_index_count++;
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

static void record_table_site_key_profile(
    VM* vm, Chunk* chunk, int opcode_offset, ObjString* key, int is_set) {
    if (!vm->table_site_prof_enabled) return;
    if (chunk == NULL) return;
    if (opcode_offset < 0 || opcode_offset >= chunk->capacity) return;

    uint32_t* accesses = is_set ? chunk->set_table_site_accesses : chunk->get_table_site_accesses;
    uint8_t* unique = is_set ? chunk->set_table_site_unique_keys : chunk->get_table_site_unique_keys;
    ObjString** keys = is_set ? chunk->set_table_site_keys : chunk->get_table_site_keys;
    if (accesses == NULL || unique == NULL || keys == NULL || key == NULL) return;

    accesses[opcode_offset]++;
    uint8_t u = unique[opcode_offset];
    int base = opcode_offset * 4;
    int known = (u > 4) ? 4 : (int)u;
    for (int lane = 0; lane < known; lane++) {
        if (objstring_equals_local(keys[base + lane], key)) {
            return;
        }
    }
    if (u < 4) {
        keys[base + u] = key;
        unique[opcode_offset] = (uint8_t)(u + 1);
        return;
    }
    if (u < 5) unique[opcode_offset] = 5;
}

static int get_table_ic_try_lane(
    ObjTable* t,
    ObjString* key_str,
    uint32_t shape_version,
    ObjTable** tables,
    ObjString** keys,
    int* slots,
    uint32_t* versions,
    int site,
    Value* out_value,
    int* out_exact_hit) {
    if (tables == NULL || keys == NULL || slots == NULL || versions == NULL) return 0;
    if (keys[site] == NULL || !objstring_equals_local(keys[site], key_str)) return 0;

    int slot = slots[site];
    if (slot == -2) {
        if (tables[site] != t || versions[site] != shape_version) return 0;
        *out_value = NIL_VAL;
        *out_exact_hit = 1;
        return 1;
    }
    if (slot < 0 || slot >= t->table.capacity) return 0;
    Entry* entry = &t->table.entries[slot];
    if (!objstring_equals_local(entry->key, key_str)) return 0;

    *out_value = entry->value;
    *out_exact_hit = (tables[site] == t && versions[site] == shape_version);
    return 1;
}

static void get_table_ic_update_lane(
    ObjTable** tables,
    ObjString** keys,
    int* slots,
    uint32_t* versions,
    int site,
    ObjTable* table,
    ObjString* key,
    int slot,
    uint32_t version) {
    if (tables == NULL || keys == NULL || slots == NULL || versions == NULL) return;
    tables[site] = table;
    keys[site] = key;
    slots[site] = slot;
    versions[site] = version;
}

static void update_get_table_ic(
    Chunk* chunk, int site, ObjTable* t, ObjString* key_str, int slot) {
    if (chunk == NULL || site < 0 || site >= chunk->capacity) return;
    if (chunk->get_table_ic_tables == NULL || chunk->get_table_ic_keys == NULL ||
        chunk->get_table_ic_slots == NULL || chunk->get_table_ic_versions == NULL) {
        return;
    }

    uint32_t version = t->table.shape_version;
    get_table_ic_update_lane(
        chunk->get_table_ic_tables,
        chunk->get_table_ic_keys,
        chunk->get_table_ic_slots,
        chunk->get_table_ic_versions,
        site,
        t,
        key_str,
        slot,
        version);
}

int vm_handle_op_append(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);

    if (IS_TABLE(a)) {
        ObjTable* ta = AS_TABLE(a);
        if (ta->no_metatable_fast) {
            int index = append_to_table_local(ta, b);
            push(vm, NUMBER_VAL((double)index));
            return 1;
        }
    }

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

int vm_handle_get_table_inner(
    VM* vm, CallFrame** frame, uint8_t** ip, Chunk* chunk, int opcode_offset, Value table, Value key) {
    Value result = NIL_VAL;

    if (IS_TABLE(table)) {
        ObjTable* t = AS_TABLE(table);
        if (IS_STRING(key)) {
            ObjString* key_str = AS_STRING(key);
            record_table_site_key_profile(vm, chunk, opcode_offset, key_str, 0);
            int no_meta_fast = t->no_metatable_fast;
            if (t->no_metatable_fast &&
                opcode_offset >= 0 && opcode_offset < chunk->capacity &&
                chunk->get_table_ic_tables != NULL &&
                chunk->get_table_ic_slots != NULL &&
                chunk->get_table_ic_versions != NULL &&
                chunk->get_table_ic_keys != NULL) {
                int exact_hit = 0;
                int ic_hit = 0;
                ic_hit = get_table_ic_try_lane(
                    t, key_str, t->table.shape_version,
                    chunk->get_table_ic_tables,
                    chunk->get_table_ic_keys,
                    chunk->get_table_ic_slots,
                    chunk->get_table_ic_versions,
                    opcode_offset,
                    &result,
                    &exact_hit);
                if (ic_hit) {
                    if (vm->table_prof_enabled) {
                        if (exact_hit) vm->table_prof_get_ic_exact_hit_count++;
                        else vm->table_prof_get_ic_slot_hit_count++;
                    }
                    result = maybe_bind_self_local(table, result);
                    push(vm, result);
                    maybe_collect_garbage(vm);
                    return 1;
                }
            }
            if (vm->table_prof_enabled && no_meta_fast) {
                vm->table_prof_get_ic_miss_count++;
            }
            int slot = -1;
            if (table_get_with_slot(&t->table, key_str, &result, &slot)) {
                if (vm->table_prof_enabled) vm->table_prof_get_generic_lookup_count++;
                if (t->no_metatable_fast &&
                    opcode_offset >= 0 && opcode_offset < chunk->capacity) {
                    update_get_table_ic(chunk, opcode_offset, t, key_str, slot);
                }
                result = maybe_bind_self_local(table, result);
            } else if (!t->no_metatable_fast) {
                int handled = handle_index_metamethod_local(vm, t, table, key, &result, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) result = NIL_VAL;
                result = maybe_bind_self_local(table, result);
            } else if (opcode_offset >= 0 && opcode_offset < chunk->capacity) {
                update_get_table_ic(chunk, opcode_offset, t, key_str, -2);
            }
        } else if (IS_NUMBER(key)) {
            double num_key = AS_NUMBER(key);
            int idx = (int)num_key;
            if (num_key == (double)idx) {
                if (idx < 0) {
                    idx = t->table.array_max + idx + 1;
                }
                if (idx >= 1 && idx <= t->table.array_capacity) {
                    Value arr_val = t->table.array[idx - 1];
                    if (!IS_NIL(arr_val)) {
                        result = arr_val;
                        result = maybe_bind_self_local(table, result);
                        push(vm, result);
                        maybe_collect_garbage(vm);
                        return 1;
                    }
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
            } else if (!t->no_metatable_fast) {
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
            ObjString* method_name = AS_STRING(key);
            Value cached_method = NIL_VAL;
            int use_cached = 0;
            int invoke_next_offset = -1;
            if (chunk != NULL &&
                opcode_offset >= 0 &&
                opcode_offset < chunk->count &&
                chunk->code[opcode_offset] == OP_GET_FIELD_CONST) {
                invoke_next_offset = opcode_offset + 2;
            }

            if (objstring_equals_local(method_name, vm->str_upper_name)) {
                if (!IS_NIL(vm->str_upper_fn)) {
                    cached_method = vm->str_upper_fn;
                    use_cached = 1;
                }
            } else if (objstring_equals_local(method_name, vm->str_lower_name)) {
                if (!IS_NIL(vm->str_lower_fn)) {
                    cached_method = vm->str_lower_fn;
                    use_cached = 1;
                }
            } else if (objstring_equals_local(method_name, vm->str_find_name)) {
                if (!IS_NIL(vm->str_find_fn)) {
                    cached_method = vm->str_find_fn;
                    use_cached = 1;
                }
            } else if (objstring_equals_local(method_name, vm->str_replace_name)) {
                if (!IS_NIL(vm->str_replace_fn)) {
                    cached_method = vm->str_replace_fn;
                    use_cached = 1;
                }
            }

            if (use_cached) {
                if (method_name == vm->str_find_name &&
                    IS_NATIVE(cached_method) &&
                    (NativeFastKind)AS_NATIVE_OBJ(cached_method)->fast_kind == NATIVE_FAST_STR_FIND &&
                    invoke_next_offset >= 0 &&
                    invoke_next_offset < chunk->count &&
                    chunk->code[invoke_next_offset] == OP_CALL1) {
                    chunk->code[invoke_next_offset] = OP_QINVOKE1_STR_FIND;
                    push(vm, cached_method);
                    push(vm, table);
                    maybe_collect_garbage(vm);
                    return 1;
                }
                if (invoke_next_offset >= 0 &&
                    invoke_next_offset < chunk->count &&
                    chunk->code[invoke_next_offset] == OP_QINVOKE1_STR_FIND) {
                    chunk->code[invoke_next_offset] = OP_CALL1;
                }
                result = bind_self_for_meta_local(table, cached_method);
                push(vm, result);
                maybe_collect_garbage(vm);
                return 1;
            }

            if ((!table_get(&vm->globals, vm->str_module_name, &string_module) || !IS_TABLE(string_module)) &&
                load_native_module(vm, "string")) {
                string_module = peek(vm, 0);
                pop(vm);
            }
            if (IS_TABLE(string_module)) {
                ObjString* lookup_name = method_name;
                if (objstring_equals_local(method_name, vm->str_upper_name)) {
                    lookup_name = vm->str_upper_name;
                } else if (objstring_equals_local(method_name, vm->str_lower_name)) {
                    lookup_name = vm->str_lower_name;
                } else if (objstring_equals_local(method_name, vm->str_find_name)) {
                    lookup_name = vm->str_find_name;
                } else if (objstring_equals_local(method_name, vm->str_replace_name)) {
                    lookup_name = vm->str_replace_name;
                }
                if (table_get(&AS_TABLE(string_module)->table, lookup_name, &result)) {
                    if (lookup_name == vm->str_upper_name) {
                        vm->str_upper_fn = result;
                    } else if (lookup_name == vm->str_lower_name) {
                        vm->str_lower_fn = result;
                    } else if (lookup_name == vm->str_find_name) {
                        vm->str_find_fn = result;
                    } else if (lookup_name == vm->str_replace_name) {
                        vm->str_replace_fn = result;
                    }
                    if (lookup_name == vm->str_find_name &&
                        IS_NATIVE(result) &&
                        (NativeFastKind)AS_NATIVE_OBJ(result)->fast_kind == NATIVE_FAST_STR_FIND &&
                        invoke_next_offset >= 0 &&
                        invoke_next_offset < chunk->count &&
                        chunk->code[invoke_next_offset] == OP_CALL1) {
                        chunk->code[invoke_next_offset] = OP_QINVOKE1_STR_FIND;
                        push(vm, result);
                        push(vm, table);
                        maybe_collect_garbage(vm);
                        return 1;
                    }
                    if (invoke_next_offset >= 0 &&
                        invoke_next_offset < chunk->count &&
                        chunk->code[invoke_next_offset] == OP_QINVOKE1_STR_FIND) {
                        chunk->code[invoke_next_offset] = OP_CALL1;
                    }
                    result = bind_self_for_meta_local(table, result);
                } else {
                    result = NIL_VAL;
                }
            }
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
    } else if (IS_CLOSURE(table)) {
        if (IS_STRING(key)) {
            ObjString* key_str = AS_STRING(key);
            ObjFunction* fn = AS_CLOSURE(table)->function;
            if (key_str->length == 5 && memcmp(key_str->chars, "__doc", 5) == 0 && fn->doc != NULL) {
                result = OBJ_VAL(fn->doc);
            } else {
                result = NIL_VAL;
            }
        } else {
            result = NIL_VAL;
        }
    } else if (IS_BOUND_METHOD(table)) {
        if (IS_STRING(key)) {
            ObjString* key_str = AS_STRING(key);
            ObjBoundMethod* bound = AS_BOUND_METHOD(table);
            if (key_str->length == 5 && memcmp(key_str->chars, "__doc", 5) == 0 &&
                bound->method->type == OBJ_CLOSURE) {
                ObjFunction* fn = ((ObjClosure*)bound->method)->function;
                if (fn->doc != NULL) {
                    result = OBJ_VAL(fn->doc);
                }
            }
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

int vm_handle_op_get_table(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value key = pop(vm);
    Value table = pop(vm);
    return vm_handle_get_table_inner(vm, frame, ip, chunk, opcode_offset, table, key);
}

int vm_handle_op_get_table_num(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value key = pop(vm);
    Value table = pop(vm);

    if (IS_TABLE(table) && IS_NUMBER(key)) {
        ObjTable* t = AS_TABLE(table);
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        Value result = NIL_VAL;
        if (num_key == (double)idx) {
            if (idx < 0) idx = t->table.array_max + idx + 1;
            if (idx >= 1 && idx <= t->table.array_capacity) {
                Value arr_val = t->table.array[idx - 1];
                if (!IS_NIL(arr_val)) {
                    push(vm, maybe_bind_self_local(table, arr_val));
                    maybe_collect_garbage(vm);
                    return 1;
                }
            }
            if (table_get_array(&t->table, idx, &result)) {
                push(vm, maybe_bind_self_local(table, result));
                maybe_collect_garbage(vm);
                return 1;
            }
        }
        ObjString* n_key = number_key_string(num_key);
        if (table_get(&t->table, n_key, &result)) {
            push(vm, maybe_bind_self_local(table, result));
        } else if (!t->no_metatable_fast) {
            int handled = handle_index_metamethod_local(vm, t, table, key, &result, frame, ip);
            if (handled < 0) return 0;
            if (handled == 0) result = NIL_VAL;
            push(vm, maybe_bind_self_local(table, result));
        } else {
            push(vm, NIL_VAL);
        }
        maybe_collect_garbage(vm);
        return 1;
    }

    return vm_handle_get_table_inner(vm, frame, ip, chunk, opcode_offset, table, key);
}

int vm_handle_op_get_field_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 2;
    uint8_t key_index = *(*ip)++;
    Value key = chunk->constants.values[key_index];
    Value table = pop(vm);
    return vm_handle_get_table_inner(vm, frame, ip, chunk, opcode_offset, table, key);
}

static int handle_new_index_metamethod_local(
    VM* vm, ObjTable* t, Value table_val, Value key, Value value, CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value ni = NIL_VAL;
    if (!table_get(&t->metatable->table, vm->mm_newindex, &ni)) return 0;

    if (IS_CLOSURE(ni) || IS_NATIVE(ni)) {
        if (vm->table_prof_enabled) vm->table_prof_set_meta_newindex_count++;
        push(vm, ni);
        push(vm, table_val);
        push(vm, key);
        push(vm, value);
        if (!call_value(vm, ni, 3, frame, ip)) return -1;
        return 1;
    }
    if (IS_TABLE(ni)) {
        if (vm->table_prof_enabled) vm->table_prof_set_meta_newindex_count++;
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

int vm_handle_op_get_meta_table(VM* vm) {
    Value key = pop(vm);
    Value receiver = pop(vm);
    Value result = NIL_VAL;

    if (IS_STRING(key)) {
        ObjString* key_str = AS_STRING(key);
        if (IS_TABLE(receiver)) {
            ObjTable* t = AS_TABLE(receiver);
            if (t->metatable && table_get(&t->metatable->table, key_str, &result)) {
                result = bind_self_for_meta_local(receiver, result);
            }
        } else if (IS_USERDATA(receiver)) {
            ObjUserdata* udata = AS_USERDATA(receiver);
            if (udata->metatable && table_get(&udata->metatable->table, key_str, &result)) {
                result = bind_self_for_meta_local(receiver, result);
            }
        }
    }

    push(vm, result);
    maybe_collect_garbage(vm);
    return 1;
}

static int vm_handle_set_table_inner(
    VM* vm, CallFrame** frame, uint8_t** ip, Chunk* chunk, int opcode_offset, Value table, Value key, Value value, int keep_table) {
    if (!IS_TABLE(table)) {
        vm_runtime_error(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        ObjString* key_str = AS_STRING(key);
        record_table_site_key_profile(vm, chunk, opcode_offset, key_str, 1);
        int no_meta_fast = t->no_metatable_fast;
        if (t->no_metatable_fast &&
            opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->set_table_ic_tables != NULL &&
            chunk->set_table_ic_versions != NULL &&
            chunk->set_table_ic_tables[opcode_offset] == t &&
            objstring_equals_local(chunk->set_table_ic_keys[opcode_offset], key_str) &&
            chunk->set_table_ic_slots != NULL &&
            chunk->set_table_ic_versions[opcode_offset] == t->table.shape_version) {
            int slot = chunk->set_table_ic_slots[opcode_offset];
            if (table_set_at_slot_or_empty(&t->table, slot, key_str, value)) {
                if (vm->table_prof_enabled) vm->table_prof_set_ic_exact_hit_count++;
                chunk->set_table_ic_versions[opcode_offset] = t->table.shape_version;
                push(vm, keep_table ? table : value);
                maybe_collect_garbage(vm);
                return 1;
            }
        }

        // Cross-instance monomorphic slot cache for repeated key stores on
        // many same-layout tables.
        if (t->no_metatable_fast &&
            opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->set_table_ic_slots != NULL &&
            chunk->set_table_ic_keys != NULL &&
            objstring_equals_local(chunk->set_table_ic_keys[opcode_offset], key_str)) {
            int slot = chunk->set_table_ic_slots[opcode_offset];
            if (table_set_at_slot_or_empty(&t->table, slot, key_str, value)) {
                if (vm->table_prof_enabled) vm->table_prof_set_ic_slot_hit_count++;
                push(vm, keep_table ? table : value);
                maybe_collect_garbage(vm);
                return 1;
            }
        }

        if (vm->table_prof_enabled && no_meta_fast) {
            vm->table_prof_set_ic_miss_count++;
        }

        int did_direct_set = 0;
        int slot = -1;
        Value dummy;
        if (table_get_with_slot(&t->table, key_str, &dummy, &slot)) {
            if (!table_set_at_slot(&t->table, slot, key_str, value)) {
                table_set_with_slot(&t->table, key_str, value, &slot);
            }
            did_direct_set = 1;
        } else if (!t->no_metatable_fast) {
            int handled = handle_new_index_metamethod_local(vm, t, table, key, value, frame, ip);
            if (handled < 0) return 0;
            if (handled == 0) {
                table_set_with_slot(&t->table, key_str, value, &slot);
                did_direct_set = 1;
            }
        } else {
            table_set_with_slot(&t->table, key_str, value, &slot);
            did_direct_set = 1;
        }

        if (did_direct_set &&
            t->no_metatable_fast &&
            opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->set_table_ic_tables != NULL) {
            if (vm->table_prof_enabled) vm->table_prof_set_generic_store_count++;
            chunk->set_table_ic_tables[opcode_offset] = t;
            chunk->set_table_ic_keys[opcode_offset] = key_str;
            if (chunk->set_table_ic_slots != NULL) {
                chunk->set_table_ic_slots[opcode_offset] = slot;
            }
            chunk->set_table_ic_versions[opcode_offset] = t->table.shape_version;
        }
    } else if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        int is_array = 0;
        if (num_key == (double)idx) {
            if (idx < 0) {
                idx = t->table.array_max + idx + 1;
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
            } else if (!t->no_metatable_fast) {
                int handled = handle_new_index_metamethod_local(vm, t, table, key, value, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) table_set(&t->table, n_key, value);
            } else {
                table_set(&t->table, n_key, value);
            }
        }
    }

    push(vm, keep_table ? table : value);
    maybe_collect_garbage(vm);
    return 1;
}

int vm_handle_op_set_table(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value value = pop(vm);
    Value key = pop(vm);
    Value table = pop(vm);
    return vm_handle_set_table_inner(vm, frame, ip, chunk, opcode_offset, table, key, value, 0);
}

int vm_handle_op_set_table_keep(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value value = pop(vm);
    Value key = pop(vm);
    Value table = pop(vm);
    return vm_handle_set_table_inner(vm, frame, ip, chunk, opcode_offset, table, key, value, 1);
}

int vm_handle_op_set_table_num(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value value = pop(vm);
    Value key = pop(vm);
    Value table = pop(vm);

    if (IS_TABLE(table) && IS_NUMBER(key)) {
        ObjTable* t = AS_TABLE(table);
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        int is_array = 0;
        if (num_key == (double)idx) {
            if (idx < 0) idx = t->table.array_max + idx + 1;
            if (table_set_array(&t->table, idx, value)) is_array = 1;
        }
        if (!is_array) {
            ObjString* n_key = number_key_string(num_key);
            Value dummy;
            if (table_get(&t->table, n_key, &dummy)) {
                table_set(&t->table, n_key, value);
            } else if (!t->no_metatable_fast) {
                int handled = handle_new_index_metamethod_local(vm, t, table, key, value, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) table_set(&t->table, n_key, value);
            } else {
                table_set(&t->table, n_key, value);
            }
        }
        push(vm, value);
        maybe_collect_garbage(vm);
        return 1;
    }

    return vm_handle_set_table_inner(vm, frame, ip, chunk, opcode_offset, table, key, value, 0);
}

int vm_handle_op_set_field_const(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 2;
    uint8_t key_index = *(*ip)++;
    Value key = chunk->constants.values[key_index];
    Value value = pop(vm);
    Value table = pop(vm);
    return vm_handle_set_table_inner(vm, frame, ip, chunk, opcode_offset, table, key, value, 0);
}

int vm_handle_op_set_field_const_keep(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 2;
    uint8_t key_index = *(*ip)++;
    Value key = chunk->constants.values[key_index];
    Value value = pop(vm);
    Value table = pop(vm);
    return vm_handle_set_table_inner(vm, frame, ip, chunk, opcode_offset, table, key, value, 1);
}

int vm_handle_op_delete_table(VM* vm, CallFrame** frame, uint8_t** ip) {
    Chunk* chunk = &(*frame)->closure->function->chunk;
    int opcode_offset = (int)(*ip - chunk->code) - 1;
    Value key = pop(vm);
    Value table = pop(vm);

    if (!IS_TABLE(table)) {
        vm_runtime_error(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        ObjString* key_str = AS_STRING(key);
        if (opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->delete_table_ic_tables != NULL &&
            chunk->delete_table_ic_versions != NULL &&
            chunk->delete_table_ic_tables[opcode_offset] == t &&
            chunk->delete_table_ic_keys[opcode_offset] == key_str &&
            chunk->delete_table_ic_slots != NULL &&
            chunk->delete_table_ic_versions[opcode_offset] == t->table.shape_version) {
            int slot = chunk->delete_table_ic_slots[opcode_offset];
            if (table_delete_at_slot(&t->table, slot, key_str)) {
                chunk->delete_table_ic_versions[opcode_offset] = t->table.shape_version;
                return 1;
            }
        }
        if (opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->delete_table_ic_slots != NULL &&
            chunk->delete_table_ic_keys != NULL &&
            chunk->delete_table_ic_keys[opcode_offset] == key_str) {
            int slot = chunk->delete_table_ic_slots[opcode_offset];
            if (table_delete_at_slot(&t->table, slot, key_str)) {
                return 1;
            }
        }
        int slot = -1;
        Value dummy;
        if (!table_get_with_slot(&t->table, key_str, &dummy, &slot)) {
            vm_runtime_error(vm, "Key not found.");
            return 0;
        }
        if (!table_delete_at_slot(&t->table, slot, key_str)) {
            vm_runtime_error(vm, "Key not found.");
            return 0;
        }
        if (opcode_offset >= 0 && opcode_offset < chunk->capacity &&
            chunk->delete_table_ic_tables != NULL) {
            chunk->delete_table_ic_tables[opcode_offset] = t;
            chunk->delete_table_ic_keys[opcode_offset] = key_str;
            if (chunk->delete_table_ic_slots != NULL) {
                chunk->delete_table_ic_slots[opcode_offset] = slot;
            }
            chunk->delete_table_ic_versions[opcode_offset] = t->table.shape_version;
        }
        return 1;
    }

    if (IS_NUMBER(key)) {
        double num_key = AS_NUMBER(key);
        int idx = (int)num_key;
        if (num_key == (double)idx) {
            if (idx < 0) {
                idx = t->table.array_max + idx + 1;
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
