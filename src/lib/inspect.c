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
    ObjString* keyStr = copyString(key, (int)strlen(key));
    tableSet(&table->table, keyStr, value);
}

static int inspect_signature(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);

    Value callable = args[0];
    ObjTable* sig = newTable();
    push(vm, OBJ_VAL(sig));

    if (IS_CLOSURE(callable)) {
        ObjFunction* fn = AS_CLOSURE(callable)->function;
        set_field(sig, "kind", OBJ_VAL(copyString("function", 8)));
        set_field(sig, "arity", NUMBER_VAL((double)fn->arity));
        set_field(sig, "variadic", BOOL_VAL(fn->isVariadic));
        set_field(sig, "is_self", BOOL_VAL(fn->isSelf));
        set_field(sig, "defaults_count", NUMBER_VAL((double)fn->defaultsCount));
        if (fn->name != NULL) {
            set_field(sig, "name", OBJ_VAL(fn->name));
        } else {
            set_field(sig, "name", NIL_VAL);
        }

        ObjTable* params = newTable();
        push(vm, OBJ_VAL(params));
        int positionalCount = fn->arity - (fn->isVariadic ? 1 : 0);
        int defaultStart = positionalCount - fn->defaultsCount;
        for (int i = 0; i < fn->arity; i++) {
            ObjTable* p = newTable();
            push(vm, OBJ_VAL(p));
            set_field(p, "index", NUMBER_VAL((double)(i + 1)));

            if (fn->paramNames != NULL && i < fn->paramNamesCount && fn->paramNames[i] != NULL) {
                set_field(p, "name", OBJ_VAL(fn->paramNames[i]));
            } else {
                set_field(p, "name", NIL_VAL);
            }

            uint8_t t = TYPEHINT_ANY;
            if (fn->paramTypes != NULL && i < fn->paramTypesCount) {
                t = fn->paramTypes[i];
            }
            const char* tn = typehint_name(t);
            set_field(p, "type", OBJ_VAL(copyString(tn, (int)strlen(tn))));
            int hasDefault = (i >= defaultStart && i < positionalCount);
            set_field(p, "has_default", BOOL_VAL(hasDefault));
            set_field(p, "variadic", BOOL_VAL(fn->isVariadic && i == fn->arity - 1));

            tableSetArray(&params->table, i + 1, OBJ_VAL(p));
            pop(vm);
        }

        set_field(sig, "params", OBJ_VAL(params));
        pop(vm);
    } else if (IS_NATIVE(callable)) {
        ObjNative* native = AS_NATIVE_OBJ(callable);
        set_field(sig, "kind", OBJ_VAL(copyString("native", 6)));
        set_field(sig, "arity", NIL_VAL);
        set_field(sig, "variadic", NIL_VAL);
        set_field(sig, "is_self", BOOL_VAL(native->isSelf));
        set_field(sig, "defaults_count", NIL_VAL);
        if (native->name != NULL) {
            set_field(sig, "name", OBJ_VAL(native->name));
        } else {
            set_field(sig, "name", NIL_VAL);
        }
        ObjTable* params = newTable();
        push(vm, OBJ_VAL(params));
        set_field(sig, "params", OBJ_VAL(params));
        pop(vm);
    } else {
        pop(vm);
        vmRuntimeError(vm, "inspect.signature expects function or native function.");
        return 0;
    }

    pop(vm);
    RETURN_OBJ(sig);
}

void registerInspect(VM* vm) {
    const NativeReg inspectFuncs[] = {
        {"signature", inspect_signature},
        {NULL, NULL}
    };
    registerModule(vm, "inspect", inspectFuncs);
    pop(vm); // Pop inspect module
}
