#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "libs.h"
#include "string_case.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    uint32_t magic;
    int length;
    char* chars;
} MutableString;

#define MUTABLE_STRING_MAGIC 0x4d535452u

static void mutable_string_finalizer(void* ptr) {
    MutableString* ms = (MutableString*)ptr;
    if (ms == NULL) return;
    free(ms->chars);
    ms->chars = NULL;
    free(ms);
}

static ObjTable* string_lookup_metatable(VM* vm, const char* key, int key_len) {
    Value string_module = NIL_VAL;
    if (!table_get(&vm->modules, vm->str_module_name, &string_module) || !IS_TABLE(string_module)) {
        if (!table_get(&vm->globals, vm->str_module_name, &string_module) || !IS_TABLE(string_module)) {
            return NULL;
        }
    }

    ObjString* key_str = copy_string(key, key_len);
    Value mt = NIL_VAL;
    if (!table_get(&AS_TABLE(string_module)->table, key_str, &mt) || !IS_TABLE(mt)) {
        return NULL;
    }
    return AS_TABLE(mt);
}

static void set_string_fast_kind(ObjTable* string_module, const char* name, NativeFastKind kind) {
    Value fn = NIL_VAL;
    ObjString* key = copy_string(name, (int)strlen(name));
    if (table_get(&string_module->table, key, &fn) && IS_NATIVE(fn)) {
        AS_NATIVE_OBJ(fn)->fast_kind = (uint8_t)kind;
    }
}

static MutableString* mutable_string_from_userdata(VM* vm, ObjUserdata* udata) {
    MutableString* ms = (MutableString*)udata->data;
    if (ms == NULL || ms->magic != MUTABLE_STRING_MAGIC) {
        vm_runtime_error(vm, "Invalid mutable string.");
        return NULL;
    }
    return ms;
}

static int string_mutable(VM* vm, int arg_count, Value* args) {
    if (arg_count > 1) {
        vm_runtime_error(vm, "string.mutable() expects at most 1 argument.");
        return 0;
    }

    const char* src = "";
    int len = 0;
    if (arg_count == 1) {
        if (!IS_STRING(args[0])) {
            vm_runtime_error(vm, "string.mutable() expects string.");
            return 0;
        }
        ObjString* str = GET_STRING(0);
        src = str->chars;
        len = str->length;
    }

    MutableString* ms = (MutableString*)malloc(sizeof(MutableString));
    if (ms == NULL) {
        vm_runtime_error(vm, "string.mutable(): out of memory.");
        return 0;
    }
    ms->chars = (char*)malloc((size_t)len + 1);
    if (ms->chars == NULL) {
        free(ms);
        vm_runtime_error(vm, "string.mutable(): out of memory.");
        return 0;
    }
    if (len > 0) memcpy(ms->chars, src, (size_t)len);
    ms->chars[len] = '\0';
    ms->length = len;
    ms->magic = MUTABLE_STRING_MAGIC;

    ObjUserdata* udata = new_userdata_with_finalizer(ms, mutable_string_finalizer);
    udata->metatable = string_lookup_metatable(vm, "_mutable_mt", 11);
    RETURN_OBJ(udata);
}

static int mutable_toupper(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    MutableString* ms = mutable_string_from_userdata(vm, udata);
    if (ms == NULL) return 0;

    for (int i = 0; i < ms->length; i++) {
        unsigned char c = (unsigned char)ms->chars[i];
        ms->chars[i] = (char)toupper((int)c);
    }
    RETURN_VAL(args[0]);
}

static int mutable_tolower(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    MutableString* ms = mutable_string_from_userdata(vm, udata);
    if (ms == NULL) return 0;

    for (int i = 0; i < ms->length; i++) {
        unsigned char c = (unsigned char)ms->chars[i];
        ms->chars[i] = (char)tolower((int)c);
    }
    RETURN_VAL(args[0]);
}

static int mutable_value(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    MutableString* ms = mutable_string_from_userdata(vm, udata);
    if (ms == NULL) return 0;

    RETURN_STRING(ms->chars, ms->length);
}

static int mutable_len(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    MutableString* ms = mutable_string_from_userdata(vm, udata);
    if (ms == NULL) return 0;

    RETURN_NUMBER(ms->length);
}

static int string_len(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    RETURN_NUMBER(GET_STRING(0)->length);
}

static int string_sub(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_NUMBER(1);

    ObjString* str = GET_STRING(0);
    int start = (int)GET_NUMBER(1);
    int end = str->length;

    if (arg_count >= 3) { ASSERT_NUMBER(2); end = (int)GET_NUMBER(2); }

    // Adjust 1-based indexing to 0-based
    start--;

    // Handle negative indices
    if (start < 0) start = str->length + start + 1;
    if (end < 0) end = str->length + end + 1;

    // Clamp
    if (start < 0) start = 0;
    if (end > str->length) end = str->length;

    int len = end - start;
    if (len <= 0) {
        RETURN_STRING("", 0);
    }
    if (start == 0 && end == str->length) {
        RETURN_OBJ(str);
    }

    char* sub = (char*)malloc((size_t)len + 1);
    if (sub == NULL) {
        vm_runtime_error(vm, "string.sub(): out of memory.");
        return 0;
    }
    memcpy(sub, str->chars + start, len);
    sub[len] = '\0';

    RETURN_OBJ(take_string(sub, len));
}

