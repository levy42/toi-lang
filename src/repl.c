#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include "repl.h"
#include "vm.h"
#include "compiler.h"
#include "lexer.h"
#include "token.h"
#include "object.h"
#include "value.h"
#include "toi_lineedit.h"

#define VERSION "0.0.1"
// ANSI color codes for syntax highlighting
#define COLOR_RESET     "\033[0m"
#define COLOR_KEYWORD   "\033[35m"  // Magenta for keywords
#define COLOR_NUMBER    "\033[36m"  // Cyan for numbers
#define COLOR_STRING    "\033[32m"  // Green for strings
#define COLOR_FSTRING   "\033[92m"  // Green for strings
#define COLOR_OPERATOR  "\033[33m"  // Yellow for operators
#define COLOR_FUNCTION  "\033[94m"  // Bright blue for 'fn'
#define COLOR_ERROR     "\033[91m"  // Bright red for errors
#define COLOR_ESCAPE    "\033[36m"  // Cyan for escape sequences
#define COLOR_BUILTIN   "\033[96m"  // Bright cyan for builtins
#define COLOR_BOOL      "\033[91m"  // Bright red for booleans

static void highlight_line(const char* line, char* output, size_t output_size);
static VM* repl_vm_for_completion = NULL;
enum { REPL_COMPLETION_MAX = 7 };

typedef struct {
    const char* word;
    int length;
} CompletionWord;

typedef struct {
    TokenType type;
    const char* word;
    int length;
} KeywordInfo;

static const KeywordInfo keyword_info[] = {
    {TOKEN_FN, "fn", 2},
    {TOKEN_FOR, "for", 3},
    {TOKEN_FALSE, "false", 5},
    {TOKEN_IF, "if", 2},
    {TOKEN_IN, "in", 2},
    {TOKEN_IMPORT, "import", 6},
    {TOKEN_LOCAL, "local", 5},
    {TOKEN_RETURN, "return", 6},
    {TOKEN_YIELD, "yield", 5},
    {TOKEN_WHILE, "while", 5},
    {TOKEN_TRUE, "true", 4},
    {TOKEN_NIL, "nil", 3},
    {TOKEN_PRINT, "print", 5},
    {TOKEN_BREAK, "break", 5},
    {TOKEN_CONTINUE, "continue", 8},
    {TOKEN_ELSE, "else", 4},
    {TOKEN_ELIF, "elif", 4},
    {TOKEN_FROM, "from", 4},
    {TOKEN_AS, "as", 2},
    {TOKEN_WITH, "with", 4},
    {TOKEN_GC, "gc", 2},
    {TOKEN_DEL, "del", 3},
    {TOKEN_NOT, "not", 3},
    {TOKEN_AND, "and", 3},
    {TOKEN_OR, "or", 2},
    {TOKEN_ERROR, NULL, 0}
};

static const CompletionWord builtin_words[] = {
    {"exit", 4},
    {"bool", 4},
    {"int", 3},
    {"float", 5},
    {"input", 5},
    {"next", 4},
    {"inext", 5},
    {"range_iter", 10},
    {"range", 5},
    {"slice", 5},
    {"min", 3},
    {"max", 3},
    {"sum", 3},
    {"setmetatable", 12},
    {"getmetatable", 12},
    {"error", 5},
    {"type", 4},
    {NULL, 0}
};

static int is_identifier_start_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static int is_identifier_char(char c) {
    return is_identifier_start_char(c) || (c >= '0' && c <= '9');
}

static int is_valid_identifier(const char* s, int length) {
    if (length <= 0 || !is_identifier_start_char(s[0])) return 0;
    for (int i = 1; i < length; i++) {
        if (!is_identifier_char(s[i])) return 0;
    }
    return 1;
}

static int starts_with(const char* s, int slen, const char* prefix, int plen) {
    if (plen > slen) return 0;
    return memcmp(s, prefix, (size_t)plen) == 0;
}

