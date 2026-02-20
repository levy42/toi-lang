#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../table.h"
#include "../value.h"
#include "../vm.h"

enum {
    BIN_TAG_NIL = 0,
    BIN_TAG_FALSE = 1,
    BIN_TAG_TRUE = 2,
    BIN_TAG_NUMBER = 3,
    BIN_TAG_STRING = 4,
    BIN_TAG_TABLE = 5,
};

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    int failed;
} BinWriter;

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    const char* error;
} BinReader;

static void bw_init(BinWriter* w) {
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    w->failed = 0;
}

static void bw_free(BinWriter* w) {
    free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

static int bw_reserve(BinWriter* w, size_t add) {
    if (w->failed) return 0;
    if (w->len + add <= w->cap) return 1;
    size_t ncap = w->cap == 0 ? 128 : w->cap * 2;
    while (ncap < w->len + add) ncap *= 2;
    uint8_t* p = (uint8_t*)realloc(w->data, ncap);
    if (p == NULL) {
        w->failed = 1;
        return 0;
    }
    w->data = p;
    w->cap = ncap;
    return 1;
}

static int bw_write_u8(BinWriter* w, uint8_t v) {
    if (!bw_reserve(w, 1)) return 0;
    w->data[w->len++] = v;
    return 1;
}

static int bw_write_u32(BinWriter* w, uint32_t v) {
    if (!bw_reserve(w, 4)) return 0;
    w->data[w->len++] = (uint8_t)(v & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 8) & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 16) & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 24) & 0xFF);
    return 1;
}

static int bw_write_f64(BinWriter* w, double v) {
    uint8_t raw[8];
    memcpy(raw, &v, sizeof(double));
    if (!bw_reserve(w, 8)) return 0;
    memcpy(w->data + w->len, raw, 8);
    w->len += 8;
    return 1;
}

static int bw_write_bytes(BinWriter* w, const uint8_t* bytes, size_t n) {
    if (!bw_reserve(w, n)) return 0;
    memcpy(w->data + w->len, bytes, n);
    w->len += n;
    return 1;
}

