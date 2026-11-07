#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    uint32_t magic;
    regex_t re;
    int has_start_anchor;
} CompiledRegex;

#define COMPILED_REGEX_MAGIC 0x52454758u

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} StrBuf;

#define REGEX_CACHE_CAPACITY 128
#define REGEX_MATCH_STACK_SLOTS 16

typedef struct {
    regex_t re;
    char* pattern;
    size_t pattern_len;
    int cflags;
    uint64_t stamp;
    int used;
} RegexCacheEntry;

typedef struct {
    RegexCacheEntry entries[REGEX_CACHE_CAPACITY];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    int initialized;
} RegexCache;

static RegexCache g_regex_cache;
static ObjString* g_key_start = NULL;
static ObjString* g_key_end = NULL;
static ObjString* g_key_match = NULL;
static ObjString* g_key_groups = NULL;

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

static int compile_with_cflags_or_error(VM* vm, regex_t* re, ObjString* pattern, int cflags) {
    int rc = regcomp(re, pattern->chars, cflags);
    if (rc != 0) {
        char err[256];
        regerror(rc, re, err, sizeof(err));
        vm_runtime_error(vm, "regex compile error: %s", err);
        return 0;
    }
    return 1;
}

static int compile_or_error(VM* vm, regex_t* re, ObjString* pattern, Value flags_val) {
    int cflags = 0;
    if (!parse_flags(flags_val, &cflags)) {
        vm_runtime_error(vm, "regex flags must be string containing [i,n,m,x].");
        return 0;
    }
    return compile_with_cflags_or_error(vm, re, pattern, cflags);
}

static void regex_cache_clear_entry(RegexCacheEntry* entry) {
    if (!entry->used) return;
    regfree(&entry->re);
    free(entry->pattern);
    entry->pattern = NULL;
    entry->pattern_len = 0;
    entry->stamp = 0;
    entry->used = 0;
}

static void regex_cache_reset(void) {
    for (int i = 0; i < REGEX_CACHE_CAPACITY; i++) {
        regex_cache_clear_entry(&g_regex_cache.entries[i]);
        g_regex_cache.entries[i].cflags = 0;
    }
    g_regex_cache.clock = 0;
}

static void regex_cache_guard_finalizer(void* ptr) {
    regex_cache_reset();
    free(ptr);
}

static void regex_cache_ensure_init(void) {
    if (g_regex_cache.initialized) return;
    memset(&g_regex_cache, 0, sizeof(g_regex_cache));
    g_regex_cache.initialized = 1;
}

static RegexCacheEntry* regex_cache_find(ObjString* pattern, int cflags) {
    for (int i = 0; i < REGEX_CACHE_CAPACITY; i++) {
        RegexCacheEntry* entry = &g_regex_cache.entries[i];
        if (!entry->used) continue;
        if (entry->cflags != cflags) continue;
        if (entry->pattern_len != (size_t)pattern->length) continue;
        if (memcmp(entry->pattern, pattern->chars, entry->pattern_len) != 0) continue;
        return entry;
    }
    return NULL;
}

static RegexCacheEntry* regex_cache_pick_victim(void) {
    int victim = -1;
    uint64_t oldest = 0;
    for (int i = 0; i < REGEX_CACHE_CAPACITY; i++) {
        RegexCacheEntry* entry = &g_regex_cache.entries[i];
        if (!entry->used) return entry;
        if (victim < 0 || entry->stamp < oldest) {
            victim = i;
            oldest = entry->stamp;
        }
    }
    return &g_regex_cache.entries[victim];
}

static int regex_cache_get(VM* vm, ObjString* pattern, Value flags_val, regex_t** out_re) {
    regex_cache_ensure_init();

    int cflags = 0;
    if (!parse_flags(flags_val, &cflags)) {
        vm_runtime_error(vm, "regex flags must be string containing [i,n,m,x].");
        return 0;
    }

    RegexCacheEntry* hit = regex_cache_find(pattern, cflags);
    if (hit != NULL) {
        g_regex_cache.hits++;
        hit->stamp = ++g_regex_cache.clock;
        *out_re = &hit->re;
        return 1;
    }
    g_regex_cache.misses++;

    RegexCacheEntry* entry = regex_cache_pick_victim();
    if (entry->used) {
        g_regex_cache.evictions++;
    }
    regex_cache_clear_entry(entry);

    if (!compile_with_cflags_or_error(vm, &entry->re, pattern, cflags)) {
        return 0;
    }

    entry->pattern = (char*)malloc((size_t)pattern->length + 1);
    if (entry->pattern == NULL) {
        regfree(&entry->re);
        vm_runtime_error(vm, "regex cache out of memory.");
        return 0;
    }

    memcpy(entry->pattern, pattern->chars, (size_t)pattern->length);
    entry->pattern[(size_t)pattern->length] = '\0';
    entry->pattern_len = (size_t)pattern->length;
    entry->cflags = cflags;
    entry->used = 1;
    entry->stamp = ++g_regex_cache.clock;

    *out_re = &entry->re;
    return 1;
}

