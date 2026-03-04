#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build_string.h"
#include "../lib/libs.h"

static inline int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

static inline int int64_dec_len(int64_t value) {
    uint64_t u = (value < 0) ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    int len = 1;
    while (u >= 10) {
        u /= 10;
        len++;
    }
    if (value < 0) len++;
    return len;
}

static inline char* write_int64_dec(char* dst, int64_t value) {
    char tmp[32];
    int idx = 0;
    uint64_t u = (value < 0) ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    do {
        tmp[idx++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u > 0);
    if (value < 0) tmp[idx++] = '-';
    for (int i = idx - 1; i >= 0; i--) {
        *dst++ = tmp[i];
    }
    return dst;
}

static inline int number_text_len(double number) {
    int64_t i64;
    if (to_int64_local(number, &i64)) {
        return int64_dec_len(i64);
    }
    char num_buf[32];
    int num_len = snprintf(num_buf, sizeof(num_buf), "%.14g", number);
    return num_len < 0 ? -1 : num_len;
}

static inline char* write_number_text(char* dst, double number) {
    int64_t i64;
    if (to_int64_local(number, &i64)) {
        return write_int64_dec(dst, i64);
    }
    int written = snprintf(dst, 32, "%.14g", number);
    return written < 0 ? NULL : dst + written;
}

static int append_bytes(char** buffer, int* length, int* capacity, const char* src, int src_len) {
    if (src_len <= 0) return 1;
    if (*length + src_len > *capacity) {
        int new_capacity = *capacity < 8 ? 8 : *capacity * 2;
        while (new_capacity < *length + src_len) new_capacity *= 2;
        char* new_buffer = (char*)realloc(*buffer, (size_t)new_capacity + 1);
        if (new_buffer == NULL) return 0;
        *buffer = new_buffer;
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, src, (size_t)src_len);
    *length += src_len;
    (*buffer)[*length] = '\0';
    return 1;
}

static int append_number(char** buffer, int* length, int* capacity, double number) {
    int64_t i64;
    if (to_int64_local(number, &i64)) {
        char tmp[32];
        int idx = 0;
        uint64_t u = (i64 < 0) ? (uint64_t)(-(i64 + 1)) + 1 : (uint64_t)i64;
        do {
            tmp[idx++] = (char)('0' + (u % 10));
            u /= 10;
        } while (u > 0);
        if (i64 < 0) tmp[idx++] = '-';

        char out[32];
        for (int j = 0; j < idx; j++) out[j] = tmp[idx - 1 - j];
        return append_bytes(buffer, length, capacity, out, idx);
    }

    char num_buf[32];
    int num_len = snprintf(num_buf, sizeof(num_buf), "%.14g", number);
    if (num_len < 0) return 0;
    return append_bytes(buffer, length, capacity, num_buf, num_len);
}

static inline uint32_t hash_string_local(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

static int push_built_string_interned(VM* vm, char* buffer, int length) {
    uint32_t hash = hash_string_local(buffer, length);
    uint32_t slot = hash & 65535u;
    ObjString* cached = vm->build_string_cache[slot];
    if (cached != NULL) {
        if (cached->length == length &&
            objstring_hash(cached) == hash &&
            memcmp(cached->chars, buffer, (size_t)length) == 0) {
            free(buffer);
            push(vm, OBJ_VAL(cached));
            return 1;
        }
    }

    ObjString* built = take_string_with_hash(buffer, length, hash);
    vm->build_string_cache[slot] = built;
    push(vm, OBJ_VAL(built));
    return 1;
}

static int push_built_string_interned_view(VM* vm, const char* chars, int length) {
    uint32_t hash = hash_string_local(chars, length);
    uint32_t slot = hash & 65535u;
    ObjString* cached = vm->build_string_cache[slot];
    if (cached != NULL) {
        if (cached->length == length &&
            objstring_hash(cached) == hash &&
            memcmp(cached->chars, chars, (size_t)length) == 0) {
            push(vm, OBJ_VAL(cached));
            return 1;
        }
    }

    char* heap = (char*)malloc((size_t)length + 1);
    if (heap == NULL) {
        vm_runtime_error(vm, "Out of memory while building string.");
        return 0;
    }
    memcpy(heap, chars, (size_t)length);
    heap[length] = '\0';
    ObjString* built = take_string_with_hash(heap, length, hash);
    vm->build_string_cache[slot] = built;
    push(vm, OBJ_VAL(built));
    return 1;
}

int vm_build_string(VM* vm, uint8_t part_count) {
    if (part_count == 0) {
        push(vm, OBJ_VAL(copy_string("", 0)));
        return 1;
    }

    if (part_count == 3) {
        Value v0 = peek(vm, 2);
        Value v1 = peek(vm, 1);
        Value v2 = peek(vm, 0);
        if (IS_STRING(v0) && IS_STRING(v1) && IS_STRING(v2)) {
            ObjString* s0 = AS_STRING(v0);
            ObjString* s1 = AS_STRING(v1);
            ObjString* s2 = AS_STRING(v2);
            uint32_t slot = (uint32_t)((((uintptr_t)s0 >> 4) ^
                                        ((uintptr_t)s1 >> 9) ^
                                        ((uintptr_t)s2 >> 14)) & 4095u);
            BuildString3CacheEntry* e = &vm->build_string3_cache[slot];
            if (e->a == s0 && e->b == s1 && e->c == s2 && e->out != NULL) {
                pop(vm);
                pop(vm);
                pop(vm);
                push(vm, OBJ_VAL(e->out));
                return 1;
            }

            int total_len = s0->length + s1->length + s2->length;
            int ok = 0;
            if (total_len <= 255) {
                char buffer[256];
                char* out = buffer;
                memcpy(out, s0->chars, (size_t)s0->length);
                out += s0->length;
                memcpy(out, s1->chars, (size_t)s1->length);
                out += s1->length;
                memcpy(out, s2->chars, (size_t)s2->length);
                out += s2->length;
                buffer[total_len] = '\0';
                pop(vm);
                pop(vm);
                pop(vm);
                ok = push_built_string_interned_view(vm, buffer, total_len);
            } else {
                char* buffer = (char*)malloc((size_t)total_len + 1);
                if (buffer == NULL) {
                    vm_runtime_error(vm, "Out of memory while building string.");
                    return 0;
                }
                char* out = buffer;
                memcpy(out, s0->chars, (size_t)s0->length);
                out += s0->length;
                memcpy(out, s1->chars, (size_t)s1->length);
                out += s1->length;
                memcpy(out, s2->chars, (size_t)s2->length);
                out += s2->length;
                buffer[total_len] = '\0';
                pop(vm);
                pop(vm);
                pop(vm);
                ok = push_built_string_interned(vm, buffer, total_len);
            }
            if (!ok) return 0;
            ObjString* built = AS_STRING(peek(vm, 0));
            e->a = s0;
            e->b = s1;
            e->c = s2;
            e->out = built;
            return 1;
        }
    }

    int total_len = 0;
    int primitive_only = 1;
    for (int i = part_count - 1; i >= 0; i--) {
        Value part = peek(vm, i);
        if (IS_STRING(part)) {
            total_len += AS_STRING(part)->length;
            continue;
        }
        if (IS_NUMBER(part)) {
            int nlen = number_text_len(AS_NUMBER(part));
            if (nlen < 0) {
                vm_runtime_error(vm, "Out of memory while building string.");
                return 0;
            }
            total_len += nlen;
            continue;
        }
        if (IS_BOOL(part)) {
            total_len += AS_BOOL(part) ? 4 : 5;
            continue;
        }
        if (IS_NIL(part)) {
            total_len += 3;
            continue;
        }
        primitive_only = 0;
        break;
    }

    if (primitive_only) {
        if (total_len <= 255) {
            char buffer[256];
            char* out = buffer;
            for (int i = part_count - 1; i >= 0; i--) {
                Value part = peek(vm, i);
                if (IS_STRING(part)) {
                    ObjString* s = AS_STRING(part);
                    memcpy(out, s->chars, (size_t)s->length);
                    out += s->length;
                } else if (IS_NUMBER(part)) {
                    char* next = write_number_text(out, AS_NUMBER(part));
                    if (next == NULL) {
                        vm_runtime_error(vm, "Out of memory while building string.");
                        return 0;
                    }
                    out = next;
                } else if (IS_BOOL(part)) {
                    if (AS_BOOL(part)) {
                        memcpy(out, "true", 4);
                        out += 4;
                    } else {
                        memcpy(out, "false", 5);
                        out += 5;
                    }
                } else {
                    memcpy(out, "nil", 3);
                    out += 3;
                }
            }
            buffer[total_len] = '\0';
            for (int i = 0; i < part_count; i++) pop(vm);
            return push_built_string_interned_view(vm, buffer, total_len);
        }

        char* buffer = (char*)malloc((size_t)total_len + 1);
        if (buffer == NULL) {
            vm_runtime_error(vm, "Out of memory while building string.");
            return 0;
        }
        char* out = buffer;
        for (int i = part_count - 1; i >= 0; i--) {
            Value part = peek(vm, i);
            if (IS_STRING(part)) {
                ObjString* s = AS_STRING(part);
                memcpy(out, s->chars, (size_t)s->length);
                out += s->length;
            } else if (IS_NUMBER(part)) {
                char* next = write_number_text(out, AS_NUMBER(part));
                if (next == NULL) {
                    free(buffer);
                    vm_runtime_error(vm, "Out of memory while building string.");
                    return 0;
                }
                out = next;
            } else if (IS_BOOL(part)) {
                if (AS_BOOL(part)) {
                    memcpy(out, "true", 4);
                    out += 4;
                } else {
                    memcpy(out, "false", 5);
                    out += 5;
                }
            } else {
                memcpy(out, "nil", 3);
                out += 3;
            }
        }
        buffer[total_len] = '\0';
        for (int i = 0; i < part_count; i++) pop(vm);
        return push_built_string_interned(vm, buffer, total_len);
    }

    char* buffer = NULL;
    int length = 0;
    int capacity = 0;

    for (int i = part_count - 1; i >= 0; i--) {
        Value part = peek(vm, i);
        if (IS_STRING(part)) {
            ObjString* s = AS_STRING(part);
            if (!append_bytes(&buffer, &length, &capacity, s->chars, s->length)) {
                free(buffer);
                vm_runtime_error(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_NUMBER(part)) {
            if (!append_number(&buffer, &length, &capacity, AS_NUMBER(part))) {
                free(buffer);
                vm_runtime_error(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_BOOL(part)) {
            const char* b = AS_BOOL(part) ? "true" : "false";
            int b_len = AS_BOOL(part) ? 4 : 5;
            if (!append_bytes(&buffer, &length, &capacity, b, b_len)) {
                free(buffer);
                vm_runtime_error(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_NIL(part)) {
            if (!append_bytes(&buffer, &length, &capacity, "nil", 3)) {
                free(buffer);
                vm_runtime_error(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        Value arg = part;
        if (!core_tostring(vm, 1, &arg)) {
            free(buffer);
            return 0;
        }

        Value str_val = pop(vm);
        if (!IS_STRING(str_val)) {
            free(buffer);
            vm_runtime_error(vm, "str() must return a string.");
            return 0;
        }
        ObjString* s = AS_STRING(str_val);
        if (!append_bytes(&buffer, &length, &capacity, s->chars, s->length)) {
            free(buffer);
            vm_runtime_error(vm, "Out of memory while building string.");
            return 0;
        }
    }

    for (int i = 0; i < part_count; i++) pop(vm);

    if (buffer == NULL) {
        push(vm, OBJ_VAL(copy_string("", 0)));
    } else {
        if (!push_built_string_interned(vm, buffer, length)) return 0;
    }
    return 1;
}
