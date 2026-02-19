#include <stdio.h>

#include "../lib/libs.h"
#include "ops_table.h"

static int isSelfCallableLocal(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->isSelf;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->isSelf;
    return 0;
}

static Value maybeBindSelfLocal(Value receiver, Value result) {
    if (IS_BOUND_METHOD(result)) return result;
    if (IS_TABLE(receiver) && AS_TABLE(receiver)->isModule) {
        return result;
    }
    if (isSelfCallableLocal(result)) {
        return OBJ_VAL(newBoundMethod(receiver, AS_OBJ(result)));
    }
    return result;
}

static int handleIndexMetamethodLocal(
    VM* vm, ObjTable* t, Value tableVal, Value key, Value* result, CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value idxVal = NIL_VAL;
    if (!tableGet(&t->metatable->table, vm->mm_index, &idxVal)) return 0;

    if (IS_CLOSURE(idxVal) || IS_NATIVE(idxVal)) {
        push(vm, idxVal);
        push(vm, tableVal);
        push(vm, key);
        if (!callValue(vm, idxVal, 2, frame, ip)) return -1;
        *result = pop(vm);
        return 1;
    }
    if (IS_TABLE(idxVal)) {
        *result = NIL_VAL;
        if (IS_STRING(key)) {
            tableGet(&AS_TABLE(idxVal)->table, AS_STRING(key), result);
        } else if (IS_NUMBER(key)) {
            ObjString* nKey = numberKeyString(AS_NUMBER(key));
            tableGet(&AS_TABLE(idxVal)->table, nKey, result);
        }
        return 1;
    }
    return 0;
}

static int appendToTableLocal(ObjTable* table, Value value) {
    int index = table->table.arrayMax + 1;
    if (!tableSetArray(&table->table, index, value)) {
        ObjString* key = numberKeyString((double)index);
        tableSet(&table->table, key, value);
    }
    return index;
}

int vmHandleOpAppend(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value b = pop(vm);
    Value a = pop(vm);

    Value method = getMetamethod(vm, a, "__append");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!callValue(vm, method, 2, frame, ip)) return 0;
        return 1;
    }

    if (IS_TABLE(a)) {
        int index = appendToTableLocal(AS_TABLE(a), b);
        push(vm, NUMBER_VAL((double)index));
        return 1;
    }

    method = getMetamethod(vm, b, "__append");
    if (!IS_NIL(method)) {
        push(vm, method);
        push(vm, a);
        push(vm, b);
        (*frame)->ip = *ip;
        if (!callValue(vm, method, 2, frame, ip)) return 0;
        return 1;
    }

    vmRuntimeError(vm, "Left operand must be a table or define __append.");
    return 0;
}

int vmHandleOpGetTable(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value key = pop(vm);
    Value table = pop(vm);
    Value result = NIL_VAL;

    if (IS_TABLE(table)) {
        ObjTable* t = AS_TABLE(table);
        if (IS_STRING(key)) {
            if (tableGet(&t->table, AS_STRING(key), &result)) {
                result = maybeBindSelfLocal(table, result);
            } else if (t->metatable) {
                int handled = handleIndexMetamethodLocal(vm, t, table, key, &result, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) result = NIL_VAL;
                result = maybeBindSelfLocal(table, result);
            }
        } else if (IS_NUMBER(key)) {
            double numKey = AS_NUMBER(key);
            int idx = (int)numKey;
            if (numKey == (double)idx) {
                if (idx < 0) {
                    int len = 0;
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                            len = i - 1;
                            break;
                        }
                    }
                    idx = len + idx + 1;
                }
                if (tableGetArray(&t->table, idx, &result)) {
                    result = maybeBindSelfLocal(table, result);
                    push(vm, result);
                    maybeCollectGarbage(vm);
                    return 1;
                }
            }

            ObjString* nKey = numberKeyString(numKey);
            if (tableGet(&t->table, nKey, &result)) {
                result = maybeBindSelfLocal(table, result);
                push(vm, result);
            } else if (t->metatable) {
                int handled = handleIndexMetamethodLocal(vm, t, table, key, &result, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) result = NIL_VAL;
                result = maybeBindSelfLocal(table, result);
                push(vm, result);
            } else {
                push(vm, NIL_VAL);
            }
            maybeCollectGarbage(vm);
            return 1;
        }
    } else if (IS_USERDATA(table)) {
        ObjUserdata* udata = AS_USERDATA(table);
        if (udata->metatable) {
            Value idx = NIL_VAL;
            ObjString* idxName = vm->mm_index;
            if (tableGet(&udata->metatable->table, idxName, &idx)) {
                if (IS_CLOSURE(idx) || IS_NATIVE(idx)) {
                    push(vm, idx);
                    push(vm, table);
                    push(vm, key);
                    if (!callValue(vm, idx, 2, frame, ip)) return 0;
                    result = pop(vm);
                } else if (IS_TABLE(idx)) {
                    if (IS_STRING(key)) {
                        tableGet(&AS_TABLE(idx)->table, AS_STRING(key), &result);
                    }
                }
                result = maybeBindSelfLocal(table, result);
            }
        }
    } else if (IS_STRING(table)) {
        if (IS_STRING(key)) {
            Value stringModule = NIL_VAL;
            ObjString* stringName = copyString("string", 6);
            push(vm, OBJ_VAL(stringName));
            if ((!tableGet(&vm->globals, stringName, &stringModule) || !IS_TABLE(stringModule)) &&
                loadNativeModule(vm, "string")) {
                stringModule = peek(vm, 0);
                pop(vm);
            }
            if (IS_TABLE(stringModule)) {
                if (tableGet(&AS_TABLE(stringModule)->table, AS_STRING(key), &result)) {
                    result = maybeBindSelfLocal(table, result);
                } else {
                    result = NIL_VAL;
                }
            }
            pop(vm);
        } else if (IS_NUMBER(key)) {
            double numKey = AS_NUMBER(key);
            int idx = (int)numKey;
            if (numKey == (double)idx) {
                ObjString* s = AS_STRING(table);
                if (idx < 0) idx = s->length + idx + 1;
                if (idx >= 1 && idx <= s->length) {
                    char buf[2];
                    buf[0] = s->chars[idx - 1];
                    buf[1] = '\0';
                    push(vm, OBJ_VAL(copyString(buf, 1)));
                    maybeCollectGarbage(vm);
                    return 1;
                }
            }
            push(vm, NIL_VAL);
            maybeCollectGarbage(vm);
            return 1;
        }
    } else {
        if (IS_OBJ(table)) {
            printf("DEBUG: Attempt to index non-table object type: %d\n", OBJ_TYPE(table));
        } else {
            printf("DEBUG: Attempt to index non-table value type: %d\n", table.type);
        }
        vmRuntimeError(vm, "Attempt to index non-table.");
        return 0;
    }
    push(vm, result);
    maybeCollectGarbage(vm);
    return 1;
}