static regmatch_t* regex_alloc_match_buffer(VM* vm, size_t nmatch, regmatch_t* stack_buf) {
    if (nmatch <= REGEX_MATCH_STACK_SLOTS) {
        return stack_buf;
    }
    regmatch_t* heap = (regmatch_t*)malloc(sizeof(regmatch_t) * nmatch);
    if (heap == NULL) {
        vm_runtime_error(vm, "regex match buffer out of memory.");
        return NULL;
    }
    return heap;
}

static void regex_free_match_buffer(regmatch_t* pm, regmatch_t* stack_buf) {
    if (pm != stack_buf) {
        free(pm);
    }
}

static ObjTable* regex_lookup_metatable(VM* vm, const char* key, int key_len) {
    Value regex_module = NIL_VAL;
    ObjString* module_name = copy_string("regex", 5);
    if (!table_get(&vm->modules, module_name, &regex_module) || !IS_TABLE(regex_module)) {
        if (!table_get(&vm->globals, module_name, &regex_module) || !IS_TABLE(regex_module)) {
            return NULL;
        }
    }

    ObjString* key_str = copy_string(key, key_len);
    Value mt = NIL_VAL;
    if (!table_get(&AS_TABLE(regex_module)->table, key_str, &mt) || !IS_TABLE(mt)) {
        return NULL;
    }
    return AS_TABLE(mt);
}

static ObjTable* build_match_result(VM* vm, ObjString* text, regmatch_t* pm, size_t nmatch, size_t base_pos) {
    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    int s = (int)(base_pos + (size_t)pm[0].rm_so) + 1;
    int e = (int)(base_pos + (size_t)pm[0].rm_eo); // inclusive in 1-based terms
    table_set(&out->table, g_key_start, NUMBER_VAL((double)s));
    table_set(&out->table, g_key_end, NUMBER_VAL((double)e));

    int len = (int)(pm[0].rm_eo - pm[0].rm_so);
    ObjString* whole = copy_string(text->chars + base_pos + (size_t)pm[0].rm_so, len);
    table_set(&out->table, g_key_match, OBJ_VAL(whole));

    ObjTable* groups = new_table();
    push(vm, OBJ_VAL(groups));
    for (size_t i = 1; i < nmatch; i++) {
        if (pm[i].rm_so < 0 || pm[i].rm_eo < 0) {
            table_set_array(&groups->table, (int)i, NIL_VAL);
            continue;
        }
        int glen = (int)(pm[i].rm_eo - pm[i].rm_so);
        ObjString* g = copy_string(text->chars + base_pos + (size_t)pm[i].rm_so, glen);
        table_set_array(&groups->table, (int)i, OBJ_VAL(g));
    }
    table_set(&out->table, g_key_groups, OBJ_VAL(groups));

    pop(vm); // groups
    pop(vm); // out
    return out;
}

static void compiled_regex_finalizer(void* ptr) {
    CompiledRegex* cr = (CompiledRegex*)ptr;
    if (cr == NULL) return;
    if (cr->magic == COMPILED_REGEX_MAGIC) {
        regfree(&cr->re);
        cr->magic = 0;
    }
    free(cr);
}

static CompiledRegex* compiled_regex_from_userdata(VM* vm, ObjUserdata* udata) {
    CompiledRegex* cr = (CompiledRegex*)udata->data;
    if (cr == NULL || cr->magic != COMPILED_REGEX_MAGIC) {
        vm_runtime_error(vm, "Invalid compiled regex.");
        return NULL;
    }
    return cr;
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

    regex_t* re = NULL;
    if (!regex_cache_get(vm, pattern, flags_val, &re)) return 0;

    regmatch_t m;
    int ok = (regexec(re, text->chars, 1, &m, 0) == 0) &&
             m.rm_so == 0 &&
             m.rm_eo == text->length;

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

    regex_t* re = NULL;
    if (!regex_cache_get(vm, pattern, flags_val, &re)) return 0;

    size_t nmatch = re->re_nsub + 1;
    regmatch_t stack_buf[REGEX_MATCH_STACK_SLOTS];
    regmatch_t* pm = regex_alloc_match_buffer(vm, nmatch, stack_buf);
    if (pm == NULL) return 0;

    if (regexec(re, text->chars, nmatch, pm, 0) != 0) {
        regex_free_match_buffer(pm, stack_buf);
        RETURN_NIL;
    }
    ObjTable* out = build_match_result(vm, text, pm, nmatch, 0);

    regex_free_match_buffer(pm, stack_buf);
    RETURN_OBJ(out);
}

