#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <ctype.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifndef TOI_WASM
#include <pthread.h>
#endif

#include "common.h"
#include "jit.h"
#include "opt.h"
#include "table.h"
#include "vm.h"
#include "vm/build_string.h"
#include "vm/ops_arith.h"
#include "vm/ops_arith_const.h"
#include "vm/ops_compare.h"
#include "vm/ops_control.h"
#include "vm/ops_exception.h"
#include "vm/ops_float.h"
#include "vm/ops_has.h"
#include "vm/ops_import.h"
#include "vm/ops_import_star.h"
#include "vm/ops_iter.h"
#include "vm/ops_local_const.h"
#include "vm/ops_local_set.h"
#include "vm/ops_meta.h"
#include "vm/ops_mod.h"
#include "vm/ops_power.h"
#include "vm/ops_print.h"
#include "vm/ops_state.h"
#include "vm/ops_table.h"
#include "vm/ops_unary.h"
#include "lib/libs.h" // For register_libs
#include "object.h" // For free_object

extern struct Obj* objects;

// #define DEBUG_TABLE_KEYS

// Forward declaration for compile function
ObjFunction* compile(const char* source);
static void close_upvalues(VM* vm, Value* last);

static inline int to_int64(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

enum {
    CALL_IC_NONE = 0,
    CALL2_IC_DIRECT = 1,
    CALL2_IC_TYPED2 = 2,
    CALL2_IC_TINY_INLINE = 3,
};

static const char* fast_find_bytes(const char* hay, int hay_len, const char* needle, int needle_len) {
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

static int fast_ascii_upper_simd_copy(const char* src, char* dst, int len) {
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
        char uppered = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : (char)c;
        dst[i] = uppered;
        if (uppered != src[i]) changed = 1;
    }
    return changed;
}

static int fast_ascii_lower_simd_copy(const char* src, char* dst, int len) {
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
        char lowered = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
        dst[i] = lowered;
        if (lowered != src[i]) changed = 1;
    }
    return changed;
}

// Returns 1 when handled, 0 when not handled.
static int try_fast_bound_call1(VM* vm, ObjBoundMethod* bound, Value arg1) {
    if (bound == NULL || !IS_NATIVE(OBJ_VAL(bound->method))) return 0;
    ObjNative* native_obj = (ObjNative*)bound->method;
    if ((NativeFastKind)native_obj->fast_kind != NATIVE_FAST_STR_FIND) return 0;
    if (!IS_STRING(bound->receiver) || !IS_STRING(arg1)) return 0;

    ObjString* str = AS_STRING(bound->receiver);
    ObjString* needle = AS_STRING(arg1);
    ObjThread* thread = vm_current_thread(vm);
    // Pop [bound, arg1]
    thread->stack_top -= 2;

    if (needle->length == 0) {
        push(vm, NUMBER_VAL(1));
        push(vm, NUMBER_VAL(0));
        return 1;
    }

    const char* found = fast_find_bytes(str->chars, str->length, needle->chars, needle->length);
    if (found == NULL) {
        push(vm, NIL_VAL);
        return 1;
    }

    int pos = (int)(found - str->chars) + 1;
    push(vm, NUMBER_VAL(pos));
    push(vm, NUMBER_VAL(pos + needle->length - 1));
    return 1;
}

static void fast_replace_char_all_copy(const char* src, char* dst, int len, char old_c, char repl_c) {
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

// Returns 1 when result pushed, -1 on runtime error, 0 on invalid input.
static int push_fast_string_lower(VM* vm, Value str_val) {
    if (!IS_STRING(str_val)) return 0;
    ObjString* str = AS_STRING(str_val);
    const char* chars = str->chars;
    int len = str->length;
    if (len >= 64) {
        int all_ascii = 1;
        for (int i = 0; i < len; i++) {
            if (((unsigned char)chars[i]) & 0x80u) {
                all_ascii = 0;
                break;
            }
        }
        if (all_ascii) {
            char* out = (char*)malloc((size_t)len + 1);
            if (out == NULL) {
                vm_runtime_error(vm, "string.lower(): out of memory.");
                return -1;
            }
            int changed = fast_ascii_lower_simd_copy(chars, out, len);
            if (!changed) {
                free(out);
                push(vm, str_val);
                return 1;
            }
            out[len] = '\0';
            push(vm, OBJ_VAL(take_string_unhashed(out, len)));
            return 1;
        }
    }

    char* buf = NULL;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)chars[i];
        char lowered = c < 128
            ? ((c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c)
            : (char)tolower((int)c);
        if (buf != NULL) {
            buf[i] = lowered;
            continue;
        }
        if (lowered != chars[i]) {
            buf = (char*)malloc((size_t)len + 1);
            if (buf == NULL) {
                vm_runtime_error(vm, "string.lower(): out of memory.");
                return -1;
            }
            if (i > 0) memcpy(buf, chars, (size_t)i);
            buf[i] = lowered;
        }
    }
    if (buf == NULL) {
        push(vm, str_val);
        return 1;
    }
    buf[len] = '\0';
    push(vm, OBJ_VAL(take_string_unhashed(buf, len)));
    return 1;
}

