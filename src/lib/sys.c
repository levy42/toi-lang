#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static FILE* sys_stream_for_fd(int fd) {
    if (fd == 1) return stdout;
    if (fd == 2) return stderr;
    return NULL;
}

// sys.write(data, fd?) -> bytes_written or nil, err
static int sys_write(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    if (arg_count > 2) {
        vm_runtime_error(vm, "Expected at most 2 arguments but got %d.", arg_count);
        return 0;
    }
    ASSERT_STRING(0);

    int fd = 1;
    if (arg_count == 2) {
        ASSERT_NUMBER(1);
        double fd_value = GET_NUMBER(1);
        int fd_int = (int)fd_value;
        if ((double)fd_int != fd_value) {
            vm_runtime_error(vm, "fd must be an integer.");
            return 0;
        }
        fd = fd_int;
    }

    FILE* stream = sys_stream_for_fd(fd);
    if (stream == NULL) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("unsupported fd (expected 1 or 2)", 32)));
        return 2;
    }

    ObjString* data = GET_STRING(0);
    size_t wrote = fwrite(data->chars, 1, (size_t)data->length, stream);
    if (wrote != (size_t)data->length) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(strerror(errno), (int)strlen(strerror(errno)))));
        return 2;
    }

    fflush(stream);
    RETURN_NUMBER((double)wrote);
}

void register_sys(VM* vm) {
    const NativeReg sys_funcs[] = {
        {"write", sys_write},
        {NULL, NULL}
    };

    register_module(vm, "sys", sys_funcs);
    pop(vm);
}