static inline char ascii_lower_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
}

static inline char ascii_upper_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : (char)c;
}

static int ascii_upper_simd_copy(const char* src, char* dst, int len) {
    int changed = 0;
    int i = 0;
#if defined(__ARM_NEON)
    const uint8x16_t va = vdupq_n_u8((uint8_t)'a');
    const uint8x16_t vz = vdupq_n_u8((uint8_t)'z');
    const uint8x16_t vdelta = vdupq_n_u8((uint8_t)('a' - 'A'));
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(src + i));
        uint8x16_t ge = vcgeq_u8(v, va);
        uint8x16_t le = vcleq_u8(v, vz);
        uint8x16_t mask = vandq_u8(ge, le);
        uint8x16_t delta = vandq_u8(mask, vdelta);
        uint8x16_t out = vsubq_u8(v, delta);
        vst1q_u8((uint8_t*)(dst + i), out);
        if (vmaxvq_u8(mask) != 0) changed = 1;
    }
#elif defined(__SSE2__)
    const __m128i va = _mm_set1_epi8('a' - 1);
    const __m128i vz = _mm_set1_epi8('z' + 1);
    const __m128i vdelta = _mm_set1_epi8((char)('a' - 'A'));
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i ge = _mm_cmpgt_epi8(v, va);
        __m128i lt = _mm_cmplt_epi8(v, vz);
        __m128i mask = _mm_and_si128(ge, lt);
        __m128i delta = _mm_and_si128(mask, vdelta);
        __m128i out = _mm_sub_epi8(v, delta);
        _mm_storeu_si128((__m128i*)(dst + i), out);
        if (_mm_movemask_epi8(mask) != 0) changed = 1;
    }
#endif
    for (; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        char uppered = ascii_upper_char(c);
        dst[i] = uppered;
        if (uppered != src[i]) changed = 1;
    }
    return changed;
}

static int ascii_lower_simd_copy(const char* src, char* dst, int len) {
    int changed = 0;
    int i = 0;
#if defined(__ARM_NEON)
    const uint8x16_t va = vdupq_n_u8((uint8_t)'A');
    const uint8x16_t vz = vdupq_n_u8((uint8_t)'Z');
    const uint8x16_t vdelta = vdupq_n_u8((uint8_t)('a' - 'A'));
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(src + i));
        uint8x16_t ge = vcgeq_u8(v, va);
        uint8x16_t le = vcleq_u8(v, vz);
        uint8x16_t mask = vandq_u8(ge, le);
        uint8x16_t delta = vandq_u8(mask, vdelta);
        uint8x16_t out = vaddq_u8(v, delta);
        vst1q_u8((uint8_t*)(dst + i), out);
        if (vmaxvq_u8(mask) != 0) changed = 1;
    }
#elif defined(__SSE2__)
    const __m128i va = _mm_set1_epi8('A' - 1);
    const __m128i vz = _mm_set1_epi8('Z' + 1);
    const __m128i vdelta = _mm_set1_epi8((char)('a' - 'A'));
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i ge = _mm_cmpgt_epi8(v, va);
        __m128i lt = _mm_cmplt_epi8(v, vz);
        __m128i mask = _mm_and_si128(ge, lt);
        __m128i delta = _mm_and_si128(mask, vdelta);
        __m128i out = _mm_add_epi8(v, delta);
        _mm_storeu_si128((__m128i*)(dst + i), out);
        if (_mm_movemask_epi8(mask) != 0) changed = 1;
    }
#endif
    for (; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        char lowered = ascii_lower_char(c);
        dst[i] = lowered;
        if (lowered != src[i]) changed = 1;
    }
    return changed;
}

static void replace_char_all_simd_copy(const char* src, char* dst, int len, char old_c, char repl_c) {
    int i = 0;
#if defined(__ARM_NEON)
    uint8x16_t vold = vdupq_n_u8((uint8_t)old_c);
    uint8x16_t vrepl = vdupq_n_u8((uint8_t)repl_c);
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(src + i));
        uint8x16_t mask = vceqq_u8(v, vold);
        uint8x16_t out = vbslq_u8(mask, vrepl, v);
        vst1q_u8((uint8_t*)(dst + i), out);
    }
#elif defined(__SSE2__)
    __m128i vold = _mm_set1_epi8(old_c);
    __m128i vrepl = _mm_set1_epi8(repl_c);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i mask = _mm_cmpeq_epi8(v, vold);
        __m128i out = _mm_or_si128(_mm_and_si128(mask, vrepl), _mm_andnot_si128(mask, v));
        _mm_storeu_si128((__m128i*)(dst + i), out);
    }
#endif
    for (; i < len; i++) {
        char ch = src[i];
        dst[i] = (ch == old_c) ? repl_c : ch;
    }
}

