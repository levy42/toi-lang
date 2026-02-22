#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

#define GZIP_CHUNK_SIZE 16384

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} ByteBuffer;

static void bb_init(ByteBuffer* bb) {
    bb->data = NULL;
    bb->len = 0;
    bb->cap = 0;
}

static void bb_free(ByteBuffer* bb) {
    free(bb->data);
    bb->data = NULL;
    bb->len = 0;
    bb->cap = 0;
}

static int bb_append(ByteBuffer* bb, const unsigned char* chunk, size_t n) {
    if (n == 0) return 1;

    if (bb->len > (size_t)INT_MAX) return 0;
    if (n > (size_t)INT_MAX) return 0;
    if (bb->len + n > (size_t)INT_MAX) return 0;

    size_t needed = bb->len + n + 1;
    if (needed > bb->cap) {
        size_t new_cap = bb->cap == 0 ? GZIP_CHUNK_SIZE : bb->cap;
        while (new_cap < needed) {
            if (new_cap > ((size_t)INT_MAX / 2)) {
                new_cap = needed;
                break;
            }
            new_cap *= 2;
        }
        char* new_data = (char*)realloc(bb->data, new_cap);
        if (new_data == NULL) return 0;
        bb->data = new_data;
        bb->cap = new_cap;
    }

    memcpy(bb->data + bb->len, chunk, n);
    bb->len += n;
    bb->data[bb->len] = '\0';
    return 1;
}

static int gzip_compress(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(vm, "gzip.compress() expects 1 or 2 arguments.");
        return 0;
    }
    ASSERT_STRING(0);

    int level = Z_DEFAULT_COMPRESSION;
    if (arg_count == 2 && !IS_NIL(args[1])) {
        ASSERT_NUMBER(1);
        level = (int)AS_NUMBER(args[1]);
        if (level < -1 || level > 9) {
            vm_runtime_error(vm, "gzip.compress() level must be between -1 and 9.");
            return 0;
        }
    }

    ObjString* input = AS_STRING(args[0]);
    if (input->length < 0) {
        vm_runtime_error(vm, "gzip.compress(): invalid input length.");
        return 0;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    int rc = deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        vm_runtime_error(vm, "gzip.compress(): zlib init failed.");
        return 0;
    }

    stream.next_in = (Bytef*)input->chars;
    stream.avail_in = (uInt)input->length;

    ByteBuffer out;
    bb_init(&out);

    unsigned char chunk[GZIP_CHUNK_SIZE];
    int ok = 1;

    while (1) {
        stream.next_out = chunk;
        stream.avail_out = (uInt)sizeof(chunk);

        rc = deflate(&stream, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            ok = 0;
            break;
        }

        size_t produced = sizeof(chunk) - (size_t)stream.avail_out;
        if (!bb_append(&out, chunk, produced)) {
            ok = 0;
            break;
        }

        if (rc == Z_STREAM_END) break;
    }

    deflateEnd(&stream);

    if (!ok) {
        bb_free(&out);
        vm_runtime_error(vm, "gzip.compress(): compression failed.");
        return 0;
    }

    if (out.data == NULL) {
        out.data = (char*)malloc(1);
        if (out.data == NULL) {
            vm_runtime_error(vm, "gzip.compress(): out of memory.");
            return 0;
        }
        out.data[0] = '\0';
    }

    RETURN_OBJ(take_string(out.data, (int)out.len));
}

static int gzip_decompress(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = AS_STRING(args[0]);
    if (input->length < 0) {
        vm_runtime_error(vm, "gzip.decompress(): invalid input length.");
        return 0;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    int rc = inflateInit2(&stream, 15 + 16);
    if (rc != Z_OK) {
        vm_runtime_error(vm, "gzip.decompress(): zlib init failed.");
        return 0;
    }

    stream.next_in = (Bytef*)input->chars;
    stream.avail_in = (uInt)input->length;

    ByteBuffer out;
    bb_init(&out);

    unsigned char chunk[GZIP_CHUNK_SIZE];
    int ok = 1;
    const char* err = "gzip.decompress(): decompression failed.";

    while (1) {
        stream.next_out = chunk;
        stream.avail_out = (uInt)sizeof(chunk);

        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            if (rc == Z_DATA_ERROR || rc == Z_BUF_ERROR) {
                err = "gzip.decompress(): invalid or truncated gzip data.";
            }
            ok = 0;
            break;
        }

        size_t produced = sizeof(chunk) - (size_t)stream.avail_out;
        if (!bb_append(&out, chunk, produced)) {
            ok = 0;
            break;
        }

        if (rc == Z_STREAM_END) break;
    }

    inflateEnd(&stream);

    if (!ok) {
        bb_free(&out);
        vm_runtime_error(vm, "%s", err);
        return 0;
    }

    if (out.data == NULL) {
        out.data = (char*)malloc(1);
        if (out.data == NULL) {
            vm_runtime_error(vm, "gzip.decompress(): out of memory.");
            return 0;
        }
        out.data[0] = '\0';
    }

    RETURN_OBJ(take_string(out.data, (int)out.len));
}

void register_gzip(VM* vm) {
    NativeReg gzip_funcs[] = {
        {"compress", gzip_compress},
        {"decompress", gzip_decompress},
        {NULL, NULL}
    };

    register_module(vm, "gzip", gzip_funcs);
    pop(vm);
}
