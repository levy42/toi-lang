#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/libs.h"
#include "ops_import.h"

ObjFunction* compile(const char* source);

static void set_global_value(VM* vm, ObjString* key, Value value) {
    push(vm, OBJ_VAL(key));
    push(vm, value);
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
}

static void restore_saved_module_context(
    VM* vm,
    int had_prev_name,
    int had_prev_file,
    int had_prev_main,
    Value prev_name,
    Value prev_file,
    Value prev_main
) {
    if (had_prev_name) {
        set_global_value(vm, vm->module_name_key, prev_name);
    } else {
        table_delete(&vm->globals, vm->module_name_key);
    }

    if (had_prev_file) {
        set_global_value(vm, vm->module_file_key, prev_file);
    } else {
        table_delete(&vm->globals, vm->module_file_key);
    }

    if (had_prev_main) {
        set_global_value(vm, vm->module_main_key, prev_main);
    } else {
        table_delete(&vm->globals, vm->module_main_key);
    }
}

InterpretResult vm_handle_op_import(VM* vm, ObjString* module_name, CallFrame** frame, uint8_t** ip) {
    Value cached_module = NIL_VAL;
    if (table_get(&vm->modules, module_name, &cached_module)) {
        push(vm, cached_module);
        return INTERPRET_OK;
    }

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

    Value prev_name = NIL_VAL;
    Value prev_file = NIL_VAL;
    Value prev_main = NIL_VAL;
    int had_prev_name = table_get(&vm->globals, vm->module_name_key, &prev_name);
    int had_prev_file = table_get(&vm->globals, vm->module_file_key, &prev_file);
    int had_prev_main = table_get(&vm->globals, vm->module_main_key, &prev_main);

    ObjString* file_string = copy_string(filename, (int)strlen(filename));
    set_global_value(vm, vm->module_name_key, OBJ_VAL(module_name));
    set_global_value(vm, vm->module_file_key, OBJ_VAL(file_string));
    set_global_value(vm, vm->module_main_key, BOOL_VAL(0));

    (*frame)->ip = *ip;

    if (!call(vm, module_closure, 0)) {
        restore_saved_module_context(
            vm,
            had_prev_name,
            had_prev_file,
            had_prev_main,
            prev_name,
            prev_file,
            prev_main
        );
        return INTERPRET_RUNTIME_ERROR;
    }

    CallFrame* module_frame = &vm_current_thread(vm)->frames[vm_current_thread(vm)->frame_count - 1];
    module_frame->restore_module_context = 1;
    module_frame->cache_module_result = 1;
    module_frame->had_prev_module_name = (uint8_t)had_prev_name;
    module_frame->had_prev_module_file = (uint8_t)had_prev_file;
    module_frame->had_prev_module_main = (uint8_t)had_prev_main;
    module_frame->module_cache_name = OBJ_VAL(module_name);
    module_frame->prev_module_name = prev_name;
    module_frame->prev_module_file = prev_file;
    module_frame->prev_module_main = prev_main;

    *frame = module_frame;
    *ip = (*frame)->ip;

    return INTERPRET_OK;
}