static int word_list_contains(const CompletionWord* words, const char* s, int len) {
    for (int i = 0; words[i].word != NULL; i++) {
        if (words[i].length == len && memcmp(words[i].word, s, (size_t)len) == 0) return 1;
    }
    return 0;
}

static int completion_exists(toi_lineedit_completions *lc, const char* candidate) {
    for (size_t i = 0; i < lc->len; i++) {
        if (strcmp(lc->cvec[i], candidate) == 0) return 1;
    }
    return 0;
}

static void add_completion_candidate(const char *buf, int replace_start, const char* replacement,
                                   toi_lineedit_completions *lc) {
    if ((int)lc->len >= REPL_COMPLETION_MAX) return;

    int prefix_len = replace_start;
    int replacement_len = (int)strlen(replacement);
    int out_len = prefix_len + replacement_len;
    char* candidate = (char*)malloc((size_t)out_len + 1);
    if (candidate == NULL) return;

    memcpy(candidate, buf, (size_t)prefix_len);
    memcpy(candidate + prefix_len, replacement, (size_t)replacement_len);
    candidate[out_len] = '\0';

    if (!completion_exists(lc, candidate)) {
        toi_lineedit_add_completion(lc, candidate);
    }
    free(candidate);
}

static void add_globals_matches(const char *buf, int replace_start, const char* prefix, int prefix_len,
                              toi_lineedit_completions *lc) {
    if (repl_vm_for_completion == NULL) return;
    Table* globals = &repl_vm_for_completion->globals;

    for (int i = 0; i < globals->capacity && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        Entry* entry = &globals->entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        ObjString* key = entry->key;
        if (!is_valid_identifier(key->chars, key->length)) continue;
        if (!starts_with(key->chars, key->length, prefix, prefix_len)) continue;
        add_completion_candidate(buf, replace_start, key->chars, lc);
    }
}

static int lookup_global_by_slice(VM* vm, const char* name, int name_len, Value* out) {
    Table* globals = &vm->globals;
    for (int i = 0; i < globals->capacity; i++) {
        Entry* entry = &globals->entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;
        ObjString* key = entry->key;
        if (key->length == name_len && memcmp(key->chars, name, (size_t)name_len) == 0) {
            *out = entry->value;
            return 1;
        }
    }
    return 0;
}

static void add_table_member_matches(const char *buf, int member_start, const char* prefix, int prefix_len,
                                  ObjTable* table, toi_lineedit_completions *lc) {
    for (int i = 0; i < table->table.capacity && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        ObjString* key = entry->key;
        if (!is_valid_identifier(key->chars, key->length)) continue;
        if (!starts_with(key->chars, key->length, prefix, prefix_len)) continue;
        add_completion_candidate(buf, member_start, key->chars, lc);
    }
}

static int extract_member_context(const char *buf, int len, int* out_base_start, int* out_base_len,
                                int* out_member_start, int* out_member_len) {
    int i = len - 1;
    while (i >= 0 && is_identifier_char(buf[i])) i--;
    if (i < 0 || buf[i] != '.') return 0;

    int member_start = i + 1;
    int member_len = len - member_start;
    int base_end = i;
    i--;
    while (i >= 0 && is_identifier_char(buf[i])) i--;
    int base_start = i + 1;
    int base_len = base_end - base_start;
    if (base_len <= 0 || !is_valid_identifier(buf + base_start, base_len)) return 0;

    *out_base_start = base_start;
    *out_base_len = base_len;
    *out_member_start = member_start;
    *out_member_len = member_len;
    return 1;
}

static void format_number(double value, char* out, size_t out_size) {
    snprintf(out, out_size, "%.6f", value);
    int len = (int)strlen(out);
    while (len > 0 && out[len - 1] == '0') {
        out[--len] = '\0';
    }
    if (len > 0 && out[len - 1] == '.') {
        out[--len] = '\0';
    }
    if (len == 0) {
        strncpy(out, "0", out_size);
        out[out_size - 1] = '\0';
    }
}