// Returns 1 when handled, 0 when not handled, -1 on runtime error.
static int try_fast_native_call(VM* vm, ObjNative* native, int arg_count, Value* args) {
    switch ((NativeFastKind)native->fast_kind) {
        case NATIVE_FAST_NONE:
            return 0;
        case NATIVE_FAST_MATH_SIN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(sin(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_COS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(cos(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_TAN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(tan(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ASIN:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(asin(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ACOS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(acos(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ATAN:
            if (arg_count == 1 && IS_NUMBER(args[0])) {
                push(vm, NUMBER_VAL(atan(AS_NUMBER(args[0]))));
                return 1;
            }
            if (arg_count == 2 && IS_NUMBER(args[0]) && IS_NUMBER(args[1])) {
                push(vm, NUMBER_VAL(atan2(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
                return 1;
            }
            goto fast_arg_error;
        case NATIVE_FAST_MATH_SQRT:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(sqrt(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_FLOOR:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(floor(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_CEIL:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(ceil(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_ABS:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(fabs(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_EXP:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(exp(AS_NUMBER(args[0]))));
            return 1;
        case NATIVE_FAST_MATH_LOG:
            if (arg_count == 1 && IS_NUMBER(args[0])) {
                push(vm, NUMBER_VAL(log(AS_NUMBER(args[0]))));
                return 1;
            }
            if (arg_count == 2 && IS_NUMBER(args[0]) && IS_NUMBER(args[1])) {
                push(vm, NUMBER_VAL(log(AS_NUMBER(args[0])) / log(AS_NUMBER(args[1]))));
                return 1;
            }
            goto fast_arg_error;
        case NATIVE_FAST_MATH_POW:
            if (arg_count != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) goto fast_arg_error;
            push(vm, NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
            return 1;
        case NATIVE_FAST_MATH_FMOD:
            if (arg_count != 2 || !IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) goto fast_arg_error;
            push(vm, NUMBER_VAL(fmod(AS_NUMBER(args[0]), AS_NUMBER(args[1]))));
            return 1;
        case NATIVE_FAST_MATH_DEG:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(AS_NUMBER(args[0]) * (180.0 / 3.14159265358979323846)));
            return 1;
        case NATIVE_FAST_MATH_RAD:
            if (arg_count != 1 || !IS_NUMBER(args[0])) goto fast_arg_error;
            push(vm, NUMBER_VAL(AS_NUMBER(args[0]) * (3.14159265358979323846 / 180.0)));
            return 1;
        case NATIVE_FAST_MATH_RANDOM:
            if (arg_count == 0) {
                push(vm, NUMBER_VAL((double)rand() / (double)RAND_MAX));
                return 1;
            }
            if (arg_count == 1) {
                if (!IS_NUMBER(args[0])) goto fast_arg_error;
                int max = (int)AS_NUMBER(args[0]);
                if (max < 1) {
                    push(vm, NIL_VAL);
                } else {
                    push(vm, NUMBER_VAL((double)(rand() % max) + 1));
                }
                return 1;
            }
            if (arg_count == 2) {
                if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) goto fast_arg_error;
                int min = (int)AS_NUMBER(args[0]);
                int max = (int)AS_NUMBER(args[1]);
                if (min > max) {
                    push(vm, NIL_VAL);
                } else {
                    push(vm, NUMBER_VAL((double)(min + rand() % (max - min + 1))));
                }
                return 1;
            }
            // math.random returns nil when argc > 2.
            push(vm, NIL_VAL);
            return 1;
        case NATIVE_FAST_STR_UPPER: {
            if (arg_count != 1 || !IS_STRING(args[0])) goto fast_arg_error;
            ObjString* str = AS_STRING(args[0]);
            const char* chars = str->chars;
            int len = str->length;
            if (len >= 64) {
                int all_ascii = 1;
                for (int i = 0; i < len; i++) {
                    if (((unsigned char)chars[i]) & 0x80u) {
                        all_ascii = 0;
                        break;
                    }
                }
                if (all_ascii) {
                    char* out = (char*)malloc((size_t)len + 1);
                    if (out == NULL) {
                        vm_runtime_error(vm, "string.upper(): out of memory.");
                        return -1;
                    }
                    int changed = fast_ascii_upper_simd_copy(chars, out, len);
                    if (!changed) {
                        free(out);
                        push(vm, args[0]);
                        return 1;
                    }
                    out[len] = '\0';
                    push(vm, OBJ_VAL(take_string_unhashed(out, len)));
                    return 1;
                }
            }

            char* buf = NULL;
            for (int i = 0; i < len; i++) {
                unsigned char c = (unsigned char)chars[i];
                char uppered = c < 128
                    ? ((c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : (char)c)
                    : (char)toupper((int)c);
                if (buf != NULL) {
                    buf[i] = uppered;
                    continue;
                }
                if (uppered != chars[i]) {
                    buf = (char*)malloc((size_t)len + 1);
                    if (buf == NULL) {
                        vm_runtime_error(vm, "string.upper(): out of memory.");
                        return -1;
                    }
                    if (i > 0) memcpy(buf, chars, (size_t)i);
                    buf[i] = uppered;
                }
            }
            if (buf == NULL) {
                push(vm, args[0]);
                return 1;
            }
            buf[len] = '\0';
            push(vm, OBJ_VAL(take_string_unhashed(buf, len)));
            return 1;
        }
        case NATIVE_FAST_STR_LOWER: {
            if (arg_count != 1 || !IS_STRING(args[0])) goto fast_arg_error;
            return push_fast_string_lower(vm, args[0]);
        }
        case NATIVE_FAST_STR_FIND: {
            if ((arg_count != 2 && arg_count != 3) || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
                goto fast_arg_error;
            }
            if (arg_count == 3 && !IS_NUMBER(args[2])) goto fast_arg_error;

            ObjString* str = AS_STRING(args[0]);
            ObjString* needle = AS_STRING(args[1]);
            int start = 1;
            if (arg_count == 3) start = (int)AS_NUMBER(args[2]);
            start--; // 1-based to 0-based
            if (start < 0) start = 0;
            if (start >= str->length) {
                push(vm, NIL_VAL);
                return 1;
            }
            if (needle->length == 0) {
                int pos = start + 1;
                push(vm, NUMBER_VAL(pos));
                push(vm, NUMBER_VAL(pos - 1));
                return 1;
            }

            int hay_len = str->length - start;
            const char* found = fast_find_bytes(str->chars + start, hay_len, needle->chars, needle->length);
            if (found == NULL) {
                push(vm, NIL_VAL);
                return 1;
            }
            int pos = (int)(found - str->chars) + 1;
            push(vm, NUMBER_VAL(pos));
            push(vm, NUMBER_VAL(pos + needle->length - 1));
            return 1;
        }
        case NATIVE_FAST_STR_REPLACE: {
            if ((arg_count != 3 && arg_count != 4) ||
                !IS_STRING(args[0]) ||
                !IS_STRING(args[1]) ||
                !IS_STRING(args[2])) {
                goto fast_arg_error;
            }
            if (arg_count == 4 && !IS_NUMBER(args[3])) goto fast_arg_error;

            ObjString* str = AS_STRING(args[0]);
            ObjString* old = AS_STRING(args[1]);
            ObjString* repl = AS_STRING(args[2]);
            int limit = -1;
            if (arg_count == 4) {
                limit = (int)AS_NUMBER(args[3]);
            }

            if (limit == 0) {
                push(vm, args[0]);
                return 1;
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
                    return -1;
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
                push(vm, OBJ_VAL(take_string_unhashed(out, w)));
                return 1;
            }

            if (old_len == 1 && repl_len == 1) {
                char old_c = old_chars[0];
                char repl_c = repl_chars[0];
                if (old_c == repl_c) {
                    push(vm, args[0]);
                    return 1;
                }

                int first = -1;
                for (int i = 0; i < src_len; i++) {
                    if (src[i] == old_c) {
                        first = i;
                        break;
                    }
                }
                if (first < 0) {
                    push(vm, args[0]);
                    return 1;
                }

                char* out = (char*)malloc((size_t)src_len + 1);
                if (out == NULL) {
                    vm_runtime_error(vm, "string.replace(): out of memory.");
                    return -1;
                }

                if (limit < 0 && src_len >= 64) {
                    fast_replace_char_all_copy(src, out, src_len, old_c, repl_c);
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
                    push(vm, OBJ_VAL(take_string_unhashed(out, src_len)));
                    return 1;
                }
                out[src_len] = '\0';
                push(vm, OBJ_VAL(take_string_unhashed(out, src_len)));
                return 1;
            }

            int matches = 0;
            const char* p = src;
            int remaining = src_len;
            while ((p = fast_find_bytes(p, remaining, old_chars, old_len)) != NULL) {
                matches++;
                if (limit > 0 && matches >= limit) break;
                int consumed = (int)((p - src) + old_len);
                p += old_len;
                remaining = src_len - consumed;
            }

            if (matches == 0) {
                push(vm, args[0]);
                return 1;
            }

            int out_len = src_len + (repl_len - old_len) * matches;
            char* out = (char*)malloc((size_t)out_len + 1);
            if (out == NULL) {
                vm_runtime_error(vm, "string.replace(): out of memory.");
                return -1;
            }

            int w = 0;
            const char* cur = src;
            int replaced = 0;
            int cur_remaining = src_len;
            while ((p = fast_find_bytes(cur, cur_remaining, old_chars, old_len)) != NULL) {
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
            push(vm, OBJ_VAL(take_string_unhashed(out, w)));
            return 1;
        }
        case NATIVE_FAST_STR_JOIN: {
            if (arg_count != 2 || !IS_STRING(args[0]) || !IS_TABLE(args[1])) goto fast_arg_error;
            ObjString* sep = AS_STRING(args[0]);
            ObjTable* list = AS_TABLE(args[1]);
            Table* table = &list->table;

            int count = 0;
            int total_len = 0;
            Value val = NIL_VAL;

            for (int i = 1; ; i++) {
                if (!table_get_array(table, i, &val) || IS_NIL(val)) {
                    ObjString* key = number_key_string((double)i);
                    Value hv = NIL_VAL;
                    if (table_get(table, key, &hv) && !IS_NIL(hv)) goto fast_arg_error;
                    break;
                }
                if (!IS_STRING(val)) goto fast_arg_error;
                total_len += AS_STRING(val)->length;
                count++;
            }

            if (count == 0) {
                push(vm, OBJ_VAL(copy_string("", 0)));
                return 1;
            }

            total_len += sep->length * (count - 1);
            char* buffer = (char*)malloc((size_t)total_len + 1);
            if (buffer == NULL) {
                vm_runtime_error(vm, "string.join: out of memory");
                return -1;
            }

            int length = 0;
            for (int i = 1; i <= count; i++) {
                table_get_array(table, i, &val);
                ObjString* item = AS_STRING(val);
                if (i > 1) {
                    memcpy(buffer + length, sep->chars, (size_t)sep->length);
                    length += sep->length;
                }
                memcpy(buffer + length, item->chars, (size_t)item->length);
                length += item->length;
            }
            buffer[length] = '\0';
            push(vm, OBJ_VAL(take_string_unhashed(buffer, length)));
            return 1;
        }
        case NATIVE_FAST_STR_FORMAT: {
            if (arg_count < 1 || !IS_STRING(args[0])) goto fast_arg_error;
            ObjString* fmt = AS_STRING(args[0]);
            const char* f = fmt->chars;
            int flen = fmt->length;

            // Hot benchmark pattern: string.format("%s %f sec", name, elapsed)
            // Also supports precision variant: "%s %.6f sec".
            if (arg_count == 3 &&
                flen >= 9 &&
                f[0] == '%' && f[1] == 's' && f[2] == ' ' &&
                f[3] == '%' &&
                f[flen - 4] == ' ' && f[flen - 3] == 's' &&
                f[flen - 2] == 'e' && f[flen - 1] == 'c' &&
                IS_STRING(args[1]) &&
                IS_NUMBER(args[2])) {
                int spec_len = flen - 7; // from '%' at index 3 up to 'f' before " sec"
                int float_spec_ok = 0;
                if (spec_len == 2 && f[3] == '%' && f[4] == 'f') {
                    float_spec_ok = 1;
                } else if (spec_len >= 4 && f[3] == '%' && f[4] == '.' && f[3 + spec_len - 1] == 'f') {
                    int ok = 1;
                    for (int i = 5; i < 3 + spec_len - 1; i++) {
                        if (!isdigit((unsigned char)f[i])) {
                            ok = 0;
                            break;
                        }
                    }
                    float_spec_ok = ok;
                }

                if (float_spec_ok) {
                    ObjString* s = AS_STRING(args[1]);
                    double n = AS_NUMBER(args[2]);
                    char numbuf[96];
                    char spec[32];
                    if (spec_len <= 0 || spec_len >= (int)sizeof(spec)) goto fast_arg_error;
                    memcpy(spec, f + 3, (size_t)spec_len);
                    spec[spec_len] = '\0';
                    int num_len = snprintf(numbuf, sizeof(numbuf), spec, n);
                    if (num_len < 0 || num_len >= (int)sizeof(numbuf)) goto fast_arg_error;
                    int out_len = s->length + 1 + num_len + 4; // " sec"
                    char* out = (char*)malloc((size_t)out_len + 1);
                    if (out == NULL) {
                        vm_runtime_error(vm, "string.format: out of memory");
                        return -1;
                    }
                    int w = 0;
                    memcpy(out + w, s->chars, (size_t)s->length);
                    w += s->length;
                    out[w++] = ' ';
                    memcpy(out + w, numbuf, (size_t)num_len);
                    w += num_len;
                    memcpy(out + w, " sec", 4);
                    w += 4;
                    out[w] = '\0';
                    push(vm, OBJ_VAL(take_string_unhashed(out, w)));
                    return 1;
                }
            }

            // Hot benchmark pattern family: prefix%d[suffix] / prefix%i[suffix]
            // with exactly one integer placeholder and one numeric argument.
            if (arg_count == 2 && IS_NUMBER(args[1])) {
                int first_pct = -1;
                for (int i = 0; i < flen; i++) {
                    if (f[i] == '%') {
                        first_pct = i;
                        break;
                    }
                }
                if (first_pct >= 0 &&
                    first_pct + 1 < flen &&
                    (f[first_pct + 1] == 'd' || f[first_pct + 1] == 'i')) {
                    int second_pct = -1;
                    for (int i = first_pct + 2; i < flen; i++) {
                        if (f[i] == '%') {
                            second_pct = i;
                            break;
                        }
                    }
                    if (second_pct < 0) {
                        int64_t iv = 0;
                        if (to_int64(AS_NUMBER(args[1]), &iv)) {
                            char numbuf[48];
                            int nlen = snprintf(numbuf, sizeof(numbuf), "%lld", (long long)iv);
                            if (nlen > 0 && nlen < (int)sizeof(numbuf)) {
                                int prefix_len = first_pct;
                                int suffix_len = flen - (first_pct + 2);
                                int out_len = prefix_len + nlen + suffix_len;
                                char* out = (char*)malloc((size_t)out_len + 1);
                                if (out == NULL) {
                                    vm_runtime_error(vm, "string.format: out of memory");
                                    return -1;
                                }
                                int w = 0;
                                if (prefix_len > 0) {
                                    memcpy(out + w, f, (size_t)prefix_len);
                                    w += prefix_len;
                                }
                                memcpy(out + w, numbuf, (size_t)nlen);
                                w += nlen;
                                if (suffix_len > 0) {
                                    memcpy(out + w, f + first_pct + 2, (size_t)suffix_len);
                                    w += suffix_len;
                                }
                                out[w] = '\0';
                                push(vm, OBJ_VAL(take_string_unhashed(out, w)));
                                return 1;
                            }
                        }
                    }
                }
            }
            goto fast_arg_error;
        }
        case NATIVE_FAST_CORE_NEXT: {
            if (arg_count != 2) goto fast_arg_error;
            Value state = args[0];
            Value current_key = args[1];

            if (IS_TABLE(state)) {
                ObjTable* obj_table = AS_TABLE(state);
                Table* table = &obj_table->table;

                if (IS_NIL(current_key) || IS_NUMBER(current_key)) {
                    double num = IS_NUMBER(current_key) ? AS_NUMBER(current_key) : 0;
                    int start = 1;
                    if (IS_NUMBER(current_key) && num >= 1 && (double)(int)num == num) {
                        start = (int)num + 1;
                    }
                    for (int i = start; i <= table->array_capacity; i++) {
                        Value val = NIL_VAL;
                        if (table_get_array(table, i, &val) && !IS_NIL(val)) {
                            push(vm, NUMBER_VAL((double)i));
                            push(vm, val);
                            return 1;
                        }
                    }
                    current_key = NIL_VAL;
                }

                int found_current = IS_NIL(current_key);
                ObjString* num_key = NULL;
                if (IS_NUMBER(current_key)) {
                    num_key = number_key_string(AS_NUMBER(current_key));
                }

                for (int i = 0; i < table->capacity; i++) {
                    Entry* entry = &table->entries[i];
                    if (entry->key == NULL) continue;
                    if (found_current) {
                        push(vm, OBJ_VAL(entry->key));
                        push(vm, entry->value);
                        return 1;
                    }
                    if (IS_STRING(current_key)) {
                        ObjString* s_key = AS_STRING(current_key);
                        if (entry->key == s_key ||
                            (entry->key->length == s_key->length &&
                             memcmp(entry->key->chars, s_key->chars, (size_t)entry->key->length) == 0)) {
                            found_current = 1;
                        }
                    } else if (IS_NUMBER(current_key)) {
                        if (entry->key == num_key ||
                            (entry->key->length == num_key->length &&
                             memcmp(entry->key->chars, num_key->chars, (size_t)entry->key->length) == 0)) {
                            found_current = 1;
                        }
                    }
                }
                push(vm, NIL_VAL);
                push(vm, NIL_VAL);
                return 1;
            }

            if (IS_STRING(state)) {
                ObjString* str = AS_STRING(state);
                int index = 1;
                if (IS_NUMBER(current_key)) {
                    double n = AS_NUMBER(current_key);
                    if (n >= 1 && (double)(int)n == n) {
                        index = (int)n + 1;
                    }
                } else if (!IS_NIL(current_key)) {
                    vm_runtime_error(vm, "next() string control must be number or nil.");
                    return -1;
                }

                if (index < 1 || index > str->length) {
                    push(vm, NIL_VAL);
                    push(vm, NIL_VAL);
                    return 1;
                }

                push(vm, NUMBER_VAL((double)index));
                push(vm, OBJ_VAL(copy_string(str->chars + (index - 1), 1)));
                return 1;
            }

            vm_runtime_error(vm, "next expects table or string as first argument.");
            return -1;
        }
        case NATIVE_FAST_CORE_MIN: {
            if (arg_count == 0) {
                push(vm, NIL_VAL);
                return 1;
            }
            if (!IS_NUMBER(args[0])) goto fast_arg_error;
            double min = AS_NUMBER(args[0]);
            for (int i = 1; i < arg_count; i++) {
                if (!IS_NUMBER(args[i])) goto fast_arg_error;
                double val = AS_NUMBER(args[i]);
                if (val < min) min = val;
            }
            push(vm, NUMBER_VAL(min));
            return 1;
        }
        case NATIVE_FAST_CORE_MAX: {
            if (arg_count == 0) {
                push(vm, NIL_VAL);
                return 1;
            }
            if (!IS_NUMBER(args[0])) goto fast_arg_error;
            double max = AS_NUMBER(args[0]);
            for (int i = 1; i < arg_count; i++) {
                if (!IS_NUMBER(args[i])) goto fast_arg_error;
                double val = AS_NUMBER(args[i]);
                if (val > max) max = val;
            }
            push(vm, NUMBER_VAL(max));
            return 1;
        }
        case NATIVE_FAST_CORE_SUM: {
            if (arg_count == 0) {
                push(vm, NIL_VAL);
                return 1;
            }

            if (arg_count == 1 && IS_TABLE(args[0])) {
                ObjTable* table = AS_TABLE(args[0]);
                double sum = 0.0;
                for (int i = 1; ; i++) {
                    Value val;
                    if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;
                    if (!IS_NUMBER(val)) goto fast_arg_error;
                    sum += AS_NUMBER(val);
                }
                push(vm, NUMBER_VAL(sum));
                return 1;
            }

            if (arg_count == 1 && IS_THREAD(args[0])) goto fast_arg_error;

            double sum = 0.0;
            for (int i = 0; i < arg_count; i++) {
                if (!IS_NUMBER(args[i])) goto fast_arg_error;
                sum += AS_NUMBER(args[i]);
            }
            push(vm, NUMBER_VAL(sum));
            return 1;
        }
    }
    return 0;

fast_arg_error:
    // Fall back to native implementation for exact error text/semantics.
    return 0;
}

static int value_matches_type(Value v, uint8_t type) {
    switch (type) {
        case TYPEHINT_ANY:
            return 1;
        case TYPEHINT_INT: {
            if (!IS_NUMBER(v)) return 0;
            int64_t tmp;
            return to_int64(AS_NUMBER(v), &tmp);
        }
        case TYPEHINT_FLOAT:
            return IS_NUMBER(v);
        case TYPEHINT_BOOL:
            return IS_BOOL(v);
        case TYPEHINT_STR:
            return IS_STRING(v);
        case TYPEHINT_TABLE:
            return IS_TABLE(v);
        default:
            return 0;
    }
}

static inline int call_args_match_param_types(
    ObjFunction* function,
    Value* args,
    int arg_count) {
    if (function == NULL || arg_count < 0) return 0;
    if (function->param_types_count <= 0 || function->param_types == NULL) return 1;
    int check_count = function->param_types_count < arg_count ? function->param_types_count : arg_count;
    for (int i = 0; i < check_count; i++) {
        if (!value_matches_type(args[i], function->param_types[i])) return 0;
    }
    return 1;
}

static inline int value_matches_numeric_hint_fast(Value v, uint8_t type) {
    if (!IS_NUMBER(v)) return 0;
    if (type == TYPEHINT_FLOAT) return 1;
    if (type == TYPEHINT_INT) {
        int64_t tmp;
        return to_int64(AS_NUMBER(v), &tmp);
    }
    return 0;
}

static uint64_t vm_prof_now_ns(void) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return (uint64_t)((double)clock() * (1000000000.0 / (double)CLOCKS_PER_SEC));
#endif
}

static void vm_prof_record_native_slow(VM* vm, ObjNative* native, uint64_t elapsed_ns) {
    for (int i = 0; i < 16; i++) {
        if (vm->call_prof_native_slow_keys[i] == native) {
            vm->call_prof_native_slow_call_counts[i]++;
            vm->call_prof_native_slow_ns_by_key[i] += elapsed_ns;
            return;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (vm->call_prof_native_slow_keys[i] == NULL) {
            vm->call_prof_native_slow_keys[i] = native;
            vm->call_prof_native_slow_call_counts[i] = 1;
            vm->call_prof_native_slow_ns_by_key[i] = elapsed_ns;
            return;
        }
    }
    int min_i = 0;
    for (int i = 1; i < 16; i++) {
        if (vm->call_prof_native_slow_ns_by_key[i] < vm->call_prof_native_slow_ns_by_key[min_i]) {
            min_i = i;
        }
    }
    vm->call_prof_native_slow_keys[min_i] = native;
    vm->call_prof_native_slow_call_counts[min_i] = 1;
    vm->call_prof_native_slow_ns_by_key[min_i] = elapsed_ns;
}

static const char* vm_opcode_name(uint8_t op) {
    switch (op) {
        case OP_CALL: return "OP_CALL";
        case OP_CALL0: return "OP_CALL0";
        case OP_CALL1: return "OP_CALL1";
        case OP_QCALL0: return "OP_QCALL0";
        case OP_QCALL1: return "OP_QCALL1";
        case OP_CALL2: return "OP_CALL2";
        case OP_QCALL2: return "OP_QCALL2";
        case OP_GET_TABLE: return "OP_GET_TABLE";
        case OP_SET_TABLE: return "OP_SET_TABLE";
        case OP_GET_FIELD_CONST: return "OP_GET_FIELD_CONST";
        case OP_SET_FIELD_CONST: return "OP_SET_FIELD_CONST";
        case OP_SET_GLOBAL_POP: return "OP_SET_GLOBAL_POP";
        case OP_SET_LOCAL_POP: return "OP_SET_LOCAL_POP";
        case OP_SET_UPVALUE_POP: return "OP_SET_UPVALUE_POP";
        case OP_INVOKE: return "OP_INVOKE";
        case OP_IMPORT: return "OP_IMPORT";
        case OP_RETURN: return "OP_RETURN";
        default: return NULL;
    }
}

static void vm_prof_print(VM* vm) {
    if (vm->opcode_prof_enabled) {
        fprintf(stderr, "\n[vm-opcode-prof] top opcodes by total ns:\n");
        uint8_t top_idx[16];
        uint64_t top_ns[16];
        for (int i = 0; i < 16; i++) {
            top_idx[i] = 0;
            top_ns[i] = 0;
        }
        for (int op = 0; op < 256; op++) {
            uint64_t ns = vm->opcode_prof_ns[op];
            if (ns == 0) continue;
            for (int slot = 0; slot < 16; slot++) {
                if (ns > top_ns[slot]) {
                    for (int move = 15; move > slot; move--) {
                        top_ns[move] = top_ns[move - 1];
                        top_idx[move] = top_idx[move - 1];
                    }
                    top_ns[slot] = ns;
                    top_idx[slot] = (uint8_t)op;
                    break;
                }
            }
        }
        for (int i = 0; i < 16; i++) {
            if (top_ns[i] == 0) continue;
            uint8_t op = top_idx[i];
            const char* name = vm_opcode_name(op);
            if (name == NULL) {
                fprintf(stderr,
                        "  - opcode=%u count=%llu ns=%llu avg_ns=%llu\n",
                        (unsigned)op,
                        (unsigned long long)vm->opcode_prof_count[op],
                        (unsigned long long)vm->opcode_prof_ns[op],
                        (unsigned long long)(vm->opcode_prof_ns[op] / (vm->opcode_prof_count[op] ? vm->opcode_prof_count[op] : 1)));
            } else {
                fprintf(stderr,
                        "  - %s(%u) count=%llu ns=%llu avg_ns=%llu\n",
                        name,
                        (unsigned)op,
                        (unsigned long long)vm->opcode_prof_count[op],
                        (unsigned long long)vm->opcode_prof_ns[op],
                        (unsigned long long)(vm->opcode_prof_ns[op] / (vm->opcode_prof_count[op] ? vm->opcode_prof_count[op] : 1)));
            }
        }
    }

    if (vm->call_prof_enabled) {
        fprintf(stderr,
                "\n[vm-call-prof] op_counts call=%llu call0=%llu call1=%llu call2=%llu qcall2=%llu call_named=%llu call_expand=%llu\n",
                (unsigned long long)vm->call_prof_op_call_count,
                (unsigned long long)vm->call_prof_op_call0_count,
                (unsigned long long)vm->call_prof_op_call1_count,
                (unsigned long long)vm->call_prof_op_call2_count,
                (unsigned long long)vm->call_prof_op_qcall2_count,
                (unsigned long long)vm->call_prof_op_call_named_count,
                (unsigned long long)vm->call_prof_op_call_expand_count);
        fprintf(stderr,
                "[vm-call-prof] op_ns call=%llu call0=%llu call1=%llu call2=%llu call_named=%llu call_expand=%llu\n",
                (unsigned long long)vm->call_prof_op_call_ns,
                (unsigned long long)vm->call_prof_op_call0_ns,
                (unsigned long long)vm->call_prof_op_call1_ns,
                (unsigned long long)vm->call_prof_op_call2_ns,
                (unsigned long long)vm->call_prof_op_call_named_ns,
                (unsigned long long)vm->call_prof_op_call_expand_ns);
        fprintf(stderr,
                "[vm-call-prof] call_paths ic_hit=%llu ic_miss=%llu direct_fast=%llu fallback=%llu\n",
                (unsigned long long)vm->call_prof_call_ic_hit_count,
                (unsigned long long)vm->call_prof_call_ic_miss_count,
                (unsigned long long)vm->call_prof_call_direct_fast_count,
                (unsigned long long)vm->call_prof_call_fallback_count);
        fprintf(stderr,
                "[vm-call-prof] call_expand_paths named=%llu positional=%llu spread_values=%llu\n",
                (unsigned long long)vm->call_prof_call_expand_named_count,
                (unsigned long long)vm->call_prof_call_expand_positional_count,
                (unsigned long long)vm->call_prof_call_expand_spread_values);
        fprintf(stderr,
                "[vm-call-prof] call2_paths ic_hit=%llu ic_miss=%llu direct=%llu typed_fast=%llu tiny_inline_fast=%llu fallback=%llu\n",
                (unsigned long long)vm->call_prof_call2_ic_hit_count,
                (unsigned long long)vm->call_prof_call2_ic_miss_count,
                (unsigned long long)vm->call_prof_call2_direct_fast_count,
                (unsigned long long)vm->call_prof_call2_typed_fast_count,
                (unsigned long long)vm->call_prof_call2_tiny_inline_fast_count,
                (unsigned long long)vm->call_prof_call2_fallback_count);
        fprintf(stderr,
                "[vm-call-prof] call0_paths direct=%llu bound_native_fast=%llu native=%llu fallback=%llu\n",
                (unsigned long long)vm->call_prof_call0_direct_fast_count,
                (unsigned long long)vm->call_prof_call0_bound_native_fast_count,
                (unsigned long long)vm->call_prof_call0_native_count,
                (unsigned long long)vm->call_prof_call0_fallback_count);
        fprintf(stderr,
                "[vm-call-prof] call1_paths direct=%llu typed_fast=%llu native=%llu fallback=%llu\n",
                (unsigned long long)vm->call_prof_call1_direct_fast_count,
                (unsigned long long)vm->call_prof_call1_typed_fast_count,
                (unsigned long long)vm->call_prof_call1_native_count,
                (unsigned long long)vm->call_prof_call1_fallback_count);
        fprintf(stderr,
                "[vm-call-prof] call_value native=%llu closure=%llu fast=%llu\n",
                (unsigned long long)vm->call_prof_call_value_native_count,
                (unsigned long long)vm->call_prof_call_value_closure_count,
                (unsigned long long)vm->call_prof_call_value_fast_count);
        fprintf(stderr,
                "[vm-call-prof] call_value_ns native=%llu closure=%llu native_fast=%llu native_slow=%llu\n",
                (unsigned long long)vm->call_prof_call_value_native_ns,
                (unsigned long long)vm->call_prof_call_value_closure_ns,
                (unsigned long long)vm->call_prof_call_value_native_fast_ns,
                (unsigned long long)vm->call_prof_call_value_native_slow_ns);
        fprintf(stderr,
                "[vm-call-prof] call_value_paths noncallable=%llu native_fast_try=%llu native_fast_hit=%llu native_fast_miss=%llu native_fast_error=%llu native_slow=%llu native_thread_switch=%llu\n",
                (unsigned long long)vm->call_prof_call_value_noncallable_count,
                (unsigned long long)vm->call_prof_native_fast_try_count,
                (unsigned long long)vm->call_prof_native_fast_hit_count,
                (unsigned long long)vm->call_prof_native_fast_miss_count,
                (unsigned long long)vm->call_prof_native_fast_error_count,
                (unsigned long long)vm->call_prof_native_slow_count,
                (unsigned long long)vm->call_prof_native_thread_switch_count);
        fprintf(stderr,
                "[vm-call-prof] native_fast_hits str.upper=%llu str.lower=%llu str.replace=%llu str.join=%llu str.format=%llu core.next=%llu core.min=%llu core.max=%llu core.sum=%llu\n",
                (unsigned long long)vm->call_prof_native_fast_str_upper_hit_count,
                (unsigned long long)vm->call_prof_native_fast_str_lower_hit_count,
                (unsigned long long)vm->call_prof_native_fast_str_replace_hit_count,
                (unsigned long long)vm->call_prof_native_fast_str_join_hit_count,
                (unsigned long long)vm->call_prof_native_fast_str_format_hit_count,
                (unsigned long long)vm->call_prof_native_fast_core_next_hit_count,
                (unsigned long long)vm->call_prof_native_fast_core_min_hit_count,
                (unsigned long long)vm->call_prof_native_fast_core_max_hit_count,
                (unsigned long long)vm->call_prof_native_fast_core_sum_hit_count);
        fprintf(stderr, "[vm-call-prof] native_slow_top:\n");
        for (int i = 0; i < 16; i++) {
            ObjNative* native = vm->call_prof_native_slow_keys[i];
            if (native == NULL || vm->call_prof_native_slow_call_counts[i] == 0) continue;
            const char* name = (native->name != NULL) ? native->name->chars : "<anon>";
            fprintf(stderr,
                    "  - %s calls=%llu ns=%llu\n",
                    name,
                    (unsigned long long)vm->call_prof_native_slow_call_counts[i],
                    (unsigned long long)vm->call_prof_native_slow_ns_by_key[i]);
        }
    }

    if (vm->table_prof_enabled) {
        fprintf(stderr,
                "\n[vm-table-prof] get_ic exact_hit=%llu slot_hit=%llu miss=%llu generic_lookup=%llu meta_index=%llu\n",
                (unsigned long long)vm->table_prof_get_ic_exact_hit_count,
                (unsigned long long)vm->table_prof_get_ic_slot_hit_count,
                (unsigned long long)vm->table_prof_get_ic_miss_count,
                (unsigned long long)vm->table_prof_get_generic_lookup_count,
                (unsigned long long)vm->table_prof_get_meta_index_count);
        fprintf(stderr,
                "[vm-table-prof] set_ic exact_hit=%llu slot_hit=%llu miss=%llu generic_store=%llu meta_newindex=%llu\n",
                (unsigned long long)vm->table_prof_set_ic_exact_hit_count,
                (unsigned long long)vm->table_prof_set_ic_slot_hit_count,
                (unsigned long long)vm->table_prof_set_ic_miss_count,
                (unsigned long long)vm->table_prof_set_generic_store_count,
                (unsigned long long)vm->table_prof_set_meta_newindex_count);
    }

    if (vm->table_site_prof_enabled) {
        typedef struct {
            ObjFunction* fn;
            int offset;
            uint32_t accesses;
            uint8_t unique;
        } TableSiteTop;
        TableSiteTop top_get[8] = {0};
        TableSiteTop top_set[8] = {0};
        uint64_t get_sites = 0, set_sites = 0;
        uint64_t get_poly = 0, set_poly = 0;
        uint64_t get_mega = 0, set_mega = 0;
        uint8_t get_max_unique = 0, set_max_unique = 0;

        for (struct Obj* obj = objects; obj != NULL; obj = obj->next) {
            if (obj->type != OBJ_FUNCTION) continue;
            ObjFunction* fn = (ObjFunction*)obj;
            Chunk* chunk = &fn->chunk;
            if (chunk->capacity <= 0) continue;
            for (int i = 0; i < chunk->capacity; i++) {
                if (chunk->get_table_site_accesses != NULL && chunk->get_table_site_unique_keys != NULL) {
                    uint32_t a = chunk->get_table_site_accesses[i];
                    uint8_t u = chunk->get_table_site_unique_keys[i];
                    if (a > 0) {
                        get_sites++;
                        if (u >= 2) get_poly++;
                        if (u >= 4) get_mega++;
                        if (u > get_max_unique) get_max_unique = u;
                        uint64_t score = ((uint64_t)a << 8) + (uint64_t)u;
                        int min_idx = 0;
                        uint64_t min_score = ((uint64_t)top_get[0].accesses << 8) + (uint64_t)top_get[0].unique;
                        for (int t = 1; t < 8; t++) {
                            uint64_t s = ((uint64_t)top_get[t].accesses << 8) + (uint64_t)top_get[t].unique;
                            if (s < min_score) {
                                min_score = s;
                                min_idx = t;
                            }
                        }
                        if (score > min_score) {
                            top_get[min_idx].fn = fn;
                            top_get[min_idx].offset = i;
                            top_get[min_idx].accesses = a;
                            top_get[min_idx].unique = u;
                        }
                    }
                }
                if (chunk->set_table_site_accesses != NULL && chunk->set_table_site_unique_keys != NULL) {
                    uint32_t a = chunk->set_table_site_accesses[i];
                    uint8_t u = chunk->set_table_site_unique_keys[i];
                    if (a > 0) {
                        set_sites++;
                        if (u >= 2) set_poly++;
                        if (u >= 4) set_mega++;
                        if (u > set_max_unique) set_max_unique = u;
                        uint64_t score = ((uint64_t)a << 8) + (uint64_t)u;
                        int min_idx = 0;
                        uint64_t min_score = ((uint64_t)top_set[0].accesses << 8) + (uint64_t)top_set[0].unique;
                        for (int t = 1; t < 8; t++) {
                            uint64_t s = ((uint64_t)top_set[t].accesses << 8) + (uint64_t)top_set[t].unique;
                            if (s < min_score) {
                                min_score = s;
                                min_idx = t;
                            }
                        }
                        if (score > min_score) {
                            top_set[min_idx].fn = fn;
                            top_set[min_idx].offset = i;
                            top_set[min_idx].accesses = a;
                            top_set[min_idx].unique = u;
                        }
                    }
                }
            }
        }

        fprintf(stderr,
                "\n[vm-table-sites] get sites=%llu poly(>=2)=%llu mega(>=4)=%llu max_unique=%u\n",
                (unsigned long long)get_sites,
                (unsigned long long)get_poly,
                (unsigned long long)get_mega,
                (unsigned)get_max_unique);
        for (int printed = 0; printed < 8; printed++) {
            int best = -1;
            uint64_t best_score = 0;
            for (int i = 0; i < 8; i++) {
                if (top_get[i].fn == NULL || top_get[i].accesses == 0) continue;
                uint64_t s = ((uint64_t)top_get[i].accesses << 8) + (uint64_t)top_get[i].unique;
                if (best < 0 || s > best_score) {
                    best = i;
                    best_score = s;
                }
            }
            if (best < 0) break;
            const char* fn_name = (top_get[best].fn->name != NULL) ? top_get[best].fn->name->chars : "<script>";
            fprintf(stderr,
                    "  - get fn=%s off=%d accesses=%u unique=%u%s\n",
                    fn_name,
                    top_get[best].offset,
                    top_get[best].accesses,
                    (unsigned)(top_get[best].unique > 5 ? 5 : top_get[best].unique),
                    (top_get[best].unique >= 5) ? "+" : "");
            top_get[best].fn = NULL;
        }

        fprintf(stderr,
                "[vm-table-sites] set sites=%llu poly(>=2)=%llu mega(>=4)=%llu max_unique=%u\n",
                (unsigned long long)set_sites,
                (unsigned long long)set_poly,
                (unsigned long long)set_mega,
                (unsigned)set_max_unique);
        for (int printed = 0; printed < 8; printed++) {
            int best = -1;
            uint64_t best_score = 0;
            for (int i = 0; i < 8; i++) {
                if (top_set[i].fn == NULL || top_set[i].accesses == 0) continue;
                uint64_t s = ((uint64_t)top_set[i].accesses << 8) + (uint64_t)top_set[i].unique;
                if (best < 0 || s > best_score) {
                    best = i;
                    best_score = s;
                }
            }
            if (best < 0) break;
            const char* fn_name = (top_set[best].fn->name != NULL) ? top_set[best].fn->name->chars : "<script>";
            fprintf(stderr,
                    "  - set fn=%s off=%d accesses=%u unique=%u%s\n",
                    fn_name,
                    top_set[best].offset,
                    top_set[best].accesses,
                    (unsigned)(top_set[best].unique > 5 ? 5 : top_set[best].unique),
                    (top_set[best].unique >= 5) ? "+" : "");
            top_set[best].fn = NULL;
        }
    }

    if (vm->gc_prof_enabled) {
        uint64_t uptime_ns = 0;
        if (vm->gc_prof_start_ns != 0) {
            uptime_ns = vm_prof_now_ns() - vm->gc_prof_start_ns;
        }
        uint64_t avg_ns = (vm->gc_prof_collect_count > 0)
                ? (vm->gc_prof_total_ns / vm->gc_prof_collect_count) : 0;
        double gc_pct = 0.0;
        if (uptime_ns > 0) {
            gc_pct = ((double)vm->gc_prof_total_ns * 100.0) / (double)uptime_ns;
        }
        uint64_t avg_freed = (vm->gc_prof_collect_count > 0)
                ? (vm->gc_prof_bytes_freed_sum / vm->gc_prof_collect_count) : 0;
        fprintf(stderr,
                "\n[vm-gc-prof] collections total=%llu auto=%llu manual=%llu final=%llu\n",
                (unsigned long long)vm->gc_prof_collect_count,
                (unsigned long long)vm->gc_prof_auto_count,
                (unsigned long long)vm->gc_prof_manual_count,
                (unsigned long long)vm->gc_prof_final_count);
        fprintf(stderr,
                "[vm-gc-prof] time_ns total=%llu mark=%llu sweep=%llu avg=%llu max=%llu uptime=%llu gc_share=%.2f%%\n",
                (unsigned long long)vm->gc_prof_total_ns,
                (unsigned long long)vm->gc_prof_mark_ns,
                (unsigned long long)vm->gc_prof_sweep_ns,
                (unsigned long long)avg_ns,
                (unsigned long long)vm->gc_prof_max_ns,
                (unsigned long long)uptime_ns,
                gc_pct);
        fprintf(stderr,
                "[vm-gc-prof] bytes avg_freed=%llu before_sum=%llu after_sum=%llu freed_sum=%llu\n",
                (unsigned long long)avg_freed,
                (unsigned long long)vm->gc_prof_bytes_before_sum,
                (unsigned long long)vm->gc_prof_bytes_after_sum,
                (unsigned long long)vm->gc_prof_bytes_freed_sum);
        fprintf(stderr,
                "[vm-gc-prof] auto_trigger checks=%llu hits=%llu\n",
                (unsigned long long)vm->gc_prof_auto_trigger_checks,
                (unsigned long long)vm->gc_prof_auto_trigger_hits);
    }
    if (vm->tiered_jit_profile) {
        fprintf(stderr,
                "\n[vm-tier] enabled=%d tier1_hot_threshold=%u tier1_promotions=%llu tier1_call2_patches=%llu "
                "tier2_hot_threshold=%u tier2_inline_hot_threshold=%u tier2_promotions=%llu tier2_exec=%llu\n",
                vm->tiered_jit_enabled,
                vm->tier1_hot_threshold,
                (unsigned long long)vm->tier1_promote_count,
                (unsigned long long)vm->tier1_call2_patch_count,
                vm->tier2_hot_threshold,
                vm->tier2_inline_hot_threshold,
                (unsigned long long)vm->tier2_promote_count,
                (unsigned long long)vm->tier2_exec_count);
    }
    if (object_prof_enabled()) {
        object_prof_print();
    }
    toi_jit_print_stats(vm);
}

static void apply_pending_set_local(VM* vm) {
    ObjThread* thread = vm_current_thread(vm);
    if (thread->pending_set_local_count == 0) return;
    int top = thread->pending_set_local_count - 1;
    int frame_index = thread->pending_set_local_frames[top];
    if (frame_index != vm_current_thread(vm)->frame_count - 1) return;
    CallFrame* target = &vm_current_thread(vm)->frames[frame_index];
    target->slots[thread->pending_set_local_slots[top]] = peek(vm, 0);
    thread->pending_set_local_count--;
}

static int vm_handle_op_unpack(VM* vm, CallFrame* frame, uint8_t** ip) {
    uint8_t base_depth = *(*ip)++;
    uint8_t target_count = *(*ip)++;
    Value* base = frame->slots + base_depth;
    ptrdiff_t available = vm_current_thread(vm)->stack_top - base;
    if (available < 0) {
        vm_runtime_error(vm, "Internal error: invalid stack state in unpack.");
        return 0;
    }

    if (available == 1 && IS_TABLE(base[0])) {
        ObjTable* values = AS_TABLE(base[0]);
        for (int i = 0; i < target_count; i++) {
            Value element = NIL_VAL;
            table_get_array(&values->table, i + 1, &element);
            base[i] = element;
        }
        vm_current_thread(vm)->stack_top = base + target_count;
        return 1;
    }

    int have = (int)available;
    if (have < target_count) {
        for (int i = have; i < target_count; i++) {
            base[i] = NIL_VAL;
        }
    }
    vm_current_thread(vm)->stack_top = base + (have > target_count ? have : target_count);
    return 1;
}

extern void register_libs(VM* vm);
extern void free_object(struct Obj* object);
static void collect_garbage_reason(VM* vm, int reason); // Defined later in this file

typedef enum {
    VM_GC_REASON_AUTO = 0,
    VM_GC_REASON_MANUAL = 1,
    VM_GC_REASON_FINAL = 2
} VmGcReason;

static void reset_stack(VM* vm) {
    if (vm_current_thread(vm) != NULL) {
        vm_current_thread(vm)->stack_top = vm_current_thread(vm)->stack;
        vm_current_thread(vm)->frame_count = 0;
        vm_current_thread(vm)->open_upvalues = NULL;
    }
}

void vm_runtime_error(VM* vm, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    vm_current_thread(vm)->has_exception = 1;
    vm_current_thread(vm)->exception = OBJ_VAL(copy_string(buffer, (int)strlen(buffer)));
    vm_current_thread(vm)->last_error = vm_current_thread(vm)->exception;
}

static void report_exception(VM* vm) {
    if (!vm_current_thread(vm)->has_exception) return;
    Value ex = vm_current_thread(vm)->exception;
    if (IS_STRING(ex)) {
        fprintf(stderr, COLOR_RED "Error: " COLOR_RESET "%s\n", AS_CSTRING(ex));
    } else if (IS_TABLE(ex)) {
        ObjTable* t = AS_TABLE(ex);
        Value msg = NIL_VAL;
        Value type = NIL_VAL;
        ObjString* msg_key = copy_string("msg", 3);
        ObjString* type_key = copy_string("type", 4);
        table_get(&t->table, msg_key, &msg);
        table_get(&t->table, type_key, &type);

        const char* msg_s = IS_STRING(msg) ? AS_CSTRING(msg) : "<exception>";
        const char* type_s = IS_STRING(type) ? AS_CSTRING(type) : "Error";
        fprintf(stderr, COLOR_RED "%s: " COLOR_RESET "%s\n", type_s, msg_s);
    } else {
        fprintf(stderr, COLOR_RED "Error: " COLOR_RESET "<exception>\n");
    }

    for (int i = vm_current_thread(vm)->frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm_current_thread(vm)->frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
}

Value peek(VM* vm, int distance) {
    return vm_current_thread(vm)->stack_top[-1 - distance];
}

static int handle_exception(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (!vm_current_thread(vm)->has_exception) return 0;
    ObjThread* thread = vm_current_thread(vm);
    while (thread->handler_count > 0) {
        ExceptionHandler* handler = &thread->handlers[thread->handler_count - 1];

        while (thread->frame_count > handler->frame_count) {
            CallFrame* f = &thread->frames[thread->frame_count - 1];
            close_upvalues(vm, f->slots);
            thread->frame_count--;
        }

        if (thread->frame_count == 0) {
            report_exception(vm);
            reset_stack(vm);
            thread->has_exception = 0;
            thread->exception = NIL_VAL;
            return 0;
        }

        *frame = &thread->frames[thread->frame_count - 1];
        thread->stack_top = handler->stack_top;

        if (handler->has_except && !handler->in_except) {
            handler->in_except = 1;
            *ip = handler->except_ip;
            push(vm, thread->exception);
            thread->has_exception = 0;
            thread->exception = NIL_VAL;
            return 1;
        }

        if (handler->has_finally && handler->finally_ip != NULL) {
            thread->handler_count--;
            *ip = handler->finally_ip;
            return 1;
        }

        thread->handler_count--;
    }

    report_exception(vm);
    reset_stack(vm);
    thread->has_exception = 0;
    thread->exception = NIL_VAL;
    return 0;
}

static volatile sig_atomic_t interrupt_requested = 0;

#ifndef TOI_WASM
static pthread_key_t current_thread_key;
static pthread_once_t current_thread_key_once = PTHREAD_ONCE_INIT;

static void make_current_thread_key(void) {
    pthread_key_create(&current_thread_key, NULL);
}

ObjThread* vm_current_thread(VM* vm) {
    if (!vm->use_thread_tls) {
        return vm->current_thread;
    }
    pthread_once(&current_thread_key_once, make_current_thread_key);
    ObjThread* t = (ObjThread*)pthread_getspecific(current_thread_key);
    if (t != NULL) return t;
    return vm->current_thread;
}

void vm_set_current_thread(VM* vm, ObjThread* thread) {
    if (!vm->use_thread_tls) {
        vm->current_thread = thread;
        return;
    }
    pthread_once(&current_thread_key_once, make_current_thread_key);
    pthread_setspecific(current_thread_key, thread);
    if (thread == NULL || thread == vm->current_thread) return;
    if (vm->current_thread == NULL) {
        vm->current_thread = thread;
    }
}

void vm_enable_thread_tls(VM* vm) {
    if (vm->use_thread_tls) return;
    vm->use_thread_tls = 1;
    pthread_once(&current_thread_key_once, make_current_thread_key);
    pthread_setspecific(current_thread_key, vm->current_thread);
}
#else
ObjThread* vm_current_thread(VM* vm) {
    return vm->current_thread;
}

void vm_set_current_thread(VM* vm, ObjThread* thread) {
    vm->current_thread = thread;
}

void vm_enable_thread_tls(VM* vm) {
    (void)vm;
}
#endif

void vm_request_interrupt(void) {
    interrupt_requested = 1;
}

void push(VM* vm, Value value) {
    ObjThread* thread = vm_current_thread(vm);
    *thread->stack_top++ = value;
}

Value pop(VM* vm) {
    ObjThread* thread = vm_current_thread(vm);
    return *--thread->stack_top;
}

static Value get_metamethod_cached(VM* vm, Value val, ObjString* name);
static int call_named(VM* vm, ObjClosure* closure, int arg_count);
int call(VM* vm, ObjClosure* closure, int arg_count);

static int function_active_in_thread(ObjThread* thread, ObjFunction* function) {
    if (thread == NULL || function == NULL) return 0;
    for (int i = 0; i < thread->frame_count; i++) {
        ObjClosure* closure = thread->frames[i].closure;
        if (closure != NULL && closure->function == function) return 1;
    }
    return 0;
}

static int function_active_anywhere(VM* vm, ObjFunction* function) {
    if (function_active_in_thread(vm_current_thread(vm), function)) return 1;
    for (ObjThread* parked = vm->gc_parked_threads; parked != NULL; parked = parked->gc_park_next) {
        if (function_active_in_thread(parked, function)) return 1;
    }
    return 0;
}

static void maybe_promote_function_tiers(VM* vm, ObjFunction* function) {
    if (!vm->tiered_jit_enabled) return;
    if (function == NULL) return;
    if (vm->tier1_hot_threshold == 0) return;

    if (function->hot_count < UINT32_MAX) {
        function->hot_count++;
    }
    if (function_active_anywhere(vm, function)) return;

    if (function->exec_tier == FUNCTION_TIER0_INTERPRET &&
        function->hot_count >= vm->tier1_hot_threshold) {
        int patched = 0;
        Chunk* chunk = &function->chunk;
        for (int offset = 0; offset < chunk->count; ) {
            uint8_t op = chunk->code[offset];
            if (op == OP_CALL2) {
                chunk->code[offset] = OP_QCALL2;
                patched++;
            }
            int len = chunk_instr_length(chunk, offset);
            if (len <= 0) break;
            offset += len;
        }

        function->tier1_patched = (patched > 0) ? 1 : 0;
        function->exec_tier = FUNCTION_TIER1_QUICKENED;
        vm->tier1_promote_count++;
        vm->tier1_call2_patch_count += (uint64_t)patched;
        if (vm->tiered_jit_profile) {
            const char* name = (function->name != NULL) ? function->name->chars : "<script>";
            fprintf(stderr,
                    "[vm-tier] promote tier1 fn=%s calls=%u patched_call2=%d\n",
                    name,
                    function->hot_count,
                    patched);
        }
    }

    if (function->exec_tier == FUNCTION_TIER1_QUICKENED &&
        vm->tier2_hot_threshold > vm->tier1_hot_threshold &&
        function->hot_count >= vm->tier2_hot_threshold &&
        (vm->native_jit_enabled || function->tiny_inline_kind != TINY_INLINE_NONE)) {
        function->exec_tier = FUNCTION_TIER2_JIT;
        vm->tier2_promote_count++;
        toi_jit_notify_hot_function(vm, function);
        if (vm->tiered_jit_profile) {
            const char* name = (function->name != NULL) ? function->name->chars : "<script>";
            fprintf(stderr,
                    "[vm-tier] promote tier2 fn=%s calls=%u tiny_inline=%u\n",
                    name,
                    function->hot_count,
                    (unsigned)function->tiny_inline_kind);
        }
    }
}

static inline void maybe_promote_inline_tiers(VM* vm, ObjFunction* function) {
    if (!vm->tiered_jit_enabled || function == NULL) return;
    if (function->hot_count < UINT32_MAX) function->hot_count++;
    if (function->exec_tier == FUNCTION_TIER2_JIT) return;

    if (function->exec_tier == FUNCTION_TIER0_INTERPRET &&
        function->hot_count >= vm->tier1_hot_threshold) {
        function->exec_tier = FUNCTION_TIER1_QUICKENED;
        vm->tier1_promote_count++;
        if (vm->tiered_jit_profile) {
            const char* name = (function->name != NULL) ? function->name->chars : "<script>";
            fprintf(stderr,
                    "[vm-tier] promote tier1 fn=%s calls=%u patched_call2=%d\n",
                    name,
                    function->hot_count,
                    0);
        }
    }

    uint32_t tier2_target = vm->tier2_hot_threshold;
    if (vm->tier2_inline_hot_threshold > vm->tier1_hot_threshold) {
        tier2_target = vm->tier2_inline_hot_threshold;
    }
    if (function->exec_tier == FUNCTION_TIER1_QUICKENED &&
        tier2_target > vm->tier1_hot_threshold &&
        function->hot_count >= tier2_target &&
        (vm->native_jit_enabled || function->tiny_inline_kind != TINY_INLINE_NONE)) {
        function->exec_tier = FUNCTION_TIER2_JIT;
        vm->tier2_promote_count++;
        toi_jit_notify_hot_function(vm, function);
        if (vm->tiered_jit_profile) {
            const char* name = (function->name != NULL) ? function->name->chars : "<script>";
            fprintf(stderr,
                    "[vm-tier] promote tier2 fn=%s calls=%u tiny_inline=%u\n",
                    name,
                    function->hot_count,
                    (unsigned)function->tiny_inline_kind);
        }
    }
}

static inline int enter_closure_frame(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* thread = vm_current_thread(vm);
    ObjFunction* function = closure->function;

    maybe_promote_function_tiers(vm, function);

    if (thread->frame_count == thread->frame_capacity) {
        vm_runtime_error(vm, "Stack overflow.");
        return 0;
    }

    CallFrame* frame = &thread->frames[thread->frame_count++];
    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = thread->stack_top - arg_count - 1;
    // Hot-path call setup: only reset flags that gate module-context work.
    // Import opcodes populate the remaining fields when needed.
    frame->restore_module_context = 0;
    frame->cache_module_result = 0;
    return 1;
}

static int try_eval_tiny_inline2(ObjFunction* function, Value arg0, Value arg1, Value* out) {
    switch (function->tiny_inline_kind) {
        case TINY_INLINE_IADD2:
        case TINY_INLINE_FADD2:
        case TINY_INLINE_ADD2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) + AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_ISUB2:
        case TINY_INLINE_FSUB2:
        case TINY_INLINE_SUB2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) - AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_IMUL2:
        case TINY_INLINE_FMUL2:
        case TINY_INLINE_MUL2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) * AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_IDIV2:
        case TINY_INLINE_FDIV2:
        case TINY_INLINE_DIV2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = NUMBER_VAL(AS_NUMBER(arg0) / AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_GT2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = BOOL_VAL(AS_NUMBER(arg0) > AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_LT2:
            if (!IS_NUMBER(arg0) || !IS_NUMBER(arg1)) return 0;
            *out = BOOL_VAL(AS_NUMBER(arg0) < AS_NUMBER(arg1));
            return 1;
        case TINY_INLINE_EQ2:
            if (arg0.type != arg1.type) {
                *out = BOOL_VAL(0);
            } else if (IS_NIL(arg0)) {
                *out = BOOL_VAL(1);
            } else if (IS_BOOL(arg0)) {
                *out = BOOL_VAL(AS_BOOL(arg0) == AS_BOOL(arg1));
            } else if (IS_NUMBER(arg0)) {
                *out = BOOL_VAL(AS_NUMBER(arg0) == AS_NUMBER(arg1));
            } else if (IS_OBJ(arg0)) {
                *out = BOOL_VAL(AS_OBJ(arg0) == AS_OBJ(arg1));
            } else {
                *out = BOOL_VAL(0);
            }
            return 1;
        case TINY_INLINE_RET0_2:
            *out = arg0;
            return 1;
        case TINY_INLINE_RET1_2:
            *out = arg1;
            return 1;
        default:
            return 0;
    }
}

static int try_execute_tier2_template(VM* vm, ObjClosure* closure, int arg_count) {
    ObjFunction* function = closure->function;
    if (function->exec_tier != FUNCTION_TIER2_JIT) return 0;
    if (function->param_types_count > 0) {
        int check_count = function->param_types_count < function->arity ? function->param_types_count : function->arity;
        Value* args = vm_current_thread(vm)->stack_top - arg_count;
        for (int i = 0; i < check_count; i++) {
            uint8_t type = function->param_types[i];
            if (type == TYPEHINT_ANY) continue;
            if (!value_matches_type(args[i], type)) return 0;
        }
    }
    if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, arg_count)) {
        vm->tier2_exec_count++;
        return 1;
    }
    if (function->tiny_inline_kind == TINY_INLINE_NONE) return 0;
    if (function->is_variadic || function->defaults_count != 0 || function->arity != 2) return 0;
    if (arg_count != 2) return 0;

    ObjThread* thread = vm_current_thread(vm);
    Value arg0 = thread->stack_top[-2];
    Value arg1 = thread->stack_top[-1];

    Value out = NIL_VAL;
    if (!try_eval_tiny_inline2(function, arg0, arg1, &out)) return 0;
    thread->stack_top -= 3; // callee + 2 args
    push(vm, out);
    vm->tier2_exec_count++;
    return 1;
}

static int call_native_direct(VM* vm, ObjNative* native_obj, int arg_count, Value* args, CallFrame** frame, uint8_t** ip) {
    NativeFn native = native_obj->function;
    (*frame)->ip = *ip;

    int fast = try_fast_native_call(vm, native_obj, arg_count, args);
    if (fast > 0) {
        return 1;
    }
    if (fast < 0) {
        return 0;
    }

    ObjThread* current = vm_current_thread(vm);
    if (!native(vm, arg_count, args)) {
        return 0;
    }

    ObjThread* after = vm_current_thread(vm);
    if (after != current) {
        *frame = &after->frames[after->frame_count - 1];
        *ip = (*frame)->ip;
    }
    return 1;
}

static int finish_closure_call(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* thread = vm_current_thread(vm);
    ObjFunction* function = closure->function;

    if (function->param_types_count > 0) {
        int check_count = function->param_types_count < function->arity ? function->param_types_count : function->arity;
        Value* args = thread->stack_top - arg_count;
        for (int i = 0; i < check_count; i++) {
            uint8_t type = function->param_types[i];
            if (type == TYPEHINT_ANY) continue;
            if (!value_matches_type(args[i], type)) {
                vm_runtime_error(vm, "Type mismatch for parameter %d.", i + 1);
                return 0;
            }
        }
    }
    return enter_closure_frame(vm, closure, arg_count);
}

// Fast-path direct closure calls when argument shaping/checking is unnecessary.
static int call_closure_positional(VM* vm, ObjClosure* closure, int arg_count) {
    if (try_execute_tier2_template(vm, closure, arg_count)) return 1;
    ObjFunction* function = closure->function;
    if (!function->is_variadic &&
        function->defaults_count == 0 &&
        function->param_types_count == 0 &&
        arg_count == function->arity) {
        if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, arg_count)) {
            return 1;
        }
        return enter_closure_frame(vm, closure, arg_count);
    }
    return call(vm, closure, arg_count);
}

int call_value(VM* vm, Value callee, int arg_count, CallFrame** frame, uint8_t** ip) {
    if (IS_NATIVE(callee)) {
        uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
        ObjThread* thread = vm_current_thread(vm);
        ObjNative* native_obj = AS_NATIVE_OBJ(callee);
        NativeFastKind fast_kind = (NativeFastKind)native_obj->fast_kind;
        NativeFn native = native_obj->function;
        Value* args = thread->stack_top - arg_count;
        thread->stack_top -= arg_count + 1; // Pop args and callee

        if (vm->call_prof_enabled) {
            vm->call_prof_call_value_native_count++;
            if (fast_kind != NATIVE_FAST_NONE) vm->call_prof_native_fast_try_count++;
        }
        (*frame)->ip = *ip;
        int fast = try_fast_native_call(vm, native_obj, arg_count, args);
        if (fast > 0) {
            if (vm->call_prof_enabled) {
                vm->call_prof_call_value_fast_count++;
                vm->call_prof_native_fast_hit_count++;
                switch (fast_kind) {
                    case NATIVE_FAST_STR_UPPER: vm->call_prof_native_fast_str_upper_hit_count++; break;
                    case NATIVE_FAST_STR_LOWER: vm->call_prof_native_fast_str_lower_hit_count++; break;
                    case NATIVE_FAST_STR_REPLACE: vm->call_prof_native_fast_str_replace_hit_count++; break;
                    case NATIVE_FAST_STR_JOIN: vm->call_prof_native_fast_str_join_hit_count++; break;
                    case NATIVE_FAST_STR_FORMAT: vm->call_prof_native_fast_str_format_hit_count++; break;
                    case NATIVE_FAST_CORE_NEXT: vm->call_prof_native_fast_core_next_hit_count++; break;
                    case NATIVE_FAST_CORE_MIN: vm->call_prof_native_fast_core_min_hit_count++; break;
                    case NATIVE_FAST_CORE_MAX: vm->call_prof_native_fast_core_max_hit_count++; break;
                    case NATIVE_FAST_CORE_SUM: vm->call_prof_native_fast_core_sum_hit_count++; break;
                    default: break;
                }
                uint64_t elapsed = vm_prof_now_ns() - start_ns;
                vm->call_prof_call_value_native_fast_ns += elapsed;
                vm->call_prof_call_value_native_ns += elapsed;
            }
            return 1;
        }
        if (fast < 0) {
            if (vm->call_prof_enabled) {
                vm->call_prof_native_fast_error_count++;
                uint64_t elapsed = vm_prof_now_ns() - start_ns;
                vm->call_prof_call_value_native_fast_ns += elapsed;
                vm->call_prof_call_value_native_ns += elapsed;
            }
            return 0;
        }
        uint64_t slow_start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
        if (vm->call_prof_enabled) {
            vm->call_prof_native_slow_count++;
            if (fast_kind != NATIVE_FAST_NONE) vm->call_prof_native_fast_miss_count++;
        }
        ObjThread* current = thread;
        if (!native(vm, arg_count, args)) {
            if (vm->call_prof_enabled) {
                uint64_t slow_elapsed = vm_prof_now_ns() - slow_start_ns;
                vm->call_prof_call_value_native_slow_ns += slow_elapsed;
                vm_prof_record_native_slow(vm, native_obj, slow_elapsed);
                vm->call_prof_call_value_native_ns += vm_prof_now_ns() - start_ns;
            }
            return 0;
        }

        ObjThread* after = vm_current_thread(vm);
        if (after != current) {
            if (vm->call_prof_enabled) vm->call_prof_native_thread_switch_count++;
            *frame = &after->frames[after->frame_count - 1];
            *ip = (*frame)->ip;
        }
        if (vm->call_prof_enabled) {
            uint64_t slow_elapsed = vm_prof_now_ns() - slow_start_ns;
            vm->call_prof_call_value_native_slow_ns += slow_elapsed;
            vm_prof_record_native_slow(vm, native_obj, slow_elapsed);
            vm->call_prof_call_value_native_ns += vm_prof_now_ns() - start_ns;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
        if (vm->call_prof_enabled) {
            vm->call_prof_call_value_closure_count++;
        }
        (*frame)->ip = *ip;
        if (!call_closure_positional(vm, AS_CLOSURE(callee), arg_count)) {
            if (vm->call_prof_enabled) {
                vm->call_prof_call_value_closure_ns += vm_prof_now_ns() - start_ns;
            }
            return 0;
        }
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        if (vm->call_prof_enabled) {
            vm->call_prof_call_value_closure_ns += vm_prof_now_ns() - start_ns;
        }
        return 1;
    }

    if (vm->call_prof_enabled) vm->call_prof_call_value_noncallable_count++;
    return 0;
}

static int create_generator_positional(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* caller = vm_current_thread(vm);
    Value* args_start = caller->stack_top - arg_count;

    ObjThread* gen = new_thread_with_caps(GEN_STACK_MAX, GEN_FRAMES_MAX, GEN_HANDLERS_MAX);
    gen->vm = vm;
    gen->is_generator = 1;
    gen->generator_mode = 0;
    gen->generator_index = 0;

    vm_set_current_thread(vm, gen);
    push(vm, OBJ_VAL(closure));
    for (int i = 0; i < arg_count; i++) {
        push(vm, args_start[i]);
    }
    if (!call(vm, closure, arg_count)) {
        vm_set_current_thread(vm, caller);
        return 0;
    }
    vm_set_current_thread(vm, caller);

    caller->stack_top -= arg_count + 1;
    push(vm, OBJ_VAL(gen));
    return 1;
}

static int create_generator_named(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* caller = vm_current_thread(vm);
    Value* args_start = caller->stack_top - arg_count;

    ObjThread* gen = new_thread_with_caps(GEN_STACK_MAX, GEN_FRAMES_MAX, GEN_HANDLERS_MAX);
    gen->vm = vm;
    gen->is_generator = 1;
    gen->generator_mode = 0;
    gen->generator_index = 0;

    vm_set_current_thread(vm, gen);
    push(vm, OBJ_VAL(closure));
    for (int i = 0; i < arg_count; i++) {
        push(vm, args_start[i]);
    }
    if (!call_named(vm, closure, arg_count)) {
        vm_set_current_thread(vm, caller);
        return 0;
    }
    vm_set_current_thread(vm, caller);

    caller->stack_top -= arg_count + 1;
    push(vm, OBJ_VAL(gen));
    return 1;
}

// Shift positional args right by one slot to insert an implicit first argument.
static inline void shift_args_right(ObjThread* thread, int arg_count) {
    if (arg_count > 0) {
        Value* src = thread->stack_top - arg_count;
        memmove(src + 1, src, (size_t)arg_count * sizeof(Value));
    }
    thread->stack_top++;
}

// Rewrite [callee, arg1..argN] to [target, implicit_first, arg1..argN].
static inline void rewrite_call_with_implicit_first_arg(
    ObjThread* thread, int arg_count, Value target, Value implicit_first) {
    Value* callee_slot = thread->stack_top - arg_count - 1;
    if (arg_count > 0) {
        memmove(callee_slot + 2, callee_slot + 1, (size_t)arg_count * sizeof(Value));
    }
    callee_slot[0] = target;
    callee_slot[1] = implicit_first;
    thread->stack_top++;
}

static inline int value_is_self_callable(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->is_self;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->is_self;
    return 0;
}

static int invoke_call_with_arg_count(VM* vm, int arg_count, CallFrame** frame, uint8_t** ip) {
    ObjThread* thread = vm_current_thread(vm);
    Value callee = peek(vm, arg_count);

    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);

        rewrite_call_with_implicit_first_arg(thread, arg_count, method_val, bound->receiver);
        arg_count++;
        callee = method_val;
    }

    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        ObjFunction* function = closure->function;
        if (try_execute_tier2_template(vm, closure, arg_count)) {
            return 1;
        }
        if (function->is_generator) {
            if (!create_generator_positional(vm, closure, arg_count)) {
                return 0;
            }
            return 1;
        }
        // Fast path: direct frame entry for common positional calls.
        if (!function->is_variadic &&
            function->defaults_count == 0 &&
            arg_count == function->arity) {
            if (function->param_types_count == 0) {
                (*frame)->ip = *ip;
                if (!enter_closure_frame(vm, closure, arg_count)) {
                    return 0;
                }
                *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                *ip = (*frame)->ip;
                return 1;
            }
            if (arg_count == 1 &&
                function->param_types_count == 1 &&
                (function->param_types[0] == TYPEHINT_INT || function->param_types[0] == TYPEHINT_FLOAT)) {
                Value a0 = peek(vm, 0);
                if (value_matches_numeric_hint_fast(a0, function->param_types[0])) {
                    (*frame)->ip = *ip;
                    if (!enter_closure_frame(vm, closure, arg_count)) {
                        return 0;
                    }
                    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                    *ip = (*frame)->ip;
                    return 1;
                }
            }
            if (arg_count == 2 &&
                function->param_types_count == 2 &&
                (function->param_types[0] == TYPEHINT_INT || function->param_types[0] == TYPEHINT_FLOAT) &&
                (function->param_types[1] == TYPEHINT_INT || function->param_types[1] == TYPEHINT_FLOAT)) {
                Value a0 = peek(vm, 1);
                Value a1 = peek(vm, 0);
                if (value_matches_numeric_hint_fast(a0, function->param_types[0]) &&
                    value_matches_numeric_hint_fast(a1, function->param_types[1])) {
                    (*frame)->ip = *ip;
                    if (!enter_closure_frame(vm, closure, arg_count)) {
                        return 0;
                    }
                    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                    *ip = (*frame)->ip;
                    return 1;
                }
            }
        }
        (*frame)->ip = *ip;
        if (!call(vm, closure, arg_count)) return 0;
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_NATIVE(callee)) {
        if (!call_value(vm, callee, arg_count, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_TABLE(callee)) {
        // __call metamethod: __call(table, ...)
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            rewrite_call_with_implicit_first_arg(thread, arg_count, mm, callee);
            arg_count++;
            if (!call_value(vm, mm, arg_count, frame, ip)) {
                return 0;
            }
            return 1;
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static int invoke_call0(VM* vm, CallFrame** frame, uint8_t** ip) {
    ObjThread* thread = vm_current_thread(vm);
    int arg_count = 0;
    Value callee = thread->stack_top[-1];
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);
        thread->stack_top[0] = bound->receiver;
        thread->stack_top[-1] = method_val;
        thread->stack_top++;
        arg_count = 1;
        callee = method_val;
    }

    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        if (closure->function->is_generator) {
            return create_generator_positional(vm, closure, arg_count);
        }
        (*frame)->ip = *ip;
        if (!call_closure_positional(vm, closure, arg_count)) {
            return 0;
        }
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_NATIVE(callee)) {
        Value* args = thread->stack_top - arg_count;
        thread->stack_top -= arg_count + 1; // pop args and callee
        return call_native_direct(vm, AS_NATIVE_OBJ(callee), arg_count, args, frame, ip);
    }

    if (IS_TABLE(callee)) {
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            thread->stack_top[0] = callee;
            thread->stack_top[-1] = mm;
            thread->stack_top++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!call(vm, AS_CLOSURE(mm), 1)) {
                    return 0;
                }
                *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                *ip = (*frame)->ip;
                return 1;
            }
            return call_value(vm, mm, 1, frame, ip);
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static int invoke_call1(VM* vm, CallFrame** frame, uint8_t** ip) {
    ObjThread* thread = vm_current_thread(vm);
    int arg_count = 1;
    Value callee = thread->stack_top[-2];

    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);
        thread->stack_top[0] = thread->stack_top[-1];
        thread->stack_top[-1] = bound->receiver;
        thread->stack_top[-2] = method_val;
        thread->stack_top++;
        arg_count = 2;
        callee = method_val;
    }

    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        if (closure->function->is_generator) {
            return create_generator_positional(vm, closure, arg_count);
        }
        (*frame)->ip = *ip;
        if (!call_closure_positional(vm, closure, arg_count)) {
            return 0;
        }
        *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_NATIVE(callee)) {
        Value* args = thread->stack_top - arg_count;
        thread->stack_top -= arg_count + 1; // pop args and callee
        return call_native_direct(vm, AS_NATIVE_OBJ(callee), arg_count, args, frame, ip);
    }

    if (IS_TABLE(callee)) {
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            thread->stack_top[0] = thread->stack_top[-1];
            thread->stack_top[-1] = callee;
            thread->stack_top[-2] = mm;
            thread->stack_top++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!call(vm, AS_CLOSURE(mm), 2)) {
                    return 0;
                }
                *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                *ip = (*frame)->ip;
                return 1;
            }
            return call_value(vm, mm, 2, frame, ip);
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static int invoke_call2(VM* vm, CallFrame** frame, uint8_t** ip) {
    ObjThread* thread = vm_current_thread(vm);
    int arg_count = 2;
    Value callee = thread->stack_top[-3];

    // Hot path for ordinary positional closure/native calls.
    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        if (closure->function->is_generator) {
            return create_generator_positional(vm, closure, arg_count);
        }
        (*frame)->ip = *ip;
        if (!call_closure_positional(vm, closure, arg_count)) {
            return 0;
        }
        *frame = &thread->frames[thread->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_NATIVE(callee)) {
        Value* args = thread->stack_top - arg_count;
        thread->stack_top -= arg_count + 1; // pop args and callee
        return call_native_direct(vm, AS_NATIVE_OBJ(callee), arg_count, args, frame, ip);
    }

    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);
        thread->stack_top[0] = thread->stack_top[-1];
        thread->stack_top[-1] = thread->stack_top[-2];
        thread->stack_top[-2] = bound->receiver;
        thread->stack_top[-3] = method_val;
        thread->stack_top++;
        arg_count = 3;
        callee = method_val;
    }

    if (IS_CLOSURE(callee)) {
        ObjClosure* closure = AS_CLOSURE(callee);
        if (closure->function->is_generator) {
            return create_generator_positional(vm, closure, arg_count);
        }
        (*frame)->ip = *ip;
        if (!call_closure_positional(vm, closure, arg_count)) {
            return 0;
        }
        *frame = &thread->frames[thread->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_NATIVE(callee)) {
        return call_value(vm, callee, arg_count, frame, ip);
    }

    if (IS_TABLE(callee)) {
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            thread->stack_top[0] = thread->stack_top[-1];
            thread->stack_top[-1] = thread->stack_top[-2];
            thread->stack_top[-2] = callee;
            thread->stack_top[-3] = mm;
            thread->stack_top++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!call(vm, AS_CLOSURE(mm), 3)) {
                    return 0;
                }
                *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                *ip = (*frame)->ip;
                return 1;
            }
            return call_value(vm, mm, 3, frame, ip);
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static int invoke_call_with_named_arg_count(VM* vm, int arg_count, CallFrame** frame, uint8_t** ip) {
    ObjThread* thread = vm_current_thread(vm);
    Value callee = peek(vm, arg_count);
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value method_val = OBJ_VAL(bound->method);

        rewrite_call_with_implicit_first_arg(thread, arg_count, method_val, bound->receiver);
        arg_count++;
        callee = method_val;
    }

    if (IS_NATIVE(callee)) {
        if (!call_value(vm, callee, arg_count, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        if (AS_CLOSURE(callee)->function->is_generator) {
            if (!create_generator_named(vm, AS_CLOSURE(callee), arg_count)) {
                return 0;
            }
            return 1;
        }
        (*frame)->ip = *ip;
        if (!call_named(vm, AS_CLOSURE(callee), arg_count)) {
            return 0;
        }
        thread = vm_current_thread(vm);
        *frame = &thread->frames[thread->frame_count - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_TABLE(callee)) {
        Value mm = get_metamethod_cached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            rewrite_call_with_implicit_first_arg(thread, arg_count, mm, callee);
            arg_count++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!call_named(vm, AS_CLOSURE(mm), arg_count)) {
                    return 0;
                }
                thread = vm_current_thread(vm);
                *frame = &thread->frames[thread->frame_count - 1];
                *ip = (*frame)->ip;
            } else {
                if (!call_value(vm, mm, arg_count, frame, ip)) {
                    return 0;
                }
            }
            return 1;
        }
    }

    vm_runtime_error(vm, "Can only call functions.");
    return 0;
}

static void discard_handlers_for_frame_return(ObjThread* thread) {
    int current_frame_count = thread->frame_count;
    while (thread->handler_count > 0 &&
           thread->handlers[thread->handler_count - 1].frame_count >= current_frame_count) {
        thread->handler_count--;
    }
}

static Value get_metamethod_cached(VM* vm, Value val, ObjString* name) {
    (void)vm;
    Value method = NIL_VAL;
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            table_get(&table->metatable->table, name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            table_get(&udata->metatable->table, name, &method);
        }
    }
    return method;
}

static ObjUpvalue* capture_upvalue(VM* vm, Value* local) {
    ObjUpvalue* prev_upvalue = NULL;
    ObjUpvalue* upvalue = vm_current_thread(vm)->open_upvalues;

    // Find existing upvalue or position to insert
    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    // If we found an existing upvalue for this slot, reuse it
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    // Create new upvalue
    ObjUpvalue* created_upvalue = new_upvalue(local);
    created_upvalue->next = upvalue;

    if (prev_upvalue == NULL) {
        vm_current_thread(vm)->open_upvalues = created_upvalue;
    } else {
        prev_upvalue->next = created_upvalue;
    }

    return created_upvalue;
}

static void close_upvalues(VM* vm, Value* last) {

    while (vm_current_thread(vm)->open_upvalues != NULL &&

           vm_current_thread(vm)->open_upvalues->location >= last) {

        ObjUpvalue* upvalue = vm_current_thread(vm)->open_upvalues;

        upvalue->closed = *upvalue->location;

        upvalue->location = &upvalue->closed;

        vm_current_thread(vm)->open_upvalues = upvalue->next;

    }

}

int call(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* thread = vm_current_thread(vm);
    ObjFunction* function = closure->function;

    if (try_execute_tier2_template(vm, closure, arg_count)) {
        return 1;
    }

    // Hot path: exact-arity, non-variadic calls avoid argument shaping.
    if (!function->is_variadic &&
        function->defaults_count == 0 &&
        arg_count == function->arity) {
        if (function->param_types_count == 0) {
            return enter_closure_frame(vm, closure, arg_count);
        }
        if (arg_count == 1 &&
            function->param_types_count == 1 &&
            (function->param_types[0] == TYPEHINT_INT || function->param_types[0] == TYPEHINT_FLOAT)) {
            Value a0 = peek(vm, 0);
            if (value_matches_numeric_hint_fast(a0, function->param_types[0])) {
                return enter_closure_frame(vm, closure, arg_count);
            }
        }
        if (arg_count == 2 &&
            function->param_types_count == 2 &&
            (function->param_types[0] == TYPEHINT_INT || function->param_types[0] == TYPEHINT_FLOAT) &&
            (function->param_types[1] == TYPEHINT_INT || function->param_types[1] == TYPEHINT_FLOAT)) {
            Value a0 = peek(vm, 1);
            Value a1 = peek(vm, 0);
            if (value_matches_numeric_hint_fast(a0, function->param_types[0]) &&
                value_matches_numeric_hint_fast(a1, function->param_types[1])) {
                return enter_closure_frame(vm, closure, arg_count);
            }
        }
    }

    // Handle variadic functions
    if (function->is_variadic) {
        int required_args = function->arity - 1;  // Minus the varargs parameter

        if (arg_count < required_args) {
            vm_runtime_error(vm, "Expected at least %d arguments but got %d.", required_args, arg_count);
            return 0;
        }

        // Collect extra arguments into a table
        int extra_args = arg_count - required_args;
        ObjTable* varargs = new_table();

        // Pop extra args and put them in the varargs array part so
        // call-spread (`fn(*args)`) can iterate them via table_get_array.
        for (int i = 0; i < extra_args; i++) {
            Value arg = thread->stack_top[-extra_args + i];
            table_set_array(&varargs->table, i + 1, arg);
            #ifdef DEBUG_VARIADIC
            printf("Vararg[%d] = ", i + 1);
            print_value(arg);
            printf("\n");
            #endif
        }

        // Remove the extra args from the stack
        thread->stack_top -= extra_args;

        // Push the varargs table as the last argument
        *thread->stack_top++ = OBJ_VAL(varargs);

        #ifdef DEBUG_VARIADIC
        printf("Created varargs table with %d args\n", extra_args);
        #endif
        arg_count = function->arity;
    } else {
        // Non-variadic function handling
        if (arg_count > function->arity) {
            vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, arg_count);
            return 0;
        }

        if (arg_count < function->arity) {
            if (function->defaults_count == 0) {
                vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, arg_count);
                return 0;
            }

            int default_start = function->arity - function->defaults_count;
            if (arg_count < default_start) {
                vm_runtime_error(vm, "Expected at least %d arguments (non-default parameters) but got %d.", default_start, arg_count);
                return 0;
            }

            for (int i = arg_count; i < function->arity; i++) {
                *thread->stack_top++ = function->defaults[i - default_start];
            }
            arg_count = function->arity;
        }
    }

    return finish_closure_call(vm, closure, arg_count);
}

static int objstring_equals_vm(ObjString* a, ObjString* b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (objstring_hash(a) != objstring_hash(b)) return 0;
    if (a->length != b->length) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

static int build_named_param_ic(ObjFunction* function, int non_variadic_arity) {
    if (function->param_names == NULL || non_variadic_arity <= 0) {
        function->named_param_ic_arity = non_variadic_arity;
        return 1;
    }

    if (function->named_param_ic_arity == non_variadic_arity &&
        function->named_param_ic_keys != NULL &&
        function->named_param_ic_values != NULL &&
        function->named_param_ic_capacity > 0) {
        return 1;
    }

    if (function->named_param_ic_keys != NULL) {
        free(function->named_param_ic_keys);
        function->named_param_ic_keys = NULL;
    }
    if (function->named_param_ic_values != NULL) {
        free(function->named_param_ic_values);
        function->named_param_ic_values = NULL;
    }
    function->named_param_ic_capacity = 0;

    int limit = function->param_names_count < non_variadic_arity ? function->param_names_count : non_variadic_arity;
    if (limit <= 0) {
        function->named_param_ic_arity = non_variadic_arity;
        return 1;
    }

    int capacity = 8;
    while (capacity < limit * 2) capacity *= 2;

    ObjString** keys = (ObjString**)calloc((size_t)capacity, sizeof(ObjString*));
    int* values = (int*)malloc(sizeof(int) * (size_t)capacity);
    if (keys == NULL || values == NULL) {
        free(keys);
        free(values);
        return 0;
    }
    for (int i = 0; i < capacity; i++) values[i] = -1;

    int mask = capacity - 1;
    for (int i = 0; i < limit; i++) {
        ObjString* name = function->param_names[i];
        if (name == NULL) continue;

        int idx = (int)(objstring_hash(name) & (uint32_t)mask);
        for (;;) {
            if (keys[idx] == NULL) {
                keys[idx] = name;
                values[idx] = i;
                break;
            }
            idx = (idx + 1) & mask;
        }
    }

    function->named_param_ic_keys = keys;
    function->named_param_ic_values = values;
    function->named_param_ic_capacity = capacity;
    function->named_param_ic_arity = non_variadic_arity;
    return 1;
}

static int find_named_param_index_linear(ObjFunction* function, ObjString* key, int non_variadic_arity) {
    if (function->param_names == NULL) return -1;
    int limit = function->param_names_count < non_variadic_arity ? function->param_names_count : non_variadic_arity;
    if (limit <= 0) return -1;

    for (int i = 0; i < limit; i++) {
        ObjString* name = function->param_names[i];
        if (name == NULL) continue;
        if (objstring_equals_vm(name, key)) return i;
    }
    return -1;
}

static int is_options_param_name(ObjString* name) {
    if (name == NULL) return 0;
    if (name->length == 4 && memcmp(name->chars, "opts", 4) == 0) return 1;
    if (name->length == 7 && memcmp(name->chars, "options", 7) == 0) return 1;
    if (name->length == 6 && memcmp(name->chars, "kwargs", 6) == 0) return 1;
    return 0;
}

static int call_named(VM* vm, ObjClosure* closure, int arg_count) {
    ObjThread* thread = vm_current_thread(vm);
    ObjFunction* function = closure->function;
    if (arg_count < 1) {
        vm_runtime_error(vm, "Named call requires a named-arguments table.");
        return 0;
    }

    Value named_value = thread->stack_top[-1];
    if (!IS_TABLE(named_value)) {
        vm_runtime_error(vm, "Named call requires a table as final argument.");
        return 0;
    }

    ObjTable* named_args = AS_TABLE(named_value);
    int positional_count = arg_count - 1;
    int non_variadic_arity = function->is_variadic ? (function->arity - 1) : function->arity;
    if (non_variadic_arity < 0) non_variadic_arity = 0;

    Value* incoming = thread->stack_top - arg_count;
    Value bound_args[UINT8_MAX + 1];
    uint8_t assigned[UINT8_MAX + 1];
    memset(assigned, 0, sizeof(assigned));

    ObjTable* varargs = NULL;
    int vararg_pos = 0;
    if (function->is_variadic) {
        varargs = new_table();
    }
    ObjTable* legacy_options = NULL;
    ObjString* first_unexpected = NULL;

    int positional_to_bind = positional_count < non_variadic_arity ? positional_count : non_variadic_arity;
    for (int i = 0; i < positional_to_bind; i++) {
        bound_args[i] = incoming[i];
        assigned[i] = 1;
    }

    int use_named_ic = 0;
    ObjString** named_ic_keys = NULL;
    int* named_ic_values = NULL;
    int named_ic_mask = 0;
    if (non_variadic_arity >= 4 &&
        build_named_param_ic(function, non_variadic_arity) &&
        function->named_param_ic_keys != NULL &&
        function->named_param_ic_values != NULL &&
        function->named_param_ic_capacity > 0) {
        use_named_ic = 1;
        named_ic_keys = function->named_param_ic_keys;
        named_ic_values = function->named_param_ic_values;
        named_ic_mask = function->named_param_ic_capacity - 1;
    }

    if (positional_count > non_variadic_arity) {
        if (!function->is_variadic) {
            vm_runtime_error(vm, "Expected %d arguments but got %d.", function->arity, positional_count);
            return 0;
        }
        for (int i = non_variadic_arity; i < positional_count; i++) {
            table_set_array(&varargs->table, ++vararg_pos, incoming[i]);
        }
    }

    for (int i = 0; i < named_args->table.capacity; i++) {
        Entry* entry = &named_args->table.entries[i];
        if (entry->key == NULL) continue;

        int index = -1;
        if (use_named_ic) {
            int idx = (int)(objstring_hash(entry->key) & (uint32_t)named_ic_mask);
            for (;;) {
                ObjString* probe = named_ic_keys[idx];
                if (probe == NULL) break;
                if (objstring_equals_vm(probe, entry->key)) {
                    index = named_ic_values[idx];
                    break;
                }
                idx = (idx + 1) & named_ic_mask;
            }
        } else {
            index = find_named_param_index_linear(function, entry->key, non_variadic_arity);
        }
        if (index >= 0) {
            if (assigned[index]) {
                vm_runtime_error(vm, "Multiple values for argument '%s'.", entry->key->chars);
                return 0;
            }
            bound_args[index] = entry->value;
            assigned[index] = 1;
            continue;
        }

        if (function->is_variadic) {
            table_set(&varargs->table, entry->key, entry->value);
            continue;
        }

        if (legacy_options == NULL) {
            legacy_options = new_table();
            first_unexpected = entry->key;
        }
        table_set(&legacy_options->table, entry->key, entry->value);
    }

    if (legacy_options != NULL) {
        int target = -1;
        for (int i = 0; i < non_variadic_arity; i++) {
            if (assigned[i]) continue;
            if (function->param_names != NULL && i < function->param_names_count &&
                is_options_param_name(function->param_names[i])) {
                target = i;
                break;
            }
        }
        if (target >= 0) {
            bound_args[target] = OBJ_VAL(legacy_options);
            assigned[target] = 1;
        } else {
            vm_runtime_error(vm, "Unexpected named argument '%s'.", first_unexpected->chars);
            return 0;
        }
    }

    int default_start = non_variadic_arity - function->defaults_count;
    for (int i = 0; i < non_variadic_arity; i++) {
        if (assigned[i]) continue;
        if (i >= default_start) {
            bound_args[i] = function->defaults[i - default_start];
            assigned[i] = 1;
            continue;
        }
        if (function->param_names != NULL && i < function->param_names_count && function->param_names[i] != NULL) {
            vm_runtime_error(vm, "Missing required argument '%s'.", function->param_names[i]->chars);
        } else {
            vm_runtime_error(vm, "Missing required argument %d.", i + 1);
        }
        return 0;
    }

    if (function->is_variadic) {
        bound_args[non_variadic_arity] = OBJ_VAL(varargs);
    }

    thread->stack_top -= arg_count;
    for (int i = 0; i < function->arity; i++) {
        *thread->stack_top++ = bound_args[i];
    }

    return finish_closure_call(vm, closure, function->arity);
}

static void mark_roots(VM* vm) {
    if (vm_current_thread(vm) != NULL) {
        mark_object((struct Obj*)vm_current_thread(vm));
    }
    for (ObjThread* parked = vm->gc_parked_threads; parked != NULL; parked = parked->gc_park_next) {
        mark_object((struct Obj*)parked);
    }
    ObjString* rooted_strings[] = {
        vm->mm_index,
        vm->mm_newindex,
        vm->mm_str,
        vm->mm_call,
        vm->mm_new,
        vm->mm_append,
        vm->mm_next,
        vm->mm_slice,
        vm->str_module_name,
        vm->str_upper_name,
        vm->str_lower_name,
        vm->str_find_name,
        vm->str_replace_name,
        vm->slice_name,
        vm->module_name_key,
        vm->module_file_key,
        vm->module_main_key,
    };
    for (size_t i = 0; i < sizeof(rooted_strings) / sizeof(rooted_strings[0]); i++) {
        if (rooted_strings[i] != NULL) {
            mark_object((struct Obj*)rooted_strings[i]);
        }
    }

    Value* rooted_values[] = {
        &vm->str_upper_fn,
        &vm->str_lower_fn,
        &vm->str_find_fn,
        &vm->str_replace_fn,
    };
    for (size_t i = 0; i < sizeof(rooted_values) / sizeof(rooted_values[0]); i++) {
        if (!IS_NIL(*rooted_values[i])) {
            mark_value(*rooted_values[i]);
        }
    }
    // Mark globals
    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL) {
            mark_object((struct Obj*)entry->key);
            mark_value(entry->value);
        }
    }
    for (int i = 0; i < vm->modules.capacity; i++) {
        Entry* entry = &vm->modules.entries[i];
        if (entry->key != NULL) {
            mark_object((struct Obj*)entry->key);
            mark_value(entry->value);
        }
    }
    object_mark_interned_strings();
    toi_jit_mark_roots(vm);
    for (size_t i = 0; i < sizeof(vm->build_string_cache) / sizeof(vm->build_string_cache[0]); i++) {
        if (vm->build_string_cache[i] != NULL) {
            mark_object((struct Obj*)vm->build_string_cache[i]);
        }
    }
    for (size_t i = 0; i < sizeof(vm->build_string3_cache) / sizeof(vm->build_string3_cache[0]); i++) {
        BuildString3CacheEntry* e = &vm->build_string3_cache[i];
        if (e->a != NULL) mark_object((struct Obj*)e->a);
        if (e->b != NULL) mark_object((struct Obj*)e->b);
        if (e->c != NULL) mark_object((struct Obj*)e->c);
        if (e->out != NULL) mark_object((struct Obj*)e->out);
    }
}

void init_vm(VM* vm) {
   vm->current_thread = new_thread(); // Main thread root/fallback
   vm_set_current_thread(vm, vm->current_thread);
   vm_current_thread(vm)->vm = vm;
   vm->gc_parked_threads = NULL;
   vm->use_thread_tls = 0;
   vm->disable_gc = 0;
   vm->gc_check_interval = 64;
   vm->gc_check_counter = 64;
   vm->is_repl = 0;
   vm->mm_index = NULL;
   vm->mm_newindex = NULL;
   vm->mm_str = NULL;
   vm->mm_call = NULL;
   vm->mm_new = NULL;
   vm->mm_append = NULL;
   vm->mm_next = NULL;
   vm->mm_slice = NULL;
   vm->str_module_name = NULL;
   vm->str_upper_name = NULL;
   vm->str_lower_name = NULL;
   vm->str_find_name = NULL;
   vm->str_replace_name = NULL;
   vm->slice_name = NULL;
   vm->module_name_key = NULL;
   vm->module_file_key = NULL;
   vm->module_main_key = NULL;
   vm->str_upper_fn = NIL_VAL;
   vm->str_lower_fn = NIL_VAL;
   vm->str_find_fn = NIL_VAL;
   vm->str_replace_fn = NIL_VAL;
   vm->tiered_jit_enabled = 1;
   vm->tiered_jit_profile = 0;
   vm->tier1_hot_threshold = 2000;
   vm->tier1_promote_count = 0;
   vm->tier1_call2_patch_count = 0;
   vm->tier2_hot_threshold = 12000;
   vm->tier2_inline_hot_threshold = 0;
   vm->tier2_promote_count = 0;
   vm->tier2_exec_count = 0;
   vm->native_jit_enabled = 1;
   vm->native_jit_hot_candidates = 0;
   vm->native_jit_compile_attempts = 0;
   vm->native_jit_compiled_count = 0;
   vm->native_jit_exec_attempts = 0;
   vm->native_jit_exec_hits = 0;
   vm->native_jit = NULL;
   vm->opcode_prof_enabled = 0;
   for (int i = 0; i < 256; i++) {
       vm->opcode_prof_count[i] = 0;
       vm->opcode_prof_ns[i] = 0;
   }
   vm->gc_prof_enabled = 0;
   vm->gc_prof_start_ns = vm_prof_now_ns();
   vm->gc_prof_collect_count = 0;
   vm->gc_prof_auto_count = 0;
   vm->gc_prof_manual_count = 0;
   vm->gc_prof_final_count = 0;
   vm->gc_prof_total_ns = 0;
   vm->gc_prof_mark_ns = 0;
   vm->gc_prof_sweep_ns = 0;
   vm->gc_prof_max_ns = 0;
   vm->gc_prof_auto_trigger_checks = 0;
   vm->gc_prof_auto_trigger_hits = 0;
   vm->gc_prof_bytes_before_sum = 0;
   vm->gc_prof_bytes_after_sum = 0;
   vm->gc_prof_bytes_freed_sum = 0;
   vm->call_prof_enabled = 0;
   vm->call_prof_op_call_count = 0;
   vm->call_prof_op_call0_count = 0;
   vm->call_prof_op_call1_count = 0;
   vm->call_prof_op_call2_count = 0;
   vm->call_prof_op_qcall2_count = 0;
   vm->call_prof_op_call_named_count = 0;
   vm->call_prof_op_call_expand_count = 0;
   vm->call_prof_op_call_ns = 0;
   vm->call_prof_op_call0_ns = 0;
   vm->call_prof_op_call1_ns = 0;
   vm->call_prof_op_call2_ns = 0;
   vm->call_prof_op_call_named_ns = 0;
   vm->call_prof_op_call_expand_ns = 0;
   vm->call_prof_call_ic_hit_count = 0;
   vm->call_prof_call_ic_miss_count = 0;
   vm->call_prof_call_direct_fast_count = 0;
   vm->call_prof_call_fallback_count = 0;
   vm->call_prof_call_expand_named_count = 0;
   vm->call_prof_call_expand_positional_count = 0;
   vm->call_prof_call_expand_spread_values = 0;
   vm->call_prof_call_value_native_count = 0;
   vm->call_prof_call_value_closure_count = 0;
   vm->call_prof_call_value_fast_count = 0;
   vm->call_prof_call_value_noncallable_count = 0;
   vm->call_prof_call_value_native_ns = 0;
   vm->call_prof_call_value_closure_ns = 0;
   vm->call_prof_call_value_native_fast_ns = 0;
   vm->call_prof_call_value_native_slow_ns = 0;
   vm->call_prof_native_fast_try_count = 0;
   vm->call_prof_native_fast_hit_count = 0;
   vm->call_prof_native_fast_miss_count = 0;
   vm->call_prof_native_fast_error_count = 0;
   vm->call_prof_native_slow_count = 0;
   vm->call_prof_native_thread_switch_count = 0;
   vm->call_prof_native_fast_str_upper_hit_count = 0;
   vm->call_prof_native_fast_str_lower_hit_count = 0;
   vm->call_prof_native_fast_str_replace_hit_count = 0;
   vm->call_prof_native_fast_str_join_hit_count = 0;
   vm->call_prof_native_fast_str_format_hit_count = 0;
   vm->call_prof_native_fast_core_next_hit_count = 0;
   vm->call_prof_native_fast_core_min_hit_count = 0;
   vm->call_prof_native_fast_core_max_hit_count = 0;
   vm->call_prof_native_fast_core_sum_hit_count = 0;
   vm->call_prof_call2_ic_hit_count = 0;
   vm->call_prof_call2_ic_miss_count = 0;
   vm->call_prof_call2_direct_fast_count = 0;
   vm->call_prof_call2_typed_fast_count = 0;
   vm->call_prof_call2_tiny_inline_fast_count = 0;
   vm->call_prof_call2_fallback_count = 0;
   vm->call_prof_call0_direct_fast_count = 0;
   vm->call_prof_call0_bound_native_fast_count = 0;
   vm->call_prof_call0_native_count = 0;
   vm->call_prof_call0_fallback_count = 0;
   vm->call_prof_call1_direct_fast_count = 0;
   vm->call_prof_call1_typed_fast_count = 0;
   vm->call_prof_call1_native_count = 0;
   vm->call_prof_call1_fallback_count = 0;
   for (int i = 0; i < 16; i++) {
       vm->call_prof_native_slow_keys[i] = NULL;
       vm->call_prof_native_slow_call_counts[i] = 0;
       vm->call_prof_native_slow_ns_by_key[i] = 0;
   }
   vm->table_prof_enabled = 0;
   vm->table_site_prof_enabled = 0;
   vm->table_prof_get_ic_exact_hit_count = 0;
   vm->table_prof_get_ic_slot_hit_count = 0;
   vm->table_prof_get_ic_miss_count = 0;
   vm->table_prof_get_generic_lookup_count = 0;
   vm->table_prof_get_meta_index_count = 0;
   vm->table_prof_set_ic_exact_hit_count = 0;
   vm->table_prof_set_ic_slot_hit_count = 0;
   vm->table_prof_set_ic_miss_count = 0;
   vm->table_prof_set_generic_store_count = 0;
   vm->table_prof_set_meta_newindex_count = 0;

   const char* call_prof = getenv("TOI_VM_CALL_PROFILE");
   if (call_prof != NULL && call_prof[0] != '\0' && strcmp(call_prof, "0") != 0) {
       vm->call_prof_enabled = 1;
   }
   const char* table_prof = getenv("TOI_VM_TABLE_PROFILE");
   if (table_prof == NULL || table_prof[0] == '\0' || strcmp(table_prof, "0") == 0) {
       table_prof = getenv("TOI_TABLE_PROFILE");
   }
   if (table_prof != NULL && table_prof[0] != '\0' && strcmp(table_prof, "0") != 0) {
       vm->table_prof_enabled = 1;
   }
   const char* table_site_prof = getenv("TOI_VM_TABLE_SITE_PROFILE");
   if (table_site_prof == NULL || table_site_prof[0] == '\0' || strcmp(table_site_prof, "0") == 0) {
       table_site_prof = getenv("TOI_TABLE_SITE_PROFILE");
   }
   if (table_site_prof != NULL && table_site_prof[0] != '\0' && strcmp(table_site_prof, "0") != 0) {
       vm->table_site_prof_enabled = 1;
   }
   const char* gc_prof = getenv("TOI_GC_PROFILE");
   if (gc_prof == NULL || gc_prof[0] == '\0' || strcmp(gc_prof, "0") == 0) {
       gc_prof = getenv("TOI_VM_GC_PROFILE");
   }
   if (gc_prof != NULL && gc_prof[0] != '\0' && strcmp(gc_prof, "0") != 0) {
       vm->gc_prof_enabled = 1;
   }
   const char* opcode_prof = getenv("TOI_VM_OPCODE_PROFILE");
   if (opcode_prof != NULL && opcode_prof[0] != '\0' && strcmp(opcode_prof, "0") != 0) {
       vm->opcode_prof_enabled = 1;
   }
   object_prof_reset();
   const char* alloc_prof = getenv("TOI_VM_ALLOC_PROFILE");
   if (alloc_prof == NULL || alloc_prof[0] == '\0' || strcmp(alloc_prof, "0") == 0) {
       alloc_prof = getenv("TOI_ALLOC_PROFILE");
   }
   if (alloc_prof != NULL && alloc_prof[0] != '\0' && strcmp(alloc_prof, "0") != 0) {
       object_prof_set_enabled(1);
   } else {
       object_prof_set_enabled(0);
   }
   const char* jit = getenv("TOI_JIT");
   if (jit != NULL && jit[0] != '\0') {
       vm->native_jit_enabled = (strcmp(jit, "0") != 0);
   }
   const char* gc_check_interval = getenv("TOI_GC_CHECK_INTERVAL");
   if (gc_check_interval != NULL && gc_check_interval[0] != '\0') {
       unsigned long parsed = strtoul(gc_check_interval, NULL, 10);
       if (parsed > 0 && parsed <= 4096) {
           vm->gc_check_interval = (uint32_t)parsed;
           vm->gc_check_counter = (uint32_t)parsed;
       }
   }

    init_table(&vm->globals);
    init_table(&vm->modules);
    object_intern_init();
    vm->cli_argc = 0;
    vm->cli_argv = NULL;
    for (size_t i = 0; i < sizeof(vm->build_string_cache) / sizeof(vm->build_string_cache[0]); i++) {
        vm->build_string_cache[i] = NULL;
    }
    for (size_t i = 0; i < sizeof(vm->build_string3_cache) / sizeof(vm->build_string3_cache[0]); i++) {
        vm->build_string3_cache[i].a = NULL;
        vm->build_string3_cache[i].b = NULL;
        vm->build_string3_cache[i].c = NULL;
        vm->build_string3_cache[i].out = NULL;
    }
   vm_current_thread(vm)->open_upvalues = NULL;

    vm->mm_index = copy_string("__index", 7);
    vm->mm_newindex = copy_string("__newindex", 10);
    vm->mm_str = copy_string("__str", 5);
    vm->mm_call = copy_string("__call", 6);
    vm->mm_new = copy_string("__new", 5);
    vm->mm_append = copy_string("__append", 8);
    vm->mm_next = copy_string("__next", 6);
    vm->mm_slice = copy_string("__slice", 7);
    vm->str_module_name = copy_string("string", 6);
    vm->str_upper_name = copy_string("upper", 5);
    vm->str_lower_name = copy_string("lower", 5);
    vm->str_find_name = copy_string("find", 4);
    vm->str_replace_name = copy_string("replace", 7);
    vm->slice_name = copy_string("slice", 5);
    vm->module_name_key = copy_string("__name", 6);
    vm->module_file_key = copy_string("__file", 6);
    vm->module_main_key = copy_string("__main", 6);
    toi_jit_init(vm);

    // Register built-in native functions (from libs module)
    register_libs(vm);
}

void free_vm(VM* vm) {
   vm_prof_print(vm);
   toi_jit_free(vm);
   object_intern_free();
   free_table(&vm->globals);
   free_table(&vm->modules);
   // The current_thread will be freed as part of GC if it's reachable.
    // We manually free the main thread if it's not part of GC collection.
    // Since we explicitly control it, we can free it here.
    // free_object((struct Obj*)vm_current_thread(vm));
    vm_set_current_thread(vm, NULL);

    collect_garbage_reason(vm, VM_GC_REASON_FINAL); // Final garbage collection
#ifdef DEBUG_LOG_GC
    printf("-- GC DONE --\n");
#endif
}

static void collect_garbage_reason(VM* vm, int reason) {
    extern size_t bytes_allocated;
    uint64_t start_ns = 0;
    uint64_t mark_start_ns = 0;
    uint64_t sweep_start_ns = 0;
    size_t before_bytes = 0;
    if (vm->gc_prof_enabled) {
        start_ns = vm_prof_now_ns();
        before_bytes = bytes_allocated;
        vm->gc_prof_collect_count++;
        if (reason == VM_GC_REASON_AUTO) vm->gc_prof_auto_count++;
        else if (reason == VM_GC_REASON_MANUAL) vm->gc_prof_manual_count++;
        else if (reason == VM_GC_REASON_FINAL) vm->gc_prof_final_count++;
        mark_start_ns = vm_prof_now_ns();
    }
    mark_roots(vm);
    if (vm->gc_prof_enabled) {
        vm->gc_prof_mark_ns += vm_prof_now_ns() - mark_start_ns;
        sweep_start_ns = vm_prof_now_ns();
    }
    sweep_objects();
    if (vm->gc_prof_enabled) {
        uint64_t elapsed = vm_prof_now_ns() - start_ns;
        size_t after_bytes = bytes_allocated;
        vm->gc_prof_sweep_ns += vm_prof_now_ns() - sweep_start_ns;
        vm->gc_prof_total_ns += elapsed;
        if (elapsed > vm->gc_prof_max_ns) vm->gc_prof_max_ns = elapsed;
        vm->gc_prof_bytes_before_sum += (uint64_t)before_bytes;
        vm->gc_prof_bytes_after_sum += (uint64_t)after_bytes;
        if (before_bytes > after_bytes) {
            vm->gc_prof_bytes_freed_sum += (uint64_t)(before_bytes - after_bytes);
        }
    }
}

void collect_garbage(VM* vm) {
    collect_garbage_reason(vm, VM_GC_REASON_MANUAL);
}

void maybe_collect_garbage_slow(VM* vm) {
    extern size_t bytes_allocated;
    extern size_t next_gc;
    if (vm->gc_prof_enabled) vm->gc_prof_auto_trigger_checks++;
    if (bytes_allocated > next_gc) {
        if (vm->gc_prof_enabled) vm->gc_prof_auto_trigger_hits++;
        collect_garbage_reason(vm, VM_GC_REASON_AUTO);
    }
}

void define_native(VM* vm, const char* name, NativeFn function) {
    ObjString* name_str = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(name_str));
    push(vm, OBJ_VAL(new_native(function, name_str)));
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // Native function.
    pop(vm); // Native name.
}

ObjString* number_key_string(double num) {
    if (num == 0) num = 0.0; // normalize -0 to 0
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%.17g", num);
    char buffer[2 + 32];
    buffer[0] = '\x1F';
    buffer[1] = 'n';
    size_t len = strlen(numbuf);
    memcpy(buffer + 2, numbuf, len);
    return copy_string(buffer, (int)(2 + len));
}

Value get_metamethod(VM* vm, Value val, const char* name) {
    ObjString* method_name = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(method_name)); // Protect from GC
    Value method = NIL_VAL;

    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            table_get(&table->metatable->table, method_name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            table_get(&udata->metatable->table, method_name, &method);
        }
    }
    pop(vm); // Pop method_name
    return method;
}

static ObjThread* run_stop_thread = NULL;

static void set_global_value(VM* vm, ObjString* key, Value value) {
    push(vm, OBJ_VAL(key));
    push(vm, value);
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
}

static void restore_module_context(VM* vm, CallFrame* frame) {
    if (!frame->restore_module_context) return;

    if (frame->had_prev_module_name) {
        set_global_value(vm, vm->module_name_key, frame->prev_module_name);
    } else {
        table_delete(&vm->globals, vm->module_name_key);
    }

    if (frame->had_prev_module_file) {
        set_global_value(vm, vm->module_file_key, frame->prev_module_file);
    } else {
        table_delete(&vm->globals, vm->module_file_key);
    }

    if (frame->had_prev_module_main) {
        set_global_value(vm, vm->module_main_key, frame->prev_module_main);
    } else {
        table_delete(&vm->globals, vm->module_main_key);
    }
}

InterpretResult vm_run(VM* vm, int min_frame_count) {
    CallFrame* frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    uint8_t* ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_SHORT() \
    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define OP_PROF_FLUSH() do { \
    if (vm->opcode_prof_enabled && opcode_prof_active) { \
        uint64_t _op_end_ns = vm_prof_now_ns(); \
        vm->opcode_prof_ns[opcode_prof_active_op] += (_op_end_ns - opcode_prof_start_ns); \
        opcode_prof_active = 0; \
    } \
} while (0)

#if defined(__GNUC__) || defined(__clang__)
#define TOI_COMPUTED_GOTO 1
#else
#define TOI_COMPUTED_GOTO 0
#endif

    uint8_t instruction = 0;
    uint8_t opcode_prof_active_op = 0;
    uint64_t opcode_prof_start_ns = 0;
    int opcode_prof_active = 0;
#if TOI_COMPUTED_GOTO
    static void* dispatch_table[256];
    static int dispatch_table_init = 0;
    if (!dispatch_table_init) {
        for (int i = 0; i < 256; i++) {
            dispatch_table[i] = &&op_invalid;
        }
        dispatch_table[OP_CONSTANT] = &&op_OP_CONSTANT;
        dispatch_table[OP_APPEND] = &&op_OP_APPEND;
        dispatch_table[OP_ADD] = &&op_OP_ADD;
        dispatch_table[OP_ADD_INPLACE] = &&op_OP_ADD_INPLACE;
        dispatch_table[OP_ADD_CONST] = &&op_OP_ADD_CONST;
        dispatch_table[OP_SUBTRACT] = &&op_OP_SUBTRACT;
        dispatch_table[OP_SUB_CONST] = &&op_OP_SUB_CONST;
        dispatch_table[OP_MULTIPLY] = &&op_OP_MULTIPLY;
        dispatch_table[OP_MUL_CONST] = &&op_OP_MUL_CONST;
        dispatch_table[OP_DIVIDE] = &&op_OP_DIVIDE;
        dispatch_table[OP_DIV_CONST] = &&op_OP_DIV_CONST;
        dispatch_table[OP_NOT] = &&op_OP_NOT;
        dispatch_table[OP_NEGATE] = &&op_OP_NEGATE;
        dispatch_table[OP_LENGTH] = &&op_OP_LENGTH;
        dispatch_table[OP_PRINT] = &&op_OP_PRINT;
        dispatch_table[OP_POP] = &&op_OP_POP;
        dispatch_table[OP_GET_GLOBAL] = &&op_OP_GET_GLOBAL;
        dispatch_table[OP_DEFINE_GLOBAL] = &&op_OP_DEFINE_GLOBAL;
        dispatch_table[OP_SET_GLOBAL] = &&op_OP_SET_GLOBAL;
        dispatch_table[OP_SET_GLOBAL_POP] = &&op_OP_SET_GLOBAL_POP;
        dispatch_table[OP_DELETE_GLOBAL] = &&op_OP_DELETE_GLOBAL;
        dispatch_table[OP_GET_LOCAL] = &&op_OP_GET_LOCAL;
        dispatch_table[OP_GET2_LOCAL] = &&op_OP_GET2_LOCAL;
        dispatch_table[OP_SET_LOCAL] = &&op_OP_SET_LOCAL;
        dispatch_table[OP_SET_LOCAL_POP] = &&op_OP_SET_LOCAL_POP;
        dispatch_table[OP_ADD_SET_LOCAL] = &&op_OP_ADD_SET_LOCAL;
        dispatch_table[OP_ADD_SET_LOCAL_POP] = &&op_OP_ADD_SET_LOCAL_POP;
        dispatch_table[OP_SUB_SET_LOCAL] = &&op_OP_SUB_SET_LOCAL;
        dispatch_table[OP_SUB_SET_LOCAL_POP] = &&op_OP_SUB_SET_LOCAL_POP;
        dispatch_table[OP_MUL_SET_LOCAL] = &&op_OP_MUL_SET_LOCAL;
        dispatch_table[OP_MUL_SET_LOCAL_POP] = &&op_OP_MUL_SET_LOCAL_POP;
        dispatch_table[OP_DIV_SET_LOCAL] = &&op_OP_DIV_SET_LOCAL;
        dispatch_table[OP_DIV_SET_LOCAL_POP] = &&op_OP_DIV_SET_LOCAL_POP;
        dispatch_table[OP_MOD_SET_LOCAL] = &&op_OP_MOD_SET_LOCAL;
        dispatch_table[OP_MOD_SET_LOCAL_POP] = &&op_OP_MOD_SET_LOCAL_POP;
        dispatch_table[OP_INC_LOCAL] = &&op_OP_INC_LOCAL;
        dispatch_table[OP_SUB_LOCAL_CONST] = &&op_OP_SUB_LOCAL_CONST;
        dispatch_table[OP_MUL_LOCAL_CONST] = &&op_OP_MUL_LOCAL_CONST;
        dispatch_table[OP_DIV_LOCAL_CONST] = &&op_OP_DIV_LOCAL_CONST;
        dispatch_table[OP_MOD_LOCAL_CONST] = &&op_OP_MOD_LOCAL_CONST;
        dispatch_table[OP_GET_UPVALUE] = &&op_OP_GET_UPVALUE;
        dispatch_table[OP_SET_UPVALUE] = &&op_OP_SET_UPVALUE;
        dispatch_table[OP_SET_UPVALUE_POP] = &&op_OP_SET_UPVALUE_POP;
        dispatch_table[OP_CLOSE_UPVALUE] = &&op_OP_CLOSE_UPVALUE;
        dispatch_table[OP_GET_TABLE] = &&op_OP_GET_TABLE;
        dispatch_table[OP_GET_TABLE_NUM] = &&op_OP_GET_TABLE_NUM;
        dispatch_table[OP_GET_FIELD_CONST] = &&op_OP_GET_FIELD_CONST;
        dispatch_table[OP_GET_META_TABLE] = &&op_OP_GET_META_TABLE;
        dispatch_table[OP_SET_TABLE] = &&op_OP_SET_TABLE;
        dispatch_table[OP_SET_TABLE_KEEP] = &&op_OP_SET_TABLE_KEEP;
        dispatch_table[OP_SET_TABLE_NUM] = &&op_OP_SET_TABLE_NUM;
        dispatch_table[OP_SET_FIELD_CONST] = &&op_OP_SET_FIELD_CONST;
        dispatch_table[OP_SET_FIELD_CONST_KEEP] = &&op_OP_SET_FIELD_CONST_KEEP;
        dispatch_table[OP_DELETE_TABLE] = &&op_OP_DELETE_TABLE;
        dispatch_table[OP_NEW_TABLE] = &&op_OP_NEW_TABLE;
        dispatch_table[OP_DUP] = &&op_OP_DUP;
        dispatch_table[OP_JUMP] = &&op_OP_JUMP;
        dispatch_table[OP_JUMP_IF_FALSE] = &&op_OP_JUMP_IF_FALSE;
        dispatch_table[OP_JUMP_IF_TRUE] = &&op_OP_JUMP_IF_TRUE;
        dispatch_table[OP_LOOP] = &&op_OP_LOOP;
        dispatch_table[OP_CALL] = &&op_OP_CALL;
        dispatch_table[OP_INVOKE] = &&op_OP_INVOKE;
        dispatch_table[OP_CALL0] = &&op_OP_CALL0;
        dispatch_table[OP_CALL1] = &&op_OP_CALL1;
        dispatch_table[OP_QCALL0] = &&op_OP_QCALL0;
        dispatch_table[OP_QCALL1] = &&op_OP_QCALL1;
        dispatch_table[OP_CALL2] = &&op_OP_CALL2;
        dispatch_table[OP_QCALL2] = &&op_OP_QCALL2;
        dispatch_table[OP_QCALL1_STR_FIND] = &&op_OP_QCALL1_STR_FIND;
        dispatch_table[OP_QINVOKE1_STR_FIND] = &&op_OP_QINVOKE1_STR_FIND;
        dispatch_table[OP_QINVOKE0_STR_LOWER] = &&op_OP_QINVOKE0_STR_LOWER;
        dispatch_table[OP_CALL_NAMED] = &&op_OP_CALL_NAMED;
        dispatch_table[OP_CALL_EXPAND] = &&op_OP_CALL_EXPAND;
        dispatch_table[OP_CLOSURE] = &&op_OP_CLOSURE;
        dispatch_table[OP_RETURN] = &&op_OP_RETURN;
        dispatch_table[OP_TRUE] = &&op_OP_TRUE;
        dispatch_table[OP_FALSE] = &&op_OP_FALSE;
        dispatch_table[OP_NIL] = &&op_OP_NIL;
        dispatch_table[OP_EQUAL] = &&op_OP_EQUAL;
        dispatch_table[OP_GREATER] = &&op_OP_GREATER;
        dispatch_table[OP_LESS] = &&op_OP_LESS;
        dispatch_table[OP_HAS] = &&op_OP_HAS;
        dispatch_table[OP_IN] = &&op_OP_IN;
        dispatch_table[OP_POWER] = &&op_OP_POWER;
        dispatch_table[OP_INT_DIV] = &&op_OP_INT_DIV;
        dispatch_table[OP_MODULO] = &&op_OP_MODULO;
        dispatch_table[OP_IADD] = &&op_OP_IADD;
        dispatch_table[OP_ISUB] = &&op_OP_ISUB;
        dispatch_table[OP_IMUL] = &&op_OP_IMUL;
        dispatch_table[OP_IDIV] = &&op_OP_IDIV;
        dispatch_table[OP_IMOD] = &&op_OP_IMOD;
        dispatch_table[OP_FADD] = &&op_OP_FADD;
        dispatch_table[OP_FSUB] = &&op_OP_FSUB;
        dispatch_table[OP_FMUL] = &&op_OP_FMUL;
        dispatch_table[OP_FDIV] = &&op_OP_FDIV;
        dispatch_table[OP_FMOD] = &&op_OP_FMOD;
        dispatch_table[OP_QFADD] = &&op_OP_QFADD;
        dispatch_table[OP_QFSUB] = &&op_OP_QFSUB;
        dispatch_table[OP_QFMUL] = &&op_OP_QFMUL;
        dispatch_table[OP_QFDIV] = &&op_OP_QFDIV;
        dispatch_table[OP_QFMOD] = &&op_OP_QFMOD;
        dispatch_table[OP_QGREATER_NUM] = &&op_OP_QGREATER_NUM;
        dispatch_table[OP_QLESS_NUM] = &&op_OP_QLESS_NUM;
        dispatch_table[OP_MOD_CONST] = &&op_OP_MOD_CONST;
        dispatch_table[OP_GC] = &&op_OP_GC;
        dispatch_table[OP_SET_METATABLE] = &&op_OP_SET_METATABLE;
        dispatch_table[OP_RETURN_N] = &&op_OP_RETURN_N;
        dispatch_table[OP_ADJUST_STACK] = &&op_OP_ADJUST_STACK;
        dispatch_table[OP_UNPACK] = &&op_OP_UNPACK;
        dispatch_table[OP_TRY] = &&op_OP_TRY;
        dispatch_table[OP_END_TRY] = &&op_OP_END_TRY;
        dispatch_table[OP_END_FINALLY] = &&op_OP_END_FINALLY;
        dispatch_table[OP_IMPORT] = &&op_OP_IMPORT;
        dispatch_table[OP_IMPORT_STAR] = &&op_OP_IMPORT_STAR;
        dispatch_table[OP_THROW] = &&op_OP_THROW;
        dispatch_table[OP_BUILD_STRING] = &&op_OP_BUILD_STRING;
        dispatch_table[OP_BUILD_STRING2] = &&op_OP_BUILD_STRING2;
        dispatch_table[OP_BUILD_STRING3] = &&op_OP_BUILD_STRING3;
        dispatch_table[OP_BUILD_STRING4] = &&op_OP_BUILD_STRING4;
        dispatch_table[OP_BUILD_STRING5] = &&op_OP_BUILD_STRING5;
        dispatch_table[OP_ITER_PREP] = &&op_OP_ITER_PREP;
        dispatch_table[OP_ITER_PREP_IPAIRS] = &&op_OP_ITER_PREP_IPAIRS;
        dispatch_table[OP_RANGE] = &&op_OP_RANGE;
        dispatch_table[OP_FOR_PREP] = &&op_OP_FOR_PREP;
        dispatch_table[OP_FOR_LOOP] = &&op_OP_FOR_LOOP;
        dispatch_table[OP_QFOR_PREP_NUM] = &&op_OP_QFOR_PREP_NUM;
        dispatch_table[OP_QFOR_LOOP_NUM] = &&op_OP_QFOR_LOOP_NUM;
        dispatch_table[OP_SLICE] = &&op_OP_SLICE;
        dispatch_table_init = 1;
    }
#endif

    for (;;) {
        OP_PROF_FLUSH();
        if (run_stop_thread != NULL && vm_current_thread(vm) == run_stop_thread) {
            return INTERPRET_OK;
        }
        if (interrupt_requested) {
            interrupt_requested = 0;
            vm_runtime_error(vm, "Interrupted.");
            goto runtime_error;
        }
#ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value* slot = vm_current_thread(vm)->stack; slot < vm_current_thread(vm)->stack_top; slot++) {
            printf("[ ");
            print_value(*slot);
            printf(" ]");
        }
        printf("\n");
        disassemble_instruction(&frame->closure->function->chunk, (int)(ip - frame->closure->function->chunk.code));
#endif

#if TOI_COMPUTED_GOTO
        instruction = READ_BYTE();
        if (vm->opcode_prof_enabled) {
            vm->opcode_prof_count[instruction]++;
            opcode_prof_active_op = instruction;
            opcode_prof_start_ns = vm_prof_now_ns();
            opcode_prof_active = 1;
        }
        goto *dispatch_table[instruction];
op_invalid:
        vm_runtime_error(vm, "Unknown opcode %d.", instruction);
        goto runtime_error;
#else
        instruction = READ_BYTE();
        if (vm->opcode_prof_enabled) {
            vm->opcode_prof_count[instruction]++;
            opcode_prof_active_op = instruction;
            opcode_prof_start_ns = vm_prof_now_ns();
            opcode_prof_active = 1;
        }
#endif
        switch (instruction) {
            op_OP_TRY: case OP_TRY: {
                if (!vm_handle_op_try(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_END_TRY: case OP_END_TRY: {
                vm_handle_op_end_try(vm);
                break;
            }
            op_OP_END_FINALLY: case OP_END_FINALLY: {
                if (!vm_handle_op_end_finally(vm)) goto runtime_error;
                break;
            }
            op_OP_THROW: case OP_THROW: {
                vm_handle_op_throw(vm);
                goto runtime_error;
            }
            op_OP_CONSTANT: case OP_CONSTANT: {
                Value constant = frame->closure->function->chunk.constants.values[READ_BYTE()];
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                *top++ = constant;
                thread->stack_top = top;
                maybe_collect_garbage(vm);
                break;
            }
            op_OP_BUILD_STRING: case OP_BUILD_STRING: {
                uint8_t part_count = READ_BYTE();
                if (!vm_build_string(vm, part_count)) goto runtime_error;
                break;
            }
            op_OP_BUILD_STRING2: case OP_BUILD_STRING2: {
                if (!vm_build_string(vm, 2)) goto runtime_error;
                break;
            }
            op_OP_BUILD_STRING3: case OP_BUILD_STRING3: {
                if (!vm_build_string(vm, 3)) goto runtime_error;
                break;
            }
            op_OP_BUILD_STRING4: case OP_BUILD_STRING4: {
                if (!vm_build_string(vm, 4)) goto runtime_error;
                break;
            }
            op_OP_BUILD_STRING5: case OP_BUILD_STRING5: {
                if (!vm_build_string(vm, 5)) goto runtime_error;
                break;
            }
            op_OP_NIL: case OP_NIL:
                {
                    ObjThread* thread = vm_current_thread(vm);
                    Value* top = thread->stack_top;
                    *top++ = NIL_VAL;
                    thread->stack_top = top;
                }
                break;
            op_OP_TRUE: case OP_TRUE:
                {
                    ObjThread* thread = vm_current_thread(vm);
                    Value* top = thread->stack_top;
                    *top++ = BOOL_VAL(1);
                    thread->stack_top = top;
                }
                break;
            op_OP_FALSE: case OP_FALSE:
                {
                    ObjThread* thread = vm_current_thread(vm);
                    Value* top = thread->stack_top;
                    *top++ = BOOL_VAL(0);
                    thread->stack_top = top;
                }
                break;
            op_OP_POP: case OP_POP:
                {
                    ObjThread* thread = vm_current_thread(vm);
                    thread->stack_top--;
                }
                break;
            op_OP_GET_GLOBAL: case OP_GET_GLOBAL: {
                if (!vm_handle_op_get_global(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_DEFINE_GLOBAL: case OP_DEFINE_GLOBAL: {
                vm_handle_op_define_global(vm, frame, &ip);
                break;
            }
            op_OP_SET_GLOBAL: case OP_SET_GLOBAL: {
                vm_handle_op_set_global(vm, frame, &ip);
                break;
            }
            op_OP_SET_GLOBAL_POP: case OP_SET_GLOBAL_POP: {
                ObjString* name = AS_STRING(frame->closure->function->chunk.constants.values[READ_BYTE()]);
                table_set(&vm->globals, name, peek(vm, 0));
                vm_current_thread(vm)->stack_top--;
                maybe_collect_garbage(vm);
                break;
            }
            op_OP_DELETE_GLOBAL: case OP_DELETE_GLOBAL: {
                if (!vm_handle_op_delete_global(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GET_LOCAL: case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                *top++ = frame->slots[slot];
                thread->stack_top = top;
                break;
            }
            op_OP_GET2_LOCAL: case OP_GET2_LOCAL: {
                uint8_t slot_a = READ_BYTE();
                uint8_t slot_b = READ_BYTE();
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                *top++ = frame->slots[slot_a];
                *top++ = frame->slots[slot_b];
                thread->stack_top = top;
                break;
            }
            op_OP_SET_LOCAL: case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = vm_current_thread(vm)->stack_top[-1];
                break;
            }
            op_OP_SET_LOCAL_POP: case OP_SET_LOCAL_POP: {
                uint8_t slot = READ_BYTE();
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                frame->slots[slot] = top[-1];
                thread->stack_top = top - 1;
                break;
            }
            op_OP_ADD_SET_LOCAL: case OP_ADD_SET_LOCAL: {
                if (!vm_handle_op_add_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_ADD_SET_LOCAL_POP: case OP_ADD_SET_LOCAL_POP: {
                if (!vm_handle_op_add_set_local(vm, &frame, &ip)) goto runtime_error;
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_SUB_SET_LOCAL: case OP_SUB_SET_LOCAL: {
                if (!vm_handle_op_sub_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SUB_SET_LOCAL_POP: case OP_SUB_SET_LOCAL_POP: {
                if (!vm_handle_op_sub_set_local(vm, &frame, &ip)) goto runtime_error;
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_MUL_SET_LOCAL: case OP_MUL_SET_LOCAL: {
                if (!vm_handle_op_mul_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_MUL_SET_LOCAL_POP: case OP_MUL_SET_LOCAL_POP: {
                if (!vm_handle_op_mul_set_local(vm, &frame, &ip)) goto runtime_error;
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_DIV_SET_LOCAL: case OP_DIV_SET_LOCAL: {
                if (!vm_handle_op_div_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_DIV_SET_LOCAL_POP: case OP_DIV_SET_LOCAL_POP: {
                if (!vm_handle_op_div_set_local(vm, &frame, &ip)) goto runtime_error;
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_MOD_SET_LOCAL: case OP_MOD_SET_LOCAL: {
                if (!vm_handle_op_mod_set_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_MOD_SET_LOCAL_POP: case OP_MOD_SET_LOCAL_POP: {
                if (!vm_handle_op_mod_set_local(vm, &frame, &ip)) goto runtime_error;
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_INC_LOCAL: case OP_INC_LOCAL: {
                if (!vm_handle_op_inc_local(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SUB_LOCAL_CONST: case OP_SUB_LOCAL_CONST: {
                if (!vm_handle_op_sub_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_MUL_LOCAL_CONST: case OP_MUL_LOCAL_CONST: {
                if (!vm_handle_op_mul_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_DIV_LOCAL_CONST: case OP_DIV_LOCAL_CONST: {
                if (!vm_handle_op_div_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_MOD_LOCAL_CONST: case OP_MOD_LOCAL_CONST: {
                if (!vm_handle_op_mod_local_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GET_UPVALUE: case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *vm_current_thread(vm)->stack_top = *frame->closure->upvalues[slot]->location;
                vm_current_thread(vm)->stack_top++;
                break;
            }
            op_OP_SET_UPVALUE: case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = vm_current_thread(vm)->stack_top[-1];
                break;
            }
            op_OP_SET_UPVALUE_POP: case OP_SET_UPVALUE_POP: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = vm_current_thread(vm)->stack_top[-1];
                vm_current_thread(vm)->stack_top--;
                break;
            }
            op_OP_CLOSE_UPVALUE: case OP_CLOSE_UPVALUE: {
                close_upvalues(vm, vm_current_thread(vm)->stack_top - 1);
                pop(vm);
                break;
            }
            op_OP_NEW_TABLE: case OP_NEW_TABLE: {
                if (!vm_handle_op_new_table(vm)) goto runtime_error;
                break;
            }
            op_OP_SET_METATABLE: case OP_SET_METATABLE: {
                if (!vm_handle_op_set_metatable(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_DUP: case OP_DUP: push(vm, peek(vm, 0)); break;
            op_OP_GET_TABLE: case OP_GET_TABLE: {
                if (!vm_handle_op_get_table(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GET_TABLE_NUM: case OP_GET_TABLE_NUM: {
                if (!vm_handle_op_get_table_num(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GET_FIELD_CONST: case OP_GET_FIELD_CONST: {
                if (!vm_handle_op_get_field_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GET_META_TABLE: case OP_GET_META_TABLE: {
                if (!vm_handle_op_get_meta_table(vm)) goto runtime_error;
                break;
            }
            op_OP_SET_TABLE: case OP_SET_TABLE: {
                if (!vm_handle_op_set_table(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SET_TABLE_KEEP: case OP_SET_TABLE_KEEP: {
                if (!vm_handle_op_set_table_keep(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SET_TABLE_NUM: case OP_SET_TABLE_NUM: {
                if (!vm_handle_op_set_table_num(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SET_FIELD_CONST: case OP_SET_FIELD_CONST: {
                if (!vm_handle_op_set_field_const(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SET_FIELD_CONST_KEEP: case OP_SET_FIELD_CONST_KEEP: {
                if (!vm_handle_op_set_field_const_keep(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_DELETE_TABLE: case OP_DELETE_TABLE: {
                if (!vm_handle_op_delete_table(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_PRINT: case OP_PRINT: {
                uint8_t arg_count = READ_BYTE();
                InterpretResult print_result = INTERPRET_OK;
                int print_status = vm_handle_op_print(vm, &frame, &ip, arg_count, &print_result);
                if (print_status < 0) goto runtime_error;
                if (print_status == 0) return print_result;
                break;
            }
            op_OP_JUMP: case OP_JUMP: {
                vm_handle_op_jump(&ip);
                break;
            }
            op_OP_JUMP_IF_FALSE: case OP_JUMP_IF_FALSE: {
                vm_handle_op_jump_if_false(vm, &ip);
                break;
            }
            op_OP_JUMP_IF_TRUE: case OP_JUMP_IF_TRUE: {
                vm_handle_op_jump_if_true(vm, &ip);
                break;
            }
            op_OP_LOOP: case OP_LOOP: {
                if (vm->native_jit_enabled) {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    toi_jit_notify_loop_backedge(vm, frame, opcode_offset, OP_LOOP);
                    if (toi_jit_has_compiled_loops(vm) &&
                        toi_jit_try_execute_loop(vm, frame, &ip, opcode_offset, OP_LOOP)) {
                        break;
                    }
                }
                vm_handle_op_loop(&ip);
                break;
            }
            op_OP_CALL: case OP_CALL: {
                int arg_count = READ_BYTE();
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call_count++;
                Chunk* chunk = &frame->closure->function->chunk;
                int opcode_offset = (int)(ip - chunk->code) - 2;
                int cacheable_closure_site = opcode_offset >= 0 &&
                                             opcode_offset < chunk->capacity &&
                                             chunk->call_ic_closures != NULL &&
                                             chunk->call_ic_argc != NULL &&
                                             chunk->call_ic2_closures != NULL &&
                                             chunk->call_ic2_argc != NULL &&
                                             chunk->call_ic3_closures != NULL &&
                                             chunk->call_ic3_argc != NULL &&
                                             chunk->call_ic4_closures != NULL &&
                                             chunk->call_ic4_argc != NULL &&
                                             chunk->call_ic5_closures != NULL &&
                                             chunk->call_ic5_argc != NULL;
                int cacheable_native_site = opcode_offset >= 0 &&
                                            opcode_offset < chunk->capacity &&
                                            chunk->call_ic_natives != NULL &&
                                            chunk->call_ic_native_argc != NULL &&
                                            chunk->call_ic2_natives != NULL &&
                                            chunk->call_ic2_native_argc != NULL &&
                                            chunk->call_ic3_natives != NULL &&
                                            chunk->call_ic3_native_argc != NULL &&
                                            chunk->call_ic4_natives != NULL &&
                                            chunk->call_ic4_native_argc != NULL &&
                                            chunk->call_ic5_natives != NULL &&
                                            chunk->call_ic5_native_argc != NULL;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, arg_count);
                ObjClosure* target = NULL;
                ObjNative* native_target = NULL;
                int fast_direct = 0;
                int fast_native = 0;

                if (IS_CLOSURE(callee)) {
                    ObjClosure* closure = AS_CLOSURE(callee);
                    ObjFunction* function = closure->function;
                    int cache_hit = 0;
                    int direct_callable = !function->is_generator &&
                                          !function->is_variadic &&
                                          function->defaults_count == 0 &&
                                          function->arity == arg_count;
                    Value* args = thread->stack_top - arg_count;

                    if (cacheable_closure_site) {
                        if ((chunk->call_ic_closures[opcode_offset] == closure &&
                             chunk->call_ic_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic2_closures[opcode_offset] == closure &&
                             chunk->call_ic2_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic3_closures[opcode_offset] == closure &&
                             chunk->call_ic3_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic4_closures[opcode_offset] == closure &&
                             chunk->call_ic4_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic5_closures[opcode_offset] == closure &&
                             chunk->call_ic5_argc[opcode_offset] == arg_count)) {
                            cache_hit = 1;
                        }
                    }

                    if (cache_hit) {
                        if (!direct_callable ||
                            !call_args_match_param_types(function, args, arg_count)) {
                            if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                        } else {
                            if (vm->call_prof_enabled) vm->call_prof_call_ic_hit_count++;
                            target = closure;
                            fast_direct = 1;
                        }
                    } else if (direct_callable &&
                               call_args_match_param_types(function, args, arg_count)) {
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                        target = closure;
                        fast_direct = 1;
                        if (cacheable_closure_site) {
                            if (chunk->call_ic_closures[opcode_offset] == NULL ||
                                chunk->call_ic_closures[opcode_offset] == closure) {
                                chunk->call_ic_closures[opcode_offset] = closure;
                                chunk->call_ic_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic2_closures[opcode_offset] == NULL ||
                                       chunk->call_ic2_closures[opcode_offset] == closure) {
                                chunk->call_ic2_closures[opcode_offset] = closure;
                                chunk->call_ic2_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic3_closures[opcode_offset] == NULL ||
                                       chunk->call_ic3_closures[opcode_offset] == closure) {
                                chunk->call_ic3_closures[opcode_offset] = closure;
                                chunk->call_ic3_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic4_closures[opcode_offset] == NULL ||
                                       chunk->call_ic4_closures[opcode_offset] == closure) {
                                chunk->call_ic4_closures[opcode_offset] = closure;
                                chunk->call_ic4_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic5_closures[opcode_offset] == NULL ||
                                       chunk->call_ic5_closures[opcode_offset] == closure) {
                                chunk->call_ic5_closures[opcode_offset] = closure;
                                chunk->call_ic5_argc[opcode_offset] = (uint8_t)arg_count;
                            } else {
                                // Keep the first four slots sticky and use slot5 as replacement.
                                chunk->call_ic5_closures[opcode_offset] = closure;
                                chunk->call_ic5_argc[opcode_offset] = (uint8_t)arg_count;
                            }
                        } else if (opcode_offset >= 0 &&
                                   opcode_offset < chunk->capacity &&
                                   chunk->call_ic_closures != NULL &&
                                   chunk->call_ic_argc != NULL) {
                            chunk->call_ic_closures[opcode_offset] = closure;
                            chunk->call_ic_argc[opcode_offset] = (uint8_t)arg_count;
                        }
                    } else {
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                    }
                } else if (IS_NATIVE(callee)) {
                    ObjNative* native = AS_NATIVE_OBJ(callee);
                    struct Obj* native_obj = AS_OBJ(callee);
                    int cache_hit = 0;

                    if (cacheable_native_site) {
                        if ((chunk->call_ic_natives[opcode_offset] == native_obj &&
                             chunk->call_ic_native_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic2_natives[opcode_offset] == native_obj &&
                             chunk->call_ic2_native_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic3_natives[opcode_offset] == native_obj &&
                             chunk->call_ic3_native_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic4_natives[opcode_offset] == native_obj &&
                             chunk->call_ic4_native_argc[opcode_offset] == arg_count) ||
                            (chunk->call_ic5_natives[opcode_offset] == native_obj &&
                             chunk->call_ic5_native_argc[opcode_offset] == arg_count)) {
                            cache_hit = 1;
                        }
                    }

                    if (cache_hit) {
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_hit_count++;
                        native_target = native;
                        fast_native = 1;
                    } else {
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                        native_target = native;
                        fast_native = 1;
                        if (cacheable_native_site) {
                            if (chunk->call_ic_natives[opcode_offset] == NULL ||
                                chunk->call_ic_natives[opcode_offset] == native_obj) {
                                chunk->call_ic_natives[opcode_offset] = native_obj;
                                chunk->call_ic_native_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic2_natives[opcode_offset] == NULL ||
                                       chunk->call_ic2_natives[opcode_offset] == native_obj) {
                                chunk->call_ic2_natives[opcode_offset] = native_obj;
                                chunk->call_ic2_native_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic3_natives[opcode_offset] == NULL ||
                                       chunk->call_ic3_natives[opcode_offset] == native_obj) {
                                chunk->call_ic3_natives[opcode_offset] = native_obj;
                                chunk->call_ic3_native_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic4_natives[opcode_offset] == NULL ||
                                       chunk->call_ic4_natives[opcode_offset] == native_obj) {
                                chunk->call_ic4_natives[opcode_offset] = native_obj;
                                chunk->call_ic4_native_argc[opcode_offset] = (uint8_t)arg_count;
                            } else if (chunk->call_ic5_natives[opcode_offset] == NULL ||
                                       chunk->call_ic5_natives[opcode_offset] == native_obj) {
                                chunk->call_ic5_natives[opcode_offset] = native_obj;
                                chunk->call_ic5_native_argc[opcode_offset] = (uint8_t)arg_count;
                            } else {
                                chunk->call_ic5_natives[opcode_offset] = native_obj;
                                chunk->call_ic5_native_argc[opcode_offset] = (uint8_t)arg_count;
                            }
                        }
                    }
                }

                if (fast_direct) {
                    if (vm->call_prof_enabled) vm->call_prof_call_direct_fast_count++;
                    frame->ip = ip;
                    if (vm->native_jit_enabled && toi_jit_try_execute(vm, target, arg_count)) {
                        // JIT executed in-place; stay on current frame.
                    } else {
                        if (!enter_closure_frame(vm, target, arg_count)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                    }
                } else if (fast_native) {
                    Value* args = thread->stack_top - arg_count;
                    thread->stack_top -= arg_count + 1; // pop args and callee
                    if (!call_native_direct(vm, native_target, arg_count, args, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else {
                    if (vm->call_prof_enabled) vm->call_prof_call_fallback_count++;
                    if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                        goto runtime_error;
                    }
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_INVOKE: case OP_INVOKE: {
                ObjString* method_name = READ_STRING();
                int arg_count = READ_BYTE();
                Chunk* chunk = &frame->closure->function->chunk;
                int opcode_offset = (int)(ip - chunk->code) - 3;
                Value receiver = peek(vm, arg_count);
                Value method = NIL_VAL;

                // Fast path for hot string.lower(method-call) shape:
                //   <str>.lower()
                if (arg_count == 0 &&
                    IS_STRING(receiver) &&
                    (method_name == vm->str_lower_name)) {
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_QINVOKE0_STR_LOWER;
                    }
                    vm_current_thread(vm)->stack_top -= 1; // pop receiver
                    if (push_fast_string_lower(vm, receiver) < 0) goto runtime_error;
                    break;
                }

                // Fast path for hot string.find(method-call) shape:
                //   <str>.find(<str>)
                if (arg_count == 1 &&
                    IS_STRING(receiver) &&
                    IS_STRING(peek(vm, 0)) &&
                    (method_name == vm->str_find_name)) {
                    ObjString* str = AS_STRING(receiver);
                    ObjString* needle = AS_STRING(peek(vm, 0));
                    ObjThread* thread = vm_current_thread(vm);
                    thread->stack_top -= 2; // pop receiver and arg

                    if (needle->length == 0) {
                        push(vm, NUMBER_VAL(1));
                        push(vm, NUMBER_VAL(0));
                    } else {
                        const char* found = fast_find_bytes(
                            str->chars, str->length, needle->chars, needle->length);
                        if (found == NULL) {
                            push(vm, NIL_VAL);
                        } else {
                            int pos = (int)(found - str->chars) + 1;
                            push(vm, NUMBER_VAL(pos));
                            push(vm, NUMBER_VAL(pos + needle->length - 1));
                        }
                    }
                    break;
                }

                // Fast path: direct method dispatch on plain tables without
                // creating a transient bound-method object.
                if (IS_TABLE(receiver)) {
                    ObjTable* table_obj = AS_TABLE(receiver);
                    if (table_obj->no_metatable_fast) {
                        Value raw_method = NIL_VAL;
                        int resolved = 0;

                        if (opcode_offset >= 0 &&
                            opcode_offset < chunk->capacity &&
                            chunk->get_table_ic_tables != NULL &&
                            chunk->get_table_ic_slots != NULL &&
                            chunk->get_table_ic_versions != NULL &&
                            chunk->get_table_ic_keys != NULL &&
                            chunk->get_table_ic_tables[opcode_offset] == table_obj &&
                            chunk->get_table_ic_keys[opcode_offset] == method_name &&
                            chunk->get_table_ic_versions[opcode_offset] == table_obj->table.shape_version) {
                            int slot = chunk->get_table_ic_slots[opcode_offset];
                            if (slot >= 0 && slot < table_obj->table.capacity) {
                                Entry* entry = &table_obj->table.entries[slot];
                                if (entry->key == method_name) {
                                    raw_method = entry->value;
                                    resolved = 1;
                                }
                            }
                        }

                        if (!resolved) {
                            if (opcode_offset >= 0 &&
                                opcode_offset < chunk->capacity &&
                                chunk->get_table_ic_slots != NULL &&
                                chunk->get_table_ic_keys != NULL &&
                                chunk->get_table_ic_keys[opcode_offset] == method_name) {
                                int slot = chunk->get_table_ic_slots[opcode_offset];
                                if (slot >= 0 && slot < table_obj->table.capacity) {
                                    Entry* entry = &table_obj->table.entries[slot];
                                    if (entry->key == method_name) {
                                        raw_method = entry->value;
                                        resolved = 1;
                                    }
                                }
                            }
                        }

                        if (!resolved) {
                            int slot = -1;
                            if (table_get_with_slot(&table_obj->table, method_name, &raw_method, &slot)) {
                                resolved = 1;
                                if (opcode_offset >= 0 &&
                                    opcode_offset < chunk->capacity &&
                                    chunk->get_table_ic_tables != NULL &&
                                    chunk->get_table_ic_slots != NULL &&
                                    chunk->get_table_ic_versions != NULL &&
                                    chunk->get_table_ic_keys != NULL) {
                                    chunk->get_table_ic_tables[opcode_offset] = table_obj;
                                    chunk->get_table_ic_keys[opcode_offset] = method_name;
                                    chunk->get_table_ic_slots[opcode_offset] = slot;
                                    chunk->get_table_ic_versions[opcode_offset] =
                                        table_obj->table.shape_version;
                                }
                            }
                        }

                        if (resolved) {
                            ObjThread* thread = vm_current_thread(vm);
                            int call_arg_count = arg_count;
                            if (!table_obj->is_module && value_is_self_callable(raw_method)) {
                                // [receiver, arg1, ..., argN] -> [method, receiver, arg1, ..., argN]
                                shift_args_right(thread, arg_count);
                                thread->stack_top[-arg_count - 1] = receiver;
                                thread->stack_top[-arg_count - 2] = raw_method;
                                call_arg_count++;
                            } else {
                                // [receiver, arg1, ..., argN] -> [method, arg1, ..., argN]
                                thread->stack_top[-arg_count - 1] = raw_method;
                            }

                            Value callee = thread->stack_top[-call_arg_count - 1];
                            if (IS_CLOSURE(callee)) {
                                ObjClosure* closure = AS_CLOSURE(callee);
                                if (closure->function->is_generator) {
                                    if (!create_generator_positional(vm, closure, call_arg_count)) {
                                        goto runtime_error;
                                    }
                                } else {
                                    frame->ip = ip;
                                    int prev_frame_count = vm_current_thread(vm)->frame_count;
                                    if (!call_closure_positional(vm, closure, call_arg_count)) {
                                        goto runtime_error;
                                    }
                                    thread = vm_current_thread(vm);
                                    if (thread->frame_count > prev_frame_count) {
                                        thread = vm_current_thread(vm);
                                        frame = &thread->frames[thread->frame_count - 1];
                                        ip = frame->ip;
                                    }
                                }
                            } else if (IS_NATIVE(callee)) {
                                Value* args = thread->stack_top - call_arg_count;
                                thread->stack_top -= call_arg_count + 1; // pop args and callee
                                if (!call_native_direct(vm, AS_NATIVE_OBJ(callee), call_arg_count, args, &frame, &ip)) {
                                    goto runtime_error;
                                }
                            } else if (!invoke_call_with_arg_count(vm, call_arg_count, &frame, &ip)) {
                                goto runtime_error;
                            }
                            break;
                        }
                    }
                }

                if (!vm_handle_get_table_inner(
                        vm, &frame, &ip, chunk, opcode_offset, receiver, OBJ_VAL(method_name))) {
                    goto runtime_error;
                }
                method = pop(vm);
                vm_current_thread(vm)->stack_top[-arg_count - 1] = method;
                if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            op_OP_CALL0: case OP_CALL0: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call0_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 0);
                if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                    Value method_val = OBJ_VAL(bound->method);
                    if (IS_NATIVE(method_val)) {
                        thread->stack_top[0] = bound->receiver;
                        thread->stack_top[-1] = method_val;
                        thread->stack_top++;
                        if (vm->call_prof_enabled) {
                            vm->call_prof_call0_bound_native_fast_count++;
                            vm->call_prof_call0_native_count++;
                        }
                        if (!call_value(vm, method_val, 1, &frame, &ip)) {
                            goto runtime_error;
                        }
                        if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                        break;
                    }
                }
                if (IS_CLOSURE(callee)) {
                    ObjClosure* closure = AS_CLOSURE(callee);
                    ObjFunction* function = closure->function;
                    if (!function->is_generator &&
                        !function->is_variadic &&
                        function->defaults_count == 0 &&
                        function->param_types_count == 0 &&
                        function->arity == 0) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->code[opcode_offset] = OP_QCALL0;
                            chunk->call_ic_closures[opcode_offset] = closure;
                            chunk->call_ic_argc[opcode_offset] = 0;
                        }
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                        if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, 0)) {
                            if (vm->call_prof_enabled) vm->call_prof_call0_direct_fast_count++;
                            if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        frame->ip = ip;
                        if (!enter_closure_frame(vm, closure, 0)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                        if (vm->call_prof_enabled) vm->call_prof_call0_direct_fast_count++;
                    } else if (!invoke_call0(vm, &frame, &ip)) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->call_ic_closures[opcode_offset] = NULL;
                            chunk->call_ic_argc[opcode_offset] = 0;
                        }
                        if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->call_ic_closures[opcode_offset] = NULL;
                            chunk->call_ic_argc[opcode_offset] = 0;
                        }
                        vm->call_prof_call0_fallback_count++;
                    }
                } else if (IS_NATIVE(callee)) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call0_native_count++;
                    ObjNative* native_obj = AS_NATIVE_OBJ(callee);
                    NativeFastKind fast_kind = (NativeFastKind)native_obj->fast_kind;
                    NativeFn native = native_obj->function;
                    Value* args = thread->stack_top; // 0-arg native call
                    thread->stack_top -= 1; // pop callee

                    if (vm->call_prof_enabled && fast_kind != NATIVE_FAST_NONE) {
                        vm->call_prof_native_fast_try_count++;
                    }
                    frame->ip = ip;
                    int fast = try_fast_native_call(vm, native_obj, 0, args);
                    if (fast > 0) {
                        if (vm->call_prof_enabled && fast_kind != NATIVE_FAST_NONE) {
                            vm->call_prof_native_fast_hit_count++;
                        }
                    } else if (fast < 0) {
                        if (vm->call_prof_enabled) vm->call_prof_native_fast_error_count++;
                        goto runtime_error;
                    } else {
                        if (vm->call_prof_enabled && fast_kind != NATIVE_FAST_NONE) {
                            vm->call_prof_native_fast_miss_count++;
                        }
                        ObjThread* current = thread;
                        if (!native(vm, 0, args)) {
                            goto runtime_error;
                        }
                        ObjThread* after = vm_current_thread(vm);
                        if (after != current) {
                            if (vm->call_prof_enabled) vm->call_prof_native_thread_switch_count++;
                            frame = &after->frames[after->frame_count - 1];
                            ip = frame->ip;
                            thread = after;
                        }
                    }
                } else if (!invoke_call0(vm, &frame, &ip)) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                    goto runtime_error;
                } else if (vm->call_prof_enabled) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    vm->call_prof_call0_fallback_count++;
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_CALL1: case OP_CALL1: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call1_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 1);
                if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                    Value method_val = OBJ_VAL(bound->method);
                    if (IS_NATIVE(method_val)) {
                        ObjNative* native_obj = AS_NATIVE_OBJ(method_val);
                        if ((NativeFastKind)native_obj->fast_kind == NATIVE_FAST_STR_FIND &&
                            IS_STRING(peek(vm, 0)) &&
                            opcode_offset >= 0) {
                            chunk->code[opcode_offset] = OP_QCALL1_STR_FIND;
                        }
                        if (try_fast_bound_call1(vm, bound, peek(vm, 0))) {
                            if (vm->call_prof_enabled) {
                                vm->call_prof_call1_native_count++;
                                vm->call_prof_native_fast_try_count++;
                                vm->call_prof_native_fast_hit_count++;
                            }
                            if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        // [bound, arg1] -> [native, receiver, arg1]
                        thread->stack_top[0] = thread->stack_top[-1];
                        thread->stack_top[-1] = bound->receiver;
                        thread->stack_top[-2] = method_val;
                        thread->stack_top++;
                        if (vm->call_prof_enabled) vm->call_prof_call1_native_count++;
                        if (!call_value(vm, method_val, 2, &frame, &ip)) {
                            goto runtime_error;
                        }
                    } else if (!invoke_call1(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        vm->call_prof_call1_fallback_count++;
                    }
                } else if (IS_CLOSURE(callee)) {
                    ObjClosure* closure = AS_CLOSURE(callee);
                    ObjFunction* function = closure->function;
                    if (!function->is_generator &&
                        !function->is_variadic &&
                        function->defaults_count == 0 &&
                        function->param_types_count == 0 &&
                        function->arity == 1) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->code[opcode_offset] = OP_QCALL1;
                            chunk->call_ic_closures[opcode_offset] = closure;
                            chunk->call_ic_argc[opcode_offset] = 1;
                        }
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                        if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, 1)) {
                            if (vm->call_prof_enabled) vm->call_prof_call1_direct_fast_count++;
                            if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        frame->ip = ip;
                        if (!enter_closure_frame(vm, closure, 1)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                        if (vm->call_prof_enabled) vm->call_prof_call1_direct_fast_count++;
                    } else if (!function->is_generator &&
                               !function->is_variadic &&
                               function->defaults_count == 0 &&
                               function->param_types_count == 1 &&
                               function->arity == 1 &&
                               (function->param_types[0] == TYPEHINT_INT ||
                                function->param_types[0] == TYPEHINT_FLOAT)) {
                        Value arg0 = peek(vm, 0);
                        if (value_matches_numeric_hint_fast(arg0, function->param_types[0])) {
                            if (opcode_offset >= 0 &&
                                chunk->call_ic_closures != NULL &&
                                chunk->call_ic_argc != NULL) {
                                chunk->code[opcode_offset] = OP_QCALL1;
                                chunk->call_ic_closures[opcode_offset] = closure;
                                chunk->call_ic_argc[opcode_offset] = 1;
                            }
                            if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                            if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, 1)) {
                                if (vm->call_prof_enabled) vm->call_prof_call1_typed_fast_count++;
                                if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                                break;
                            }
                            frame->ip = ip;
                            if (!enter_closure_frame(vm, closure, 1)) goto runtime_error;
                            thread = vm_current_thread(vm);
                            frame = &thread->frames[thread->frame_count - 1];
                            ip = frame->ip;
                            if (vm->call_prof_enabled) vm->call_prof_call1_typed_fast_count++;
                        } else if (!invoke_call1(vm, &frame, &ip)) {
                            if (opcode_offset >= 0 &&
                                chunk->call_ic_closures != NULL &&
                                chunk->call_ic_argc != NULL) {
                                chunk->call_ic_closures[opcode_offset] = NULL;
                                chunk->call_ic_argc[opcode_offset] = 0;
                            }
                            if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                            goto runtime_error;
                        } else if (vm->call_prof_enabled) {
                            if (opcode_offset >= 0 &&
                                chunk->call_ic_closures != NULL &&
                                chunk->call_ic_argc != NULL) {
                                chunk->call_ic_closures[opcode_offset] = NULL;
                                chunk->call_ic_argc[opcode_offset] = 0;
                            }
                            vm->call_prof_call1_fallback_count++;
                        }
                    } else if (!invoke_call1(vm, &frame, &ip)) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->call_ic_closures[opcode_offset] = NULL;
                            chunk->call_ic_argc[opcode_offset] = 0;
                        }
                        if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        if (opcode_offset >= 0 &&
                            chunk->call_ic_closures != NULL &&
                            chunk->call_ic_argc != NULL) {
                            chunk->call_ic_closures[opcode_offset] = NULL;
                            chunk->call_ic_argc[opcode_offset] = 0;
                        }
                        vm->call_prof_call1_fallback_count++;
                    }
                } else if (IS_NATIVE(callee)) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call1_native_count++;
                    Value* args = thread->stack_top - 1;
                    thread->stack_top -= 2; // pop arg and callee
                    if (!call_native_direct(vm, AS_NATIVE_OBJ(callee), 1, args, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else if (!invoke_call1(vm, &frame, &ip)) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                    goto runtime_error;
                } else if (vm->call_prof_enabled) {
                    if (opcode_offset >= 0 &&
                        chunk->call_ic_closures != NULL &&
                        chunk->call_ic_argc != NULL) {
                        chunk->call_ic_closures[opcode_offset] = NULL;
                        chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    vm->call_prof_call1_fallback_count++;
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_QCALL0: case OP_QCALL0: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call0_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 0);

                ObjClosure* cached = (opcode_offset >= 0 && chunk->call_ic_closures != NULL)
                    ? chunk->call_ic_closures[opcode_offset]
                    : NULL;
                uint8_t cached_argc = (opcode_offset >= 0 && chunk->call_ic_argc != NULL)
                    ? chunk->call_ic_argc[opcode_offset]
                    : 0;

                if (!IS_CLOSURE(callee) || AS_CLOSURE(callee) != cached || cached_argc != 0) {
                    if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_CALL0;
                        if (chunk->call_ic_closures != NULL) chunk->call_ic_closures[opcode_offset] = NULL;
                        if (chunk->call_ic_argc != NULL) chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (!invoke_call0(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                        goto runtime_error;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                    break;
                }

                ObjFunction* function = cached->function;
                if (function->is_generator ||
                    function->is_variadic ||
                    function->defaults_count != 0 ||
                    function->param_types_count != 0 ||
                    function->arity != 0) {
                    if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_CALL0;
                        if (chunk->call_ic_closures != NULL) chunk->call_ic_closures[opcode_offset] = NULL;
                        if (chunk->call_ic_argc != NULL) chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (!invoke_call0(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                        goto runtime_error;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call0_fallback_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                    break;
                }

                if (vm->call_prof_enabled) vm->call_prof_call_ic_hit_count++;
                if (vm->native_jit_enabled && toi_jit_try_execute(vm, cached, 0)) {
                    if (vm->call_prof_enabled) vm->call_prof_call0_direct_fast_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                    break;
                }
                frame->ip = ip;
                if (!enter_closure_frame(vm, cached, 0)) goto runtime_error;
                thread = vm_current_thread(vm);
                frame = &thread->frames[thread->frame_count - 1];
                ip = frame->ip;
                if (vm->call_prof_enabled) vm->call_prof_call0_direct_fast_count++;
                if (vm->call_prof_enabled) vm->call_prof_op_call0_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_QCALL1: case OP_QCALL1: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call1_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 1);

                ObjClosure* cached = (opcode_offset >= 0 && chunk->call_ic_closures != NULL)
                    ? chunk->call_ic_closures[opcode_offset]
                    : NULL;
                uint8_t cached_argc = (opcode_offset >= 0 && chunk->call_ic_argc != NULL)
                    ? chunk->call_ic_argc[opcode_offset]
                    : 0;

                if (!IS_CLOSURE(callee) || AS_CLOSURE(callee) != cached || cached_argc != 1) {
                    if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_CALL1;
                        if (chunk->call_ic_closures != NULL) chunk->call_ic_closures[opcode_offset] = NULL;
                        if (chunk->call_ic_argc != NULL) chunk->call_ic_argc[opcode_offset] = 0;
                    }
                    if (!invoke_call1(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                        goto runtime_error;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                    break;
                }

                ObjFunction* function = cached->function;
                if (!function->is_generator &&
                    !function->is_variadic &&
                    function->defaults_count == 0 &&
                    function->param_types_count == 0 &&
                    function->arity == 1) {
                    if (vm->call_prof_enabled) vm->call_prof_call_ic_hit_count++;
                    if (vm->native_jit_enabled && toi_jit_try_execute(vm, cached, 1)) {
                        if (vm->call_prof_enabled) vm->call_prof_call1_direct_fast_count++;
                        if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                        break;
                    }
                    frame->ip = ip;
                    if (!enter_closure_frame(vm, cached, 1)) goto runtime_error;
                    thread = vm_current_thread(vm);
                    frame = &thread->frames[thread->frame_count - 1];
                    ip = frame->ip;
                    if (vm->call_prof_enabled) vm->call_prof_call1_direct_fast_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                    break;
                }

                if (!function->is_generator &&
                    !function->is_variadic &&
                    function->defaults_count == 0 &&
                    function->param_types_count == 1 &&
                    function->arity == 1 &&
                    (function->param_types[0] == TYPEHINT_INT ||
                     function->param_types[0] == TYPEHINT_FLOAT)) {
                    Value arg0 = peek(vm, 0);
                    if (value_matches_numeric_hint_fast(arg0, function->param_types[0])) {
                        if (vm->call_prof_enabled) vm->call_prof_call_ic_hit_count++;
                        if (vm->native_jit_enabled && toi_jit_try_execute(vm, cached, 1)) {
                            if (vm->call_prof_enabled) vm->call_prof_call1_typed_fast_count++;
                            if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        frame->ip = ip;
                        if (!enter_closure_frame(vm, cached, 1)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                        if (vm->call_prof_enabled) vm->call_prof_call1_typed_fast_count++;
                        if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                        break;
                    }
                }

                if (vm->call_prof_enabled) vm->call_prof_call_ic_miss_count++;
                if (opcode_offset >= 0) {
                    chunk->code[opcode_offset] = OP_CALL1;
                    if (chunk->call_ic_closures != NULL) chunk->call_ic_closures[opcode_offset] = NULL;
                    if (chunk->call_ic_argc != NULL) chunk->call_ic_argc[opcode_offset] = 0;
                }
                if (!invoke_call1(vm, &frame, &ip)) {
                    if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                    goto runtime_error;
                }
                if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_QCALL1_STR_FIND: case OP_QCALL1_STR_FIND: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call1_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                Value callee = peek(vm, 1);
                int handled = 0;

                if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                    Value method_val = OBJ_VAL(bound->method);
                    if (IS_NATIVE(method_val)) {
                        ObjNative* native_obj = AS_NATIVE_OBJ(method_val);
                        if ((NativeFastKind)native_obj->fast_kind == NATIVE_FAST_STR_FIND &&
                            IS_STRING(peek(vm, 0))) {
                            handled = try_fast_bound_call1(vm, bound, peek(vm, 0));
                            if (handled && vm->call_prof_enabled) {
                                vm->call_prof_call1_native_count++;
                                vm->call_prof_native_fast_try_count++;
                                vm->call_prof_native_fast_hit_count++;
                            }
                        }
                    }
                }

                if (!handled) {
                    if (opcode_offset >= 0) chunk->code[opcode_offset] = OP_CALL1;
                    if (!invoke_call1(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                        goto runtime_error;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                }

                if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_QINVOKE0_STR_LOWER: case OP_QINVOKE0_STR_LOWER: {
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjString* method_name = READ_STRING();
                int arg_count = READ_BYTE();
                Value receiver = peek(vm, arg_count);

                if (arg_count == 0 &&
                    IS_STRING(receiver) &&
                    (method_name == vm->str_lower_name)) {
                    vm_current_thread(vm)->stack_top -= 1; // pop receiver
                    if (push_fast_string_lower(vm, receiver) < 0) goto runtime_error;
                    break;
                }

                if (opcode_offset >= 0) {
                    chunk->code[opcode_offset] = OP_INVOKE;
                }

                Value method = NIL_VAL;
                if (!vm_handle_get_table_inner(
                        vm, &frame, &ip, chunk, opcode_offset, receiver, OBJ_VAL(method_name))) {
                    goto runtime_error;
                }
                method = pop(vm);
                vm_current_thread(vm)->stack_top[-arg_count - 1] = method;
                if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            op_OP_QINVOKE1_STR_FIND: case OP_QINVOKE1_STR_FIND: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call1_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);

                Value callee = peek(vm, 2);
                Value receiver = peek(vm, 1);
                Value arg1 = peek(vm, 0);
                int handled = 0;

                if (IS_NATIVE(callee) && IS_STRING(receiver) && IS_STRING(arg1)) {
                    ObjNative* native_obj = AS_NATIVE_OBJ(callee);
                    if ((NativeFastKind)native_obj->fast_kind == NATIVE_FAST_STR_FIND) {
                        ObjString* str = AS_STRING(receiver);
                        ObjString* needle = AS_STRING(arg1);
                        thread->stack_top -= 3;
                        if (needle->length == 0) {
                            push(vm, NUMBER_VAL(1));
                            push(vm, NUMBER_VAL(0));
                        } else {
                            const char* found = fast_find_bytes(str->chars, str->length, needle->chars, needle->length);
                            if (found == NULL) {
                                push(vm, NIL_VAL);
                            } else {
                                int pos = (int)(found - str->chars) + 1;
                                push(vm, NUMBER_VAL(pos));
                                push(vm, NUMBER_VAL(pos + needle->length - 1));
                            }
                        }
                        handled = 1;
                        if (vm->call_prof_enabled) {
                            vm->call_prof_call1_native_count++;
                            vm->call_prof_native_fast_try_count++;
                            vm->call_prof_native_fast_hit_count++;
                        }
                    }
                }

                if (!handled) {
                    if (opcode_offset >= 0) chunk->code[opcode_offset] = OP_CALL1;
                    if (IS_NATIVE(callee)) {
                        Value bound = OBJ_VAL(new_bound_method(receiver, AS_OBJ(callee)));
                        thread->stack_top[-3] = bound;
                        thread->stack_top[-2] = arg1;
                        thread->stack_top--;
                    }
                    if (!invoke_call1(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                        goto runtime_error;
                    }
                    if (vm->call_prof_enabled) vm->call_prof_call1_fallback_count++;
                }

                if (vm->call_prof_enabled) vm->call_prof_op_call1_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_CALL2: case OP_CALL2: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call2_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 2);
                if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                    Value method_val = OBJ_VAL(bound->method);
                    if (IS_NATIVE(method_val)) {
                        // [bound, arg1, arg2] -> [native, receiver, arg1, arg2]
                        thread->stack_top[0] = thread->stack_top[-1];
                        thread->stack_top[-1] = thread->stack_top[-2];
                        thread->stack_top[-2] = bound->receiver;
                        thread->stack_top[-3] = method_val;
                        thread->stack_top++;
                        if (!call_value(vm, method_val, 3, &frame, &ip)) {
                            goto runtime_error;
                        }
                    } else if (!invoke_call2(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        vm->call_prof_call2_fallback_count++;
                    }
                } else if (IS_CLOSURE(callee)) {
                    ObjClosure* closure = AS_CLOSURE(callee);
                    ObjFunction* function = closure->function;
                    if (function->tiny_inline_kind != TINY_INLINE_NONE) {
                        maybe_promote_inline_tiers(vm, function);
                    }
                    if (function->exec_tier == FUNCTION_TIER2_JIT &&
                        try_execute_tier2_template(vm, closure, 2)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_tiny_inline_fast_count++;
                        if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                        break;
                    }
                    if (function->tiny_inline_kind != TINY_INLINE_NONE &&
                        function->arity == 2 &&
                        !function->is_generator &&
                        !function->is_variadic &&
                        function->defaults_count == 0) {
                        if (opcode_offset >= 0 &&
                            chunk->call2_ic_closures != NULL &&
                            chunk->call2_ic_kind != NULL) {
                            chunk->code[opcode_offset] = OP_QCALL2;
                            chunk->call2_ic_closures[opcode_offset] = closure;
                            chunk->call2_ic_kind[opcode_offset] = CALL2_IC_TINY_INLINE;
                        }
                        Value arg0 = peek(vm, 1);
                        Value arg1 = peek(vm, 0);
                        int param_ok = 1;
                        if (function->param_types_count > 0) {
                            int check_count = function->param_types_count < function->arity ? function->param_types_count : function->arity;
                            Value* args = thread->stack_top - 2;
                            for (int i = 0; i < check_count; i++) {
                                uint8_t type = function->param_types[i];
                                if (type == TYPEHINT_ANY) continue;
                                if (!value_matches_type(args[i], type)) {
                                    param_ok = 0;
                                    break;
                                }
                            }
                        }
                        if (param_ok &&
                            function->tiny_inline_kind != TINY_INLINE_NONE) {
                            Value out = NIL_VAL;
                            if (!try_eval_tiny_inline2(function, arg0, arg1, &out)) {
                                if (!invoke_call2(vm, &frame, &ip)) {
                                    if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                                    goto runtime_error;
                                }
                                if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                                break;
                            }
                            thread->stack_top -= 3;
                            if (vm->call_prof_enabled) vm->call_prof_call2_tiny_inline_fast_count++;
                            push(vm, out);
                        } else if (!invoke_call2(vm, &frame, &ip)) {
                            if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                            goto runtime_error;
                        } else if (vm->call_prof_enabled) {
                            vm->call_prof_call2_fallback_count++;
                        }
                    } else if (!function->is_generator &&
                        !function->is_variadic &&
                        function->defaults_count == 0 &&
                        function->param_types_count == 0 &&
                        function->arity == 2) {
                        if (opcode_offset >= 0 &&
                            chunk->call2_ic_closures != NULL &&
                            chunk->call2_ic_kind != NULL) {
                            chunk->code[opcode_offset] = OP_QCALL2;
                            chunk->call2_ic_closures[opcode_offset] = closure;
                            chunk->call2_ic_kind[opcode_offset] = CALL2_IC_DIRECT;
                        }
                        if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, 2)) {
                            if (vm->call_prof_enabled) vm->call_prof_call2_direct_fast_count++;
                            if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        frame->ip = ip;
                        if (!enter_closure_frame(vm, closure, 2)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                        if (vm->call_prof_enabled) vm->call_prof_call2_direct_fast_count++;
                    } else if (!function->is_generator &&
                               !function->is_variadic &&
                               function->defaults_count == 0 &&
                               function->param_types_count == 2 &&
                               function->arity == 2) {
                        if (opcode_offset >= 0 &&
                            chunk->call2_ic_closures != NULL &&
                            chunk->call2_ic_kind != NULL) {
                            chunk->code[opcode_offset] = OP_QCALL2;
                            chunk->call2_ic_closures[opcode_offset] = closure;
                            chunk->call2_ic_kind[opcode_offset] = CALL2_IC_TYPED2;
                        }
                        Value arg0 = peek(vm, 1);
                        Value arg1 = peek(vm, 0);
                        uint8_t type0 = function->param_types[0];
                        uint8_t type1 = function->param_types[1];
                        int arg0_ok = (type0 == TYPEHINT_ANY) || value_matches_type(arg0, type0);
                        int arg1_ok = (type1 == TYPEHINT_ANY) || value_matches_type(arg1, type1);
                        if (arg0_ok && arg1_ok) {
                            if (vm->native_jit_enabled && toi_jit_try_execute(vm, closure, 2)) {
                                if (vm->call_prof_enabled) vm->call_prof_call2_typed_fast_count++;
                                if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                                break;
                            }
                            frame->ip = ip;
                            if (!enter_closure_frame(vm, closure, 2)) goto runtime_error;
                            thread = vm_current_thread(vm);
                            frame = &thread->frames[thread->frame_count - 1];
                            ip = frame->ip;
                            if (vm->call_prof_enabled) vm->call_prof_call2_typed_fast_count++;
                        } else if (!invoke_call2(vm, &frame, &ip)) {
                            if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                            goto runtime_error;
                        } else if (vm->call_prof_enabled) {
                            vm->call_prof_call2_fallback_count++;
                        }
                    } else if (!invoke_call2(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        vm->call_prof_call2_fallback_count++;
                    }
                } else if (IS_NATIVE(callee)) {
                    Value* args = thread->stack_top - 2;
                    thread->stack_top -= 3; // pop args and callee
                    if (!call_native_direct(vm, AS_NATIVE_OBJ(callee), 2, args, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else if (!invoke_call2(vm, &frame, &ip)) {
                    if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                    goto runtime_error;
                } else if (vm->call_prof_enabled) {
                    vm->call_prof_call2_fallback_count++;
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_QCALL2: case OP_QCALL2: {
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_qcall2_count++;
                int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                Chunk* chunk = &frame->closure->function->chunk;
                ObjThread* thread = vm_current_thread(vm);
                Value callee = peek(vm, 2);
                ObjClosure* cached = (opcode_offset >= 0 && chunk->call2_ic_closures != NULL)
                    ? chunk->call2_ic_closures[opcode_offset]
                    : NULL;
                uint8_t kind = (opcode_offset >= 0 && chunk->call2_ic_kind != NULL)
                    ? chunk->call2_ic_kind[opcode_offset]
                    : CALL_IC_NONE;

                if (!IS_CLOSURE(callee) || AS_CLOSURE(callee) != cached || kind == CALL_IC_NONE) {
                    if (vm->call_prof_enabled) vm->call_prof_call2_ic_miss_count++;
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_CALL2;
                        if (chunk->call2_ic_closures != NULL) chunk->call2_ic_closures[opcode_offset] = NULL;
                        if (chunk->call2_ic_kind != NULL) chunk->call2_ic_kind[opcode_offset] = CALL_IC_NONE;
                    }
                    if (!invoke_call2(vm, &frame, &ip)) goto runtime_error;
                    if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                    break;
                }

                if (vm->call_prof_enabled) vm->call_prof_call2_ic_hit_count++;
                ObjFunction* function = cached->function;
                if (kind == CALL2_IC_TINY_INLINE) {
                    maybe_promote_inline_tiers(vm, function);
                }
                if (function->exec_tier == FUNCTION_TIER2_JIT &&
                    try_execute_tier2_template(vm, cached, 2)) {
                    if (vm->call_prof_enabled) vm->call_prof_call2_tiny_inline_fast_count++;
                    if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                    break;
                }
                if (kind == CALL2_IC_DIRECT) {
                    if (vm->native_jit_enabled && toi_jit_try_execute(vm, cached, 2)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_direct_fast_count++;
                        if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                        break;
                    }
                    frame->ip = ip;
                    if (!enter_closure_frame(vm, cached, 2)) goto runtime_error;
                    thread = vm_current_thread(vm);
                    frame = &thread->frames[thread->frame_count - 1];
                    ip = frame->ip;
                    if (vm->call_prof_enabled) vm->call_prof_call2_direct_fast_count++;
                } else if (kind == CALL2_IC_TYPED2) {
                    Value arg0 = peek(vm, 1);
                    Value arg1 = peek(vm, 0);
                    uint8_t type0 = function->param_types[0];
                    uint8_t type1 = function->param_types[1];
                    int arg0_ok = (type0 == TYPEHINT_ANY) || value_matches_type(arg0, type0);
                    int arg1_ok = (type1 == TYPEHINT_ANY) || value_matches_type(arg1, type1);
                    if (arg0_ok && arg1_ok) {
                        if (vm->native_jit_enabled && toi_jit_try_execute(vm, cached, 2)) {
                            if (vm->call_prof_enabled) vm->call_prof_call2_typed_fast_count++;
                            if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                            break;
                        }
                        frame->ip = ip;
                        if (!enter_closure_frame(vm, cached, 2)) goto runtime_error;
                        thread = vm_current_thread(vm);
                        frame = &thread->frames[thread->frame_count - 1];
                        ip = frame->ip;
                        if (vm->call_prof_enabled) vm->call_prof_call2_typed_fast_count++;
                    } else if (!invoke_call2(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        vm->call_prof_call2_fallback_count++;
                    }
                } else if (kind == CALL2_IC_TINY_INLINE) {
                    Value arg0 = peek(vm, 1);
                    Value arg1 = peek(vm, 0);
                    int param_ok = 1;
                    if (function->param_types_count > 0) {
                        int check_count = function->param_types_count < function->arity ? function->param_types_count : function->arity;
                        Value* args = thread->stack_top - 2;
                        for (int i = 0; i < check_count; i++) {
                            uint8_t type = function->param_types[i];
                            if (type == TYPEHINT_ANY) continue;
                            if (!value_matches_type(args[i], type)) {
                                param_ok = 0;
                                break;
                            }
                        }
                    }
                    if (param_ok && function->tiny_inline_kind != TINY_INLINE_NONE) {
                        Value out = NIL_VAL;
                        if (!try_eval_tiny_inline2(function, arg0, arg1, &out)) {
                            if (!invoke_call2(vm, &frame, &ip)) {
                                if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                                goto runtime_error;
                            }
                            if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                            break;
                        }
                        thread->stack_top -= 3;
                        if (vm->call_prof_enabled) vm->call_prof_call2_tiny_inline_fast_count++;
                        push(vm, out);
                    } else if (!invoke_call2(vm, &frame, &ip)) {
                        if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                        goto runtime_error;
                    } else if (vm->call_prof_enabled) {
                        vm->call_prof_call2_fallback_count++;
                    }
                } else {
                    if (opcode_offset >= 0) {
                        chunk->code[opcode_offset] = OP_CALL2;
                    }
                    if (!invoke_call2(vm, &frame, &ip)) goto runtime_error;
                    if (vm->call_prof_enabled) vm->call_prof_call2_fallback_count++;
                }

                if (vm->call_prof_enabled) vm->call_prof_op_call2_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_CALL_NAMED: case OP_CALL_NAMED: {
                int arg_count = READ_BYTE();
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call_named_count++;
                if (!invoke_call_with_named_arg_count(vm, arg_count, &frame, &ip)) {
                    goto runtime_error;
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call_named_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_CALL_EXPAND: case OP_CALL_EXPAND: {
                int fixed_arg_count = READ_BYTE();
                uint64_t start_ns = vm->call_prof_enabled ? vm_prof_now_ns() : 0;
                if (vm->call_prof_enabled) vm->call_prof_op_call_expand_count++;
                Value spread = peek(vm, 0);
                if (!IS_TABLE(spread)) {
                    vm_runtime_error(vm, "Spread argument must be a table.");
                    goto runtime_error;
                }

                ObjTable* spread_table = AS_TABLE(spread);
                int spread_count = 0;
                int has_named = spread_table->table.count > 0;
                Value* spread_array = spread_table->table.array;
                int spread_array_cap = spread_table->table.array_capacity;
                while (spread_count < spread_array_cap) {
                    Value val = spread_array[spread_count];
                    if (IS_NIL(val)) break;
                    spread_count++;
                    if (fixed_arg_count + spread_count > 255) {
                        vm_runtime_error(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                }
                if (vm->call_prof_enabled) vm->call_prof_call_expand_spread_values += (uint64_t)spread_count;

                pop(vm); // Remove spread table
                for (int i = 0; i < spread_count; i++) {
                    push(vm, spread_array[i]);
                }

                int arg_count = fixed_arg_count + spread_count;
                if (has_named) {
                    if (vm->call_prof_enabled) vm->call_prof_call_expand_named_count++;
                    // Reuse spread table as named-args source to avoid per-call copying.
                    push(vm, OBJ_VAL(spread_table));
                    if (arg_count + 1 > 255) {
                        vm_runtime_error(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                    if (!invoke_call_with_named_arg_count(vm, arg_count + 1, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else {
                    if (vm->call_prof_enabled) vm->call_prof_call_expand_positional_count++;
                    if (!invoke_call_with_arg_count(vm, arg_count, &frame, &ip)) {
                        goto runtime_error;
                    }
                }
                if (vm->call_prof_enabled) vm->call_prof_op_call_expand_ns += vm_prof_now_ns() - start_ns;
                break;
            }
            op_OP_ITER_PREP: case OP_ITER_PREP: {
                if (!vm_handle_op_iter_prep(vm)) goto runtime_error;
                break;
            }
            op_OP_ITER_PREP_IPAIRS: case OP_ITER_PREP_IPAIRS: {
                if (!vm_handle_op_iter_prep_i_pairs(vm)) goto runtime_error;
                break;
            }
            op_OP_RANGE: case OP_RANGE: {
                if (!vm_handle_op_range(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_FOR_PREP: case OP_FOR_PREP: {
                if (!vm_handle_op_for_prep(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_FOR_LOOP: case OP_FOR_LOOP: {
                if (!vm_handle_op_for_loop(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_QFOR_PREP_NUM: case OP_QFOR_PREP_NUM: {
                if (!vm_handle_op_qfor_prep_num(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_QFOR_LOOP_NUM: case OP_QFOR_LOOP_NUM: {
                if (vm->native_jit_enabled) {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    toi_jit_notify_loop_backedge(vm, frame, opcode_offset, OP_QFOR_LOOP_NUM);
                    if (toi_jit_has_compiled_loops(vm) &&
                        toi_jit_try_execute_loop(vm, frame, &ip, opcode_offset, OP_QFOR_LOOP_NUM)) {
                        break;
                    }
                }
                if (!vm_handle_op_qfor_loop_num(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_SLICE: case OP_SLICE: {
                if (!vm_handle_op_slice(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_CLOSURE: case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = new_closure(function);
                push(vm, OBJ_VAL(closure));

                // Read upvalue information
                for (int i = 0; i < function->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        closure->upvalues[i] = capture_upvalue(vm, frame->slots + index);
                    }
                    else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                closure->upvalue_count = function->upvalue_count;
                break;
            }
            op_OP_RETURN: case OP_RETURN: {
                Value result = pop(vm);
                if (frame->cache_module_result && IS_STRING(frame->module_cache_name)) {
                    table_set(&vm->modules, AS_STRING(frame->module_cache_name), result);
                }
                close_upvalues(vm, frame->slots);
                discard_handlers_for_frame_return(vm_current_thread(vm));
                restore_module_context(vm, frame);
                vm_current_thread(vm)->frame_count--;

                // Restore stack and push result
                vm_current_thread(vm)->stack_top = frame->slots;
                push(vm, result);
                apply_pending_set_local(vm);

                if (vm_current_thread(vm)->frame_count <= min_frame_count) {
                    if (vm_current_thread(vm)->caller != NULL) {
                        ObjThread* caller = vm_current_thread(vm)->caller;
                        vm_current_thread(vm)->caller = NULL;

                        // Check stack overflow
                        if (caller->stack_top + 2 >= caller->stack + caller->stack_capacity) {
                            vm_runtime_error(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        if (vm_current_thread(vm)->is_generator && vm_current_thread(vm)->generator_mode) {
                            vm_current_thread(vm)->generator_mode = 0;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                        } else {
                            *caller->stack_top = BOOL_VAL(1);
                            caller->stack_top++;
                            *caller->stack_top = result;
                            caller->stack_top++;
                        }

                        vm_set_current_thread(vm, caller);
                        frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                        ip = frame->ip;
                        break;
                    }

                    // In REPL mode, leave the result on stack so it can be printed
                    // In normal mode, pop the script closure
                    if (min_frame_count == 0 && !vm->is_repl) {
                        pop(vm);  // Pop the script closure when completely done
                    }
                    return INTERPRET_OK;
                }

                frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                ip = frame->ip;
                break;
            }
            op_OP_RETURN_N: case OP_RETURN_N: {
                uint8_t count = READ_BYTE();
                Value* results = vm_current_thread(vm)->stack_top - count;
                if (frame->cache_module_result && IS_STRING(frame->module_cache_name)) {
                    Value module_result = count > 0 ? results[0] : NIL_VAL;
                    table_set(&vm->modules, AS_STRING(frame->module_cache_name), module_result);
                }
                close_upvalues(vm, frame->slots);
                discard_handlers_for_frame_return(vm_current_thread(vm));
                restore_module_context(vm, frame);
                vm_current_thread(vm)->frame_count--;

                // Copy results to where the function was called (frame->slots)
                // This replaces the function + args with the return values
                Value* dest = frame->slots;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_RETURN_N: count=%d, dest offset=%ld\n", count, dest - vm_current_thread(vm)->stack);
                for (int i = 0; i < count; i++) {
                    printf("  result[%d] = ", i);
                    print_value(results[i]);
                    printf("\n");
                }
                #endif
                for (int i = 0; i < count; i++) {
                    dest[i] = results[i];
                }
                vm_current_thread(vm)->stack_top = dest + count;
                apply_pending_set_local(vm);

                if (vm_current_thread(vm)->frame_count <= min_frame_count) {
                    if (vm_current_thread(vm)->caller != NULL) {
                        ObjThread* caller = vm_current_thread(vm)->caller;
                        vm_current_thread(vm)->caller = NULL;

                        if (caller->stack_top + 1 + count >= caller->stack + caller->stack_capacity) {
                            vm_runtime_error(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        if (vm_current_thread(vm)->is_generator && vm_current_thread(vm)->generator_mode) {
                            vm_current_thread(vm)->generator_mode = 0;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                            *caller->stack_top = NIL_VAL;
                            caller->stack_top++;
                        } else {
                            *caller->stack_top = BOOL_VAL(1);
                            caller->stack_top++;

                            Value* results = vm_current_thread(vm)->stack_top - count;
                            for (int i = 0; i < count; i++) {
                                *caller->stack_top = results[i];
                                caller->stack_top++;
                            }
                        }

                        vm_set_current_thread(vm, caller);
                        frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                        ip = frame->ip;
                        break;
                    }

                    if (min_frame_count == 0) {
                        vm_current_thread(vm)->stack_top -= count;
                    }
                    return INTERPRET_OK;
                }

                frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
                ip = frame->ip;
                break;
            }
            op_OP_ADJUST_STACK: case OP_ADJUST_STACK: {
                uint8_t target_depth = READ_BYTE();
                vm_current_thread(vm)->stack_top = frame->slots + target_depth;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_ADJUST_STACK: target depth=%d, new stack_top offset=%ld\n",
                       target_depth, vm_current_thread(vm)->stack_top - vm_current_thread(vm)->stack);
                #endif
                break;
            }
            op_OP_UNPACK: case OP_UNPACK: {
                if (!vm_handle_op_unpack(vm, frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_ADD_CONST: case OP_ADD_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_add_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            op_OP_ADD: case OP_ADD:
                if (!vm_handle_op_add(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_ADD_INPLACE: case OP_ADD_INPLACE:
                if (!vm_handle_op_add_inplace(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_SUBTRACT: case OP_SUBTRACT:
                if (!vm_handle_op_subtract(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_MULTIPLY: case OP_MULTIPLY:
                if (!vm_handle_op_multiply(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_DIVIDE: case OP_DIVIDE:
                if (!vm_handle_op_divide(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_MODULO: case OP_MODULO:
                if (!vm_handle_op_modulo(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_APPEND: case OP_APPEND:
                if (!vm_handle_op_append(vm, &frame, &ip)) goto runtime_error;
                break;
            op_OP_IADD: case OP_IADD:
                {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a + b));
                }
                break;
            op_OP_SUB_CONST: case OP_SUB_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_sub_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            op_OP_ISUB: case OP_ISUB:
                {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a - b));
                }
                break;
            op_OP_MUL_CONST: case OP_MUL_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_mul_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            op_OP_IMUL: case OP_IMUL:
                {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a * b));
                }
                break;
            op_OP_DIV_CONST: case OP_DIV_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_div_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            op_OP_IDIV: case OP_IDIV:
                {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a / b));
                }
                break;
            op_OP_NEGATE: case OP_NEGATE:
                vm_handle_op_negate(vm);
                break;
            op_OP_NOT: case OP_NOT:
                vm_handle_op_not(vm);
                break;
            op_OP_LENGTH: case OP_LENGTH:
                if (!vm_handle_op_length(vm)) goto runtime_error;
                break;
            op_OP_EQUAL: case OP_EQUAL: {
                if (!vm_handle_op_equal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_GREATER: case OP_GREATER: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_QGREATER_NUM;
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, BOOL_VAL(a > b));
                } else if (!vm_handle_op_greater(vm, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            op_OP_LESS: case OP_LESS: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_QLESS_NUM;
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, BOOL_VAL(a < b));
                } else if (!vm_handle_op_less(vm, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            op_OP_QGREATER_NUM: case OP_QGREATER_NUM: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, BOOL_VAL(a > b));
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_GREATER;
                    if (!vm_handle_op_greater(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_QLESS_NUM: case OP_QLESS_NUM: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    double b = AS_NUMBER(top[-1]);
                    double a = AS_NUMBER(top[-2]);
                    top[-2] = BOOL_VAL(a < b);
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_LESS;
                    if (!vm_handle_op_less(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_HAS: case OP_HAS: {
                if (!vm_handle_op_has(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_IN: case OP_IN: {
                if (!vm_handle_op_in(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_POWER: case OP_POWER: {
                if (!vm_handle_op_power(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_INT_DIV: case OP_INT_DIV: {
                if (!vm_handle_op_int_div(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            op_OP_IMOD: case OP_IMOD: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                double bd = AS_NUMBER(top[-1]);
                double ad = AS_NUMBER(top[-2]);
                if (ad > (double)INT64_MIN && ad < (double)INT64_MAX &&
                    bd > (double)INT64_MIN && bd < (double)INT64_MAX) {
                    int64_t ia = (int64_t)ad;
                    int64_t ib = (int64_t)bd;
                    if ((double)ia == ad && (double)ib == bd && ib != 0) {
                        top[-2] = NUMBER_VAL((double)(ia % ib));
                        thread->stack_top = top - 1;
                        break;
                    }
                }
                top[-2] = NUMBER_VAL(fmod(ad, bd));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_FADD: case OP_FADD: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) + AS_NUMBER(top[-1]));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_FSUB: case OP_FSUB: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) - AS_NUMBER(top[-1]));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_FMUL: case OP_FMUL: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) * AS_NUMBER(top[-1]));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_FDIV: case OP_FDIV: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) / AS_NUMBER(top[-1]));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_FMOD: case OP_FMOD: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                top[-2] = NUMBER_VAL(fmod(AS_NUMBER(top[-2]), AS_NUMBER(top[-1])));
                thread->stack_top = top - 1;
                break;
            }
            op_OP_QFADD: case OP_QFADD: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) + AS_NUMBER(top[-1]));
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_ADD;
                    if (!vm_handle_op_add(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_QFSUB: case OP_QFSUB: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) - AS_NUMBER(top[-1]));
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_SUBTRACT;
                    if (!vm_handle_op_subtract(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_QFMUL: case OP_QFMUL: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) * AS_NUMBER(top[-1]));
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_MULTIPLY;
                    if (!vm_handle_op_multiply(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_QFDIV: case OP_QFDIV: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    top[-2] = NUMBER_VAL(AS_NUMBER(top[-2]) / AS_NUMBER(top[-1]));
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_DIVIDE;
                    if (!vm_handle_op_divide(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_QFMOD: case OP_QFMOD: {
                ObjThread* thread = vm_current_thread(vm);
                Value* top = thread->stack_top;
                if (IS_NUMBER(top[-1]) && IS_NUMBER(top[-2])) {
                    double bd = AS_NUMBER(top[-1]);
                    double ad = AS_NUMBER(top[-2]);
                    if (ad > (double)INT64_MIN && ad < (double)INT64_MAX &&
                        bd > (double)INT64_MIN && bd < (double)INT64_MAX) {
                        int64_t ia = (int64_t)ad;
                        int64_t ib = (int64_t)bd;
                        if ((double)ia == ad && (double)ib == bd && ib != 0) {
                            top[-2] = NUMBER_VAL((double)(ia % ib));
                            thread->stack_top = top - 1;
                            break;
                        }
                    }
                    top[-2] = NUMBER_VAL(fmod(ad, bd));
                    thread->stack_top = top - 1;
                } else {
                    int opcode_offset = (int)(ip - frame->closure->function->chunk.code) - 1;
                    frame->closure->function->chunk.code[opcode_offset] = OP_MODULO;
                    if (!vm_handle_op_modulo(vm, &frame, &ip)) goto runtime_error;
                }
                break;
            }
            op_OP_MOD_CONST: case OP_MOD_CONST: {
                Value b = READ_CONSTANT();
                if (!vm_handle_op_mod_const(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            op_OP_GC: case OP_GC: {
                collect_garbage(vm);
                break;
            }
            op_OP_IMPORT: case OP_IMPORT: {
               ObjString* module_name = READ_STRING();
               InterpretResult import_result = vm_handle_op_import(vm, module_name, &frame, &ip);
               if (import_result == INTERPRET_RUNTIME_ERROR) goto runtime_error;
               if (import_result == INTERPRET_COMPILE_ERROR) {
                   OP_PROF_FLUSH();
                   return INTERPRET_COMPILE_ERROR;
               }
                break;
            }
            op_OP_IMPORT_STAR: case OP_IMPORT_STAR: {
                if (!vm_handle_op_import_star(vm)) goto runtime_error;
                break;
            }
        }
        continue;
runtime_error:
        OP_PROF_FLUSH();
        if (handle_exception(vm, &frame, &ip)) {
            continue;
        }
        return INTERPRET_RUNTIME_ERROR;
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef OP_PROF_FLUSH
}

InterpretResult vm_run_until_thread(VM* vm, int min_frame_count, ObjThread* stop_thread) {
    ObjThread* saved_stop_thread = run_stop_thread;
    run_stop_thread = stop_thread;
    InterpretResult result = vm_run(vm, min_frame_count);
    run_stop_thread = saved_stop_thread;
    return result;
}

InterpretResult interpret(VM* vm, ObjFunction* function) {
    ObjClosure* closure = new_closure(function);
    push(vm, OBJ_VAL(closure));
    call(vm, closure, 0);
    return vm_run(vm, 0);  // Run until all frames complete
}