int string_case_transform(ObjString* str, int to_upper, Value* result) {
    if (str == NULL || result == NULL) return 0;
    const char* chars = str->chars;
    int len = str->length;
    if (len >= 64) {
        int all_ascii = 1;
        for (int i = 0; i < len; i++) {
            if (((unsigned char)chars[i]) & 0x80u) { all_ascii = 0; break; }
        }
        if (all_ascii) {
            char* out = (char*)malloc((size_t)len + 1);
            if (out == NULL) return 0;
            int changed = to_upper ? ascii_upper_simd_copy(chars, out, len) : ascii_lower_simd_copy(chars, out, len);
            if (!changed) {
                free(out); *result = OBJ_VAL(str); return 1;
            }
            out[len] = '\0';
            *result = OBJ_VAL(take_string_unhashed(out, len)); return 1;
        }
    }
    char* buf = NULL;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)chars[i];
        char converted = c < 128 ? (to_upper ? ascii_upper_char(c) : ascii_lower_char(c)) :
                                  (char)(to_upper ? toupper((int)c) : tolower((int)c));
        if (buf != NULL) { buf[i] = converted; continue; }
        if (converted != chars[i]) {
            buf = (char*)malloc((size_t)len + 1);
            if (buf == NULL) return 0;
            if (i > 0) memcpy(buf, chars, (size_t)i);
            buf[i] = converted;
        }
    }
    if (buf == NULL) { *result = OBJ_VAL(str); return 1; }
    buf[len] = '\0';
    *result = OBJ_VAL(take_string_unhashed(buf, len)); return 1;
}

static int string_lower(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    Value out;
    if (!string_case_transform(GET_STRING(0), 0, &out)) {
        vm_runtime_error(vm, "string.lower(): out of memory."); return 0;
    }
    RETURN_VAL(out);
}

static int string_upper(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    Value out;
    if (!string_case_transform(GET_STRING(0), 1, &out)) {
        vm_runtime_error(vm, "string.upper(): out of memory."); return 0;
    }
    RETURN_VAL(out);
}

static int string_char(VM* vm, int arg_count, Value* args) {
    int len = arg_count;
    char* buf = (char*)malloc(len + 1);

    for (int i = 0; i < len; i++) {
        ASSERT_NUMBER(i);
        buf[i] = (char)GET_NUMBER(i);
    }
    buf[len] = '\0';

    RETURN_OBJ(take_string(buf, len));
}

static int string_byte(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    int index = 1;
    if (arg_count >= 2) { ASSERT_NUMBER(1); index = (int)GET_NUMBER(1); }

    index--; // 1-based
    if (index < 0 || index >= str->length) {
        RETURN_NIL;
    }

    RETURN_NUMBER((double)(unsigned char)str->chars[index]);
}