static void append_highlighted_snippet(const char* code, size_t len, char* output, size_t output_size, size_t* out_pos) {
    if (len == 0) return;
    char* buf = (char*)malloc(len + 1);
    memcpy(buf, code, len);
    buf[len] = '\0';

    size_t temp_size = len * 8 + 64;
    char* temp = (char*)malloc(temp_size);
    highlight_line(buf, temp, temp_size);

    size_t temp_len = strlen(temp);
    if (*out_pos + temp_len < output_size - 1) {
        memcpy(output + *out_pos, temp, temp_len);
        *out_pos += temp_len;
    }

    free(temp);
    free(buf);
}

static int is_builtin_identifier(const char* start, int length) {
    return word_list_contains(builtin_words, start, length);
}

// Check if a token is a keyword
static int is_keyword(TokenType type) {
    for (int i = 0; keyword_info[i].word != NULL; i++) {
        if (keyword_info[i].type == type) return 1;
    }
    return 0;
}

// Check if a token is an operator
static int is_operator(TokenType type) {
    return type == TOKEN_PLUS || type == TOKEN_MINUS || type == TOKEN_STAR ||
           type == TOKEN_SLASH || type == TOKEN_PERCENT || type == TOKEN_EQUALS ||
           type == TOKEN_BANG_EQUAL || type == TOKEN_EQUAL_EQUAL ||
           type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL ||
           type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
           type == TOKEN_APPEND ||
           type == TOKEN_AND || type == TOKEN_OR || type == TOKEN_NOT ||
           type == TOKEN_DOT || type == TOKEN_DOT_DOT || type == TOKEN_QUESTION ||
           type == TOKEN_HASH || type == TOKEN_COLON || type == TOKEN_POWER ||
           type == TOKEN_COLON_COLON || type == TOKEN_INT_DIV;
}

static void append_raw(char* output, size_t output_size, size_t* out_pos, const char* src, size_t len) {
    if (*out_pos >= output_size - 1 || len == 0) return;
    size_t available = (output_size - 1) - *out_pos;
    if (len > available) len = available;
    memcpy(output + *out_pos, src, len);
    *out_pos += len;
}

static void append_char(char* output, size_t output_size, size_t* out_pos, char c) {
    if (*out_pos < output_size - 1) output[(*out_pos)++] = c;
}

static void append_ansi(char* output, size_t output_size, size_t* out_pos, const char* ansi) {
    append_raw(output, output_size, out_pos, ansi, strlen(ansi));
}

static const char* token_color(Token token) {
    if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) return COLOR_BOOL;
    if (is_keyword(token.type)) return COLOR_KEYWORD;
    if (token.type == TOKEN_IDENTIFIER && is_builtin_identifier(token.start, token.length)) return COLOR_BUILTIN;
    if (token.type == TOKEN_NUMBER) return COLOR_NUMBER;
    if (token.type == TOKEN_STRING) return COLOR_STRING;
    if (token.type == TOKEN_FSTRING) return COLOR_FSTRING;
    if (is_operator(token.type)) return COLOR_OPERATOR;
    if (token.type == TOKEN_ERROR) return COLOR_ERROR;
    return COLOR_RESET;
}

static void append_plain_token(Token token, char* output, size_t output_size, size_t* out_pos, size_t* line_pos) {
    append_raw(output, output_size, out_pos, token.start, (size_t)token.length);
    *line_pos += (size_t)token.length;
}

