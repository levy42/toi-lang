#include "ops_iter.h"

static int isCallableValueLocal(Value value) {
    return IS_CLOSURE(value) || IS_NATIVE(value);
}

static Value getMetamethodCachedLocal(VM* vm, Value val, ObjString* name) {
    (void)vm;
    Value method = NIL_VAL;
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            tableGet(&table->metatable->table, name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            tableGet(&udata->metatable->table, name, &method);
        }
    }
    return method;
}

static Value getIteratorNextFunctionLocal(VM* vm, Value iterable) {
    if (IS_TABLE(iterable)) {
        Value next = NIL_VAL;
        if (tableGet(&AS_TABLE(iterable)->table, vm->mm_next, &next) && isCallableValueLocal(next)) {
            return next;
        }
    }
    Value next = getMetamethodCachedLocal(vm, iterable, vm->mm_next);
    if (isCallableValueLocal(next)) return next;
    return NIL_VAL;
}

int vmHandleOpIterPrep(VM* vm) {
    Value val = peek(vm, 0);
    Value nextMethod = getIteratorNextFunctionLocal(vm, val);
    if (isCallableValueLocal(nextMethod)) {
        pop(vm);
        push(vm, nextMethod);
        push(vm, val);
        push(vm, NIL_VAL);
        return 1;
    }

    if (IS_TABLE(val) || IS_STRING(val)) {
        Value nextFn = NIL_VAL;
        ObjString* name = copyString("next", 4);
        if (!tableGet(&vm->globals, name, &nextFn)) {
            vmRuntimeError(vm, "Global 'next' not found for implicit iteration.");
            return 0;
        }
        if (!IS_NATIVE(nextFn) && !IS_CLOSURE(nextFn)) {
            vmRuntimeError(vm, "Global 'next' is not a function.");
            return 0;
        }

        pop(vm);
        push(vm, nextFn);
        push(vm, val);
        push(vm, NIL_VAL);
        return 1;
    }

    vmRuntimeError(vm, "Value is not iterable.");
    return 0;
}

int vmHandleOpIterPrepIPairs(VM* vm) {
    Value val = peek(vm, 0);
    if (IS_TABLE(val)) {
        Value inextFn = NIL_VAL;
        ObjString* name = copyString("inext", 5);
        if (!tableGet(&vm->globals, name, &inextFn)) {
            vmRuntimeError(vm, "Global 'inext' not found for implicit iteration.");
            return 0;
        }
        if (!IS_NATIVE(inextFn) && !IS_CLOSURE(inextFn)) {
            vmRuntimeError(vm, "Global 'inext' is not a function.");
            return 0;
        }

        pop(vm);
        push(vm, inextFn);
        push(vm, val);
        push(vm, NUMBER_VAL(0));
    }
    return 1;
}

int vmHandleOpRange(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value end = pop(vm);
    Value start = pop(vm);
    Value rangeFn = NIL_VAL;
    ObjString* name = copyString("range", 5);
    if (!tableGet(&vm->globals, name, &rangeFn)) {
        vmRuntimeError(vm, "range not found.");
        return 0;
    }

    push(vm, rangeFn);
    push(vm, start);
    push(vm, end);

    if (IS_NATIVE(rangeFn) || IS_CLOSURE(rangeFn)) {
        if (!callValue(vm, rangeFn, 2, frame, ip)) return 0;
    } else {
        vmRuntimeError(vm, "Can only call functions.");
        return 0;
    }
    return 1;
}

int vmHandleOpSlice(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value step = pop(vm);
    Value end = pop(vm);
    Value start = pop(vm);
    Value obj = pop(vm);

    if (IS_NIL(step)) {
        step = NUMBER_VAL(1);
    }
    if (!IS_NUMBER(step)) {
        vmRuntimeError(vm, "slice step must be a number.");
        return 0;
    }

    double stepNum = AS_NUMBER(step);
    if (stepNum == 0) {
        vmRuntimeError(vm, "slice step cannot be 0.");
        return 0;
    }

    if (IS_NIL(start) || IS_NIL(end)) {
        int len = 0;
        if (IS_TABLE(obj)) {
            ObjTable* t = AS_TABLE(obj);
            for (int i = 1; ; i++) {
                Value val;
                if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                    len = i - 1;
                    break;
                }
            }
        } else if (IS_STRING(obj)) {
            len = AS_STRING(obj)->length;
        } else {
            vmRuntimeError(vm, "slice expects table or string.");
            return 0;
        }
        if (IS_NIL(start)) {
            start = NUMBER_VAL(stepNum < 0 ? (double)len : 1.0);
        }
        if (IS_NIL(end)) {
            end = NUMBER_VAL(stepNum < 0 ? 1.0 : (double)len);
        }
    }
    if (!IS_NUMBER(start) || !IS_NUMBER(end)) {
        vmRuntimeError(vm, "slice start/end must be numbers.");
        return 0;
    }

    Value sliceFn = NIL_VAL;
    ObjString* name = copyString("slice", 5);
    if (!tableGet(&vm->globals, name, &sliceFn)) {
        vmRuntimeError(vm, "slice not found.");
        return 0;
    }

    push(vm, sliceFn);
    push(vm, obj);
    push(vm, start);
    push(vm, end);
    push(vm, step);

    if (IS_NATIVE(sliceFn) || IS_CLOSURE(sliceFn)) {
        if (!callValue(vm, sliceFn, 4, frame, ip)) return 0;
    } else {
        vmRuntimeError(vm, "Can only call functions.");
        return 0;
    }
    return 1;
}