static const char* string_find_bytes(const char* hay, int hay_len, const char* needle, int needle_len) {
    if (needle_len == 0) return hay;
    if (needle_len > hay_len) return NULL;
    if (needle_len == 1) {
        return (const char*)memchr(hay, (unsigned char)needle[0], (size_t)hay_len);
    }

    const unsigned char first = (unsigned char)needle[0];
    int last = hay_len - needle_len;
    for (int i = 0; i <= last; i++) {
        if ((unsigned char)hay[i] != first) continue;
        if (memcmp(hay + i + 1, needle + 1, (size_t)(needle_len - 1)) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static int string_find(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    ObjString* str = GET_STRING(0);
    ObjString* pattern = GET_STRING(1);
    int start = 1;
    if (arg_count >= 3) { ASSERT_NUMBER(2); start = (int)GET_NUMBER(2); }

    start--; // 1-based to 0-based
    if (start < 0) start = 0;
    if (start >= str->length) { RETURN_NIL; }
    if (pattern->length == 0) {
        int pos = start + 1;
        push(vm, NUMBER_VAL(pos));
        push(vm, NUMBER_VAL(pos - 1));
        return 2;
    }

    int hay_len = str->length - start;
    const char* found = string_find_bytes(str->chars + start, hay_len, pattern->chars, pattern->length);
    if (found == NULL) { RETURN_NIL; }

    int pos = (int)(found - str->chars) + 1; // Back to 1-based
    push(vm, NUMBER_VAL(pos));
    push(vm, NUMBER_VAL(pos + pattern->length - 1));
    return 2;
}

static int string_trim(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    char ch = ' ';
    if (arg_count >= 2) {
        ASSERT_STRING(1);
        ObjString* chs = GET_STRING(1);
        if (chs->length > 0) {
            ch = chs->chars[0];
        }
    }
    const char* s = str->chars;
    int len = str->length;

    // Trim leading whitespace
    int start = 0;
    while (start < len && (s[start] == ch || s[start] == '\t' ||
           s[start] == '\n' || s[start] == '\r')) {
        start++;
    }

    // Trim trailing whitespace
    int end = len;
    while (end > start && (s[end-1] == ch || s[end-1] == '\t' ||
           s[end-1] == '\n' || s[end-1] == '\r')) {
        end--;
    }

    int new_len = end - start;
    if (new_len <= 0) {
        RETURN_STRING("", 0);
    }

    char* buf = (char*)malloc(new_len + 1);
    memcpy(buf, s + start, new_len);
    buf[new_len] = '\0';
    RETURN_OBJ(take_string(buf, new_len));
}

static int string_ltrim(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* s = str->chars;
    int len = str->length;

    int start = 0;
    while (start < len && (s[start] == ' ' || s[start] == '\t' ||
           s[start] == '\n' || s[start] == '\r')) {
        start++;
    }

    if (start == 0) {
        RETURN_OBJ(str);
    }
    if (start >= len) {
        RETURN_STRING("", 0);
    }

    int new_len = len - start;
    char* buf = (char*)malloc((size_t)new_len + 1);
    if (buf == NULL) {
        vm_runtime_error(vm, "string.ltrim(): out of memory.");
        return 0;
    }
    memcpy(buf, s + start, (size_t)new_len);
    buf[new_len] = '\0';
    RETURN_OBJ(take_string(buf, new_len));
}

static int string_rtrim(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* s = str->chars;
    int len = str->length;

    int end = len;
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
           s[end - 1] == '\n' || s[end - 1] == '\r')) {
        end--;
    }

    if (end == len) {
        RETURN_OBJ(str);
    }
    if (end <= 0) {
        RETURN_STRING("", 0);
    }

    char* buf = (char*)malloc((size_t)end + 1);
    if (buf == NULL) {
        vm_runtime_error(vm, "string.rtrim(): out of memory.");
        return 0;
    }
    memcpy(buf, s, (size_t)end);
    buf[end] = '\0';
    RETURN_OBJ(take_string(buf, end));
}

static int string_is_digit(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    if (str->length <= 0) {
        RETURN_FALSE;
    }

    for (int i = 0; i < str->length; i++) {
        unsigned char c = (unsigned char)str->chars[i];
        if (isdigit((int)c) == 0) {
            RETURN_FALSE;
        }
    }
    RETURN_TRUE;
}

static int string_is_alpha(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    if (str->length <= 0) {
        RETURN_FALSE;
    }

    for (int i = 0; i < str->length; i++) {
        unsigned char c = (unsigned char)str->chars[i];
        if (isalpha((int)c) == 0) {
            RETURN_FALSE;
        }
    }
    RETURN_TRUE;
}

static int string_is_alnum(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    if (str->length != 1) {
        RETURN_FALSE;
    }

    unsigned char c = (unsigned char)str->chars[0];
    RETURN_BOOL(isalnum((int)c) != 0);
}

static int string_is_space(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    if (str->length != 1) {
        RETURN_FALSE;
    }

    unsigned char c = (unsigned char)str->chars[0];
    RETURN_BOOL(isspace((int)c) != 0);
}

static int string_escape_html(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* s = str->chars;
    int len = str->length;

    int out_len = 0;
    for (int i = 0; i < len; i++) {
        switch (s[i]) {
            case '&': out_len += 5; break;   // &amp;
            case '<': out_len += 4; break;   // &lt;
            case '>': out_len += 4; break;   // &gt;
            case '"': out_len += 6; break;   // &quot;
            case '\'': out_len += 5; break;  // &#39;
            default: out_len += 1; break;
        }
    }

    if (out_len == len) {
        RETURN_OBJ(str);
    }

    char* out = (char*)malloc((size_t)out_len + 1);
    if (out == NULL) {
        vm_runtime_error(vm, "string.escape_html(): out of memory.");
        return 0;
    }

    int j = 0;
    for (int i = 0; i < len; i++) {
        switch (s[i]) {
            case '&':
                memcpy(out + j, "&amp;", 5);
                j += 5;
                break;
            case '<':
                memcpy(out + j, "&lt;", 4);
                j += 4;
                break;
            case '>':
                memcpy(out + j, "&gt;", 4);
                j += 4;
                break;
            case '"':
                memcpy(out + j, "&quot;", 6);
                j += 6;
                break;
            case '\'':
                memcpy(out + j, "&#39;", 5);
                j += 5;
                break;
            default:
                out[j++] = s[i];
                break;
        }
    }
    out[j] = '\0';

    RETURN_OBJ(take_string(out, out_len));
}

static int string_starts_with(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    ObjString* str = GET_STRING(0);
    ObjString* prefix = GET_STRING(1);

    if (prefix->length > str->length) {
        RETURN_FALSE;
    }

    RETURN_BOOL(memcmp(str->chars, prefix->chars, (size_t)prefix->length) == 0);
}

static int string_ends_with(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    ObjString* str = GET_STRING(0);
    ObjString* suffix = GET_STRING(1);

    if (suffix->length > str->length) {
        RETURN_FALSE;
    }
    if (suffix->length == 0) {
        RETURN_TRUE;
    }

    const char* start = str->chars + (str->length - suffix->length);
    RETURN_BOOL(memcmp(start, suffix->chars, (size_t)suffix->length) == 0);
}

static int string_split(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* sep = " ";
    int sep_len = 1;
    int max_splits = -1;

    if (arg_count >= 2) {
        ASSERT_STRING(1);
        sep = GET_CSTRING(1);
        sep_len = GET_STRING(1)->length;
    }
    if (arg_count >= 3) {
        max_splits = GET_NUMBER(2);
    }

    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result)); // Protect from GC

    const char* s = str->chars;
    const char* end = s + str->length;
    int index = 1;

    if (sep_len == 0) {
        for (int i = 0; i < str->length; i++) {
            Value val = OBJ_VAL(copy_string(s + i, 1));
            table_set_array(&result->table, index++, val);
        }
        pop(vm);
        RETURN_OBJ(result);
    }

    bool reverse = arg_count >= 3 && max_splits < 0;
    int limit = reverse ? -max_splits : max_splits;

    if (reverse) {
        if (limit <= 0) {
            Value val = OBJ_VAL(copy_string(s, (int)(end - s)));
            table_set_array(&result->table, index++, val);
        } else {
            typedef struct {
                const char* start;
                const char* end;
            } Range;

            Range* segments = NULL;
            int segments_capacity = 0;
            int segments_count = 0;
            const char* rest_end = end;

            while (rest_end > s && segments_count < limit) {
                int remaining = (int)(rest_end - s);
                if (remaining < sep_len) {
                    break;
                }
                const char* candidate = rest_end - sep_len;
                const char* found = NULL;
                while (candidate >= s) {
                    if (memcmp(candidate, sep, (size_t)sep_len) == 0) {
                        found = candidate;
                        break;
                    }
                    candidate--;
                }
                if (found == NULL) {
                    break;
                }

                if (segments_count == segments_capacity) {
                    int new_capacity = segments_capacity == 0 ? 4 : segments_capacity * 2;
                    if (new_capacity > limit) {
                        new_capacity = limit;
                    }
                    Range* new_segments = (Range*)realloc(segments, sizeof(Range) * new_capacity);
                    if (new_segments == NULL) {
                        vm_runtime_error(vm, "string.split: out of memory");
                        free(segments);
                        return 0;
                    }
                    segments = new_segments;
                    segments_capacity = new_capacity;
                }

                segments[segments_count].start = found + sep_len;
                segments[segments_count].end = rest_end;
                segments_count++;
                rest_end = found;
            }

            Value prefix = OBJ_VAL(copy_string(s, (int)(rest_end - s)));
            table_set_array(&result->table, index++, prefix);
            for (int i = segments_count - 1; i >= 0; i--) {
                int part_len = (int)(segments[i].end - segments[i].start);
                Value part = OBJ_VAL(copy_string(segments[i].start, part_len));
                table_set_array(&result->table, index++, part);
            }

            free(segments);
        }
    } else {
        int splits_remaining = INT_MAX;
        if (arg_count >= 3 && max_splits >= 0) {
            splits_remaining = limit;
        }

        while (splits_remaining > 0) {
            const char* found = strstr(s, sep);
            if (found == NULL) {
                break;
            }
            int len = (int)(found - s);
            Value val = OBJ_VAL(copy_string(s, len));
            table_set_array(&result->table, index++, val);
            s = found + sep_len;
            splits_remaining--;
        }

        int len = (int)(end - s);
        Value val = OBJ_VAL(copy_string(s, len));
        table_set_array(&result->table, index++, val);
    }

    pop(vm); // Pop result table (will be returned)
    RETURN_OBJ(result);
}