static void append_string_token(Token token, char* output, size_t output_size, size_t* out_pos, size_t* line_pos) {
    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];
        if (c == '\\' && i + 1 < token.length) {
            append_ansi(output, output_size, out_pos, COLOR_ESCAPE);
            append_char(output, output_size, out_pos, c);
            (*line_pos)++;
            i++;
            append_char(output, output_size, out_pos, token.start[i]);
            (*line_pos)++;
            append_ansi(output, output_size, out_pos, COLOR_STRING);
            continue;
        }

        if (token.type == TOKEN_FSTRING && c == '{') {
            int start = i + 1;
            int depth = 1;
            int j = start;
            while (j < token.length && depth > 0) {
                char cj = token.start[j];
                if (cj == '\\' && j + 1 < token.length) {
                    j += 2;
                    continue;
                }
                if (cj == '{') depth++;
                if (cj == '}') depth--;
                if (depth > 0) j++;
            }
            if (depth == 0) {
                append_char(output, output_size, out_pos, '{');
                (*line_pos)++;
                append_ansi(output, output_size, out_pos, COLOR_RESET);
                append_highlighted_snippet(token.start + start, (size_t)(j - start), output, output_size, out_pos);
                *line_pos += (size_t)(j - start);
                append_ansi(output, output_size, out_pos, COLOR_STRING);
                append_char(output, output_size, out_pos, '}');
                (*line_pos)++;
                i = j;
                continue;
            }
        }

        append_char(output, output_size, out_pos, c);
        (*line_pos)++;
    }
}

// Apply syntax highlighting to a line of code
static void highlight_line(const char* line, char* output, size_t output_size) {
    Lexer lexer;
    init_lexer(&lexer, line);

    size_t out_pos = 0;
    size_t line_pos = 0;

    for (;;) {
        Token token = scan_token(&lexer);

        // Add any whitespace/text before this token
        while (line_pos < (size_t)(token.start - line) && out_pos < output_size - 1) {
            output[out_pos++] = line[line_pos++];
        }

        if (token.type == TOKEN_EOF) break;

        append_ansi(output, output_size, &out_pos, token_color(token));

        // Add token text handling specific tokens
        if ((token.type == TOKEN_STRING || token.type == TOKEN_FSTRING) && token.length > 0 && token.start[0] != '[') {
            append_string_token(token, output, output_size, &out_pos, &line_pos);
        } else {
            append_plain_token(token, output, output_size, &out_pos, &line_pos);
        }

        append_ansi(output, output_size, &out_pos, COLOR_RESET);
    }

    // Add any remaining characters
    while (line_pos < strlen(line) && out_pos < output_size - 1) {
        output[out_pos++] = line[line_pos++];
    }

    output[out_pos] = '\0';
}

// Print a value with colors (for REPL output)
static void print_function_repr_colored(ObjFunction* function) {
    if (function->name != NULL) {
        printf(COLOR_OPERATOR "<" COLOR_KEYWORD "fn " COLOR_FUNCTION "%s" COLOR_OPERATOR ">" COLOR_RESET,
               function->name->chars);
    } else {
        printf(COLOR_FUNCTION "<script>" COLOR_RESET);
    }
}

static void print_value_colored(Value value) {
    if (IS_OBJ(value)) {
        if (IS_STRING(value)) {
            printf(COLOR_STRING "%s" COLOR_RESET, AS_CSTRING(value));
        } else if (IS_FUNCTION(value)) {
            print_function_repr_colored(AS_FUNCTION(value));
        } else if (IS_CLOSURE(value)) {
            print_function_repr_colored(AS_CLOSURE(value)->function);
        } else if (IS_TABLE(value)) {
            printf(COLOR_OPERATOR);
            print_object(value);
            printf(COLOR_RESET);
        } else {
            print_object(value);
        }
    } else if (IS_NIL(value)) {
        printf(COLOR_KEYWORD "nil" COLOR_RESET);
    } else if (IS_BOOL(value)) {
        printf(COLOR_BOOL "%s" COLOR_RESET, AS_BOOL(value) ? "true" : "false");
    } else if (IS_NUMBER(value)) {
        char buf[64];
        format_number(AS_NUMBER(value), buf, sizeof(buf));
        printf(COLOR_NUMBER "%s" COLOR_RESET, buf);
    }
}

// Linenoise syntax highlighting callback
static void syntax_highlight_callback(const char *buf, char *highlighted, size_t maxlen) {
    highlight_line(buf, highlighted, maxlen);
}

