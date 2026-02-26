#include "ops_iter.h"

static int is_callable_value_local(Value value) {
    return IS_CLOSURE(value) || IS_NATIVE(value);
}

static Value get_metamethod_cached_local(VM* vm, Value val, ObjString* name) {
    (void)vm;
    Value method = NIL_VAL;
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            table_get(&table->metatable->table, name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            table_get(&udata->metatable->table, name, &method);
        }
    }
    return method;
}

static Value get_iterator_next_function_local(VM* vm, Value iterable) {
    if (IS_THREAD(iterable)) {
        Value next = NIL_VAL;
        ObjString* name = copy_string("gen_next", 8);
        if (table_get(&vm->globals, name, &next) && is_callable_value_local(next)) {
            return next;
        }
        return NIL_VAL;
    }
    if (IS_TABLE(iterable)) {
        Value next = NIL_VAL;
        if (table_get(&AS_TABLE(iterable)->table, vm->mm_next, &next) && is_callable_value_local(next)) {
            return next;
        }
    }
    Value next = get_metamethod_cached_local(vm, iterable, vm->mm_next);
    if (is_callable_value_local(next)) return next;
    return NIL_VAL;
}

int vm_handle_op_iter_prep(VM* vm) {
    Value val = peek(vm, 0);
    Value next_method = get_iterator_next_function_local(vm, val);
    if (is_callable_value_local(next_method)) {
        pop(vm);
        push(vm, next_method);
        push(vm, val);
        push(vm, NIL_VAL);
        return 1;
    }

    if (IS_TABLE(val) || IS_STRING(val)) {
        Value next_fn = NIL_VAL;
        ObjString* name = copy_string("next", 4);
        if (!table_get(&vm->globals, name, &next_fn)) {
            vm_runtime_error(vm, "Global 'next' not found for implicit iteration.");
            return 0;
        }
        if (!IS_NATIVE(next_fn) && !IS_CLOSURE(next_fn)) {
            vm_runtime_error(vm, "Global 'next' is not a function.");
            return 0;
        }

        pop(vm);
        push(vm, next_fn);
        push(vm, val);
        push(vm, NIL_VAL);
        return 1;
    }

    vm_runtime_error(vm, "Value is not iterable.");
    return 0;
}

int vm_handle_op_iter_prep_i_pairs(VM* vm) {
    Value val = peek(vm, 0);
    if (IS_TABLE(val)) {
        Value inext_fn = NIL_VAL;
        ObjString* name = copy_string("inext", 5);
        if (!table_get(&vm->globals, name, &inext_fn)) {
            vm_runtime_error(vm, "Global 'inext' not found for implicit iteration.");
            return 0;
        }
        if (!IS_NATIVE(inext_fn) && !IS_CLOSURE(inext_fn)) {
            vm_runtime_error(vm, "Global 'inext' is not a function.");
            return 0;
        }

        pop(vm);
        push(vm, inext_fn);
        push(vm, val);
        push(vm, NUMBER_VAL(0));
    }
    return 1;
}

int vm_handle_op_range(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value end = pop(vm);
    Value start = pop(vm);
    Value range_fn = NIL_VAL;
    ObjString* name = copy_string("range", 5);
    if (!table_get(&vm->globals, name, &range_fn)) {
        vm_runtime_error(vm, "range not found.");
        return 0;
    }

    push(vm, range_fn);
    push(vm, start);
    push(vm, end);

    if (IS_NATIVE(range_fn) || IS_CLOSURE(range_fn)) {
        if (!call_value(vm, range_fn, 2, frame, ip)) return 0;
    } else {
        vm_runtime_error(vm, "Can only call functions.");
        return 0;
    }
    return 1;
}

int vm_handle_op_slice(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value step = pop(vm);
    Value end = pop(vm);
    Value start = pop(vm);
    Value obj = pop(vm);

    if (IS_NIL(step)) {
        step = NUMBER_VAL(1);
    }
    if (!IS_NUMBER(step)) {
        vm_runtime_error(vm, "slice step must be a number.");
        return 0;
    }

    double step_num = AS_NUMBER(step);
    if (step_num == 0) {
        vm_runtime_error(vm, "slice step cannot be 0.");
        return 0;
    }

    Value mm_slice = get_metamethod_cached_local(vm, obj, vm->mm_slice);
    if (!IS_NIL(mm_slice)) {
        if (!IS_CLOSURE(mm_slice) && !IS_NATIVE(mm_slice)) {
            vm_runtime_error(vm, "__slice must be a function.");
            return 0;
        }
        push(vm, mm_slice);
        push(vm, obj);
        push(vm, start);
        push(vm, end);
        push(vm, step);
        if (!call_value(vm, mm_slice, 4, frame, ip)) return 0;
        return 1;
    }

    int len = 0;
    if (IS_TABLE(obj)) {
        ObjTable* t = AS_TABLE(obj);
        for (int i = 1; ; i++) {
            Value val;
            if (!table_get_array(&t->table, i, &val) || IS_NIL(val)) {
                len = i - 1;
                break;
            }
        }
    } else if (IS_STRING(obj)) {
        len = AS_STRING(obj)->length;
    } else {
        vm_runtime_error(vm, "slice expects table or string.");
        return 0;
    }

    if (IS_NIL(start) || IS_NIL(end)) {
        if (IS_NIL(start)) {
            start = NUMBER_VAL(step_num < 0 ? (double)len : 1.0);
        }
        if (IS_NIL(end)) {
            end = NUMBER_VAL(step_num < 0 ? 1.0 : (double)len);
        }
    }
    if (!IS_NUMBER(start) || !IS_NUMBER(end)) {
        vm_runtime_error(vm, "slice start/end must be numbers.");
        return 0;
    }

    double raw_start = AS_NUMBER(start);
    double raw_end = AS_NUMBER(end);
    int start_int = (int)raw_start;
    int end_int = (int)raw_end;
    if ((double)start_int != raw_start || (double)end_int != raw_end) {
        vm_runtime_error(vm, "slice start/end must be integer for '..' syntax.");
        return 0;
    }
    if (start_int < 0) {
        start_int = len + start_int + 1;
    }
    if (end_int < 0) {
        end_int = len + end_int;
    }
    start = NUMBER_VAL((double)start_int);
    end = NUMBER_VAL((double)end_int);

    Value slice_fn = NIL_VAL;
    if (!table_get(&vm->globals, vm->slice_name, &slice_fn)) {
        vm_runtime_error(vm, "slice not found.");
        return 0;
    }

    push(vm, slice_fn);
    push(vm, obj);
    push(vm, start);
    push(vm, end);
    push(vm, step);

    if (IS_NATIVE(slice_fn) || IS_CLOSURE(slice_fn)) {
        if (!call_value(vm, slice_fn, 4, frame, ip)) return 0;
    } else {
        vm_runtime_error(vm, "Can only call functions.");
        return 0;
    }
    return 1;
}
