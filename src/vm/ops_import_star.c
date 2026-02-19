#include "ops_import_star.h"

int vmHandleOpImportStar(VM* vm) {
    Value module = pop(vm);
    if (!IS_TABLE(module)) {
        vmRuntimeError(vm, "from ... import * expects module table export.");
        return 0;
    }

    ObjTable* t = AS_TABLE(module);
    for (int i = 0; i < t->table.capacity; i++) {
        Entry* entry = &t->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            tableSet(&vm->globals, entry->key, entry->value);
        }
    }
    maybeCollectGarbage(vm);
    return 1;
}