static int string_rep(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_NUMBER(1);

    ObjString* str = GET_STRING(0);
    int n = (int)GET_NUMBER(1);

    if (n <= 0) {
        RETURN_STRING("", 0);
    }

    int new_len = str->length * n;
    char* buf = (char*)malloc(new_len + 1);

    for (int i = 0; i < n; i++) {
        memcpy(buf + i * str->length, str->chars, str->length);
    }
    buf[new_len] = '\0';

    RETURN_OBJ(take_string(buf, new_len));
}

static int string_reverse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    char* buf = (char*)malloc(str->length + 1);

    for (int i = 0; i < str->length; i++) {
        buf[i] = str->chars[str->length - 1 - i];
    }
    buf[str->length] = '\0';

    RETURN_OBJ(take_string(buf, str->length));
}

static int string_replace(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    if (arg_count > 4) {
        vm_runtime_error(vm, "Expected at most 4 arguments but got %d.", arg_count);
        return 0;
    }
    ASSERT_STRING(0);
    ASSERT_STRING(1);
    ASSERT_STRING(2);

    ObjString* str = GET_STRING(0);
    ObjString* old = GET_STRING(1);
    ObjString* repl = GET_STRING(2);
    int limit = -1;
    if (arg_count == 4) {
        ASSERT_NUMBER(3);
        limit = (int)GET_NUMBER(3);
    }

    if (limit == 0) {
        RETURN_OBJ(str);
    }

    const char* src = str->chars;
    int src_len = str->length;
    const char* old_chars = old->chars;
    int old_len = old->length;
    const char* repl_chars = repl->chars;
    int repl_len = repl->length;

    if (old_len == 0) {
        int inserts = src_len + 1;
        if (limit > 0 && limit < inserts) {
            inserts = limit;
        }

        int out_len = src_len + (inserts * repl_len);
        char* out = (char*)malloc((size_t)out_len + 1);
        if (out == NULL) {
            vm_runtime_error(vm, "string.replace(): out of memory.");
            return 0;
        }

        int w = 0;
        int i = 0;
        while (i < src_len) {
            if (inserts > 0) {
                memcpy(out + w, repl_chars, (size_t)repl_len);
                w += repl_len;
                inserts--;
            }
            out[w++] = src[i++];
        }
        if (inserts > 0) {
            memcpy(out + w, repl_chars, (size_t)repl_len);
            w += repl_len;
        }
        out[w] = '\0';
        RETURN_OBJ(take_string(out, w));
    }

    if (old_len == 1 && repl_len == 1) {
        char old_c = old_chars[0];
        char repl_c = repl_chars[0];
        if (old_c == repl_c) {
            RETURN_OBJ(str);
        }

        int first = -1;
        for (int i = 0; i < src_len; i++) {
            if (src[i] == old_c) {
                first = i;
                break;
            }
        }
        if (first < 0) {
            RETURN_OBJ(str);
        }

        char* out = (char*)malloc((size_t)src_len + 1);
        if (out == NULL) {
            vm_runtime_error(vm, "string.replace(): out of memory.");
            return 0;
        }

        if (limit < 0 && src_len >= 64) {
            replace_char_all_simd_copy(src, out, src_len, old_c, repl_c);
        } else {
            int replaced = 0;
            for (int i = 0; i < src_len; i++) {
                char ch = src[i];
                if (ch == old_c && (limit < 0 || replaced < limit)) {
                    ch = repl_c;
                    replaced++;
                }
                out[i] = ch;
            }
            out[src_len] = '\0';
            RETURN_OBJ(take_string_unhashed(out, src_len));
        }
        out[src_len] = '\0';
        RETURN_OBJ(take_string_unhashed(out, src_len));
    }

    int matches = 0;
    const char* p = src;
    int remaining = src_len;
    while ((p = string_find_bytes(p, remaining, old_chars, old_len)) != NULL) {
        matches++;
        if (limit > 0 && matches >= limit) {
            break;
        }
        int consumed = (int)((p - src) + old_len);
        p += old_len;
        remaining = src_len - consumed;
    }

    if (matches == 0) {
        RETURN_OBJ(str);
    }

    int out_len = src_len + (repl_len - old_len) * matches;
    char* out = (char*)malloc((size_t)out_len + 1);
    if (out == NULL) {
        vm_runtime_error(vm, "string.replace(): out of memory.");
        return 0;
    }

    int w = 0;
    const char* cur = src;
    int replaced = 0;
    int cur_remaining = src_len;
    while ((p = string_find_bytes(cur, cur_remaining, old_chars, old_len)) != NULL) {
        int chunk = (int)(p - cur);
        if (chunk > 0) {
            memcpy(out + w, cur, (size_t)chunk);
            w += chunk;
        }
        memcpy(out + w, repl_chars, (size_t)repl_len);
        w += repl_len;
        cur = p + old_len;
        replaced++;
        cur_remaining = src_len - (int)(cur - src);
        if (limit > 0 && replaced >= limit) {
            break;
        }
    }

    int tail = (int)(src + src_len - cur);
    if (tail > 0) {
        memcpy(out + w, cur, (size_t)tail);
        w += tail;
    }
    out[w] = '\0';
    RETURN_OBJ(take_string(out, w));
}

