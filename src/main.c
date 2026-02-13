#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "compiler.h"
#include "repl.h"

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }

    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

static int runFile(const char* path) {
    char* source = readFile(path);
    // printf("Source:\n%s\n---\n", source);
    
    VM vm;
    initVM(&vm);
    
    ObjFunction* function = compile(source);
    if (function == NULL) {
        freeVM(&vm);
        free(source);
        return 65;
    }
    
    // disassembleChunk(&function->chunk, "script");
    InterpretResult result = interpret(&vm, function);
    
    // Freeing function: Since we have GC, we should really let GC handle it, 
    // but we are tearing down VM anyway.
    // freeObject((struct Obj*)function); // Not exposed?
    // We can rely on freeVM (if it tracked objects) or just leak at exit.
    // For correctness with ASAN, we should free.
    // But `freeObject` is static in object.c.
    // We can use `freeVM` if we add `objects` list freeing there.
    
    freeVM(&vm); // Should free all objects eventually
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) return 65;
    if (result == INTERPRET_RUNTIME_ERROR) return 70;
    return 0;
}

// REPL implementation moved to repl.c for better organization

int main(int argc, char* argv[]) {
    if (argc == 1) {
        startREPL();
    } else if (argc == 2) {
        return runFile(argv[1]);
    } else {
        fprintf(stderr, "Usage: mylua [path]\n");
        exit(64);
    }

    return 0;
}
