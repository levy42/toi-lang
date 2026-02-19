#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../table.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} Buf;

typedef struct {
    const char* fmt;
    int len;
    int pos;
    int little_endian;
    const char* error;
} FmtParser;

static void bufInit(Buf* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bufFree(Buf* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int bufReserve(Buf* b, size_t add) {
    if (b->len + add <= b->cap) return 1;
    size_t ncap = b->cap == 0 ? 128 : b->cap * 2;
    while (ncap < b->len + add) ncap *= 2;
    uint8_t* p = (uint8_t*)realloc(b->data, ncap);
    if (p == NULL) return 0;
    b->data = p;
    b->cap = ncap;
    return 1;
}

static int bufWriteByte(Buf* b, uint8_t v) {
    if (!bufReserve(b, 1)) return 0;
    b->data[b->len++] = v;
    return 1;
}

static int bufWriteBytes(Buf* b, const uint8_t* s, size_t n) {
    if (!bufReserve(b, n)) return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

static int parseIntArg(Value v, int64_t minv, int64_t maxv, int64_t* out) {
    if (!IS_NUMBER(v)) return 0;
    double d = AS_NUMBER(v);
    if (!isfinite(d)) return 0;
    int64_t i = (int64_t)d;
    if ((double)i != d) return 0;
    if (i < minv || i > maxv) return 0;
    *out = i;
    return 1;
}

static int parseUIntArg(Value v, uint64_t maxv, uint64_t* out) {
    if (!IS_NUMBER(v)) return 0;
    double d = AS_NUMBER(v);
    if (!isfinite(d) || d < 0) return 0;
    uint64_t u = (uint64_t)d;
    if ((double)u != d) return 0;
    if (u > maxv) return 0;
    *out = u;
    return 1;
}

static void writeInt(Buf* b, uint64_t u, int nbytes, int little) {
    if (little) {
        for (int i = 0; i < nbytes; i++) {
            bufWriteByte(b, (uint8_t)((u >> (i * 8)) & 0xFF));
        }
    } else {
        for (int i = nbytes - 1; i >= 0; i--) {
            bufWriteByte(b, (uint8_t)((u >> (i * 8)) & 0xFF));
        }
    }
}

static uint64_t readUInt(const uint8_t* data, size_t at, int nbytes, int little) {
    uint64_t u = 0;
    if (little) {
        for (int i = 0; i < nbytes; i++) {
            u |= ((uint64_t)data[at + i]) << (i * 8);
        }
    } else {
        for (int i = 0; i < nbytes; i++) {
            u = (u << 8) | (uint64_t)data[at + i];
        }
    }
    return u;
}

static void skipWs(FmtParser* p) {
    while (p->pos < p->len && isspace((unsigned char)p->fmt[p->pos])) p->pos++;
}

static int parseRepeat(FmtParser* p) {
    int rep = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->fmt[p->pos])) {
        int d = p->fmt[p->pos] - '0';
        if (rep > 100000000) {
            p->error = "Repeat count too large.";
            return -1;
        }
        rep = rep * 10 + d;
        p->pos++;
    }
    return rep;
}

static int nextToken(FmtParser* p, int* outRepeat, char* outCode) {
    skipWs(p);
    if (p->pos >= p->len) return 0;
    int rep = parseRepeat(p);
    if (rep < 0) return -1;
    if (p->pos >= p->len) {
        p->error = "Expected format code.";
        return -1;
    }
    char c = p->fmt[p->pos++];
    *outRepeat = rep;
    *outCode = c;
    return 1;
}

static int struct_pack(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* fmt = GET_STRING(0);
    FmtParser p;
    p.fmt = fmt->chars;
    p.len = fmt->length;
    p.pos = 0;
    p.little_endian = 1;
    p.error = NULL;

    skipWs(&p);
    if (p.pos < p.len && (p.fmt[p.pos] == '<' || p.fmt[p.pos] == '>')) {
        p.little_endian = (p.fmt[p.pos] == '<');
        p.pos++;
    }

    Buf out;
    bufInit(&out);
    int argi = 1;

    for (;;) {
        int rep = 0;
        char code = 0;
        int rc = nextToken(&p, &rep, &code);
        if (rc == 0) break;
        if (rc < 0) {
            bufFree(&out);
            vmRuntimeError(vm, "struct.pack: %s", p.error);
            return 0;
        }

        int count = rep > 0 ? rep : 1;
        if (code == 'x') {
            for (int i = 0; i < count; i++) {
                if (!bufWriteByte(&out, 0)) {
                    bufFree(&out);
                    vmRuntimeError(vm, "struct.pack: out of memory.");
                    return 0;
                }
            }
            continue;
        }

        if (code == 's') {
            if (argi >= argCount) {
                bufFree(&out);
                vmRuntimeError(vm, "struct.pack: missing argument for '%ds'.", count);
                return 0;
            }
            if (!IS_STRING(args[argi])) {
                bufFree(&out);
                vmRuntimeError(vm, "struct.pack: '%ds' expects string argument.", count);
                return 0;
            }
            ObjString* s = AS_STRING(args[argi++]);
            int ncopy = s->length < count ? s->length : count;
            if (!bufWriteBytes(&out, (const uint8_t*)s->chars, (size_t)ncopy)) {
                bufFree(&out);
                vmRuntimeError(vm, "struct.pack: out of memory.");
                return 0;
            }
            for (int i = ncopy; i < count; i++) {
                if (!bufWriteByte(&out, 0)) {
                    bufFree(&out);
                    vmRuntimeError(vm, "struct.pack: out of memory.");
                    return 0;
                }
            }
            continue;
        }

        for (int i = 0; i < count; i++) {
            if (argi >= argCount) {
                bufFree(&out);
                vmRuntimeError(vm, "struct.pack: not enough arguments.");
                return 0;
            }

            switch (code) {
                case 'b': {
                    int64_t v = 0;
                    if (!parseIntArg(args[argi++], -128, 127, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'b' expects int8.");
                        return 0;
                    }
                    writeInt(&out, (uint8_t)((int8_t)v), 1, p.little_endian);
                    break;
                }
                case 'B': {
                    uint64_t v = 0;
                    if (!parseUIntArg(args[argi++], 255, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'B' expects uint8.");
                        return 0;
                    }
                    writeInt(&out, v, 1, p.little_endian);
                    break;
                }
                case 'h': {
                    int64_t v = 0;
                    if (!parseIntArg(args[argi++], -32768, 32767, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'h' expects int16.");
                        return 0;
                    }
                    writeInt(&out, (uint16_t)((int16_t)v), 2, p.little_endian);
                    break;
                }
                case 'H': {
                    uint64_t v = 0;
                    if (!parseUIntArg(args[argi++], 65535, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'H' expects uint16.");
                        return 0;
                    }
                    writeInt(&out, v, 2, p.little_endian);
                    break;
                }
                case 'i': {
                    int64_t v = 0;
                    if (!parseIntArg(args[argi++], INT32_MIN, INT32_MAX, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'i' expects int32.");
                        return 0;
                    }
                    writeInt(&out, (uint32_t)((int32_t)v), 4, p.little_endian);
                    break;
                }
                case 'I': {
                    uint64_t v = 0;
                    if (!parseUIntArg(args[argi++], UINT32_MAX, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'I' expects uint32.");
                        return 0;
                    }
                    writeInt(&out, v, 4, p.little_endian);
                    break;
                }
                case 'q': {
                    int64_t v = 0;
                    if (!parseIntArg(args[argi++], INT64_MIN, INT64_MAX, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'q' expects int64.");
                        return 0;
                    }
                    writeInt(&out, (uint64_t)v, 8, p.little_endian);
                    break;
                }
                case 'Q': {
                    uint64_t v = 0;
                    if (!parseUIntArg(args[argi++], UINT64_MAX, &v)) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'Q' expects uint64.");
                        return 0;
                    }
                    writeInt(&out, v, 8, p.little_endian);
                    break;
                }
                case 'f': {
                    if (!IS_NUMBER(args[argi])) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'f' expects number.");
                        return 0;
                    }
                    float fv = (float)AS_NUMBER(args[argi++]);
                    uint32_t u = 0;
                    memcpy(&u, &fv, sizeof(uint32_t));
                    writeInt(&out, u, 4, p.little_endian);
                    break;
                }
                case 'd': {
                    if (!IS_NUMBER(args[argi])) {
                        bufFree(&out);
                        vmRuntimeError(vm, "struct.pack: 'd' expects number.");
                        return 0;
                    }
                    double dv = AS_NUMBER(args[argi++]);
                    uint64_t u = 0;
                    memcpy(&u, &dv, sizeof(uint64_t));
                    writeInt(&out, u, 8, p.little_endian);
                    break;
                }
                default:
                    bufFree(&out);
                    vmRuntimeError(vm, "struct.pack: unsupported format '%c'.", code);
                    return 0;
            }
            if (out.data == NULL && out.len > 0) {
                vmRuntimeError(vm, "struct.pack: out of memory.");
                return 0;
            }
        }
    }

    if (argi != argCount) {
        bufFree(&out);
        vmRuntimeError(vm, "struct.pack: too many arguments.");
        return 0;
    }

    ObjString* bytes = copyString((const char*)out.data, (int)out.len);
    bufFree(&out);
    RETURN_OBJ(bytes);
}

static int struct_unpack(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    if (argCount > 3) {
        vmRuntimeError(vm, "Expected at most 3 arguments but got %d.", argCount);
        return 0;
    }
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    ObjString* fmt = GET_STRING(0);
    ObjString* bytes = GET_STRING(1);

    int offset = 1;
    if (argCount == 3) {
        ASSERT_NUMBER(2);
        offset = (int)GET_NUMBER(2);
    }
    if (offset < 1) {
        vmRuntimeError(vm, "struct.unpack: offset must be >= 1.");
        return 0;
    }

    FmtParser p;
    p.fmt = fmt->chars;
    p.len = fmt->length;
    p.pos = 0;
    p.little_endian = 1;
    p.error = NULL;

    skipWs(&p);
    if (p.pos < p.len && (p.fmt[p.pos] == '<' || p.fmt[p.pos] == '>')) {
        p.little_endian = (p.fmt[p.pos] == '<');
        p.pos++;
    }

    size_t at = (size_t)(offset - 1);
    ObjTable* out = newTable();
    push(vm, OBJ_VAL(out));
    int outIndex = 1;

    for (;;) {
        int rep = 0;
        char code = 0;
        int rc = nextToken(&p, &rep, &code);
        if (rc == 0) break;
        if (rc < 0) {
            pop(vm);
            vmRuntimeError(vm, "struct.unpack: %s", p.error);
            return 0;
        }

        int count = rep > 0 ? rep : 1;
        if (code == 'x') {
            size_t need = (size_t)count;
            if (at + need > (size_t)bytes->length) {
                pop(vm);
                vmRuntimeError(vm, "struct.unpack: buffer too short.");
                return 0;
            }
            at += need;
            continue;
        }

        if (code == 's') {
            size_t need = (size_t)count;
            if (at + need > (size_t)bytes->length) {
                pop(vm);
                vmRuntimeError(vm, "struct.unpack: buffer too short.");
                return 0;
            }
            ObjString* s = copyString(bytes->chars + at, count);
            tableSetArray(&out->table, outIndex++, OBJ_VAL(s));
            at += need;
            continue;
        }

        for (int i = 0; i < count; i++) {
            Value v = NIL_VAL;
            switch (code) {
                case 'b': {
                    if (at + 1 > (size_t)bytes->length) goto short_buffer;
                    int8_t x = (int8_t)bytes->chars[at];
                    v = NUMBER_VAL((double)x);
                    at += 1;
                    break;
                }
                case 'B': {
                    if (at + 1 > (size_t)bytes->length) goto short_buffer;
                    uint8_t x = (uint8_t)bytes->chars[at];
                    v = NUMBER_VAL((double)x);
                    at += 1;
                    break;
                }
                case 'h': {
                    if (at + 2 > (size_t)bytes->length) goto short_buffer;
                    uint16_t u = (uint16_t)readUInt((const uint8_t*)bytes->chars, at, 2, p.little_endian);
                    int16_t x;
                    memcpy(&x, &u, sizeof(int16_t));
                    v = NUMBER_VAL((double)x);
                    at += 2;
                    break;
                }
                case 'H': {
                    if (at + 2 > (size_t)bytes->length) goto short_buffer;
                    uint16_t x = (uint16_t)readUInt((const uint8_t*)bytes->chars, at, 2, p.little_endian);
                    v = NUMBER_VAL((double)x);
                    at += 2;
                    break;
                }
                case 'i': {
                    if (at + 4 > (size_t)bytes->length) goto short_buffer;
                    uint32_t u = (uint32_t)readUInt((const uint8_t*)bytes->chars, at, 4, p.little_endian);
                    int32_t x;
                    memcpy(&x, &u, sizeof(int32_t));
                    v = NUMBER_VAL((double)x);
                    at += 4;
                    break;
                }
                case 'I': {
                    if (at + 4 > (size_t)bytes->length) goto short_buffer;
                    uint32_t x = (uint32_t)readUInt((const uint8_t*)bytes->chars, at, 4, p.little_endian);
                    v = NUMBER_VAL((double)x);
                    at += 4;
                    break;
                }
                case 'q': {
                    if (at + 8 > (size_t)bytes->length) goto short_buffer;
                    uint64_t u = readUInt((const uint8_t*)bytes->chars, at, 8, p.little_endian);
                    int64_t x;
                    memcpy(&x, &u, sizeof(int64_t));
                    v = NUMBER_VAL((double)x);
                    at += 8;
                    break;
                }
                case 'Q': {
                    if (at + 8 > (size_t)bytes->length) goto short_buffer;
                    uint64_t x = readUInt((const uint8_t*)bytes->chars, at, 8, p.little_endian);
                    v = NUMBER_VAL((double)x);
                    at += 8;
                    break;
                }
                case 'f': {
                    if (at + 4 > (size_t)bytes->length) goto short_buffer;
                    uint32_t u = (uint32_t)readUInt((const uint8_t*)bytes->chars, at, 4, p.little_endian);
                    float x;
                    memcpy(&x, &u, sizeof(float));
                    v = NUMBER_VAL((double)x);
                    at += 4;
                    break;
                }
                case 'd': {
                    if (at + 8 > (size_t)bytes->length) goto short_buffer;
                    uint64_t u = readUInt((const uint8_t*)bytes->chars, at, 8, p.little_endian);
                    double x;
                    memcpy(&x, &u, sizeof(double));
                    v = NUMBER_VAL(x);
                    at += 8;
                    break;
                }
                default:
                    pop(vm);
                    vmRuntimeError(vm, "struct.unpack: unsupported format '%c'.", code);
                    return 0;
            }
            tableSetArray(&out->table, outIndex++, v);
        }
    }

    pop(vm);
    RETURN_OBJ(out);

short_buffer:
    pop(vm);
    vmRuntimeError(vm, "struct.unpack: buffer too short.");
    return 0;
}

void registerStruct(VM* vm) {
    const NativeReg structFuncs[] = {
        {"pack", struct_pack},
        {"unpack", struct_unpack},
        {NULL, NULL}
    };
    registerModule(vm, "struct", structFuncs);
    pop(vm);
}
