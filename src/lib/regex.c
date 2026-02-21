#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_init(StrBuf* sb, size_t cap) {
    sb->data = (char*)malloc(cap);
    if (sb->data == NULL) return 0;
    sb->len = 0;
    sb->cap = cap;
    return 1;
}

static void sb_free(StrBuf* sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_ensure(StrBuf* sb, size_t extra) {
    if (sb->len + extra <= sb->cap) return 1;
    size_t new_cap = sb->cap == 0 ? 64 : sb->cap;
    while (sb->len + extra > new_cap) new_cap *= 2;
    char* n = (char*)realloc(sb->data, new_cap);
    if (n == NULL) return 0;
    sb->data = n;
    sb->cap = new_cap;
    return 1;
}

static int sb_append(StrBuf* sb, const char* s, size_t n) {
    if (!sb_ensure(sb, n)) return 0;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    return 1;
}

static int parse_flags(Value v, int* cflags) {
    *cflags = REG_EXTENDED;
    if (IS_NIL(v)) return 1;
    if (!IS_STRING(v)) return 0;
    ObjString* f = AS_STRING(v);
    for (int i = 0; i < f->length; i++) {
        char ch = f->chars[i];
        if (ch == 'i') *cflags |= REG_ICASE;
        else if (ch == 'n') *cflags |= REG_NEWLINE;
        else if (ch == 'm') *cflags &= ~REG_NEWLINE;
        else if (ch == 'x') {}
        else return 0;
    }
    return 1;
}

static int compile_or_error(VM* vm, regex_t* re, ObjString* pattern, Value flags_val) {
    int cflags = 0;
    if (!parse_flags(flags_val, &cflags)) {
        vm_runtime_error(vm, "regex flags must be string containing [i,n,m,x].");
        return 0;
    }

    int rc = regcomp(re, pattern->chars, cflags);
    if (rc != 0) {
        char err[256];
        regerror(rc, re, err, sizeof(err));
        vm_runtime_error(vm, "regex compile error: %s", err);
        return 0;
    }
    return 1;
}

// regex.match(pattern, text, flags?) -> bool
static int regex_match(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    if (arg_count >= 3 && !IS_STRING(args[2]) && !IS_NIL(args[2])) {
        vm_runtime_error(vm, "Argument 3 must be a string.");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    ObjString* text = GET_STRING(1);
    Value flags_val = (arg_count >= 3) ? args[2] : NIL_VAL;

    regex_t re;
    if (!compile_or_error(vm, &re, pattern, flags_val)) return 0;

    regmatch_t m;
    int ok = (regexec(&re, text->chars, 1, &m, 0) == 0) &&
             m.rm_so == 0 &&
             m.rm_eo == text->length;

    regfree(&re);
    RETURN_BOOL(ok);
}

// regex.search(pattern, text, flags?) -> table|nil
static int regex_search(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    if (arg_count >= 3 && !IS_STRING(args[2]) && !IS_NIL(args[2])) {
        vm_runtime_error(vm, "Argument 3 must be a string.");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    ObjString* text = GET_STRING(1);
    Value flags_val = (arg_count >= 3) ? args[2] : NIL_VAL;

    regex_t re;
    if (!compile_or_error(vm, &re, pattern, flags_val)) return 0;

    size_t nmatch = re.re_nsub + 1;
    regmatch_t* pm = (regmatch_t*)malloc(sizeof(regmatch_t) * nmatch);
    if (pm == NULL) {
        regfree(&re);
        vm_runtime_error(vm, "regex.search out of memory.");
        return 0;
    }

    int rc = regexec(&re, text->chars, nmatch, pm, 0);
    if (rc != 0) {
        free(pm);
        regfree(&re);
        RETURN_NIL;
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    int s = (int)pm[0].rm_so + 1;
    int e = (int)pm[0].rm_eo; // inclusive in 1-based terms
    table_set(&out->table, copy_string("start", 5), NUMBER_VAL((double)s));
    table_set(&out->table, copy_string("end", 3), NUMBER_VAL((double)e));

    int len = (int)(pm[0].rm_eo - pm[0].rm_so);
    ObjString* whole = copy_string(text->chars + pm[0].rm_so, len);
    table_set(&out->table, copy_string("match", 5), OBJ_VAL(whole));

    ObjTable* groups = new_table();
    push(vm, OBJ_VAL(groups));
    for (size_t i = 1; i < nmatch; i++) {
        if (pm[i].rm_so < 0 || pm[i].rm_eo < 0) {
            table_set_array(&groups->table, (int)i, NIL_VAL);
            continue;
        }
        int glen = (int)(pm[i].rm_eo - pm[i].rm_so);
        ObjString* g = copy_string(text->chars + pm[i].rm_so, glen);
        table_set_array(&groups->table, (int)i, OBJ_VAL(g));
    }
    table_set(&out->table, copy_string("groups", 6), OBJ_VAL(groups));

    free(pm);
    regfree(&re);
    pop(vm); // groups
    RETURN_OBJ(out);
}

// regex.replace(pattern, text, repl, count?, flags?) -> string
static int regex_replace(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    ASSERT_STRING(2);

    int count = 0;
    if (arg_count >= 4) {
        ASSERT_NUMBER(3);
        count = (int)GET_NUMBER(3);
        if (count < 0) count = 0;
    }
    if (arg_count >= 5 && !IS_STRING(args[4]) && !IS_NIL(args[4])) {
        vm_runtime_error(vm, "Argument 5 must be a string.");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    ObjString* text = GET_STRING(1);
    ObjString* repl = GET_STRING(2);
    Value flags_val = (arg_count >= 5) ? args[4] : NIL_VAL;

    regex_t re;
    if (!compile_or_error(vm, &re, pattern, flags_val)) return 0;

    StrBuf out;
    if (!sb_init(&out, (size_t)text->length + 16)) {
        regfree(&re);
        vm_runtime_error(vm, "regex.replace out of memory.");
        return 0;
    }

    const char* src = text->chars;
    size_t src_len = (size_t)text->length;
    size_t pos = 0;
    int replaced = 0;

    while (pos <= src_len) {
        regmatch_t m;
        int rc = regexec(&re, src + pos, 1, &m, 0);
        if (rc != 0 || m.rm_so < 0) break;

        size_t ms = pos + (size_t)m.rm_so;
        size_t me = pos + (size_t)m.rm_eo;
        if (!sb_append(&out, src + pos, ms - pos) ||
            !sb_append(&out, repl->chars, (size_t)repl->length)) {
            sb_free(&out);
            regfree(&re);
            vm_runtime_error(vm, "regex.replace out of memory.");
            return 0;
        }
        replaced++;
        pos = me;

        if (count > 0 && replaced >= count) break;
        if (m.rm_so == m.rm_eo && pos < src_len) {
            if (!sb_append(&out, src + pos, 1)) {
                sb_free(&out);
                regfree(&re);
                vm_runtime_error(vm, "regex.replace out of memory.");
                return 0;
            }
            pos++;
        }
    }

    if (pos < src_len && !sb_append(&out, src + pos, src_len - pos)) {
        sb_free(&out);
        regfree(&re);
        vm_runtime_error(vm, "regex.replace out of memory.");
        return 0;
    }

    regfree(&re);
    ObjString* result = copy_string(out.data, (int)out.len);
    sb_free(&out);
    RETURN_OBJ(result);
}

// regex.split(pattern, text, maxsplit?, flags?) -> table
static int regex_split(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    int maxsplit = 0;
    if (arg_count >= 3) {
        ASSERT_NUMBER(2);
        maxsplit = (int)GET_NUMBER(2);
        if (maxsplit < 0) maxsplit = 0;
    }
    if (arg_count >= 4 && !IS_STRING(args[3]) && !IS_NIL(args[3])) {
        vm_runtime_error(vm, "Argument 4 must be a string.");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    ObjString* text = GET_STRING(1);
    Value flags_val = (arg_count >= 4) ? args[3] : NIL_VAL;

    regex_t re;
    if (!compile_or_error(vm, &re, pattern, flags_val)) return 0;

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    const char* src = text->chars;
    size_t src_len = (size_t)text->length;
    size_t pos = 0;
    int idx = 1;
    int splits = 0;

    while (pos <= src_len) {
        if (maxsplit > 0 && splits >= maxsplit) break;
        regmatch_t m;
        int rc = regexec(&re, src + pos, 1, &m, 0);
        if (rc != 0 || m.rm_so < 0) break;

        size_t ms = pos + (size_t)m.rm_so;
        size_t me = pos + (size_t)m.rm_eo;
        ObjString* part = copy_string(src + pos, (int)(ms - pos));
        table_set_array(&out->table, idx++, OBJ_VAL(part));
        splits++;
        pos = me;

        if (m.rm_so == m.rm_eo && pos < src_len) pos++;
    }

    ObjString* tail = copy_string(src + pos, (int)(src_len - pos));
    table_set_array(&out->table, idx, OBJ_VAL(tail));

    regfree(&re);
    RETURN_OBJ(out);
}

void register_regex(VM* vm) {
    const NativeReg regex_funcs[] = {
        {"match", regex_match},
        {"search", regex_search},
        {"replace", regex_replace},
        {"split", regex_split},
        {NULL, NULL}
    };
    register_module(vm, "regex", regex_funcs);
    pop(vm);
}

