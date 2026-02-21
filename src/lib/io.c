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
    size_t new_cap = b->cap == 0 ? 32 : b->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char* grown = (char*)realloc(b->data, new_cap);
    if (grown == NULL) return 0;
    b->data = grown;
    b->cap = new_cap;
    return 1;
}

static int parse_buffer_mode(const char* mode, int* out_append, int* out_truncate) {
    if (mode == NULL || mode[0] == '\0') return 0;
    *out_append = 0;
    *out_truncate = 0;

    switch (mode[0]) {
        case 'r':
            return 1;
        case 'w':
            *out_truncate = 1;
            return 1;
        case 'a':
            *out_append = 1;
            return 1;
        default:
            return 0;
    }
}

static ObjTable* io_lookup_metatable(VM* vm, const char* key, int key_len) {
    Value io_val = NIL_VAL;
    ObjString* io_name = copy_string("io", 2);
    if (!table_get(&vm->globals, io_name, &io_val) || !IS_TABLE(io_val)) {
        return NULL;
    }

    Value mt = NIL_VAL;
    ObjString* mt_name = copy_string(key, key_len);
    if (!table_get(&AS_TABLE(io_val)->table, mt_name, &mt) || !IS_TABLE(mt)) {
        return NULL;
    }
    return AS_TABLE(mt);
}

static int io_open(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    
    const char* path = GET_CSTRING(0);
    const char* mode = "r";
    if (arg_count >= 2) { ASSERT_STRING(1); mode = GET_CSTRING(1); }
    
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        vm_runtime_error(vm, "cannot open file");
        return 0;
    }
    
    ObjUserdata* udata = new_userdata(fp);
    
    // Set metatable (looked up from io._file_mt)
    udata->metatable = io_lookup_metatable(vm, "_file_mt", 8);
    
    RETURN_OBJ(udata);
}

