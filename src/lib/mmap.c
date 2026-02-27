#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    void* ptr;
    size_t len;
    int fd;
    int writable;
    int closed;
} MmapData;

static ObjTable* mmap_lookup_metatable(VM* vm, const char* key, int key_len) {
    ObjString* module_name = copy_string("mmap", 4);
    Value module_val = NIL_VAL;
    if (!table_get(&vm->modules, module_name, &module_val) || !IS_TABLE(module_val)) {
        if (!table_get(&vm->globals, module_name, &module_val) || !IS_TABLE(module_val)) {
            return NULL;
        }
    }

    ObjString* key_str = copy_string(key, key_len);
    Value mt = NIL_VAL;
    if (!table_get(&AS_TABLE(module_val)->table, key_str, &mt) || !IS_TABLE(mt)) {
        return NULL;
    }
    return AS_TABLE(mt);
}

static void mmap_data_close(MmapData* data) {
    if (data == NULL || data->closed) return;
    if (data->ptr != NULL && data->len > 0) {
        munmap(data->ptr, data->len);
    }
    if (data->fd >= 0) {
        close(data->fd);
    }
    data->ptr = NULL;
    data->len = 0;
    data->fd = -1;
    data->closed = 1;
}

static void mmap_userdata_finalizer(void* ptr) {
    MmapData* data = (MmapData*)ptr;
    if (data == NULL) return;
    mmap_data_close(data);
    free(data);
}

static MmapData* mmap_from_userdata(VM* vm, ObjUserdata* udata) {
    MmapData* data = (MmapData*)udata->data;
    if (data == NULL || data->closed) {
        vm_runtime_error(vm, "mmap region is closed.");
        return NULL;
    }
    return data;
}

// mmap.map(path, mode?) -> region | nil, err
static int mmap_map(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    if (arg_count >= 2 && !IS_STRING(args[1])) {
        vm_runtime_error(vm, "Argument 2 must be a string.");
        return 0;
    }

    const char* path = GET_CSTRING(0);
    const char* mode = "r";
    if (arg_count >= 2) mode = GET_CSTRING(1);

    int oflags = O_RDONLY;
    int prot = PROT_READ;
    int writable = 0;
    if (strcmp(mode, "rw") == 0 || strcmp(mode, "wr") == 0 || strcmp(mode, "r+") == 0) {
        oflags = O_RDWR;
        prot = PROT_READ | PROT_WRITE;
        writable = 1;
    } else if (strcmp(mode, "r") != 0) {
        vm_runtime_error(vm, "mmap mode must be 'r' or 'rw'.");
        return 0;
    }

    int fd = open(path, oflags);
    if (fd < 0) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), (int)strlen(strerror(errno)))));
        return 2;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int err = errno;
        close(fd);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(err), (int)strlen(strerror(err)))));
        return 2;
    }

    size_t len = (size_t)st.st_size;
    void* ptr = NULL;
    if (len > 0) {
        ptr = mmap(NULL, len, prot, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            int err = errno;
            close(fd);
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string(strerror(err), (int)strlen(strerror(err)))));
            return 2;
        }
    }

    MmapData* data = (MmapData*)malloc(sizeof(MmapData));
    if (data == NULL) {
        if (ptr != NULL && len > 0) munmap(ptr, len);
        close(fd);
        vm_runtime_error(vm, "mmap.map out of memory.");
        return 0;
    }

    data->ptr = ptr;
    data->len = len;
    data->fd = fd;
    data->writable = writable;
    data->closed = 0;

    ObjUserdata* udata = new_userdata_with_finalizer(data, mmap_userdata_finalizer);
    udata->metatable = mmap_lookup_metatable(vm, "_mmap_mt", 8);
    RETURN_OBJ(udata);
}

// region:len()
static int mmap_len(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    MmapData* data = mmap_from_userdata(vm, GET_USERDATA(0));
    if (data == NULL) return 0;
    RETURN_NUMBER((double)data->len);
}

// region:read(start?, count?) -> string
static int mmap_read(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);
    if (arg_count >= 2) ASSERT_NUMBER(1);
    if (arg_count >= 3) ASSERT_NUMBER(2);

    MmapData* data = mmap_from_userdata(vm, GET_USERDATA(0));
    if (data == NULL) return 0;
    if (data->len == 0 || data->ptr == NULL) {
        RETURN_STRING("", 0);
    }

    int start = 1;
    int count = (int)data->len;
    if (arg_count >= 2) start = (int)AS_NUMBER(args[1]);
    if (arg_count >= 3) count = (int)AS_NUMBER(args[2]);

    if (start < 1) start = 1;
    if (count < 0) count = 0;

    size_t off = (size_t)(start - 1);
    if (off >= data->len || count == 0) {
        RETURN_STRING("", 0);
    }

    size_t n = (size_t)count;
    if (off + n > data->len) n = data->len - off;
    RETURN_OBJ(copy_string(((const char*)data->ptr) + off, (int)n));
}

// region:write(offset, data) -> bool
static int mmap_write(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(3);
    ASSERT_USERDATA(0);
    ASSERT_NUMBER(1);
    ASSERT_STRING(2);

    MmapData* data = mmap_from_userdata(vm, GET_USERDATA(0));
    if (data == NULL) return 0;
    if (!data->writable) {
        vm_runtime_error(vm, "mmap region is read-only.");
        return 0;
    }
    if (data->len == 0 || data->ptr == NULL) {
        vm_runtime_error(vm, "mmap region is empty.");
        return 0;
    }

    int offset_1 = (int)AS_NUMBER(args[1]);
    if (offset_1 < 1) {
        vm_runtime_error(vm, "offset must be >= 1.");
        return 0;
    }
    size_t off = (size_t)(offset_1 - 1);
    ObjString* src = AS_STRING(args[2]);
    size_t n = (size_t)src->length;

    if (off > data->len || off + n > data->len) {
        vm_runtime_error(vm, "write out of range.");
        return 0;
    }

    memcpy(((char*)data->ptr) + off, src->chars, n);
    RETURN_TRUE;
}

