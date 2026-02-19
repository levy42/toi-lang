#include "ops_meta.h"

static int findPropertyLocal(VM* vm, ObjTable* table, ObjString* name, Value* result) {
    if (tableGet(&table->table, name, result)) return 1;

    ObjTable* current = table;
    int depth = 0;
    ObjString* idxName = vm->mm_index;

    while (current->metatable && depth < 10) {
        Value idxVal = NIL_VAL;
        if (tableGet(&current->metatable->table, idxName, &idxVal)) {
            if (IS_TABLE(idxVal)) {
                current = AS_TABLE(idxVal);
                if (tableGet(&current->table, name, result)) {
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

int vmHandleOpNewTable(VM* vm) {
    push(vm, OBJ_VAL(newTable()));
    maybeCollectGarbage(vm);
    return 1;
}

int vmHandleOpSetMetatable(VM* vm, CallFrame** frame, uint8_t** ip) {
    Value table = peek(vm, 0);
    Value metatable = peek(vm, 1);
    if (!IS_TABLE(table) || (!IS_TABLE(metatable) && !IS_NIL(metatable))) {
        vmRuntimeError(vm, "Invalid arguments to setmetatable.");
        return 0;
    }
    AS_TABLE(table)->metatable = IS_NIL(metatable) ? NULL : AS_TABLE(metatable);

    int constructorCalled = 0;
    if (!IS_NIL(metatable)) {
        Value initMethod = NIL_VAL;
        ObjString* newStr = vm->mm_new;

        int found = findPropertyLocal(vm, AS_TABLE(table), newStr, &initMethod);
        if (!found) {
            if (tableGet(&AS_TABLE(metatable)->table, newStr, &initMethod)) {
                found = 1;
            }
        }

        if (found && (IS_CLOSURE(initMethod) || IS_NATIVE(initMethod))) {
            pop(vm);
            pop(vm);

            push(vm, initMethod);
            push(vm, metatable);
            push(vm, table);

            constructorCalled = 1;
            if (!callValue(vm, initMethod, 2, frame, ip)) {
                return 0;
            }
        }
    }

    if (!constructorCalled) {
        pop(vm);
        pop(vm);
        push(vm, table);
    }
    return 1;
}