static int regex_replace_with_re(VM* vm, regex_t* re, ObjString* text, ObjString* repl,
                                 int count, int has_start_anchor) {
#ifndef REG_STARTEND
    (void)has_start_anchor;
#endif
    StrBuf out;
    if (!sb_init(&out, (size_t)text->length + 16)) {
        vm_runtime_error(vm, "regex.replace out of memory.");
        return 0;
    }

    const char* src = text->chars;
    size_t src_len = (size_t)text->length;
    size_t pos = 0;
    int replaced = 0;

    while (pos <= src_len) {
        regmatch_t m;
#ifdef REG_STARTEND
        int use_startend = !has_start_anchor;
        if (use_startend) {
            m.rm_so = (regoff_t)pos;
            m.rm_eo = (regoff_t)src_len;
        }
        int rc = regexec(re, use_startend ? src : src + pos, 1, &m,
                         use_startend ? REG_STARTEND : 0);
#else
        int rc = regexec(re, src + pos, 1, &m, 0);
#endif
        if (rc != 0 || m.rm_so < 0) break;

#ifdef REG_STARTEND
        size_t ms = use_startend ? (size_t)m.rm_so : pos + (size_t)m.rm_so;
        size_t me = use_startend ? (size_t)m.rm_eo : pos + (size_t)m.rm_eo;
#else
        size_t ms = pos + (size_t)m.rm_so;
        size_t me = pos + (size_t)m.rm_eo;
#endif
        if (!sb_append(&out, src + pos, ms - pos) ||
            !sb_append(&out, repl->chars, (size_t)repl->length)) {
            sb_free(&out);
            vm_runtime_error(vm, "regex.replace out of memory.");
            return 0;
        }
        replaced++;
        pos = me;

        if (count > 0 && replaced >= count) break;
        if (m.rm_so == m.rm_eo && pos < src_len) {
            if (!sb_append(&out, src + pos, 1)) {
                sb_free(&out);
                vm_runtime_error(vm, "regex.replace out of memory.");
                return 0;
            }
            pos++;
        }
    }

    if (pos < src_len && !sb_append(&out, src + pos, src_len - pos)) {
        sb_free(&out);
        vm_runtime_error(vm, "regex.replace out of memory.");
        return 0;
    }

    if (!sb_ensure(&out, 1)) {
        sb_free(&out);
        vm_runtime_error(vm, "regex.replace out of memory.");
        return 0;
    }
    out.data[out.len] = '\0';
    RETURN_OBJ(take_string(out.data, (int)out.len));
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

    regex_t* re = NULL;
    if (!regex_cache_get(vm, GET_STRING(0), (arg_count >= 5) ? args[4] : NIL_VAL, &re)) return 0;
    ObjString* pattern = GET_STRING(0);
    int has_start_anchor = memchr(pattern->chars, '^', (size_t)pattern->length) != NULL;
    return regex_replace_with_re(vm, re, GET_STRING(1), GET_STRING(2), count, has_start_anchor);
}

// compiled.replace(text, repl, count?) -> string
static int cregex_replace(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);
    ASSERT_STRING(2);

    int count = 0;
    if (arg_count >= 4) {
        ASSERT_NUMBER(3);
        count = (int)GET_NUMBER(3);
        if (count < 0) count = 0;
    }

    CompiledRegex* cr = compiled_regex_from_userdata(vm, GET_USERDATA(0));
    if (cr == NULL) return 0;
    return regex_replace_with_re(vm, &cr->re, GET_STRING(1), GET_STRING(2), count,
                                 cr->has_start_anchor);
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

    regex_t* re = NULL;
    if (!regex_cache_get(vm, pattern, flags_val, &re)) return 0;

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
        int rc = regexec(re, src + pos, 1, &m, 0);
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

    pop(vm); // out
    RETURN_OBJ(out);
}

