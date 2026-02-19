#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    char* data;
    size_t len;
    size_t cap;
    size_t pos;
    uint8_t append_mode;
    uint8_t closed;
} BufferData;

static void buffer_userdata_finalizer(void* ptr) {
    BufferData* b = (BufferData*)ptr;
    if (b == NULL) return;
    free(b->data);
    free(b);
}

static int buffer_ensure_capacity(BufferData* b, size_t needed) {
    if (needed <= b->cap) return 1;
    size_t newCap = b->cap == 0 ? 32 : b->cap;
    while (newCap < needed) {
        if (newCap > SIZE_MAX / 2) {
            newCap = needed;
            break;
        }
        newCap *= 2;
    }
    char* grown = (char*)realloc(b->data, newCap);
    if (grown == NULL) return 0;
    b->data = grown;
    b->cap = newCap;
    return 1;
}

static int parse_buffer_mode(const char* mode, int* outAppend, int* outTruncate) {
    if (mode == NULL || mode[0] == '\0') return 0;
    *outAppend = 0;
    *outTruncate = 0;

    switch (mode[0]) {
        case 'r':
            return 1;
        case 'w':
            *outTruncate = 1;
            return 1;
        case 'a':
            *outAppend = 1;
            return 1;
        default:
            return 0;
    }
}

static ObjTable* io_lookup_metatable(VM* vm, const char* key, int keyLen) {
    Value ioVal = NIL_VAL;
    ObjString* ioName = copyString("io", 2);
    if (!tableGet(&vm->globals, ioName, &ioVal) || !IS_TABLE(ioVal)) {
        return NULL;
    }

    Value mt = NIL_VAL;
    ObjString* mtName = copyString(key, keyLen);
    if (!tableGet(&AS_TABLE(ioVal)->table, mtName, &mt) || !IS_TABLE(mt)) {
        return NULL;
    }
    return AS_TABLE(mt);
}

static int io_open(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    
    const char* path = GET_CSTRING(0);
    const char* mode = "r";
    if (argCount >= 2) { ASSERT_STRING(1); mode = GET_CSTRING(1); }
    
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        vmRuntimeError(vm, "cannot open file");
        return 0;
    }
    
    ObjUserdata* udata = newUserdata(fp);
    
    // Set metatable (looked up from io._file_mt)
    udata->metatable = io_lookup_metatable(vm, "_file_mt", 8);
    
    RETURN_OBJ(udata);
}

