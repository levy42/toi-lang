#include <stdio.h>
#include <stdlib.h>

#include "../lib/libs.h"
#include "ops_import.h"

ObjFunction* compile(const char* source);

InterpretResult vmHandleOpImport(VM* vm, ObjString* moduleName, CallFrame** frame, uint8_t** ip) {
    if (loadNativeModule(vm, moduleName->chars)) {
        return INTERPRET_OK;
    }

    char modulePath[256];
    int j = 0;
    for (int i = 0; i < moduleName->length && j < 250; i++) {
        if (moduleName->chars[i] == '.') {
            modulePath[j++] = '/';
        } else {
            modulePath[j++] = moduleName->chars[i];
        }
    }
    modulePath[j] = '\0';

    char filename[512];
    FILE* file = NULL;
    const char* candidates[4] = {
        "%s.pua",
        "%s/__.pua",
        "lib/%s.pua",
        "lib/%s/__.pua"
    };

    for (int ci = 0; ci < 4 && file == NULL; ci++) {
        snprintf(filename, sizeof(filename), candidates[ci], modulePath);
        file = fopen(filename, "rb");
    }

    if (file == NULL) {
        printf("\033[31mCould not open module '%s'\033[0m (tried '%s.pua', '%s/__.pua', "
               "'lib/%s.pua', and 'lib/%s/__.pua').\n",
               moduleName->chars, modulePath, modulePath, modulePath, modulePath);
        return INTERPRET_RUNTIME_ERROR;
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fclose(file);
        printf("Not enough memory to read module '%s'.\n", moduleName->chars);
        return INTERPRET_RUNTIME_ERROR;
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);

    ObjFunction* moduleFunction = compile(buffer);
    free(buffer);

    if (moduleFunction == NULL) {
        printf("Failed to compile module '%s'.\n", moduleName->chars);
        return INTERPRET_COMPILE_ERROR;
    }

    ObjClosure* moduleClosure = newClosure(moduleFunction);
    push(vm, OBJ_VAL(moduleClosure));

    (*frame)->ip = *ip;

    if (!call(vm, moduleClosure, 0)) {
        return INTERPRET_RUNTIME_ERROR;
    }

    *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
    *ip = (*frame)->ip;

    return INTERPRET_OK;
}