static int io_buffer(VM* vm, int arg_count, Value* args) {
    const char* initial = "";
    int initial_len = 0;
    const char* mode = "r";

    if (arg_count >= 1) {
        ASSERT_STRING(0);
        initial = GET_CSTRING(0);
        initial_len = GET_STRING(0)->length;
    }
    if (arg_count >= 2) {
        ASSERT_STRING(1);
        mode = GET_CSTRING(1);
    }
    if (arg_count > 2) {
        vm_runtime_error(vm, "io.buffer() expects at most 2 arguments.");
        return 0;
    }

    int append_mode = 0;
    int truncate = 0;
    if (!parse_buffer_mode(mode, &append_mode, &truncate)) {
        vm_runtime_error(vm, "io.buffer() mode must start with 'r', 'w', or 'a'.");
        return 0;
    }

    BufferData* b = (BufferData*)malloc(sizeof(BufferData));
    if (b == NULL) {
        vm_runtime_error(vm, "Out of memory in io.buffer().");
        return 0;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->pos = 0;
    b->append_mode = (uint8_t)append_mode;
    b->closed = 0;

    if (!truncate && initial_len > 0) {
        if (!buffer_ensure_capacity(b, (size_t)initial_len)) {
            free(b);
            vm_runtime_error(vm, "Out of memory in io.buffer().");
            return 0;
        }
        memcpy(b->data, initial, (size_t)initial_len);
        b->len = (size_t)initial_len;
    }

    if (b->append_mode) {
        b->pos = b->len;
    }

    ObjUserdata* udata = new_userdata_with_finalizer(b, buffer_userdata_finalizer);
    udata->metatable = io_lookup_metatable(vm, "_buffer_mt", 10);
    RETURN_OBJ(udata);
}

static int file_close(VM* vm, int arg_count, Value* args) {
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

static int file_read(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    if (arg_count == 1) {
        size_t cap = 4096;
        size_t len = 0;
        char* buffer = (char*)malloc(cap + 1);
        if (!buffer) {
            vm_runtime_error(vm, "Out of memory in file.read().");
            return 0;
        }

        for (;;) {
            if (len + 4096 + 1 > cap) {
                cap *= 2;
                char* grown = (char*)realloc(buffer, cap + 1);
                if (!grown) {
                    free(buffer);
                    vm_runtime_error(vm, "Out of memory in file.read().");
                    return 0;
                }
                buffer = grown;
            }

            size_t n = fread(buffer + len, 1, 4096, fp);
            len += n;
            if (n < 4096) {
                if (ferror(fp)) {
                    free(buffer);
                    vm_runtime_error(vm, "I/O error in file.read().");
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
        ObjString* out = copy_string(buffer, (int)len);
        free(buffer);
        RETURN_OBJ(out);
    }

    if (arg_count == 2) {
        ASSERT_NUMBER(1);
        double nbytes_d = GET_NUMBER(1);
        int nbytes = (int)nbytes_d;
        if ((double)nbytes != nbytes_d || nbytes < 0) {
            vm_runtime_error(vm, "file.read(n) expects non-negative integer n.");
            return 0;
        }
        if (nbytes == 0) {
            RETURN_STRING("", 0);
        }

        char* buffer = (char*)malloc((size_t)nbytes + 1);
        if (!buffer) {
            vm_runtime_error(vm, "Out of memory in file.read(n).");
            return 0;
        }

        size_t n = fread(buffer, 1, (size_t)nbytes, fp);
        if (n == 0) {
            free(buffer);
            RETURN_NIL;
        }

        buffer[n] = '\0';
        ObjString* out = copy_string(buffer, (int)n);
        free(buffer);
        RETURN_OBJ(out);
    }

    vm_runtime_error(vm, "file.read() expects 0 or 1 extra argument.");
    return 0;
}

static int file_readline(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    size_t cap = 128;
    size_t len = 0;
    char* buffer = (char*)malloc(cap);
    if (!buffer) {
        vm_runtime_error(vm, "Out of memory in file.readline().");
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
                vm_runtime_error(vm, "Out of memory in file.readline().");
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
    ObjString* out = copy_string(buffer, (int)len);
    free(buffer);
    RETURN_OBJ(out);
}

static int file_write(VM* vm, int arg_count, Value* args) {
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

static int buffer_close(VM* vm, int arg_count, Value* args) {
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

static int buffer_read(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }

    if (arg_count == 1) {
        if (b->pos >= b->len) RETURN_NIL;
        size_t remaining = b->len - b->pos;
        ObjString* out = copy_string(b->data + b->pos, (int)remaining);
        b->pos = b->len;
        RETURN_OBJ(out);
    }

    if (arg_count == 2) {
        ASSERT_NUMBER(1);
        double nbytes_d = GET_NUMBER(1);
        int nbytes = (int)nbytes_d;
        if ((double)nbytes != nbytes_d || nbytes < 0) {
            vm_runtime_error(vm, "file.read(n) expects non-negative integer n.");
            return 0;
        }
        if (nbytes == 0) {
            RETURN_STRING("", 0);
        }
        if (b->pos >= b->len) RETURN_NIL;

        size_t remaining = b->len - b->pos;
        size_t n = (size_t)nbytes;
        if (n > remaining) n = remaining;
        ObjString* out = copy_string(b->data + b->pos, (int)n);
        b->pos += n;
        RETURN_OBJ(out);
    }

    vm_runtime_error(vm, "file.read() expects 0 or 1 extra argument.");
    return 0;
}

static int buffer_readline(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }
    if (b->pos >= b->len) { RETURN_NIL; }

    size_t start = b->pos;
    size_t end = start;
    while (end < b->len && b->data[end] != '\n') end++;

    ObjString* out = copy_string(b->data + start, (int)(end - start));
    b->pos = end;
    if (b->pos < b->len && b->data[b->pos] == '\n') b->pos++;
    RETURN_OBJ(out);
}

static int buffer_write(VM* vm, int arg_count, Value* args) {
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
    size_t end_pos = b->pos + n;
    if (!buffer_ensure_capacity(b, end_pos)) {
        vm_runtime_error(vm, "Out of memory in file.write().");
        return 0;
    }

    if (b->pos > b->len) {
        memset(b->data + b->len, 0, b->pos - b->len);
    }
    memcpy(b->data + b->pos, str->chars, n);
    b->pos = end_pos;
    if (b->pos > b->len) b->len = b->pos;
    RETURN_VAL(args[0]);
}

static int number_to_long_checked(VM* vm, double value, const char* what, long* out) {
    if (value < (double)LONG_MIN || value > (double)LONG_MAX) {
        vm_runtime_error(vm, "%s is out of range for this platform.", what);
        return 0;
    }
    long converted = (long)value;
    if ((double)converted != value) {
        vm_runtime_error(vm, "%s must be an integer.", what);
        return 0;
    }
    *out = converted;
    return 1;
}

static int parse_seek_whence(VM* vm, ObjString* whence_str, int* whence_out) {
    const char* whence = whence_str->chars;
    if (strcmp(whence, "set") == 0 || strcmp(whence, "start") == 0) {
        *whence_out = SEEK_SET;
        return 1;
    }
    if (strcmp(whence, "cur") == 0 || strcmp(whence, "current") == 0) {
        *whence_out = SEEK_CUR;
        return 1;
    }
    if (strcmp(whence, "end") == 0) {
        *whence_out = SEEK_END;
        return 1;
    }
    vm_runtime_error(vm, "file.seek() whence must be 'set', 'cur', or 'end'.");
    return 0;
}

static int parse_seek_args(VM* vm, int arg_count, Value* args, const char* who, int* whence_out, long* offset_out) {
    if (arg_count > 3) {
        vm_runtime_error(vm, "%s expects at most 2 extra arguments.", who);
        return 0;
    }

    *whence_out = SEEK_SET;
    *offset_out = 0;
    if (arg_count == 2) {
        if (IS_STRING(args[1])) {
            if (!parse_seek_whence(vm, AS_STRING(args[1]), whence_out)) return 0;
        } else if (IS_NUMBER(args[1])) {
            if (!number_to_long_checked(vm, GET_NUMBER(1), "file.seek offset", offset_out)) return 0;
        } else {
            vm_runtime_error(vm, "file.seek(x) expects x to be number offset or whence string.");
            return 0;
        }
    } else if (arg_count == 3) {
        ASSERT_STRING(1);
        ASSERT_NUMBER(2);
        if (!parse_seek_whence(vm, GET_STRING(1), whence_out)) return 0;
        if (!number_to_long_checked(vm, GET_NUMBER(2), "file.seek offset", offset_out)) return 0;
    }
    return 1;
}

static int file_tell(VM* vm, int arg_count, Value* args) {
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

static int file_seek(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    ObjUserdata* udata = GET_USERDATA(0);
    FILE* fp = (FILE*)udata->data;
    if (!fp) { RETURN_NIL; }

    int whence = SEEK_SET;
    long offset = 0;
    if (!parse_seek_args(vm, arg_count, args, "file.seek()", &whence, &offset)) return 0;

    if (fseek(fp, offset, whence) != 0) {
        RETURN_NIL;
    }

    long pos = ftell(fp);
    if (pos < 0) {
        RETURN_NIL;
    }
    RETURN_NUMBER((double)pos);
}

static int buffer_tell(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }
    RETURN_NUMBER((double)b->pos);
}

static int buffer_seek(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);
    ObjUserdata* udata = GET_USERDATA(0);
    BufferData* b = buffer_from_userdata(udata);
    if (b == NULL || b->closed) { RETURN_NIL; }

    int whence = SEEK_SET;
    long offset = 0;
    if (!parse_seek_args(vm, arg_count, args, "file.seek()", &whence, &offset)) return 0;

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

void register_io(VM* vm) {
    const NativeReg io_funcs[] = {
        {"open", io_open},
        {"buffer", io_buffer},
        {NULL, NULL}
    };
    register_module(vm, "io", io_funcs);
    ObjTable* io_module = AS_TABLE(peek(vm, 0)); // io_module is on stack
    
    // File Metatable
    ObjTable* file_mt = new_table();
    push(vm, OBJ_VAL(file_mt)); // Protect from GC
    
    const NativeReg file_methods[] = {
        {"close", file_close},
        {"read", file_read},
        {"readline", file_readline},
        {"write", file_write},
        {"seek", file_seek},
        {"tell", file_tell},
        {NULL, NULL}
    };
    // Register file methods into file_mt
    for (int i = 0; file_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(file_methods[i].name, (int)strlen(file_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        ObjNative* method = new_native(file_methods[i].function, name_str);
        method->is_self = 1;
        push(vm, OBJ_VAL(method));
        table_set(&file_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm); 
        pop(vm); 
    }
    
    ObjString* index_name = copy_string("__index", 7);
    push(vm, OBJ_VAL(index_name));
    push(vm, OBJ_VAL(file_mt));
    table_set(&file_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("io.file", 7)));
    table_set(&file_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    
    push(vm, OBJ_VAL(copy_string("_file_mt", 8)));
    push(vm, OBJ_VAL(file_mt)); // file_mt is on stack
    table_set(&io_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); 
    pop(vm); 
    pop(vm); // Pop file_mt

    // Buffer Metatable
    ObjTable* buffer_mt = new_table();
    push(vm, OBJ_VAL(buffer_mt));

    const NativeReg buffer_methods[] = {
        {"close", buffer_close},
        {"read", buffer_read},
        {"readline", buffer_readline},
        {"write", buffer_write},
        {"seek", buffer_seek},
        {"tell", buffer_tell},
        {NULL, NULL}
    };

    for (int i = 0; buffer_methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(buffer_methods[i].name, (int)strlen(buffer_methods[i].name));
        push(vm, OBJ_VAL(name_str));
        ObjNative* method = new_native(buffer_methods[i].function, name_str);
        method->is_self = 1;
        push(vm, OBJ_VAL(method));
        table_set(&buffer_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjString* buffer_index_name = copy_string("__index", 7);
    push(vm, OBJ_VAL(buffer_index_name));
    push(vm, OBJ_VAL(buffer_mt));
    table_set(&buffer_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("io.buffer", 9)));
    table_set(&buffer_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("_buffer_mt", 10)));
    push(vm, OBJ_VAL(buffer_mt));
    table_set(&io_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // Pop buffer_mt
    
    pop(vm); // Pop io_module
}