static int io_buffer(VM* vm, int argCount, Value* args) {
    const char* initial = "";
    int initialLen = 0;
    const char* mode = "r";

    if (argCount >= 1) {
        ASSERT_STRING(0);
        initial = GET_CSTRING(0);
        initialLen = GET_STRING(0)->length;
    }
    if (argCount >= 2) {
        ASSERT_STRING(1);
        mode = GET_CSTRING(1);
    }
    if (argCount > 2) {
        vmRuntimeError(vm, "io.buffer() expects at most 2 arguments.");
        return 0;
    }

    int appendMode = 0;
    int truncate = 0;
    if (!parse_buffer_mode(mode, &appendMode, &truncate)) {
        vmRuntimeError(vm, "io.buffer() mode must start with 'r', 'w', or 'a'.");
        return 0;
    }

    BufferData* b = (BufferData*)malloc(sizeof(BufferData));
    if (b == NULL) {
        vmRuntimeError(vm, "Out of memory in io.buffer().");
        return 0;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->pos = 0;
    b->append_mode = (uint8_t)appendMode;
    b->closed = 0;

    if (!truncate && initialLen > 0) {
        if (!buffer_ensure_capacity(b, (size_t)initialLen)) {
            free(b);
            vmRuntimeError(vm, "Out of memory in io.buffer().");
            return 0;
        }
        memcpy(b->data, initial, (size_t)initialLen);
        b->len = (size_t)initialLen;
    }

    if (b->append_mode) {
        b->pos = b->len;
    }

    ObjUserdata* udata = newUserdataWithFinalizer(b, buffer_userdata_finalizer);
    udata->metatable = io_lookup_metatable(vm, "_buffer_mt", 10);
    RETURN_OBJ(udata);
}

static int file_close(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (fp) {
        fclose(fp);
        udata->data = NULL;
    }
    RETURN_TRUE;
}

static int file_read(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    if (argCount == 1) {
        size_t cap = 4096;
        size_t len = 0;
        char* buffer = (char*)malloc(cap + 1);
        if (!buffer) {
            vmRuntimeError(vm, "Out of memory in file.read().");
            return 0;
        }

        for (;;) {
            if (len + 4096 + 1 > cap) {
                cap *= 2;
                char* grown = (char*)realloc(buffer, cap + 1);
                if (!grown) {
                    free(buffer);
                    vmRuntimeError(vm, "Out of memory in file.read().");
                    return 0;
                }
                buffer = grown;
            }

            size_t n = fread(buffer + len, 1, 4096, fp);
            len += n;
            if (n < 4096) {
                if (ferror(fp)) {
                    free(buffer);
                    vmRuntimeError(vm, "I/O error in file.read().");
                    return 0;
                }
                break;
            }
        }

        if (len == 0) {
            free(buffer);
            RETURN_NIL;
        }

        buffer[len] = '\0';
        ObjString* out = copyString(buffer, (int)len);
        free(buffer);
        RETURN_OBJ(out);
    }

    if (argCount == 2) {
        ASSERT_NUMBER(1);
        double nbytesD = GET_NUMBER(1);
        int nbytes = (int)nbytesD;
        if ((double)nbytes != nbytesD || nbytes < 0) {
            vmRuntimeError(vm, "file.read(n) expects non-negative integer n.");
            return 0;
        }
        if (nbytes == 0) {
            RETURN_STRING("", 0);
        }

        char* buffer = (char*)malloc((size_t)nbytes + 1);
        if (!buffer) {
            vmRuntimeError(vm, "Out of memory in file.read(n).");
            return 0;
        }

        size_t n = fread(buffer, 1, (size_t)nbytes, fp);
        if (n == 0) {
            free(buffer);
            RETURN_NIL;
        }

        buffer[n] = '\0';
        ObjString* out = copyString(buffer, (int)n);
        free(buffer);
        RETURN_OBJ(out);
    }

    vmRuntimeError(vm, "file.read() expects 0 or 1 extra argument.");
    return 0;
}

static int file_readline(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    size_t cap = 128;
    size_t len = 0;
    char* buffer = (char*)malloc(cap);
    if (!buffer) {
        vmRuntimeError(vm, "Out of memory in file.readline().");
        return 0;
    }

    int ch = 0;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char* grown = (char*)realloc(buffer, cap);
            if (!grown) {
                free(buffer);
                vmRuntimeError(vm, "Out of memory in file.readline().");
                return 0;
            }
            buffer = grown;
        }
        buffer[len++] = (char)ch;
    }

    if (ch == EOF && len == 0) {
        free(buffer);
        RETURN_NIL;
    }

    buffer[len] = '\0';
    ObjString* out = copyString(buffer, (int)len);
    free(buffer);
    RETURN_OBJ(out);
}

static int file_write(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }
    
    const char* s = GET_CSTRING(1);
    fputs(s, fp);
    RETURN_VAL(args[0]); // Return self for chaining
}

static BufferData* buffer_from_userdata(ObjUserdata* udata) {
    if (udata == NULL || udata->data == NULL) return NULL;
    return (BufferData*)udata->data;
}

static int buffer_close(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b != NULL) {
        b->closed = 1;
        b->pos = 0;
    }
    RETURN_TRUE;
}

static int buffer_read(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }

    if (argCount == 1) {
        if (b->pos >= b->len) RETURN_NIL;
        size_t remaining = b->len - b->pos;
        ObjString* out = copyString(b->data + b->pos, (int)remaining);
        b->pos = b->len;
        RETURN_OBJ(out);
    }

    if (argCount == 2) {
        ASSERT_NUMBER(1);
        double nbytesD = GET_NUMBER(1);
        int nbytes = (int)nbytesD;
        if ((double)nbytes != nbytesD || nbytes < 0) {
            vmRuntimeError(vm, "file.read(n) expects non-negative integer n.");
            return 0;
        }
        if (nbytes == 0) {
            RETURN_STRING("", 0);
        }
        if (b->pos >= b->len) RETURN_NIL;

        size_t remaining = b->len - b->pos;
        size_t n = (size_t)nbytes;
        if (n > remaining) n = remaining;
        ObjString* out = copyString(b->data + b->pos, (int)n);
        b->pos += n;
        RETURN_OBJ(out);
    }

    vmRuntimeError(vm, "file.read() expects 0 or 1 extra argument.");
    return 0;
}

