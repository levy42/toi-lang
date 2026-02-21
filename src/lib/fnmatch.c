#include <fnmatch.h>
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
        if (ch == 'p') {
            *out |= FNM_PATHNAME;
        } else if (ch == 'd') {
            *out |= FNM_PERIOD;
        } else if (ch == 'n') {
            *out |= FNM_NOESCAPE;
#ifdef FNM_CASEFOLD
        } else if (ch == 'i') {
            *out |= FNM_CASEFOLD;
#endif
#ifdef FNM_LEADING_DIR
        } else if (ch == 'l') {
            *out |= FNM_LEADING_DIR;
#endif
        } else {
            return 0;
        }
    }
    return 1;
}

// fnmatch.match(pattern, text, flags?) -> bool
static int fnmatch_match(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    if (arg_count >= 3 && !IS_STRING(args[2]) && !IS_NIL(args[2])) {
        vm_runtime_error(vm, "Argument 3 must be a string.");
        return 0;
    }

    int flags = 0;
    Value flags_val = (arg_count >= 3) ? args[2] : NIL_VAL;
    if (!parse_flags(flags_val, &flags)) {
        vm_runtime_error(vm, "fnmatch flags must be string containing [p,d,n,i,l] (platform-dependent).");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    ObjString* text = GET_STRING(1);
    int rc = fnmatch(pattern->chars, text->chars, flags);

    RETURN_BOOL(rc == 0);
}

void register_fnmatch(VM* vm) {
    const NativeReg funcs[] = {
        {"match", fnmatch_match},
        {NULL, NULL}
    };
    register_module(vm, "fnmatch", funcs);
    pop(vm);
}

