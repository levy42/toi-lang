#include <stdio.h>
#include <stdlib.h>

#include "../lib/libs.h"
#include "ops_import.h"

ObjFunction* compile(const char* source);

InterpretResult vm_handle_op_import(VM* vm, ObjString* module_name, CallFrame** frame, uint8_t** ip) {
    if (load_native_module(vm, module_name->chars)) {
        return INTERPRET_OK;
    }

    char module_path[256];
    int j = 0;
    for (int i = 0; i < module_name->length && j < 250; i++) {
        if (module_name->chars[i] == '.') {
            module_path[j++] = '/';
        } else {
            module_path[j++] = module_name->chars[i];
        }
    }
    module_path[j] = '\0';

    char filename[512];
    FILE* file = NULL;
    const char* candidates[4] = {
        "%s.toi",
        "%s/__.toi",
        "lib/%s.toi",
        "lib/%s/__.toi"
    };

    for (int ci = 0; ci < 4 && file == NULL; ci++) {
        snprintf(filename, sizeof(filename), candidates[ci], module_path);
        file = fopen(filename, "rb");
    }

    if (file == NULL) {
        printf("\033[31mCould not open module '%s'\033[0m (tried '%s.toi', '%s/__.toi', "
               "'lib/%s.toi', and 'lib/%s/__.toi').\n",
               module_name->chars, module_path, module_path, module_path, module_path);
        return INTERPRET_RUNTIME_ERROR;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        printf("Not enough memory to read module '%s'.\n", module_name->chars);
        return INTERPRET_RUNTIME_ERROR;
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    buffer[bytes_read] = '\0';
    fclose(file);

    ObjFunction* module_function = compile(buffer);
    free(buffer);

    if (module_function == NULL) {
        printf("Failed to compile module '%s'.\n", module_name->chars);
        return INTERPRET_COMPILE_ERROR;
    }

    ObjClosure* module_closure = new_closure(module_function);
    push(vm, OBJ_VAL(module_closure));

    (*frame)->ip = *ip;

    if (!call(vm, module_closure, 0)) {
        return INTERPRET_RUNTIME_ERROR;
    }

    *frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    *ip = (*frame)->ip;

    return INTERPRET_OK;
}