// regex.finditer(pattern, text, flags?) -> table
static int regex_finditer(VM* vm, int arg_count, Value* args) {
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

    regex_t* re = NULL;
    if (!regex_cache_get(vm, pattern, flags_val, &re)) return 0;

    size_t nmatch = re->re_nsub + 1;
    regmatch_t stack_buf[REGEX_MATCH_STACK_SLOTS];
    regmatch_t* pm = regex_alloc_match_buffer(vm, nmatch, stack_buf);
    if (pm == NULL) return 0;

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    size_t pos = 0;
    size_t text_len = (size_t)text->length;
    int idx = 1;
    while (pos <= text_len) {
        int rc = regexec(re, text->chars + pos, nmatch, pm, 0);
        if (rc != 0 || pm[0].rm_so < 0) break;

        ObjTable* match = build_match_result(vm, text, pm, nmatch, pos);
        table_set_array(&out->table, idx++, OBJ_VAL(match));

        if (pm[0].rm_so == pm[0].rm_eo) {
            if (pos < text_len) pos++;
            else break;
        } else {
            pos += (size_t)pm[0].rm_eo;
        }
    }

    regex_free_match_buffer(pm, stack_buf);
    pop(vm); // out
    RETURN_OBJ(out);
}

// regex.compile(pattern, flags?) -> compiled regex userdata
static int regex_compile(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    if (arg_count >= 2 && !IS_STRING(args[1]) && !IS_NIL(args[1])) {
        vm_runtime_error(vm, "Argument 2 must be a string.");
        return 0;
    }

    ObjString* pattern = GET_STRING(0);
    Value flags_val = (arg_count >= 2) ? args[1] : NIL_VAL;

    CompiledRegex* cr = (CompiledRegex*)malloc(sizeof(CompiledRegex));
    if (cr == NULL) {
        vm_runtime_error(vm, "regex.compile out of memory.");
        return 0;
    }
    cr->magic = COMPILED_REGEX_MAGIC;
    cr->has_start_anchor = memchr(pattern->chars, '^', (size_t)pattern->length) != NULL;
    if (!compile_or_error(vm, &cr->re, pattern, flags_val)) {
        free(cr);
        return 0;
    }

    ObjUserdata* udata = new_userdata_with_finalizer(cr, compiled_regex_finalizer);
    udata->metatable = regex_lookup_metatable(vm, "_compiled_mt", 12);
    RETURN_OBJ(udata);
}

// compiled.match(text) -> bool
static int cregex_match(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    CompiledRegex* cr = compiled_regex_from_userdata(vm, GET_USERDATA(0));
    if (cr == NULL) return 0;
    ObjString* text = GET_STRING(1);

    regmatch_t m;
    int ok = (regexec(&cr->re, text->chars, 1, &m, 0) == 0) &&
             m.rm_so == 0 &&
             m.rm_eo == text->length;
    RETURN_BOOL(ok);
}

// compiled.search(text) -> table|nil
static int cregex_search(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    CompiledRegex* cr = compiled_regex_from_userdata(vm, GET_USERDATA(0));
    if (cr == NULL) return 0;
    ObjString* text = GET_STRING(1);

    size_t nmatch = cr->re.re_nsub + 1;
    regmatch_t stack_buf[REGEX_MATCH_STACK_SLOTS];
    regmatch_t* pm = regex_alloc_match_buffer(vm, nmatch, stack_buf);
    if (pm == NULL) return 0;

    if (regexec(&cr->re, text->chars, nmatch, pm, 0) != 0) {
        regex_free_match_buffer(pm, stack_buf);
        RETURN_NIL;
    }

    ObjTable* out = build_match_result(vm, text, pm, nmatch, 0);
    regex_free_match_buffer(pm, stack_buf);
    RETURN_OBJ(out);
}

// compiled.finditer(text) -> table
static int cregex_finditer(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    CompiledRegex* cr = compiled_regex_from_userdata(vm, GET_USERDATA(0));
    if (cr == NULL) return 0;
    ObjString* text = GET_STRING(1);

    size_t nmatch = cr->re.re_nsub + 1;
    regmatch_t stack_buf[REGEX_MATCH_STACK_SLOTS];
    regmatch_t* pm = regex_alloc_match_buffer(vm, nmatch, stack_buf);
    if (pm == NULL) return 0;

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    size_t pos = 0;
    size_t text_len = (size_t)text->length;
    int idx = 1;
    while (pos <= text_len) {
        int rc = regexec(&cr->re, text->chars + pos, nmatch, pm, 0);
        if (rc != 0 || pm[0].rm_so < 0) break;

        ObjTable* match = build_match_result(vm, text, pm, nmatch, pos);
        table_set_array(&out->table, idx++, OBJ_VAL(match));

        if (pm[0].rm_so == pm[0].rm_eo) {
            if (pos < text_len) pos++;
            else break;
        } else {
            pos += (size_t)pm[0].rm_eo;
        }
    }

    regex_free_match_buffer(pm, stack_buf);
    pop(vm); // out
    RETURN_OBJ(out);
}

