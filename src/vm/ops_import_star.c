#include "ops_import_star.h"

int vm_handle_op_import_star(VM* vm) {
    Value module = pop(vm);
    if (!IS_TABLE(module)) {
        vm_runtime_error(vm, "from ... import * expects module table export.");
        return 0;
    }

    ObjTable* t = AS_TABLE(module);
    for (int i = 0; i < t->table.capacity; i++) {
        Entry* entry = &t->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            table_set(&vm->globals, entry->key, entry->value);
        }
    }
    maybe_collect_garbage(vm);
    return 1;
}