static int buffer_readline(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }
    if (b->pos >= b->len) { RETURN_NIL; }

    size_t start = b->pos;
    size_t end = start;
    while (end < b->len && b->data[end] != '\n') end++;

    ObjString* out = copyString(b->data + start, (int)(end - start));
    b->pos = end;
    if (b->pos < b->len && b->data[b->pos] == '\n') b->pos++;
    RETURN_OBJ(out);
}

static int buffer_write(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }

    ObjString* str = GET_STRING(1);
    size_t n = (size_t)str->length;
    if (n == 0) {
        RETURN_VAL(args[0]);
    }

    if (b->append_mode) {
        b->pos = b->len;
    }
    size_t endPos = b->pos + n;
    if (!buffer_ensure_capacity(b, endPos)) {
        vmRuntimeError(vm, "Out of memory in file.write().");
        return 0;
    }

    if (b->pos > b->len) {
        memset(b->data + b->len, 0, b->pos - b->len);
    }
    memcpy(b->data + b->pos, str->chars, n);
    b->pos = endPos;
    if (b->pos > b->len) b->len = b->pos;
    RETURN_VAL(args[0]);
}

static int number_to_long_checked(VM* vm, double value, const char* what, long* out) {
    if (value < (double)LONG_MIN || value > (double)LONG_MAX) {
        vmRuntimeError(vm, "%s is out of range for this platform.", what);
        return 0;
    }
    long converted = (long)value;
    if ((double)converted != value) {
        vmRuntimeError(vm, "%s must be an integer.", what);
        return 0;
    }
    *out = converted;
    return 1;
}

static int parse_seek_whence(VM* vm, ObjString* whenceStr, int* whenceOut) {
    const char* whence = whenceStr->chars;
    if (strcmp(whence, "set") == 0 || strcmp(whence, "start") == 0) {
        *whenceOut = SEEK_SET;
        return 1;
    }
    if (strcmp(whence, "cur") == 0 || strcmp(whence, "current") == 0) {
        *whenceOut = SEEK_CUR;
        return 1;
    }
    if (strcmp(whence, "end") == 0) {
        *whenceOut = SEEK_END;
        return 1;
    }
    vmRuntimeError(vm, "file.seek() whence must be 'set', 'cur', or 'end'.");
    return 0;
}

static int parse_seek_args(VM* vm, int argCount, Value* args, const char* who, int* whenceOut, long* offsetOut) {
    if (argCount > 3) {
        vmRuntimeError(vm, "%s expects at most 2 extra arguments.", who);
        return 0;
    }

    *whenceOut = SEEK_SET;
    *offsetOut = 0;
    if (argCount == 2) {
        if (IS_STRING(args[1])) {
            if (!parse_seek_whence(vm, AS_STRING(args[1]), whenceOut)) return 0;
        } else if (IS_NUMBER(args[1])) {
            if (!number_to_long_checked(vm, GET_NUMBER(1), "file.seek offset", offsetOut)) return 0;
        } else {
            vmRuntimeError(vm, "file.seek(x) expects x to be number offset or whence string.");
            return 0;
        }
    } else if (argCount == 3) {
        ASSERT_STRING(1);
        ASSERT_NUMBER(2);
        if (!parse_seek_whence(vm, GET_STRING(1), whenceOut)) return 0;
        if (!number_to_long_checked(vm, GET_NUMBER(2), "file.seek offset", offsetOut)) return 0;
    }
    return 1;
}

static int file_tell(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    long pos = ftell(fp);
    if (pos < 0) {
        RETURN_NIL;
    }
    RETURN_NUMBER((double)pos);
}

static int file_seek(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    int whence = SEEK_SET;
    long offset = 0;
    if (!parse_seek_args(vm, argCount, args, "file.seek()", &whence, &offset)) return 0;

    if (fseek(fp, offset, whence) != 0) {
        RETURN_NIL;
    }

    long pos = ftell(fp);
    if (pos < 0) {
        RETURN_NIL;
    }
    RETURN_NUMBER((double)pos);
}

