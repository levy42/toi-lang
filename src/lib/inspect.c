#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static const char* typehint_name(uint8_t type) {
    switch (type) {
        case TYPEHINT_INT: return "int";
        case TYPEHINT_FLOAT: return "float";
        case TYPEHINT_BOOL: return "bool";
        case TYPEHINT_STR: return "str";
        case TYPEHINT_TABLE: return "table";
        case TYPEHINT_ANY:
        default:
            return "any";
    }
}

static void set_field(ObjTable* table, const char* key, Value value) {
    ObjString* key_str = copy_string(key, (int)strlen(key));
    table_set(&table->table, key_str, value);
}

static int inspect_signature(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);

    Value callable = args[0];
    ObjTable* sig = new_table();
    push(vm, OBJ_VAL(sig));

    if (IS_CLOSURE(callable)) {
        ObjFunction* fn = AS_CLOSURE(callable)->function;
        set_field(sig, "kind", OBJ_VAL(copy_string("function", 8)));
        set_field(sig, "arity", NUMBER_VAL((double)fn->arity));
        set_field(sig, "variadic", BOOL_VAL(fn->is_variadic));
        set_field(sig, "is_self", BOOL_VAL(fn->is_self));
        set_field(sig, "defaults_count", NUMBER_VAL((double)fn->defaults_count));
        if (fn->name != NULL) {
            set_field(sig, "name", OBJ_VAL(fn->name));
        } else {
            set_field(sig, "name", NIL_VAL);
        }

        ObjTable* params = new_table();
        push(vm, OBJ_VAL(params));
        int positional_count = fn->arity - (fn->is_variadic ? 1 : 0);
        int default_start = positional_count - fn->defaults_count;
        for (int i = 0; i < fn->arity; i++) {
            ObjTable* p = new_table();
            push(vm, OBJ_VAL(p));
            set_field(p, "index", NUMBER_VAL((double)(i + 1)));

            if (fn->param_names != NULL && i < fn->param_names_count && fn->param_names[i] != NULL) {
                set_field(p, "name", OBJ_VAL(fn->param_names[i]));
            } else {
                set_field(p, "name", NIL_VAL);
            }

            uint8_t t = TYPEHINT_ANY;
            if (fn->param_types != NULL && i < fn->param_types_count) {
                t = fn->param_types[i];
            }
            const char* tn = typehint_name(t);
            set_field(p, "type", OBJ_VAL(copy_string(tn, (int)strlen(tn))));
            int has_default = (i >= default_start && i < positional_count);
            set_field(p, "has_default", BOOL_VAL(has_default));
            set_field(p, "variadic", BOOL_VAL(fn->is_variadic && i == fn->arity - 1));

            table_set_array(&params->table, i + 1, OBJ_VAL(p));
            pop(vm);
        }

        set_field(sig, "params", OBJ_VAL(params));
        pop(vm);
    } else if (IS_NATIVE(callable)) {
        ObjNative* native = AS_NATIVE_OBJ(callable);
        set_field(sig, "kind", OBJ_VAL(copy_string("native", 6)));
        set_field(sig, "arity", NIL_VAL);
        set_field(sig, "variadic", NIL_VAL);
        set_field(sig, "is_self", BOOL_VAL(native->is_self));
        set_field(sig, "defaults_count", NIL_VAL);
        if (native->name != NULL) {
            set_field(sig, "name", OBJ_VAL(native->name));
        } else {
            set_field(sig, "name", NIL_VAL);
        }
        ObjTable* params = new_table();
        push(vm, OBJ_VAL(params));
        set_field(sig, "params", OBJ_VAL(params));
        pop(vm);
    } else {
        pop(vm);
        vm_runtime_error(vm, "inspect.signature expects function or native function.");
        return 0;
    }

    pop(vm);
    RETURN_OBJ(sig);
}

void register_inspect(VM* vm) {
    const NativeReg inspect_funcs[] = {
        {"signature", inspect_signature},
        {NULL, NULL}
    };
    register_module(vm, "inspect", inspect_funcs);
    pop(vm); // Pop inspect module
}