static int bw_patch_u32(BinWriter* w, size_t off, uint32_t v) {
    if (off + 4 > w->len) return 0;
    w->data[off + 0] = (uint8_t)(v & 0xFF);
    w->data[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    w->data[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    w->data[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    return 1;
}

static int br_read_u8(BinReader* r, uint8_t* out) {
    if (r->pos + 1 > r->len) {
        r->error = "Unexpected end of data.";
        return 0;
    }
    *out = r->data[r->pos++];
    return 1;
}

static int br_read_u32(BinReader* r, uint32_t* out) {
    if (r->pos + 4 > r->len) {
        r->error = "Unexpected end of data.";
        return 0;
    }
    uint32_t b0 = r->data[r->pos++];
    uint32_t b1 = r->data[r->pos++];
    uint32_t b2 = r->data[r->pos++];
    uint32_t b3 = r->data[r->pos++];
    *out = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    return 1;
}

static int br_read_f64(BinReader* r, double* out) {
    if (r->pos + 8 > r->len) {
        r->error = "Unexpected end of data.";
        return 0;
    }
    memcpy(out, r->data + r->pos, 8);
    r->pos += 8;
    return 1;
}

static int is_serializable(Value v) {
    if (IS_NIL(v) || IS_BOOL(v) || IS_NUMBER(v) || IS_STRING(v)) return 1;
    if (IS_TABLE(v)) return 1;
    return 0;
}

static int key_from_entry(VM* vm, Entry* entry, Value* out_key) {
    ObjString* key = entry->key;
    if (key == NULL) return 0;
    if (key->length >= 2 && key->chars[0] == '\x1F' && key->chars[1] == 'n') {
        char* endptr = NULL;
        double num = strtod(key->chars + 2, &endptr);
        if (endptr != key->chars + key->length) return 0;
        *out_key = NUMBER_VAL(num);
        return 1;
    }
    (void)vm;
    *out_key = OBJ_VAL(key);
    return 1;
}

static int serialize_value(VM* vm, BinWriter* w, Value v, int depth, int strict_table);

static int serialize_table(VM* vm, BinWriter* w, ObjTable* t, int depth) {
    if (!bw_write_u8(w, BIN_TAG_TABLE)) return 0;

    size_t arr_count_pos = w->len;
    if (!bw_write_u32(w, 0)) return 0;
    uint32_t arr_count = 0;

    for (int i = 1; ; i++) {
        Value val = NIL_VAL;
        if (!table_get_array(&t->table, i, &val) || IS_NIL(val)) break;
        if (!is_serializable(val)) continue;
        if (!bw_write_u32(w, (uint32_t)i)) return 0;
        if (!serialize_value(vm, w, val, depth + 1, 1)) return 0;
        arr_count++;
    }
    if (!bw_patch_u32(w, arr_count_pos, arr_count)) return 0;

    size_t hash_count_pos = w->len;
    if (!bw_write_u32(w, 0)) return 0;
    uint32_t hash_count = 0;

    for (int i = 0; i < t->table.capacity; i++) {
        Entry* entry = &t->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        Value key;
        if (!key_from_entry(vm, entry, &key)) continue;
        if (!(IS_STRING(key) || IS_NUMBER(key) || IS_BOOL(key))) continue;
        if (!is_serializable(entry->value)) continue;

        if (!serialize_value(vm, w, key, depth + 1, 1)) return 0;
        if (!serialize_value(vm, w, entry->value, depth + 1, 1)) return 0;
        hash_count++;
    }
    if (!bw_patch_u32(w, hash_count_pos, hash_count)) return 0;
    return 1;
}

static int serialize_value(VM* vm, BinWriter* w, Value v, int depth, int strict_table) {
    if (depth > 64) {
        if (strict_table) return 0;
        return bw_write_u8(w, BIN_TAG_NIL);
    }
    if (IS_NIL(v)) return bw_write_u8(w, BIN_TAG_NIL);
    if (IS_BOOL(v)) return bw_write_u8(w, AS_BOOL(v) ? BIN_TAG_TRUE : BIN_TAG_FALSE);
    if (IS_NUMBER(v)) {
        if (!bw_write_u8(w, BIN_TAG_NUMBER)) return 0;
        return bw_write_f64(w, AS_NUMBER(v));
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (!bw_write_u8(w, BIN_TAG_STRING)) return 0;
        if (!bw_write_u32(w, (uint32_t)s->length)) return 0;
        return bw_write_bytes(w, (const uint8_t*)s->chars, (size_t)s->length);
    }
    if (IS_TABLE(v)) {
        return serialize_table(vm, w, AS_TABLE(v), depth);
    }
    if (strict_table) return 0;
    return bw_write_u8(w, BIN_TAG_NIL);
}

static int set_number_key(VM* vm, ObjTable* table, double num, Value value) {
    int idx = (int)num;
    if (num == (double)idx) {
        if (table_set_array(&table->table, idx, value)) return 1;
    }
    ObjString* key = number_key_string(num);
    table_set(&table->table, key, value);
    (void)vm;
    return 1;
}

static Value deserialize_value(VM* vm, BinReader* r, int depth, int* ok);

static Value deserialize_table(VM* vm, BinReader* r, int depth, int* ok) {
    if (depth > 64) {
        r->error = "Maximum unpack depth exceeded.";
        *ok = 0;
        return NIL_VAL;
    }

    uint32_t arr_count = 0;
    if (!br_read_u32(r, &arr_count)) {
        *ok = 0;
        return NIL_VAL;
    }

    ObjTable* t = new_table();
    Value table_val = OBJ_VAL(t);
    push(vm, table_val);

    for (uint32_t i = 0; i < arr_count; i++) {
        uint32_t idx = 0;
        if (!br_read_u32(r, &idx)) {
            *ok = 0;
            pop(vm);
            return NIL_VAL;
        }
        Value val = deserialize_value(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }
        if (idx > 0) {
            table_set_array(&t->table, (int)idx, val);
        }
    }

    uint32_t hash_count = 0;
    if (!br_read_u32(r, &hash_count)) {
        *ok = 0;
        pop(vm);
        return NIL_VAL;
    }

    for (uint32_t i = 0; i < hash_count; i++) {
        Value key = deserialize_value(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }
        Value val = deserialize_value(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }

        if (IS_STRING(key)) {
            table_set(&t->table, AS_STRING(key), val);
        } else if (IS_NUMBER(key)) {
            set_number_key(vm, t, AS_NUMBER(key), val);
        } else if (IS_BOOL(key)) {
            ObjString* skey = copy_string(AS_BOOL(key) ? "true" : "false", AS_BOOL(key) ? 4 : 5);
            table_set(&t->table, skey, val);
        }
    }

    pop(vm);
    return table_val;
}

static Value deserialize_value(VM* vm, BinReader* r, int depth, int* ok) {
    uint8_t tag = 0;
    if (!br_read_u8(r, &tag)) {
        *ok = 0;
        return NIL_VAL;
    }

    switch (tag) {
        case BIN_TAG_NIL:
            return NIL_VAL;
        case BIN_TAG_FALSE:
            return BOOL_VAL(0);
        case BIN_TAG_TRUE:
            return BOOL_VAL(1);
        case BIN_TAG_NUMBER: {
            double num = 0.0;
            if (!br_read_f64(r, &num)) {
                *ok = 0;
                return NIL_VAL;
            }
            return NUMBER_VAL(num);
        }
        case BIN_TAG_STRING: {
            uint32_t len = 0;
            if (!br_read_u32(r, &len)) {
                *ok = 0;
                return NIL_VAL;
            }
            if (r->pos + len > r->len) {
                r->error = "Unexpected end of data.";
                *ok = 0;
                return NIL_VAL;
            }
            ObjString* s = copy_string((const char*)(r->data + r->pos), (int)len);
            r->pos += len;
            return OBJ_VAL(s);
        }
        case BIN_TAG_TABLE:
            return deserialize_table(vm, r, depth, ok);
        default:
            r->error = "Unknown binary tag.";
            *ok = 0;
            return NIL_VAL;
    }
}

static int binary_pack(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    BinWriter w;
    bw_init(&w);

    int prev_disable = vm->disable_gc;
    vm->disable_gc = 1;
    int ok = serialize_value(vm, &w, args[0], 0, 0);
    vm->disable_gc = prev_disable;

    if (!ok || w.failed) {
        bw_free(&w);
        vm_runtime_error(vm, "binary.pack failed.");
        return 0;
    }

    ObjString* out = copy_string((const char*)w.data, (int)w.len);
    bw_free(&w);
    RETURN_OBJ(out);
}

static int binary_unpack(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* bytes = GET_STRING(0);
    BinReader r;
    r.data = (const uint8_t*)bytes->chars;
    r.len = (size_t)bytes->length;
    r.pos = 0;
    r.error = NULL;

    int ok = 1;
    int prev_disable = vm->disable_gc;
    vm->disable_gc = 1;
    Value out = deserialize_value(vm, &r, 0, &ok);
    vm->disable_gc = prev_disable;

    if (!ok) {
        vm_runtime_error(vm, "binary.unpack failed: %s", r.error ? r.error : "invalid data");
        return 0;
    }
    if (r.pos != r.len) {
        vm_runtime_error(vm, "binary.unpack failed: trailing data.");
        return 0;
    }
    RETURN_VAL(out);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int binary_hex(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* bytes = GET_STRING(0);
    int out_len = bytes->length * 2;
    char* out = (char*)malloc((size_t)out_len + 1);
    if (out == NULL) {
        vm_runtime_error(vm, "Out of memory.");
        return 0;
    }

    static const char* HEX = "0123456789abcdef";
    for (int i = 0; i < bytes->length; i++) {
        unsigned char b = (unsigned char)bytes->chars[i];
        out[i * 2] = HEX[(b >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[b & 0x0F];
    }
    out[out_len] = '\0';

    ObjString* s = copy_string(out, out_len);
    free(out);
    RETURN_OBJ(s);
}

static int binary_unhex(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* hex = GET_STRING(0);
    if ((hex->length % 2) != 0) {
        vm_runtime_error(vm, "binary.unhex expects even-length hex string.");
        return 0;
    }

    int out_len = hex->length / 2;
    char* out = (char*)malloc((size_t)out_len + 1);
    if (out == NULL) {
        vm_runtime_error(vm, "Out of memory.");
        return 0;
    }

    for (int i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex->chars[i * 2]);
        int lo = hex_nibble(hex->chars[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            vm_runtime_error(vm, "binary.unhex got invalid hex character.");
            return 0;
        }
        out[i] = (char)((hi << 4) | lo);
    }
    out[out_len] = '\0';

    ObjString* s = copy_string(out, out_len);
    free(out);
    RETURN_OBJ(s);
}

void register_binary(VM* vm) {
    const NativeReg binary_funcs[] = {
        {"pack", binary_pack},
        {"unpack", binary_unpack},
        {"hex", binary_hex},
        {"unhex", binary_unhex},
        {NULL, NULL}
    };
    register_module(vm, "binary", binary_funcs);
    pop(vm);
}