static int buffer_tell(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }
    RETURN_NUMBER((double)b->pos);
}

static int buffer_seek(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }

    int whence = SEEK_SET;
    long offset = 0;
    if (!parse_seek_args(vm, argCount, args, "file.seek()", &whence, &offset)) return 0;

    long long base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = (long long)b->pos;
    } else {
        base = (long long)b->len;
    }

    long long target = base + (long long)offset;
    if (target < 0) {
        RETURN_NIL;
    }
    b->pos = (size_t)target;
    RETURN_NUMBER((double)b->pos);
}

static int userdata_index(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);
    ASSERT_STRING(1);

    ObjUserdata* udata = GET_USERDATA(0);
    if (!udata->metatable) { RETURN_NIL; }

    Value result = NIL_VAL;
    tableGet(&udata->metatable->table, GET_STRING(1), &result);
    RETURN_VAL(result);
}

void registerIO(VM* vm) {
    const NativeReg ioFuncs[] = {
        {"open", io_open},
        {"buffer", io_buffer},
        {NULL, NULL}
    };
    registerModule(vm, "io", ioFuncs);
    ObjTable* ioModule = AS_TABLE(peek(vm, 0)); // ioModule is on stack
    
    // File Metatable
    ObjTable* fileMT = newTable();
    push(vm, OBJ_VAL(fileMT)); // Protect from GC
    
    const NativeReg fileMethods[] = {
        {"close", file_close},
        {"read", file_read},
        {"readline", file_readline},
        {"write", file_write},
        {"seek", file_seek},
        {"tell", file_tell},
        {NULL, NULL}
    };
    // Register file methods into fileMT
    for (int i = 0; fileMethods[i].name != NULL; i++) {
        ObjString* nameStr = copyString(fileMethods[i].name, (int)strlen(fileMethods[i].name));
        push(vm, OBJ_VAL(nameStr));
        ObjNative* method = newNative(fileMethods[i].function, nameStr);
        method->isSelf = 1;
        push(vm, OBJ_VAL(method));
        tableSet(&fileMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm); 
        pop(vm); 
    }
    
    ObjString* indexName = copyString("__index", 7);
    push(vm, OBJ_VAL(indexName));
    push(vm, OBJ_VAL(newNative(userdata_index, indexName)));
    tableSet(&fileMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copyString("__name", 6)));
    push(vm, OBJ_VAL(copyString("io.file", 7)));
    tableSet(&fileMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    
    push(vm, OBJ_VAL(copyString("_file_mt", 8)));
    push(vm, OBJ_VAL(fileMT)); // fileMT is on stack
    tableSet(&ioModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); 
    pop(vm); 
    pop(vm); // Pop fileMT

    // Buffer Metatable
    ObjTable* bufferMT = newTable();
    push(vm, OBJ_VAL(bufferMT));

    const NativeReg bufferMethods[] = {
        {"close", buffer_close},
        {"read", buffer_read},
        {"readline", buffer_readline},
        {"write", buffer_write},
        {"seek", buffer_seek},
        {"tell", buffer_tell},
        {NULL, NULL}
    };

    for (int i = 0; bufferMethods[i].name != NULL; i++) {
        ObjString* nameStr = copyString(bufferMethods[i].name, (int)strlen(bufferMethods[i].name));
        push(vm, OBJ_VAL(nameStr));
        ObjNative* method = newNative(bufferMethods[i].function, nameStr);
        method->isSelf = 1;
        push(vm, OBJ_VAL(method));
        tableSet(&bufferMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjString* bufferIndexName = copyString("__index", 7);
    push(vm, OBJ_VAL(bufferIndexName));
    push(vm, OBJ_VAL(newNative(userdata_index, bufferIndexName)));
    tableSet(&bufferMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copyString("__name", 6)));
    push(vm, OBJ_VAL(copyString("io.buffer", 9)));
    tableSet(&bufferMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copyString("_buffer_mt", 10)));
    push(vm, OBJ_VAL(bufferMT));
    tableSet(&ioModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // Pop bufferMT
    
    pop(vm); // Pop ioModule
}
