#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "compiler.h"
#include "repl.h"

static int leadingIndentColumns(const char* s, size_t len) {
    int col = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ' ') {
            col += 1;
        } else if (s[i] == '\t') {
            col += 4;
        } else {
            break;
        }
    }
    return col;
}

static void appendBytes(char** out, size_t* len, size_t* cap, const char* bytes, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t newCap = *cap == 0 ? 256 : *cap;
        while (*len + n + 1 > newCap) newCap *= 2;
        char* grown = (char*)realloc(*out, newCap);
        if (grown == NULL) {
            fprintf(stderr, "Out of memory while formatting.\n");
            free(*out);
            exit(74);
        }
        *out = grown;
        *cap = newCap;
    }

    memcpy(*out + *len, bytes, n);
    *len += n;
    (*out)[*len] = '\0';
}

static void appendIndent(char** out, size_t* len, size_t* cap, int indent) {
    for (int i = 0; i < indent; i++) {
        appendBytes(out, len, cap, "  ", 2);
    }
}

static void updateMultilineStringState(const char* s, size_t len, int* inMultilineString) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (!*inMultilineString && s[i] == '[' && s[i + 1] == '[') {
            *inMultilineString = 1;
            i++;
            continue;
        }

        if (*inMultilineString && s[i] == ']' && s[i + 1] == ']') {
            *inMultilineString = 0;
            i++;
        }
    }
}

static char* formatSource(const char* source) {
    size_t inLen = strlen(source);
    int hasTrailingNewline = inLen > 0 && source[inLen - 1] == '\n';

    char* out = NULL;
    size_t outLen = 0;
    size_t outCap = 0;

    int indentStack[512];
    int indentTop = 0;
    indentStack[0] = 0;
    int inMultilineString = 0;

    const char* lineStart = source;
    while (1) {
        const char* lineEnd = strchr(lineStart, '\n');
        size_t rawLen = lineEnd == NULL ? strlen(lineStart) : (size_t)(lineEnd - lineStart);

        // Strip trailing carriage return.
        while (rawLen > 0 && lineStart[rawLen - 1] == '\r') rawLen--;

        if (inMultilineString) {
            appendBytes(&out, &outLen, &outCap, lineStart, rawLen);
            appendBytes(&out, &outLen, &outCap, "\n", 1);
            updateMultilineStringState(lineStart, rawLen, &inMultilineString);

            if (lineEnd == NULL) break;
            lineStart = lineEnd + 1;
            if (*lineStart == '\0') break;
            continue;
        }

        int col = leadingIndentColumns(lineStart, rawLen);

        // Trim leading whitespace for canonical formatting.
        const char* stripped = lineStart;
        size_t strippedLen = rawLen;
        while (strippedLen > 0 && (*stripped == ' ' || *stripped == '\t')) {
            stripped++;
            strippedLen--;
        }

        if (strippedLen == 0) {
            appendBytes(&out, &outLen, &outCap, "\n", 1);
        } else {
            if (col > indentStack[indentTop]) {
                if (indentTop < (int)(sizeof(indentStack) / sizeof(indentStack[0])) - 1) {
                    indentTop++;
                    indentStack[indentTop] = col;
                }
            } else if (col < indentStack[indentTop]) {
                while (indentTop > 0 && col < indentStack[indentTop]) {
                    indentTop--;
                }
            }

            appendIndent(&out, &outLen, &outCap, indentTop);
            appendBytes(&out, &outLen, &outCap, stripped, strippedLen);
            appendBytes(&out, &outLen, &outCap, "\n", 1);
            updateMultilineStringState(stripped, strippedLen, &inMultilineString);
        }

        if (lineEnd == NULL) break;
        lineStart = lineEnd + 1;
        if (*lineStart == '\0') break;
    }

    if (!hasTrailingNewline && outLen > 0 && out[outLen - 1] == '\n') {
        out[--outLen] = '\0';
    }

    if (out == NULL) {
        out = (char*)malloc(1);
        if (out == NULL) {
            fprintf(stderr, "Out of memory while formatting.\n");
            exit(74);
        }
        out[0] = '\0';
    }

    return out;
}

static char* readStream(FILE* file) {
    size_t cap = 1024;
    size_t len = 0;
    char* buffer = (char*)malloc(cap);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read input.\n");
        exit(74);
    }

    for (;;) {
        size_t want = cap - len - 1;
        size_t got = fread(buffer + len, 1, want, file);
        len += got;

        if (got < want) {
            if (feof(file)) break;
            if (ferror(file)) {
                fprintf(stderr, "Error reading input stream.\n");
                free(buffer);
                exit(74);
            }
        }

        if (cap - len - 1 == 0) {
            cap *= 2;
            char* grown = (char*)realloc(buffer, cap);
            if (grown == NULL) {
                fprintf(stderr, "Not enough memory to read input.\n");
                free(buffer);
                exit(74);
            }
            buffer = grown;
        }
    }

    buffer[len] = '\0';
    return buffer;
}

static int writeFile(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "Could not write file \"%s\".\n", path);
        return 74;
    }

    size_t n = strlen(content);
    if (fwrite(content, 1, n, f) != n) {
        fclose(f);
        fprintf(stderr, "Could not write file \"%s\".\n", path);
        return 74;
    }

    fclose(f);
    return 0;
}

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

static int runFile(const char* path, int scriptArgc, char* scriptArgv[]) {
    char* source = readFile(path);
    // printf("Source:\n%s\n---\n", source);
    
    VM vm;
    initVM(&vm);
    vm.cliArgc = scriptArgc;
    vm.cliArgv = scriptArgv;
    
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

static int runFmt(int argc, char* argv[]) {
    int writeInPlace = 0;
    int checkOnly = 0;
    const char* path = NULL;

    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "-w") == 0) {
            writeInPlace = 1;
            continue;
        }
        if (strcmp(arg, "--check") == 0) {
            checkOnly = 1;
            continue;
        }
        if (path == NULL) {
            path = arg;
            continue;
        }
        fprintf(stderr, "Usage: pua fmt [-w|--check] [path|-]\n");
        return 64;
    }

    if (writeInPlace && path == NULL) {
        fprintf(stderr, "Usage: pua fmt [-w|--check] [path|-]\n");
        return 64;
    }

    if (writeInPlace && checkOnly) {
        fprintf(stderr, "Cannot use -w with --check.\n");
        return 64;
    }

    if (writeInPlace && strcmp(path, "-") == 0) {
        fprintf(stderr, "Cannot use -w with stdin.\n");
        return 64;
    }

    char* input = NULL;
    if (path == NULL || strcmp(path, "-") == 0) {
        input = readStream(stdin);
    } else {
        input = readFile(path);
    }

    char* formatted = formatSource(input);

    int rc = 0;
    if (checkOnly) {
        rc = strcmp(input, formatted) == 0 ? 0 : 1;
    } else if (writeInPlace) {
        rc = writeFile(path, formatted);
    } else {
        fputs(formatted, stdout);
    }

    free(input);
    free(formatted);
    return rc;
}

int main(int argc, char* argv[]) {
    if (argc == 1) {
        startREPL();
    } else if (argc >= 2 && strcmp(argv[1], "fmt") == 0) {
        return runFmt(argc - 2, argv + 2);
    } else if (argc >= 2) {
        return runFile(argv[1], argc - 2, argv + 2);
    } else {
        fprintf(stderr, "Usage: pua [path [args...]] | pua fmt [-w|--check] [path|-]\n");
        exit(64);
    }

    return 0;
}
