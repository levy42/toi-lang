#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int io_open(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);
    
    const char* path = GET_CSTRING(0);
    const char* mode = "r";
    if (argCount >= 2) { ASSERT_STRING(1); mode = GET_CSTRING(1); }
    
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString("cannot open file", 16))); // Push error message
        return 2; // Return 2 values for Lua style error handling
    }
    
    ObjUserdata* udata = newUserdata(fp);
    
    // Set metatable (looked up from io._file_mt)
    Value ioVal;
    ObjString* ioName = copyString("io", 2);
    if (tableGet(&vm->globals, ioName, &ioVal) && IS_TABLE(ioVal)) {
        Value mt;
        ObjString* mtName = copyString("_file_mt", 8);
        if (tableGet(&AS_TABLE(ioVal)->table, mtName, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }
    
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

static int file_index(VM* vm, int argCount, Value* args) {
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
    push(vm, OBJ_VAL(newNative(file_index, indexName)));
    tableSet(&fileMT->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    
    push(vm, OBJ_VAL(copyString("_file_mt", 8)));
    push(vm, OBJ_VAL(fileMT)); // fileMT is on stack
    tableSet(&ioModule->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); 
    pop(vm); 
    pop(vm); // Pop fileMT
    
    pop(vm); // Pop ioModule
}
