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

static void bwInit(BinWriter* w) {
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    w->failed = 0;
}

static void bwFree(BinWriter* w) {
    free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

static int bwReserve(BinWriter* w, size_t add) {
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

static int bwWriteU8(BinWriter* w, uint8_t v) {
    if (!bwReserve(w, 1)) return 0;
    w->data[w->len++] = v;
    return 1;
}

static int bwWriteU32(BinWriter* w, uint32_t v) {
    if (!bwReserve(w, 4)) return 0;
    w->data[w->len++] = (uint8_t)(v & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 8) & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 16) & 0xFF);
    w->data[w->len++] = (uint8_t)((v >> 24) & 0xFF);
    return 1;
}

static int bwWriteF64(BinWriter* w, double v) {
    uint8_t raw[8];
    memcpy(raw, &v, sizeof(double));
    if (!bwReserve(w, 8)) return 0;
    memcpy(w->data + w->len, raw, 8);
    w->len += 8;
    return 1;
}

static int bwWriteBytes(BinWriter* w, const uint8_t* bytes, size_t n) {
    if (!bwReserve(w, n)) return 0;
    memcpy(w->data + w->len, bytes, n);
    w->len += n;
    return 1;
}

static int bwPatchU32(BinWriter* w, size_t off, uint32_t v) {
    if (off + 4 > w->len) return 0;
    w->data[off + 0] = (uint8_t)(v & 0xFF);
    w->data[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    w->data[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    w->data[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    return 1;
}

static int brReadU8(BinReader* r, uint8_t* out) {
    if (r->pos + 1 > r->len) {
        r->error = "Unexpected end of data.";
        return 0;
    }
    *out = r->data[r->pos++];
    return 1;
}

static int brReadU32(BinReader* r, uint32_t* out) {
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

static int brReadF64(BinReader* r, double* out) {
    if (r->pos + 8 > r->len) {
        r->error = "Unexpected end of data.";
        return 0;
    }
    memcpy(out, r->data + r->pos, 8);
    r->pos += 8;
    return 1;
}

static int isSerializable(Value v) {
    if (IS_NIL(v) || IS_BOOL(v) || IS_NUMBER(v) || IS_STRING(v)) return 1;
    if (IS_TABLE(v)) return 1;
    return 0;
}

static int keyFromEntry(VM* vm, Entry* entry, Value* outKey) {
    ObjString* key = entry->key;
    if (key == NULL) return 0;
    if (key->length >= 2 && key->chars[0] == '\x1F' && key->chars[1] == 'n') {
        char* endptr = NULL;
        double num = strtod(key->chars + 2, &endptr);
        if (endptr != key->chars + key->length) return 0;
        *outKey = NUMBER_VAL(num);
        return 1;
    }
    (void)vm;
    *outKey = OBJ_VAL(key);
    return 1;
}

static int serializeValue(VM* vm, BinWriter* w, Value v, int depth, int strictTable);

static int serializeTable(VM* vm, BinWriter* w, ObjTable* t, int depth) {
    if (!bwWriteU8(w, BIN_TAG_TABLE)) return 0;

    size_t arrCountPos = w->len;
    if (!bwWriteU32(w, 0)) return 0;
    uint32_t arrCount = 0;

    for (int i = 1; ; i++) {
        Value val = NIL_VAL;
        if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) break;
        if (!isSerializable(val)) continue;
        if (!bwWriteU32(w, (uint32_t)i)) return 0;
        if (!serializeValue(vm, w, val, depth + 1, 1)) return 0;
        arrCount++;
    }
    if (!bwPatchU32(w, arrCountPos, arrCount)) return 0;

    size_t hashCountPos = w->len;
    if (!bwWriteU32(w, 0)) return 0;
    uint32_t hashCount = 0;

    for (int i = 0; i < t->table.capacity; i++) {
        Entry* entry = &t->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        Value key;
        if (!keyFromEntry(vm, entry, &key)) continue;
        if (!(IS_STRING(key) || IS_NUMBER(key) || IS_BOOL(key))) continue;
        if (!isSerializable(entry->value)) continue;

        if (!serializeValue(vm, w, key, depth + 1, 1)) return 0;
        if (!serializeValue(vm, w, entry->value, depth + 1, 1)) return 0;
        hashCount++;
    }
    if (!bwPatchU32(w, hashCountPos, hashCount)) return 0;
    return 1;
}

static int serializeValue(VM* vm, BinWriter* w, Value v, int depth, int strictTable) {
    if (depth > 64) {
        if (strictTable) return 0;
        return bwWriteU8(w, BIN_TAG_NIL);
    }
    if (IS_NIL(v)) return bwWriteU8(w, BIN_TAG_NIL);
    if (IS_BOOL(v)) return bwWriteU8(w, AS_BOOL(v) ? BIN_TAG_TRUE : BIN_TAG_FALSE);
    if (IS_NUMBER(v)) {
        if (!bwWriteU8(w, BIN_TAG_NUMBER)) return 0;
        return bwWriteF64(w, AS_NUMBER(v));
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (!bwWriteU8(w, BIN_TAG_STRING)) return 0;
        if (!bwWriteU32(w, (uint32_t)s->length)) return 0;
        return bwWriteBytes(w, (const uint8_t*)s->chars, (size_t)s->length);
    }
    if (IS_TABLE(v)) {
        return serializeTable(vm, w, AS_TABLE(v), depth);
    }
    if (strictTable) return 0;
    return bwWriteU8(w, BIN_TAG_NIL);
}

static int setNumberKey(VM* vm, ObjTable* table, double num, Value value) {
    int idx = (int)num;
    if (num == (double)idx) {
        if (tableSetArray(&table->table, idx, value)) return 1;
    }
    ObjString* key = numberKeyString(num);
    tableSet(&table->table, key, value);
    (void)vm;
    return 1;
}

static Value deserializeValue(VM* vm, BinReader* r, int depth, int* ok);

static Value deserializeTable(VM* vm, BinReader* r, int depth, int* ok) {
    if (depth > 64) {
        r->error = "Maximum unpack depth exceeded.";
        *ok = 0;
        return NIL_VAL;
    }

    uint32_t arrCount = 0;
    if (!brReadU32(r, &arrCount)) {
        *ok = 0;
        return NIL_VAL;
    }

    ObjTable* t = newTable();
    Value tableVal = OBJ_VAL(t);
    push(vm, tableVal);

    for (uint32_t i = 0; i < arrCount; i++) {
        uint32_t idx = 0;
        if (!brReadU32(r, &idx)) {
            *ok = 0;
            pop(vm);
            return NIL_VAL;
        }
        Value val = deserializeValue(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }
        if (idx > 0) {
            tableSetArray(&t->table, (int)idx, val);
        }
    }

    uint32_t hashCount = 0;
    if (!brReadU32(r, &hashCount)) {
        *ok = 0;
        pop(vm);
        return NIL_VAL;
    }

    for (uint32_t i = 0; i < hashCount; i++) {
        Value key = deserializeValue(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }
        Value val = deserializeValue(vm, r, depth + 1, ok);
        if (!*ok) {
            pop(vm);
            return NIL_VAL;
        }

        if (IS_STRING(key)) {
            tableSet(&t->table, AS_STRING(key), val);
        } else if (IS_NUMBER(key)) {
            setNumberKey(vm, t, AS_NUMBER(key), val);
        } else if (IS_BOOL(key)) {
            ObjString* skey = copyString(AS_BOOL(key) ? "true" : "false", AS_BOOL(key) ? 4 : 5);
            tableSet(&t->table, skey, val);
        }
    }

    pop(vm);
    return tableVal;
}

static Value deserializeValue(VM* vm, BinReader* r, int depth, int* ok) {
    uint8_t tag = 0;
    if (!brReadU8(r, &tag)) {
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
            if (!brReadF64(r, &num)) {
                *ok = 0;
                return NIL_VAL;
            }
            return NUMBER_VAL(num);
        }
        case BIN_TAG_STRING: {
            uint32_t len = 0;
            if (!brReadU32(r, &len)) {
                *ok = 0;
                return NIL_VAL;
            }
            if (r->pos + len > r->len) {
                r->error = "Unexpected end of data.";
                *ok = 0;
                return NIL_VAL;
            }
            ObjString* s = copyString((const char*)(r->data + r->pos), (int)len);
            r->pos += len;
            return OBJ_VAL(s);
        }
        case BIN_TAG_TABLE:
            return deserializeTable(vm, r, depth, ok);
        default:
            r->error = "Unknown binary tag.";
            *ok = 0;
            return NIL_VAL;
    }
}

static int binary_pack(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    BinWriter w;
    bwInit(&w);

    int prevDisable = vm->disableGC;
    vm->disableGC = 1;
    int ok = serializeValue(vm, &w, args[0], 0, 0);
    vm->disableGC = prevDisable;

    if (!ok || w.failed) {
        bwFree(&w);
        vmRuntimeError(vm, "binary.pack failed.");
        return 0;
    }

    ObjString* out = copyString((const char*)w.data, (int)w.len);
    bwFree(&w);
    RETURN_OBJ(out);
}

static int binary_unpack(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* bytes = GET_STRING(0);
    BinReader r;
    r.data = (const uint8_t*)bytes->chars;
    r.len = (size_t)bytes->length;
    r.pos = 0;
    r.error = NULL;

    int ok = 1;
    int prevDisable = vm->disableGC;
    vm->disableGC = 1;
    Value out = deserializeValue(vm, &r, 0, &ok);
    vm->disableGC = prevDisable;

    if (!ok) {
        vmRuntimeError(vm, "binary.unpack failed: %s", r.error ? r.error : "invalid data");
        return 0;
    }
    if (r.pos != r.len) {
        vmRuntimeError(vm, "binary.unpack failed: trailing data.");
        return 0;
    }
    RETURN_VAL(out);
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int binary_hex(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* bytes = GET_STRING(0);
    int outLen = bytes->length * 2;
    char* out = (char*)malloc((size_t)outLen + 1);
    if (out == NULL) {
        vmRuntimeError(vm, "Out of memory.");
        return 0;
    }

    static const char* HEX = "0123456789abcdef";
    for (int i = 0; i < bytes->length; i++) {
        unsigned char b = (unsigned char)bytes->chars[i];
        out[i * 2] = HEX[(b >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[b & 0x0F];
    }
    out[outLen] = '\0';

    ObjString* s = copyString(out, outLen);
    free(out);
    RETURN_OBJ(s);
}

static int binary_unhex(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* hex = GET_STRING(0);
    if ((hex->length % 2) != 0) {
        vmRuntimeError(vm, "binary.unhex expects even-length hex string.");
        return 0;
    }

    int outLen = hex->length / 2;
    char* out = (char*)malloc((size_t)outLen + 1);
    if (out == NULL) {
        vmRuntimeError(vm, "Out of memory.");
        return 0;
    }

    for (int i = 0; i < outLen; i++) {
        int hi = hexNibble(hex->chars[i * 2]);
        int lo = hexNibble(hex->chars[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            vmRuntimeError(vm, "binary.unhex got invalid hex character.");
            return 0;
        }
        out[i] = (char)((hi << 4) | lo);
    }
    out[outLen] = '\0';

    ObjString* s = copyString(out, outLen);
    free(out);
    RETURN_OBJ(s);
}

void registerBinary(VM* vm) {
    const NativeReg binaryFuncs[] = {
        {"pack", binary_pack},
        {"unpack", binary_unpack},
        {"hex", binary_hex},
        {"unhex", binary_unhex},
        {NULL, NULL}
    };
    registerModule(vm, "binary", binaryFuncs);
    pop(vm);
}
