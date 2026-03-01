#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static ObjTable* ensure_context_locals(VM* vm) {
    ObjThread* thread = vm_current_thread(vm);
    if (thread->context_locals == NULL) {
        thread->context_locals = new_table();
    }
    return thread->context_locals;
}

static ObjTable* get_context_locals(VM* vm) {
    ObjThread* thread = vm_current_thread(vm);
    return thread->context_locals;
}

static int context_set(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ObjTable* locals = ensure_context_locals(vm);
    table_set(&locals->table, GET_STRING(0), args[1]);
    RETURN_VAL(args[1]);
}

static int context_get(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    if (arg_count > 2) {
        vm_runtime_error(vm, "context.get() expects 1 or 2 arguments.");
        return 0;
    }
    ASSERT_STRING(0);

    ObjTable* locals = get_context_locals(vm);
    Value out = NIL_VAL;
    if (locals != NULL && table_get(&locals->table, GET_STRING(0), &out)) {
        RETURN_VAL(out);
    }
    if (arg_count == 2) {
        RETURN_VAL(args[1]);
    }
    RETURN_NIL;
}

static int context_has(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjTable* locals = get_context_locals(vm);
    Value out = NIL_VAL;
    if (locals != NULL && table_get(&locals->table, GET_STRING(0), &out)) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}

static int context_unset(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjTable* locals = get_context_locals(vm);
    if (locals == NULL) RETURN_FALSE;
    RETURN_BOOL(table_delete(&locals->table, GET_STRING(0)));
}

static int context_clear(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(0);
    (void)args;
    ObjThread* thread = vm_current_thread(vm);
    if (thread->context_locals != NULL) {
        thread->context_locals = new_table();
    }
    RETURN_NIL;
}

static int context_copy(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(0);
    (void)args;

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    ObjTable* locals = get_context_locals(vm);
    if (locals != NULL) {
        table_add_all(&locals->table, &out->table);
    }

    pop(vm);
    RETURN_OBJ(out);
}

static int context_items(VM* vm, int arg_count, Value* args) {
    return context_copy(vm, arg_count, args);
}

static int context_keys(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(0);
    (void)args;

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    ObjTable* locals = get_context_locals(vm);
    if (locals != NULL) {
        int idx = 1;
        for (int i = 0; i < locals->table.capacity; i++) {
            Entry* entry = &locals->table.entries[i];
            if (entry->key == NULL || IS_NIL(entry->value)) continue;
            table_set_array(&out->table, idx, OBJ_VAL(entry->key));
            idx++;
        }
    }

    pop(vm);
    RETURN_OBJ(out);
}

void register_context(VM* vm) {
    const NativeReg funcs[] = {
        {"set", context_set},
        {"get", context_get},
        {"exists", context_has},
        {"has", context_has},
        {"unset", context_unset},
        {"delete", context_unset},
        {"clear", context_clear},
        {"copy", context_copy},
        {"items", context_items},
        {"keys", context_keys},
        {NULL, NULL}
    };
    register_module(vm, "context", funcs);
    pop(vm); // Pop context module
}