static int string_join(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_TABLE(1);

    ObjString* sep = GET_STRING(0);
    ObjTable* list = GET_TABLE(1);

    int total_len = 0;
    int count = 0;

    // Iterate sequential integer keys starting from 1
    int i = 1;
    Value val;

    // We use a loop that continues as long as we find values at consecutive indices
    // Or should we use table->array_capacity?
    // Arrays in this language are 1-based.
    // Let's assume standard behavior: iterate 1..N until nil.

    // Note: iterating until nil might stop early for sparse arrays.
    // But join/split usually implies dense arrays.
    // Alternatively, iterate array_capacity? No, array part might be sparse too or have holes.
    // Iterating until nil is safer for sequence.

    while (1) {
        // Try array optimization first
        if (!table_get_array(&list->table, i, &val)) {
            // Fallback to hash lookup (for numeric keys stored in hash part)
            ObjString* key = number_key_string((double)i);
            if (!table_get(&list->table, key, &val) || IS_NIL(val)) {
                // End of sequence
                break;
            }
        }

        if (IS_STRING(val)) {
            total_len += AS_STRING(val)->length;
        } else if (IS_NUMBER(val)) {
            double num = AS_NUMBER(val);
            char numbuf[32];
            int slen;
            if (num == (int)num) {
                slen = snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
            } else {
                slen = snprintf(numbuf, sizeof(numbuf), "%g", num);
            }
            total_len += slen;
        } else {
            vm_runtime_error(vm, "string.join: list contains non-string/number element");
            return 0;
        }

        count++;
        i++;
    }

    if (count == 0) {
        RETURN_STRING("", 0);
    }

    total_len += sep->length * (count - 1);
    char* buffer = (char*)malloc(total_len + 1);
    int length = 0;
    int first = 1;

    i = 1;
    while (1) {
        if (!table_get_array(&list->table, i, &val)) {
            ObjString* key = number_key_string((double)i);
            if (!table_get(&list->table, key, &val) || IS_NIL(val)) {
                break;
            }
        }

        if (!first) {
            memcpy(buffer + length, sep->chars, sep->length);
            length += sep->length;
        }

        const char* s;
        int slen;
        char numbuf[32];

        if (IS_STRING(val)) {
            s = AS_STRING(val)->chars;
            slen = AS_STRING(val)->length;
        } else if (IS_NUMBER(val)) {
            double num = AS_NUMBER(val);
            if (num == (int)num) {
                slen = snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
            } else {
                slen = snprintf(numbuf, sizeof(numbuf), "%g", num);
            }
            s = numbuf;
        } else {
            free(buffer);
            vm_runtime_error(vm, "string.join: list contains non-string/number element");
            return 0;
        }

        memcpy(buffer + length, s, slen);
        length += slen;

        first = 0;
        i++;
    }

    buffer[length] = '\0';
    ObjString* result = take_string(buffer, length);
    RETURN_OBJ(result);
}

