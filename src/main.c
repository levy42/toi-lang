#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "compiler.h"
#include "repl.h"

static int leading_indent_columns(const char* s, size_t len) {
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

static void append_bytes(char** out, size_t* len, size_t* cap, const char* bytes, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap == 0 ? 256 : *cap;
        while (*len + n + 1 > new_cap) new_cap *= 2;
        char* grown = (char*)realloc(*out, new_cap);
        if (grown == NULL) {
            fprintf(stderr, "Out of memory while formatting.\n");
            free(*out);
            exit(74);
        }
        *out = grown;
        *cap = new_cap;
    }

    memcpy(*out + *len, bytes, n);
    *len += n;
    (*out)[*len] = '\0';
}

static void append_indent(char** out, size_t* len, size_t* cap, int indent) {
    for (int i = 0; i < indent; i++) {
        append_bytes(out, len, cap, "  ", 2);
    }
}

static void update_multiline_string_state(const char* s, size_t len, int* in_multiline_string) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (!*in_multiline_string && s[i] == '[' && s[i + 1] == '[') {
            *in_multiline_string = 1;
            i++;
            continue;
        }

        if (*in_multiline_string && s[i] == ']' && s[i + 1] == ']') {
            *in_multiline_string = 0;
            i++;
        }
    }
}

static char* format_source(const char* source) {
    size_t in_len = strlen(source);
    int has_trailing_newline = in_len > 0 && source[in_len - 1] == '\n';

    char* out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;

    int indent_stack[512];
    int indent_top = 0;
    indent_stack[0] = 0;
    int in_multiline_string = 0;

    const char* line_start = source;
    while (1) {
        const char* line_end = strchr(line_start, '\n');
        size_t raw_len = line_end == NULL ? strlen(line_start) : (size_t)(line_end - line_start);

        // Strip trailing carriage return.
        while (raw_len > 0 && line_start[raw_len - 1] == '\r') raw_len--;

        if (in_multiline_string) {
            append_bytes(&out, &out_len, &out_cap, line_start, raw_len);
            append_bytes(&out, &out_len, &out_cap, "\n", 1);
            update_multiline_string_state(line_start, raw_len, &in_multiline_string);

            if (line_end == NULL) break;
            line_start = line_end + 1;
            if (*line_start == '\0') break;
            continue;
        }

        int col = leading_indent_columns(line_start, raw_len);

        // Trim leading whitespace for canonical formatting.
        const char* stripped = line_start;
        size_t stripped_len = raw_len;
        while (stripped_len > 0 && (*stripped == ' ' || *stripped == '\t')) {
            stripped++;
            stripped_len--;
        }

        if (stripped_len == 0) {
            append_bytes(&out, &out_len, &out_cap, "\n", 1);
        } else {
            if (col > indent_stack[indent_top]) {
                if (indent_top < (int)(sizeof(indent_stack) / sizeof(indent_stack[0])) - 1) {
                    indent_top++;
                    indent_stack[indent_top] = col;
                }
            } else if (col < indent_stack[indent_top]) {
                while (indent_top > 0 && col < indent_stack[indent_top]) {
                    indent_top--;
                }
            }

            append_indent(&out, &out_len, &out_cap, indent_top);
            append_bytes(&out, &out_len, &out_cap, stripped, stripped_len);
            append_bytes(&out, &out_len, &out_cap, "\n", 1);
            update_multiline_string_state(stripped, stripped_len, &in_multiline_string);
        }

        if (line_end == NULL) break;
        line_start = line_end + 1;
        if (*line_start == '\0') break;
    }

    if (!has_trailing_newline && out_len > 0 && out[out_len - 1] == '\n') {
        out[--out_len] = '\0';
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

static char* read_stream(FILE* file) {
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

static int write_file(const char* path, const char* content) {
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

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }

    buffer[bytes_read] = '\0';

    fclose(file);
    return buffer;
}

static void set_global_value(VM* vm, ObjString* key, Value value) {
    push(vm, OBJ_VAL(key));
    push(vm, value);
    table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
}

static int run_file(const char* path, int script_argc, char* script_argv[]) {
    char* source = read_file(path);
    // printf("Source:\n%s\n---\n", source);
    
    VM vm;
    init_vm(&vm);
    vm.cli_argc = script_argc;
    vm.cli_argv = script_argv;

    ObjString* main_name = copy_string("__main", 6);
    ObjString* file_name = copy_string(path, (int)strlen(path));
    set_global_value(&vm, vm.module_name_key, OBJ_VAL(main_name));
    set_global_value(&vm, vm.module_file_key, OBJ_VAL(file_name));
    set_global_value(&vm, vm.module_main_key, BOOL_VAL(1));
    
    ObjFunction* function = compile(source);
    if (function == NULL) {
        free_vm(&vm);
        free(source);
        return 65;
    }
    
    // disassemble_chunk(&function->chunk, "script");
    InterpretResult result = interpret(&vm, function);
    
    // Freeing function: Since we have GC, we should really let GC handle it, 
    // but we are tearing down VM anyway.
    // free_object((struct Obj*)function); // Not exposed?
    // We can rely on free_vm (if it tracked objects) or just leak at exit.
    // For correctness with ASAN, we should free.
    // But `free_object` is static in object.c.
    // We can use `free_vm` if we add `objects` list freeing there.
    
    free_vm(&vm); // Should free all objects eventually
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) return 65;
    if (result == INTERPRET_RUNTIME_ERROR) return 70;
    return 0;
}

// REPL implementation moved to repl.c for better organization

static int run_fmt(int argc, char* argv[]) {
    int write_in_place = 0;
    int check_only = 0;
    const char* path = NULL;

    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "-w") == 0) {
            write_in_place = 1;
            continue;
        }
        if (strcmp(arg, "--check") == 0) {
            check_only = 1;
            continue;
        }
        if (path == NULL) {
            path = arg;
            continue;
        }
        fprintf(stderr, "Usage: toi fmt [-w|--check] [path|-]\n");
        return 64;
    }

    if (write_in_place && path == NULL) {
        fprintf(stderr, "Usage: toi fmt [-w|--check] [path|-]\n");
        return 64;
    }

    if (write_in_place && check_only) {
        fprintf(stderr, "Cannot use -w with --check.\n");
        return 64;
    }

    if (write_in_place && strcmp(path, "-") == 0) {
        fprintf(stderr, "Cannot use -w with stdin.\n");
        return 64;
    }

    char* input = NULL;
    if (path == NULL || strcmp(path, "-") == 0) {
        input = read_stream(stdin);
    } else {
        input = read_file(path);
    }

    char* formatted = format_source(input);

    int rc = 0;
    if (check_only) {
        rc = strcmp(input, formatted) == 0 ? 0 : 1;
    } else if (write_in_place) {
        rc = write_file(path, formatted);
    } else {
        fputs(formatted, stdout);
    }

    free(input);
    free(formatted);
    return rc;
}

int main(int argc, char* argv[]) {
    if (argc == 1) {
        start_repl();
    } else if (argc >= 2 && strcmp(argv[1], "fmt") == 0) {
        return run_fmt(argc - 2, argv + 2);
    } else if (argc >= 2) {
        return run_file(argv[1], argc - 2, argv + 2);
    } else {
        fprintf(stderr, "Usage: toi [path [args...]] | toi fmt [-w|--check] [path|-]\n");
        exit(64);
    }

    return 0;
}
