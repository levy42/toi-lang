#include <glob.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int parse_flags(Value v, int* out) {
    *out = 0;
    if (IS_NIL(v)) return 1;
    if (!IS_STRING(v)) return 0;

    ObjString* s = AS_STRING(v);
    for (int i = 0; i < s->length; i++) {
        char ch = s->chars[i];
        if (ch == 'n') *out |= GLOB_NOSORT;
        else if (ch == 'e') *out |= GLOB_NOESCAPE;
        else if (ch == 'm') *out |= GLOB_MARK;
        else if (ch == 'd') *out |= GLOB_NOCHECK;
        else return 0;
    }
    return 1;
}

// glob.match(pattern, flags?) -> table
static int glob_match(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    if (arg_count >= 2 && !IS_STRING(args[1]) && !IS_NIL(args[1])) {
        vm_runtime_error(vm, "Argument 2 must be a string.");
        return 0;
    }

    int flags = 0;
    Value flags_val = (arg_count >= 2) ? args[1] : NIL_VAL;
    if (!parse_flags(flags_val, &flags)) {
        vm_runtime_error(vm, "glob flags must be string containing [n,e,m,d].");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    glob_t g;
    memset(&g, 0, sizeof(g));

    int rc = glob(pattern->chars, flags, NULL, &g);
    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    if (rc == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            ObjString* s = copy_string(g.gl_pathv[i], (int)strlen(g.gl_pathv[i]));
            table_set_array(&out->table, (int)i + 1, OBJ_VAL(s));
        }
    } else if (rc != GLOB_NOMATCH) {
        globfree(&g);
        vm_runtime_error(vm, "glob failed.");
        return 0;
    }

    globfree(&g);
    RETURN_OBJ(out);
}

void register_glob(VM* vm) {
    const NativeReg funcs[] = {
        {"match", glob_match},
        {NULL, NULL}
    };
    register_module(vm, "glob", funcs);
    pop(vm);
}