static int ensure_result_capacity(char** result, int* capacity, int needed) {
    while (needed >= *capacity) {
        int next = (*capacity) * 2;
        char* grown = (char*)realloc(*result, (size_t)next);
        if (!grown) return 0;
        *result = grown;
        *capacity = next;
    }
    return 1;
}

static int is_printf_flag_char(char c) {
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

static int is_supported_printf_conv(char c) {
    switch (c) {
        case 's':
        case 'd': case 'i':
        case 'u': case 'x': case 'X': case 'o':
        case 'f': case 'F':
        case 'g': case 'G':
        case 'e': case 'E':
        case 'c':
            return 1;
        default:
            return 0;
    }
}

// string.format(fmt, ...) - printf-style formatting
static int string_format(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* fmt = GET_STRING(0);
    const char* f = fmt->chars;
    int flen = fmt->length;

    int capacity = flen * 2 + 128;
    char* result = (char*)malloc((size_t)capacity);
    if (!result) {
        vm_runtime_error(vm, "string.format: out of memory");
        return 0;
    }
    int rlen = 0;
    int arg_idx = 1;

    for (int i = 0; i < flen; i++) {
        if (f[i] != '%') {
            if (!ensure_result_capacity(&result, &capacity, rlen + 2)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            result[rlen++] = f[i];
            continue;
        }

        if (i + 1 >= flen) {
            if (!ensure_result_capacity(&result, &capacity, rlen + 2)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            result[rlen++] = '%';
            continue;
        }

        if (f[i + 1] == '%') {
            if (!ensure_result_capacity(&result, &capacity, rlen + 2)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            result[rlen++] = '%';
            i++;
            continue;
        }

        int spec_start = i;
        int j = i + 1;

        while (j < flen && is_printf_flag_char(f[j])) j++;
        while (j < flen && isdigit((unsigned char)f[j])) j++;
        if (j < flen && f[j] == '.') {
            j++;
            while (j < flen && isdigit((unsigned char)f[j])) j++;
        }

        if (j < flen && (f[j] == 'h' || f[j] == 'l' || f[j] == 'L' || f[j] == 'z' || f[j] == 't' || f[j] == 'j')) {
            free(result);
            vm_runtime_error(vm, "string.format: length modifiers are not supported");
            return 0;
        }

        if (j >= flen) {
            free(result);
            vm_runtime_error(vm, "string.format: incomplete format specifier");
            return 0;
        }

        char conv = f[j];
        if (!is_supported_printf_conv(conv)) {
            free(result);
            vm_runtime_error(vm, "string.format: unsupported format specifier");
            return 0;
        }

        int spec_len = j - spec_start + 1;
        if (spec_len <= 0 || spec_len >= 32) {
            free(result);
            vm_runtime_error(vm, "string.format: invalid format specifier");
            return 0;
        }

        char spec[32];
        memcpy(spec, f + spec_start, (size_t)spec_len);
        spec[spec_len] = '\0';
        i = j;

        if (arg_idx >= arg_count) {
            free(result);
            vm_runtime_error(vm, "string.format: not enough arguments");
            return 0;
        }

        Value v = args[arg_idx++];
        int written = 0;

        if (conv == 's') {
            const char* s = NULL;
            char numbuf[64];
            if (IS_STRING(v)) {
                s = AS_STRING(v)->chars;
            } else if (IS_NUMBER(v)) {
                double num = AS_NUMBER(v);
                if (num == (int)num) {
                    snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
                } else {
                    snprintf(numbuf, sizeof(numbuf), "%g", num);
                }
                s = numbuf;
            } else if (IS_NIL(v)) {
                s = "nil";
            } else if (IS_BOOL(v)) {
                s = AS_BOOL(v) ? "true" : "false";
            } else {
                s = "<value>";
            }
            written = snprintf(NULL, 0, spec, s);
            if (written < 0 || !ensure_result_capacity(&result, &capacity, rlen + written + 1)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            snprintf(result + rlen, (size_t)(capacity - rlen), spec, s);
            rlen += written;
            continue;
        }

        if (!IS_NUMBER(v)) {
            free(result);
            vm_runtime_error(vm, "string.format: numeric format expects number");
            return 0;
        }

        if (conv == 'd' || conv == 'i') {
            int n = (int)AS_NUMBER(v);
            written = snprintf(NULL, 0, spec, n);
            if (written < 0 || !ensure_result_capacity(&result, &capacity, rlen + written + 1)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            snprintf(result + rlen, (size_t)(capacity - rlen), spec, n);
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            unsigned int n = (unsigned int)AS_NUMBER(v);
            written = snprintf(NULL, 0, spec, n);
            if (written < 0 || !ensure_result_capacity(&result, &capacity, rlen + written + 1)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            snprintf(result + rlen, (size_t)(capacity - rlen), spec, n);
        } else if (conv == 'c') {
            int n = (int)AS_NUMBER(v);
            written = snprintf(NULL, 0, spec, n);
            if (written < 0 || !ensure_result_capacity(&result, &capacity, rlen + written + 1)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            snprintf(result + rlen, (size_t)(capacity - rlen), spec, n);
        } else {
            double n = AS_NUMBER(v);
            written = snprintf(NULL, 0, spec, n);
            if (written < 0 || !ensure_result_capacity(&result, &capacity, rlen + written + 1)) {
                free(result);
                vm_runtime_error(vm, "string.format: out of memory");
                return 0;
            }
            snprintf(result + rlen, (size_t)(capacity - rlen), spec, n);
        }

        rlen += written;
    }

    result[rlen] = '\0';
    RETURN_OBJ(take_string(result, rlen));
}

void register_string(VM* vm) {
    const NativeReg string_funcs[] = {
        {"len", string_len},
        {"sub", string_sub},
        {"lower", string_lower},
        {"upper", string_upper},
        {"starts_with", string_starts_with},
        {"ends_with", string_ends_with},
        {"mutable", string_mutable},
        {"char", string_char},
        {"byte", string_byte},
        {"find", string_find},
        {"trim", string_trim},
        {"ltrim", string_ltrim},
        {"rtrim", string_rtrim},
        {"is_digit", string_is_digit},
        {"is_alpha", string_is_alpha},
        {"is_alnum", string_is_alnum},
        {"is_space", string_is_space},
        {"escape_html", string_escape_html},
        {"split", string_split},
        {"replace", string_replace},
        {"join", string_join},
        {"rep", string_rep},

        {"reverse", string_reverse},
        {"format", string_format},
        {NULL, NULL}
    };
    register_module(vm, "string", string_funcs);
    ObjTable* string_module_table = AS_TABLE(peek(vm, 0));
    set_string_fast_kind(string_module_table, "upper", NATIVE_FAST_STR_UPPER);
    set_string_fast_kind(string_module_table, "lower", NATIVE_FAST_STR_LOWER);
    set_string_fast_kind(string_module_table, "find", NATIVE_FAST_STR_FIND);
    set_string_fast_kind(string_module_table, "replace", NATIVE_FAST_STR_REPLACE);
    set_string_fast_kind(string_module_table, "join", NATIVE_FAST_STR_JOIN);
    set_string_fast_kind(string_module_table, "format", NATIVE_FAST_STR_FORMAT);

    // Add __call metamethod to string module to act as str() constructor
    Value string_module = peek(vm, 0); // peek string module
    ObjTable* mt = new_table();
    push(vm, OBJ_VAL(mt)); // protect

    ObjString* call_str = copy_string("__call", 6);
    push(vm, OBJ_VAL(call_str));
    push(vm, OBJ_VAL(new_native(core_tostring, call_str)));
    table_set(&mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // native
    pop(vm); // call_str

    objtable_set_metatable(AS_TABLE(string_module), mt);

    // Alias 'str' global to 'string' module so str(x) works via __call
    ObjString* str_name = copy_string("str", 3);
    push(vm, OBJ_VAL(str_name));
    push(vm, string_module);
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // string_module
    pop(vm); // str_name

    // Mutable string userdata metatable.
    ObjTable* mutable_mt = new_table();
    push(vm, OBJ_VAL(mutable_mt));

    const NativeReg mutable_methods[] = {
        {"toupper", mutable_toupper},
        {"tolower", mutable_tolower},
        {"value", mutable_value},
        {"__str", mutable_value},
        {"len", mutable_len},
        {NULL, NULL}
    };

    for (int i = 0; mutable_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(mutable_methods[i].name, (int)strlen(mutable_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        ObjNative* method = new_native(mutable_methods[i].function, name_str);
        method->is_self = 1;
        push(vm, OBJ_VAL(method));
        table_set(&mutable_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjString* index_name = copy_string("__index", 7);
    push(vm, OBJ_VAL(index_name));
    push(vm, OBJ_VAL(mutable_mt));
    table_set(&mutable_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("string.mutable", 14)));
    table_set(&mutable_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("_mutable_mt", 11)));
    push(vm, OBJ_VAL(mutable_mt));
    table_set(&string_module_table->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // mutable_mt

    pop(vm); // pop mt
    pop(vm); // Pop string module
}