// Linenoise completion callback for keyword/function completion
static void completion_callback(const char *buf, toi_lineedit_completions *lc) {
    int len = (int)strlen(buf);

    int base_start = 0, base_len = 0, member_start = 0, member_len = 0;
    if (extract_member_context(buf, len, &base_start, &base_len, &member_start, &member_len)) {
        if (repl_vm_for_completion == NULL) return;

        Value base_val = NIL_VAL;
        if (!lookup_global_by_slice(repl_vm_for_completion, buf + base_start, base_len, &base_val)) return;

        if (IS_TABLE(base_val)) {
            add_table_member_matches(buf, member_start, buf + member_start, member_len, AS_TABLE(base_val), lc);
            return;
        }

        if (IS_USERDATA(base_val)) {
            ObjUserdata* udata = AS_USERDATA(base_val);
            if (udata->metatable != NULL) {
                add_table_member_matches(buf, member_start, buf + member_start, member_len, udata->metatable, lc);
            }
            return;
        }
        return;
    }

    int start = len;
    while (start > 0 && is_identifier_char(buf[start - 1])) start--;
    if (start < len && !is_identifier_start_char(buf[start])) return;

    const char* prefix = buf + start;
    int prefix_len = len - start;
    for (int i = 0; keyword_info[i].word != NULL && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        if (starts_with(keyword_info[i].word, keyword_info[i].length, prefix, prefix_len)) {
            add_completion_candidate(buf, start, keyword_info[i].word, lc);
        }
    }
    add_globals_matches(buf, start, prefix, prefix_len, lc);
}

static void init_completion_state(VM* vm) {
    repl_vm_for_completion = vm;
}

static void clear_completion_state(void) {
    repl_vm_for_completion = NULL;
}

// Check if input is complete or needs continuation
// Returns 1 if complete, 0 if needs more input
static int is_input_complete(const char* input) {
    Lexer lexer;
    init_lexer(&lexer, input);

    int brace_depth = 0;
    int paren_depth = 0;
    int bracket_depth = 0;
    TokenType last_type = TOKEN_EOF;
    int has_control_flow = 0;

    for (;;) {
        Token token = scan_token(&lexer);

        if (token.type == TOKEN_EOF) break;
        // If we hit an error (like unterminated string), it's incomplete
        if (token.type == TOKEN_ERROR) return 0;

        switch (token.type) {
            case TOKEN_LEFT_BRACE: brace_depth++; break;
            case TOKEN_RIGHT_BRACE: brace_depth--; break;
            case TOKEN_LEFT_PAREN: paren_depth++; break;
            case TOKEN_RIGHT_PAREN: paren_depth--; break;
            case TOKEN_LEFT_BRACKET: bracket_depth++; break;
            case TOKEN_RIGHT_BRACKET: bracket_depth--; break;

            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_FN:
            case TOKEN_WITH:
            case TOKEN_TRY:
            case TOKEN_EXCEPT:
            case TOKEN_FINALLY:
                has_control_flow = 1;
                break;
            default: break;
        }

        // Ignore newlines/indents/dedents for "last token" check to catch trailing operators
        if (token.type != TOKEN_INDENT && token.type != TOKEN_DEDENT) {
            last_type = token.type;
        }
    }

    if (brace_depth > 0 || paren_depth > 0 || bracket_depth > 0) return 0;

    // Trailing operators imply continuation
    if (last_type == TOKEN_PLUS || last_type == TOKEN_MINUS || last_type == TOKEN_STAR ||
        last_type == TOKEN_SLASH || last_type == TOKEN_PERCENT || last_type == TOKEN_POWER ||
        last_type == TOKEN_DOT || last_type == TOKEN_DOT_DOT || last_type == TOKEN_COMMA ||
        last_type == TOKEN_EQUAL_EQUAL || last_type == TOKEN_BANG_EQUAL ||
        last_type == TOKEN_LESS || last_type == TOKEN_LESS_EQUAL ||
        last_type == TOKEN_GREATER || last_type == TOKEN_GREATER_EQUAL ||
        last_type == TOKEN_AND || last_type == TOKEN_OR || last_type == TOKEN_NOT ||
        last_type == TOKEN_EQUALS || last_type == TOKEN_COLON || last_type == TOKEN_COLON_COLON) {
        return 0;
    }

    // If control flow keywords are present, assume incomplete until explicit empty line
    if (has_control_flow) return 0;

    return 1;
}