// regex.cache_stats() -> table
static int regex_cache_stats(VM* vm, int arg_count, Value* args) {
    (void)args;
    ASSERT_ARGC_EQ(0);
    regex_cache_ensure_init();

    int size = 0;
    for (int i = 0; i < REGEX_CACHE_CAPACITY; i++) {
        if (g_regex_cache.entries[i].used) size++;
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));
    table_set(&out->table, copy_string("size", 4), NUMBER_VAL((double)size));
    table_set(&out->table, copy_string("capacity", 8), NUMBER_VAL((double)REGEX_CACHE_CAPACITY));
    table_set(&out->table, copy_string("hits", 4), NUMBER_VAL((double)g_regex_cache.hits));
    table_set(&out->table, copy_string("misses", 6), NUMBER_VAL((double)g_regex_cache.misses));
    table_set(&out->table, copy_string("evictions", 9), NUMBER_VAL((double)g_regex_cache.evictions));
    pop(vm);
    RETURN_OBJ(out);
}

void register_regex(VM* vm) {
    regex_cache_ensure_init();
    regex_cache_reset();

    const NativeReg regex_funcs[] = {
        {"match", regex_match},
        {"search", regex_search},
        {"replace", regex_replace},
        {"split", regex_split},
        {"finditer", regex_finditer},
        {"compile", regex_compile},
        {"cache_stats", regex_cache_stats},
        {NULL, NULL}
    };
    register_module(vm, "regex", regex_funcs);

    ObjTable* regex_module = AS_TABLE(peek(vm, 0));

    if (g_key_start == NULL) g_key_start = copy_string("start", 5);
    if (g_key_end == NULL) g_key_end = copy_string("end", 3);
    if (g_key_match == NULL) g_key_match = copy_string("match", 5);
    if (g_key_groups == NULL) g_key_groups = copy_string("groups", 6);

    // Keep cached key strings strongly reachable via module table values.
    ObjString* cache_name_key = copy_string("_cache_key_start", 16);
    push(vm, OBJ_VAL(cache_name_key));
    push(vm, OBJ_VAL(g_key_start));
    table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    cache_name_key = copy_string("_cache_key_end", 14);
    push(vm, OBJ_VAL(cache_name_key));
    push(vm, OBJ_VAL(g_key_end));
    table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    cache_name_key = copy_string("_cache_key_match", 16);
    push(vm, OBJ_VAL(cache_name_key));
    push(vm, OBJ_VAL(g_key_match));
    table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    cache_name_key = copy_string("_cache_key_groups", 17);
    push(vm, OBJ_VAL(cache_name_key));
    push(vm, OBJ_VAL(g_key_groups));
    table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    void* cache_guard = malloc(1);
    if (cache_guard != NULL) {
        ObjUserdata* cache_guard_udata = new_userdata_with_finalizer(cache_guard, regex_cache_guard_finalizer);
        ObjString* cache_guard_key = copy_string("_cache_guard", 12);
        push(vm, OBJ_VAL(cache_guard_key));
        push(vm, OBJ_VAL(cache_guard_udata));
        table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjTable* compiled_mt = new_table();
    push(vm, OBJ_VAL(compiled_mt));

    const NativeReg compiled_methods[] = {
        {"match", cregex_match},
        {"search", cregex_search},
        {"finditer", cregex_finditer},
        {"replace", cregex_replace},
        {NULL, NULL}
    };

    for (int i = 0; compiled_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(compiled_methods[i].name, (int)strlen(compiled_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        ObjNative* method = new_native(compiled_methods[i].function, name_str);
        method->is_self = 1;
        push(vm, OBJ_VAL(method));
        table_set(&compiled_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjString* index_name = copy_string("__index", 7);
    push(vm, OBJ_VAL(index_name));
    push(vm, OBJ_VAL(compiled_mt));
    table_set(&compiled_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("regex.compiled", 14)));
    table_set(&compiled_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("_compiled_mt", 12)));
    push(vm, OBJ_VAL(compiled_mt));
    table_set(&regex_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // compiled_mt

    pop(vm);
}