static int handleNewIndexMetamethodLocal(
    VM* vm, ObjTable* t, Value tableVal, Value key, Value value, CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value ni = NIL_VAL;
    if (!tableGet(&t->metatable->table, vm->mm_newindex, &ni)) return 0;

    if (IS_CLOSURE(ni) || IS_NATIVE(ni)) {
        push(vm, ni);
        push(vm, tableVal);
        push(vm, key);
        push(vm, value);
        if (!callValue(vm, ni, 3, frame, ip)) return -1;
        return 1;
    }
    if (IS_TABLE(ni)) {
        if (IS_STRING(key)) {
            tableSet(&AS_TABLE(ni)->table, AS_STRING(key), value);
        } else if (IS_NUMBER(key)) {
            ObjString* nKey = numberKeyString(AS_NUMBER(key));
            tableSet(&AS_TABLE(ni)->table, nKey, value);
        }
        return 1;
    }
    return 0;
}

int vmHandleOpSetTable(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value value = pop(vm);
    Value key = pop(vm);
    Value table = pop(vm);

    if (!IS_TABLE(table)) {
        vmRuntimeError(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        Value dummy;
        if (tableGet(&t->table, AS_STRING(key), &dummy)) {
            tableSet(&t->table, AS_STRING(key), value);
        } else if (t->metatable) {
            int handled = handleNewIndexMetamethodLocal(vm, t, table, key, value, frame, ip);
            if (handled < 0) return 0;
            if (handled == 0) tableSet(&t->table, AS_STRING(key), value);
        } else {
            tableSet(&t->table, AS_STRING(key), value);
        }
    } else if (IS_NUMBER(key)) {
        double numKey = AS_NUMBER(key);
        int idx = (int)numKey;
        int isArray = 0;
        if (numKey == (double)idx) {
            if (idx < 0) {
                int len = 0;
                for (int i = 1; ; i++) {
                    Value val;
                    if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                        len = i - 1;
                        break;
                    }
                }
                idx = len + idx + 1;
            }
            if (tableSetArray(&t->table, idx, value)) {
                isArray = 1;
            }
        }

        if (!isArray) {
            ObjString* nKey = numberKeyString(numKey);
            Value dummy;
            if (tableGet(&t->table, nKey, &dummy)) {
                tableSet(&t->table, nKey, value);
            } else if (t->metatable) {
                int handled = handleNewIndexMetamethodLocal(vm, t, table, key, value, frame, ip);
                if (handled < 0) return 0;
                if (handled == 0) tableSet(&t->table, nKey, value);
            } else {
                tableSet(&t->table, nKey, value);
            }
        }
    }

    push(vm, value);
    maybeCollectGarbage(vm);
    return 1;
}

int vmHandleOpDeleteTable(VM* vm) {
    Value key = pop(vm);
    Value table = pop(vm);

    if (!IS_TABLE(table)) {
        vmRuntimeError(vm, "Attempt to index non-table.");
        return 0;
    }
    ObjTable* t = AS_TABLE(table);

    if (IS_STRING(key)) {
        if (!tableDelete(&t->table, AS_STRING(key))) {
            vmRuntimeError(vm, "Key not found.");
            return 0;
        }
        return 1;
    }

    if (IS_NUMBER(key)) {
        double numKey = AS_NUMBER(key);
        int idx = (int)numKey;
        if (numKey == (double)idx) {
            if (idx < 0) {
                int len = 0;
                for (int i = 1; ; i++) {
                    Value val;
                    if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                        len = i - 1;
                        break;
                    }
                }
                idx = len + idx + 1;
            }
            if (idx >= 1 && idx <= t->table.arrayCapacity) {
                if (!IS_NIL(t->table.array[idx - 1])) {
                    tableSetArray(&t->table, idx, NIL_VAL);
                    return 1;
                }
            }
        }

        ObjString* nKey = numberKeyString(numKey);
        if (!tableDelete(&t->table, nKey)) {
            vmRuntimeError(vm, "Key not found.");
            return 0;
        }
        return 1;
    }

    vmRuntimeError(vm, "Invalid key type for deletion.");
    return 0;
}