static void handle_sigint(int sig) {
    (void)sig;
    vm_request_interrupt();
}

void start_repl(void) {
    VM vm;
    init_vm(&vm);
    init_completion_state(&vm);
    vm.disable_gc = 1;  // Disable GC in REPL to keep all objects alive
    vm.is_repl = 1;     // Enable REPL mode

    printf("%sTOI %s%s\n", COLOR_KEYWORD, VERSION, COLOR_RESET);

    // Configure toi_lineedit
    toi_lineedit_set_multi_line(1);  // Enable multi-line editing
    toi_lineedit_set_syntax_highlight_callback(syntax_highlight_callback);
    toi_lineedit_set_completion_callback(completion_callback);
    toi_lineedit_history_set_max_len(100);  // Keep last 100 commands

    signal(SIGINT, handle_sigint);

    char *line;
    char buffer[8192];  // Buffer for accumulating multi-line input
    buffer[0] = '\0';

    for (;;) {
        line = toi_lineedit(buffer[0] == '\0' ? "> " : "... ");
        if (line == NULL) {
            if (errno == EAGAIN) {
                printf("\n");
                buffer[0] = '\0';
                continue;
            }
            if (errno == ENOENT) {
                break;
            }
            break;
        }
        int should_execute = 0;

        // Empty line during continuation submits the accumulated input
        if (line[0] == '\0' && buffer[0] != '\0') {
            // Submit what we have
            should_execute = 1;
        } else if (line[0] == '\0' && buffer[0] == '\0') {
            // Skip empty lines when not in continuation mode
            free(line);
            continue;
        } else {
            // Append to buffer
            if (buffer[0] != '\0') {
                // Add newline before appending continuation
                strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
                // Mirror continuation prompt indentation in the actual buffer.
                strncat(buffer, "  ", sizeof(buffer) - strlen(buffer) - 1);
            }
            strncat(buffer, line, sizeof(buffer) - strlen(buffer) - 1);

            // Check if input is complete
            if (!is_input_complete(buffer)) {
                // Need more input, continue to next line
                free(line);
                continue;
            }
            should_execute = 1;
        }

        free(line);

        if (should_execute) {
            // Input is complete, add to history and execute
            toi_lineedit_history_add(buffer);

            // Compile and execute
            ObjFunction* function = compile_repl(buffer);

            // Clear buffer for next input
            buffer[0] = '\0';

            if (function == NULL) {
                continue;
            }

            InterpretResult result = interpret(&vm, function);

            // If there's a value left on the stack, print it (REPL convenience)
            // Don't print the script closure itself (declarations return the script)
            if (result == INTERPRET_OK && vm.current_thread->stack_top > vm.current_thread->stack) {
                Value result_value = vm.current_thread->stack_top[-1];

                // Skip printing if it's the script closure (from declarations)
                int is_script_closure = 0;
                if (IS_CLOSURE(result_value)) {
                    ObjClosure* closure = AS_CLOSURE(result_value);
                    if (closure->function->name == NULL) {
                        is_script_closure = 1;
                    }
                }

                if (!is_script_closure) {
                    print_value_colored(result_value);
                    printf("\n");
                }
            }

            // Reset VM state for next iteration
            vm.current_thread->stack_top = vm.current_thread->stack;
            vm.current_thread->frame_count = 0;
        }
    }

    printf("\n");
    clear_completion_state();
    free_vm(&vm);
}
