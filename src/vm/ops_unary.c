#include "ops_unary.h"

static int isFalseyLocal(Value v) {
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.arrayCapacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

void vmHandleOpNegate(VM* vm) {
    push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
}

void vmHandleOpNot(VM* vm) {
    Value v = pop(vm);
    push(vm, BOOL_VAL(isFalseyLocal(v)));
}

int vmHandleOpLength(VM* vm) {
    Value val = pop(vm);
    if (IS_STRING(val)) {
        push(vm, NUMBER_VAL(AS_STRING(val)->length));
        return 1;
    }
    if (IS_TABLE(val)) {
        ObjTable* t = AS_TABLE(val);
        int count = t->table.count;
        for (int i = 0; i < t->table.arrayCapacity; i++) {
            if (!IS_NIL(t->table.array[i])) count++;
        }
        push(vm, NUMBER_VAL(count));
        return 1;
    }

    vmRuntimeError(vm, "Length operator (#) requires string or table.");
    return 0;
}