// region:flush() -> bool
static int mmap_flush(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    MmapData* data = mmap_from_userdata(vm, GET_USERDATA(0));
    if (data == NULL) return 0;
    if (data->len == 0 || data->ptr == NULL) RETURN_TRUE;
    RETURN_BOOL(msync(data->ptr, data->len, MS_SYNC) == 0);
}

// region:close() -> bool
static int mmap_close(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);
    ObjUserdata* u = GET_USERDATA(0);
    MmapData* data = (MmapData*)u->data;
    if (data == NULL) RETURN_TRUE;
    mmap_data_close(data);
    RETURN_TRUE;
}

// region.__slice(start?, end?, step?) -> string
static int mmap_slice(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_USERDATA(0);

    MmapData* data = mmap_from_userdata(vm, GET_USERDATA(0));
    if (data == NULL) return 0;
    if (data->len == 0 || data->ptr == NULL) {
        RETURN_STRING("", 0);
    }

    Value start_v = arg_count >= 2 ? args[1] : NIL_VAL;
    Value end_v = arg_count >= 3 ? args[2] : NIL_VAL;
    Value step_v = arg_count >= 4 ? args[3] : NUMBER_VAL(1);
    if (IS_NIL(step_v)) step_v = NUMBER_VAL(1);
    if (!IS_NUMBER(step_v)) {
        vm_runtime_error(vm, "__slice step must be a number.");
        return 0;
    }

    double step_d = AS_NUMBER(step_v);
    int step = (int)step_d;
    if (step_d != (double)step) {
        vm_runtime_error(vm, "__slice step must be an integer.");
        return 0;
    }
    if (step == 0) {
        vm_runtime_error(vm, "__slice step cannot be 0.");
        return 0;
    }

    int len = (int)data->len;
    int start = step < 0 ? len : 1;
    int end = step < 0 ? 1 : len;

    if (!IS_NIL(start_v)) {
        if (!IS_NUMBER(start_v)) {
            vm_runtime_error(vm, "__slice start must be a number.");
            return 0;
        }
        double d = AS_NUMBER(start_v);
        start = (int)d;
        if (d != (double)start) {
            vm_runtime_error(vm, "__slice start must be an integer.");
            return 0;
        }
    }

    if (!IS_NIL(end_v)) {
        if (!IS_NUMBER(end_v)) {
            vm_runtime_error(vm, "__slice end must be a number.");
            return 0;
        }
        double d = AS_NUMBER(end_v);
        end = (int)d;
        if (d != (double)end) {
            vm_runtime_error(vm, "__slice end must be an integer.");
            return 0;
        }
    }

    int out_len = 0;
    if (step > 0) {
        if (start < 1) start = 1;
        if (end > len) end = len;
        if (start > len || end < 1) RETURN_STRING("", 0);
        if (start > end) RETURN_STRING("", 0);
        for (int i = start; i <= end; i += step) out_len++;
    } else {
        if (start > len) start = len;
        if (end < 1) end = 1;
        if (start < 1 || end > len) RETURN_STRING("", 0);
        if (start < end) RETURN_STRING("", 0);
        for (int i = start; i >= end; i += step) out_len++;
    }

    char* buf = (char*)malloc((size_t)out_len + 1);
    if (buf == NULL) {
        vm_runtime_error(vm, "__slice out of memory.");
        return 0;
    }

    int w = 0;
    const char* src = (const char*)data->ptr;
    if (step > 0) {
        for (int i = start; i <= end; i += step) {
            buf[w++] = src[i - 1];
        }
    } else {
        for (int i = start; i >= end; i += step) {
            buf[w++] = src[i - 1];
        }
    }
    buf[w] = '\0';
    RETURN_STRING(buf, w);
}

void register_mmap(VM* vm) {
    const NativeReg mmap_funcs[] = {
        {"map", mmap_map},
        {NULL, NULL}
    };
    register_module(vm, "mmap", mmap_funcs);
    ObjTable* mmap_module = AS_TABLE(peek(vm, 0));

    ObjTable* mmap_mt = new_table();
    push(vm, OBJ_VAL(mmap_mt));

    const NativeReg methods[] = {
        {"len", mmap_len},
        {"read", mmap_read},
        {"write", mmap_write},
        {"flush", mmap_flush},
        {"close", mmap_close},
        {"__slice", mmap_slice},
        {NULL, NULL}
    };

    for (int i = 0; methods[i].name != NULL; i++) {
        ObjString* name_str = copy_string(methods[i].name, (int)strlen(methods[i].name));
        push(vm, OBJ_VAL(name_str));
        ObjNative* method = new_native(methods[i].function, name_str);
        method->is_self = 1;
        push(vm, OBJ_VAL(method));
        table_set(&mmap_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    push(vm, OBJ_VAL(copy_string("__index", 7)));
    push(vm, OBJ_VAL(mmap_mt));
    table_set(&mmap_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("mmap.region", 11)));
    table_set(&mmap_mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("_mmap_mt", 8)));
    push(vm, OBJ_VAL(mmap_mt));
    table_set(&mmap_module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // mmap_mt
    pop(vm); // mmap module
}
