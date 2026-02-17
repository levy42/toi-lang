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
#include "linenoise.h"

#define VERSION "0.0.1"
// ANSI color codes for syntax highlighting
#define COLOR_RESET     "\033[0m"
#define COLOR_KEYWORD   "\033[35m"  // Magenta for keywords
#define COLOR_NUMBER    "\033[36m"  // Cyan for numbers
#define COLOR_STRING    "\033[32m"  // Green for strings
#define COLOR_COMMENT   "\033[90m"  // Gray for comments
#define COLOR_OPERATOR  "\033[33m"  // Yellow for operators
#define COLOR_FUNCTION  "\033[94m"  // Bright blue for 'fn'
#define COLOR_ERROR     "\033[91m"  // Bright red for errors
#define COLOR_ESCAPE    "\033[36m"  // Cyan for escape sequences
#define COLOR_BUILDIN   "\033[95m"  // Bright magenta

// ANSI color codes for output
#define OUTPUT_NUMBER   "\033[36m"  // Cyan
#define OUTPUT_STRING   "\033[32m"  // Green
#define OUTPUT_BOOL     "\033[33m"  // Yellow
#define OUTPUT_NIL      "\033[90m"  // Gray
#define OUTPUT_FUNCTION "\033[35m"  // Magenta
#define OUTPUT_TABLE    "\033[34m"  // Blue

static void highlightLine(const char* line, char* output, size_t outputSize);
static VM* replVMForCompletion = NULL;
enum { REPL_COMPLETION_MAX = 7 };

typedef struct {
    const char* word;
    int length;
} CompletionWord;

static int isIdentifierStartChar(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static int isIdentifierChar(char c) {
    return isIdentifierStartChar(c) || (c >= '0' && c <= '9');
}

static int isValidIdentifier(const char* s, int length) {
    if (length <= 0 || !isIdentifierStartChar(s[0])) return 0;
    for (int i = 1; i < length; i++) {
        if (!isIdentifierChar(s[i])) return 0;
    }
    return 1;
}

static int startsWith(const char* s, int slen, const char* prefix, int plen) {
    if (plen > slen) return 0;
    return memcmp(s, prefix, (size_t)plen) == 0;
}

static int completionExists(linenoiseCompletions *lc, const char* candidate) {
    for (size_t i = 0; i < lc->len; i++) {
        if (strcmp(lc->cvec[i], candidate) == 0) return 1;
    }
    return 0;
}

static void addCompletionCandidate(const char *buf, int replaceStart, const char* replacement,
                                   linenoiseCompletions *lc) {
    if ((int)lc->len >= REPL_COMPLETION_MAX) return;

    int prefixLen = replaceStart;
    int replacementLen = (int)strlen(replacement);
    int outLen = prefixLen + replacementLen;
    char* candidate = (char*)malloc((size_t)outLen + 1);
    if (candidate == NULL) return;

    memcpy(candidate, buf, (size_t)prefixLen);
    memcpy(candidate + prefixLen, replacement, (size_t)replacementLen);
    candidate[outLen] = '\0';

    if (!completionExists(lc, candidate)) {
        linenoiseAddCompletion(lc, candidate);
    }
    free(candidate);
}

static void addIdentifierMatches(const char *buf, int replaceStart, const char* prefix, int prefixLen,
                                 linenoiseCompletions *lc, const CompletionWord* words) {
    for (int i = 0; words[i].word != NULL && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        if (startsWith(words[i].word, words[i].length, prefix, prefixLen)) {
            addCompletionCandidate(buf, replaceStart, words[i].word, lc);
        }
    }
}

static void addGlobalsMatches(const char *buf, int replaceStart, const char* prefix, int prefixLen,
                              linenoiseCompletions *lc) {
    if (replVMForCompletion == NULL) return;
    Table* globals = &replVMForCompletion->globals;

    for (int i = 0; i < globals->capacity && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        Entry* entry = &globals->entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        ObjString* key = entry->key;
        if (!isValidIdentifier(key->chars, key->length)) continue;
        if (!startsWith(key->chars, key->length, prefix, prefixLen)) continue;
        addCompletionCandidate(buf, replaceStart, key->chars, lc);
    }
}

static void addTableMemberMatches(const char *buf, int memberStart, const char* prefix, int prefixLen,
                                  ObjTable* table, linenoiseCompletions *lc) {
    for (int i = 0; i < table->table.capacity && (int)lc->len < REPL_COMPLETION_MAX; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        ObjString* key = entry->key;
        if (!isValidIdentifier(key->chars, key->length)) continue;
        if (!startsWith(key->chars, key->length, prefix, prefixLen)) continue;
        addCompletionCandidate(buf, memberStart, key->chars, lc);
    }
}

static int extractMemberContext(const char *buf, int len, int* outBaseStart, int* outBaseLen,
                                int* outMemberStart, int* outMemberLen) {
    int i = len - 1;
    while (i >= 0 && isIdentifierChar(buf[i])) i--;
    if (i < 0 || buf[i] != '.') return 0;

    int memberStart = i + 1;
    int memberLen = len - memberStart;
    int baseEnd = i;
    i--;
    while (i >= 0 && isIdentifierChar(buf[i])) i--;
    int baseStart = i + 1;
    int baseLen = baseEnd - baseStart;
    if (baseLen <= 0 || !isValidIdentifier(buf + baseStart, baseLen)) return 0;

    *outBaseStart = baseStart;
    *outBaseLen = baseLen;
    *outMemberStart = memberStart;
    *outMemberLen = memberLen;
    return 1;
}

static void formatNumber(double value, char* out, size_t outSize) {
    snprintf(out, outSize, "%.6f", value);
    int len = (int)strlen(out);
    while (len > 0 && out[len - 1] == '0') {
        out[--len] = '\0';
    }
    if (len > 0 && out[len - 1] == '.') {
        out[--len] = '\0';
    }
    if (len == 0) {
        strncpy(out, "0", outSize);
        out[outSize - 1] = '\0';
    }
}

static void appendHighlightedSnippet(const char* code, size_t len, char* output, size_t outputSize, size_t* outPos) {
    if (len == 0) return;
    char* buf = (char*)malloc(len + 1);
    memcpy(buf, code, len);
    buf[len] = '\0';

    size_t tempSize = len * 8 + 64;
    char* temp = (char*)malloc(tempSize);
    highlightLine(buf, temp, tempSize);

    size_t tempLen = strlen(temp);
    if (*outPos + tempLen < outputSize - 1) {
        memcpy(output + *outPos, temp, tempLen);
        *outPos += tempLen;
    }

    free(temp);
    free(buf);
}

typedef struct {
    const char* name;
    int length;
} BuiltinName;

static int isBuiltinIdentifier(const char* start, int length) {
    static const BuiltinName builtins[] = {
        // Core globals
        {"exit", 4},
        {"bool", 4},
        {"int", 3},
        {"float", 5},
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

    for (int i = 0; builtins[i].name != NULL; i++) {
        if (builtins[i].length == length &&
            memcmp(start, builtins[i].name, (size_t)length) == 0) {
            return 1;
        }
    }
    return 0;
}

// Check if a token is a keyword
static int isKeyword(TokenType type) {
    return type == TOKEN_IF || type == TOKEN_ELSE || type == TOKEN_ELIF ||
           type == TOKEN_WHILE || type == TOKEN_FOR || type == TOKEN_IN ||
           type == TOKEN_RETURN || type == TOKEN_LOCAL || type == TOKEN_NIL ||
           type == TOKEN_TRUE || type == TOKEN_FALSE || type == TOKEN_BREAK ||
           type == TOKEN_CONTINUE || type == TOKEN_IMPORT || type == TOKEN_PRINT ||
           type == TOKEN_GC || type == TOKEN_DEL;
}

// Check if a token is an operator
static int isOperator(TokenType type) {
    return type == TOKEN_PLUS || type == TOKEN_MINUS || type == TOKEN_STAR ||
           type == TOKEN_SLASH || type == TOKEN_PERCENT || type == TOKEN_EQUALS ||
           type == TOKEN_BANG_EQUAL || type == TOKEN_EQUAL_EQUAL ||
           type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL ||
           type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
           type == TOKEN_AND || type == TOKEN_OR || type == TOKEN_NOT ||
           type == TOKEN_DOT || type == TOKEN_DOT_DOT || type == TOKEN_QUESTION ||
           type == TOKEN_HASH || type == TOKEN_COLON || type == TOKEN_POWER ||
           type == TOKEN_INT_DIV;
}

// Apply syntax highlighting to a line of code
static void highlightLine(const char* line, char* output, size_t outputSize) {
    Lexer lexer;
    initLexer(&lexer, line);

    size_t outPos = 0;
    size_t linePos = 0;

    for (;;) {
        Token token = scanToken(&lexer);

        // Add any whitespace/text before this token
        while (linePos < (size_t)(token.start - line) && outPos < outputSize - 1) {
            output[outPos++] = line[linePos++];
        }

        if (token.type == TOKEN_EOF) break;

        // Choose color based on token type
        const char* color = COLOR_RESET;

        if (token.type == TOKEN_FN) {
            color = COLOR_FUNCTION;
        } else if (isKeyword(token.type)) {
            color = COLOR_KEYWORD;
        } else if (token.type == TOKEN_IDENTIFIER && isBuiltinIdentifier(token.start, token.length)) {
            color = COLOR_BUILDIN;
        } else if (token.type == TOKEN_NUMBER) {
            color = COLOR_NUMBER;
        } else if (token.type == TOKEN_STRING || token.type == TOKEN_FSTRING) {
            color = COLOR_STRING;
        } else if (isOperator(token.type)) {
            color = COLOR_OPERATOR;
        } else if (token.type == TOKEN_ERROR) {
            color = COLOR_ERROR;
        }

        // Add color code
        size_t colorLen = strlen(color);
        if (outPos + colorLen < outputSize - 1) {
            memcpy(output + outPos, color, colorLen);
            outPos += colorLen;
        }

        // Add token text handling specific tokens
        if ((token.type == TOKEN_STRING || token.type == TOKEN_FSTRING) && token.length > 0 && token.start[0] != '[') {
            // Highlight escape sequences in standard and f-strings
            for (int i = 0; i < token.length && outPos < outputSize - 1; i++) {
                char c = token.start[i];
                if (c == '\\' && i + 1 < token.length) {
                    // Switch to escape color
                    if (outPos + strlen(COLOR_ESCAPE) < outputSize - 1) {
                        memcpy(output + outPos, COLOR_ESCAPE, strlen(COLOR_ESCAPE));
                        outPos += strlen(COLOR_ESCAPE);
                    }
                    
                    // Print \ and the next char
                    output[outPos++] = c;
                    linePos++;
                    i++; 
                    if (outPos < outputSize - 1) {
                        output[outPos++] = token.start[i]; 
                        linePos++;
                    }
                    
                    // Switch back to string color
                    if (outPos + strlen(COLOR_STRING) < outputSize - 1) {
                        memcpy(output + outPos, COLOR_STRING, strlen(COLOR_STRING));
                        outPos += strlen(COLOR_STRING);
                    }
                } else if (token.type == TOKEN_FSTRING && c == '{') {
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
                        output[outPos++] = '{';
                        linePos++;

                        if (outPos + strlen(COLOR_RESET) < outputSize - 1) {
                            memcpy(output + outPos, COLOR_RESET, strlen(COLOR_RESET));
                            outPos += strlen(COLOR_RESET);
                        }

                        appendHighlightedSnippet(token.start + start, (size_t)(j - start), output, outputSize, &outPos);
                        linePos += (size_t)(j - start);

                        if (outPos + strlen(COLOR_STRING) < outputSize - 1) {
                            memcpy(output + outPos, COLOR_STRING, strlen(COLOR_STRING));
                            outPos += strlen(COLOR_STRING);
                        }

                        output[outPos++] = '}';
                        linePos++;
                        i = j;
                    } else {
                        output[outPos++] = c;
                        linePos++;
                    }
                } else {
                    output[outPos++] = c;
                    linePos++;
                }
            }
        } else {
            // Standard printing
            for (int i = 0; i < token.length && outPos < outputSize - 1; i++) {
                output[outPos++] = token.start[i];
                linePos++;
            }
        }

        // Reset color
        size_t resetLen = strlen(COLOR_RESET);
        if (outPos + resetLen < outputSize - 1) {
            memcpy(output + outPos, COLOR_RESET, resetLen);
            outPos += resetLen;
        }
    }

    // Add any remaining characters
    while (linePos < strlen(line) && outPos < outputSize - 1) {
        output[outPos++] = line[linePos++];
    }

    output[outPos] = '\0';
}

// Print a value with colors (for REPL output)
static void printValueColored(Value value) {
    if (IS_OBJ(value)) {
        if (IS_STRING(value)) {
            printf(OUTPUT_STRING "%s" COLOR_RESET, AS_CSTRING(value));
        } else if (IS_FUNCTION(value) || IS_CLOSURE(value)) {
            printf(OUTPUT_FUNCTION);
            printObject(value);
            printf(COLOR_RESET);
        } else if (IS_TABLE(value)) {
            printf(OUTPUT_TABLE);
            printObject(value);
            printf(COLOR_RESET);
        } else {
            printObject(value);
        }
    } else if (IS_NIL(value)) {
        printf(OUTPUT_NIL "nil" COLOR_RESET);
    } else if (IS_BOOL(value)) {
        printf(OUTPUT_BOOL "%s" COLOR_RESET, AS_BOOL(value) ? "true" : "false");
    } else if (IS_NUMBER(value)) {
        char buf[64];
        formatNumber(AS_NUMBER(value), buf, sizeof(buf));
        printf(OUTPUT_NUMBER "%s" COLOR_RESET, buf);
    }
}

// Linenoise syntax highlighting callback
static void syntaxHighlightCallback(const char *buf, char *highlighted, size_t maxlen) {
    highlightLine(buf, highlighted, maxlen);
}

// Linenoise completion callback for keyword/function completion
static void completionCallback(const char *buf, linenoiseCompletions *lc) {
    static const CompletionWord keywords[] = {
        {"fn", 2},
        {"for", 3},
        {"false", 5},
        {"if", 2},
        {"in", 2},
        {"import", 6},
        {"local", 5},
        {"return", 6},
        {"while", 5},
        {"true", 4},
        {"nil", 3},
        {"print", 5},
        {"break", 5},
        {"continue", 8},
        {"else", 4},
        {"elif", 4},
        {NULL, 0}
    };

    int len = (int)strlen(buf);

    int baseStart = 0, baseLen = 0, memberStart = 0, memberLen = 0;
    if (extractMemberContext(buf, len, &baseStart, &baseLen, &memberStart, &memberLen)) {
        if (replVMForCompletion == NULL) return;

        char baseName[128];
        if (baseLen >= (int)sizeof(baseName)) return;
        memcpy(baseName, buf + baseStart, (size_t)baseLen);
        baseName[baseLen] = '\0';

        ObjString* baseKey = copyString(baseName, baseLen);
        Value baseVal = NIL_VAL;
        if (!tableGet(&replVMForCompletion->globals, baseKey, &baseVal)) return;

        if (IS_TABLE(baseVal)) {
            addTableMemberMatches(buf, memberStart, buf + memberStart, memberLen, AS_TABLE(baseVal), lc);
            return;
        }

        if (IS_USERDATA(baseVal)) {
            ObjUserdata* udata = AS_USERDATA(baseVal);
            if (udata->metatable != NULL) {
                addTableMemberMatches(buf, memberStart, buf + memberStart, memberLen, udata->metatable, lc);
            }
            return;
        }
        return;
    }

    int start = len;
    while (start > 0 && isIdentifierChar(buf[start - 1])) start--;
    if (start < len && !isIdentifierStartChar(buf[start])) return;

    const char* prefix = buf + start;
    int prefixLen = len - start;
    addIdentifierMatches(buf, start, prefix, prefixLen, lc, keywords);
    addGlobalsMatches(buf, start, prefix, prefixLen, lc);
}

static void initCompletionState(VM* vm) {
    replVMForCompletion = vm;
}

static void clearCompletionState(void) {
    replVMForCompletion = NULL;
}

// Linenoise hints callback - shows syntax-highlighted hint after cursor
static char* hintsCallback(const char *buf, int *color, int *bold) {
    (void)buf;
    *color = 90;  // Gray
    *bold = 0;
    // Could show hints like function signatures here
    return NULL;
}

// Check if input is complete or needs continuation
// Returns 1 if complete, 0 if needs more input
static int isInputComplete(const char* input) {
    Lexer lexer;
    initLexer(&lexer, input);

    int braceDepth = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    TokenType lastType = TOKEN_EOF;
    int hasControlFlow = 0;

    for (;;) {
        Token token = scanToken(&lexer);

        if (token.type == TOKEN_EOF) break;
        // If we hit an error (like unterminated string), it's incomplete
        if (token.type == TOKEN_ERROR) return 0;

        switch (token.type) {
            case TOKEN_LEFT_BRACE: braceDepth++; break;
            case TOKEN_RIGHT_BRACE: braceDepth--; break;
            case TOKEN_LEFT_PAREN: parenDepth++; break;
            case TOKEN_RIGHT_PAREN: parenDepth--; break;
            case TOKEN_LEFT_BRACKET: bracketDepth++; break;
            case TOKEN_RIGHT_BRACKET: bracketDepth--; break;

            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_FN:
                hasControlFlow = 1;
                break;
            default: break;
        }

        // Ignore newlines/indents/dedents for "last token" check to catch trailing operators
        if (token.type != TOKEN_INDENT && token.type != TOKEN_DEDENT) {
            lastType = token.type;
        }
    }

    if (braceDepth > 0 || parenDepth > 0 || bracketDepth > 0) return 0;

    // Trailing operators imply continuation
    if (lastType == TOKEN_PLUS || lastType == TOKEN_MINUS || lastType == TOKEN_STAR ||
        lastType == TOKEN_SLASH || lastType == TOKEN_PERCENT || lastType == TOKEN_POWER ||
        lastType == TOKEN_DOT || lastType == TOKEN_DOT_DOT || lastType == TOKEN_COMMA ||
        lastType == TOKEN_EQUAL_EQUAL || lastType == TOKEN_BANG_EQUAL ||
        lastType == TOKEN_LESS || lastType == TOKEN_LESS_EQUAL ||
        lastType == TOKEN_GREATER || lastType == TOKEN_GREATER_EQUAL ||
        lastType == TOKEN_AND || lastType == TOKEN_OR || lastType == TOKEN_NOT ||
        lastType == TOKEN_EQUALS || lastType == TOKEN_COLON) {
        return 0;
    }

    // If control flow keywords are present, assume incomplete until explicit empty line
    if (hasControlFlow) return 0;

    return 1;
}

static void handleSigint(int sig) {
    (void)sig;
    vmRequestInterrupt();
}

void startREPL(void) {
    VM vm;
    initVM(&vm);
    initCompletionState(&vm);
    vm.disableGC = 1;  // Disable GC in REPL to keep all objects alive
    vm.isREPL = 1;     // Enable REPL mode

    printf("%sPUA %s%s\n", COLOR_KEYWORD, VERSION, COLOR_RESET);

    // Configure linenoise
    linenoiseSetMultiLine(1);  // Enable multi-line editing
    linenoiseSetSyntaxHighlightCallback(syntaxHighlightCallback);
    linenoiseSetCompletionCallback(completionCallback);
    linenoiseSetHintsCallback(hintsCallback);
    linenoiseHistorySetMaxLen(100);  // Keep last 100 commands

    signal(SIGINT, handleSigint);

    // Optional: Load history from file
    // linenoiseHistoryLoad(".mylang_history");

    char *line;
    char buffer[8192];  // Buffer for accumulating multi-line input
    buffer[0] = '\0';

    for (;;) {
        line = linenoise(buffer[0] == '\0' ? "> " : "... ");
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
        int shouldExecute = 0;

        // Empty line during continuation submits the accumulated input
        if (line[0] == '\0' && buffer[0] != '\0') {
            // Submit what we have
            shouldExecute = 1;
        } else if (line[0] == '\0' && buffer[0] == '\0') {
            // Skip empty lines when not in continuation mode
            free(line);
            continue;
        } else {
            // Append to buffer
            if (buffer[0] != '\0') {
                // Add newline before appending continuation
                strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
            }
            strncat(buffer, line, sizeof(buffer) - strlen(buffer) - 1);

            // Check if input is complete
            if (!isInputComplete(buffer)) {
                // Need more input, continue to next line
                free(line);
                continue;
            }
            shouldExecute = 1;
        }

        free(line);

        if (shouldExecute) {
            // Input is complete, add to history and execute
            linenoiseHistoryAdd(buffer);

            // Compile and execute
            ObjFunction* function = compileREPL(buffer);

            // Clear buffer for next input
            buffer[0] = '\0';

            if (function == NULL) {
                continue;
            }

            InterpretResult result = interpret(&vm, function);

            // If there's a value left on the stack, print it (REPL convenience)
            // Don't print the script closure itself (declarations return the script)
            if (result == INTERPRET_OK && vm.currentThread->stackTop > vm.currentThread->stack) {
                Value resultValue = vm.currentThread->stackTop[-1];

                // Skip printing if it's the script closure (from declarations)
                int isScriptClosure = 0;
                if (IS_CLOSURE(resultValue)) {
                    ObjClosure* closure = AS_CLOSURE(resultValue);
                    if (closure->function->name == NULL) {
                        isScriptClosure = 1;
                    }
                }

                if (!isScriptClosure) {
                    printValueColored(resultValue);
                    printf("\n");
                }
            }

            // Reset VM state for next iteration
            vm.currentThread->stackTop = vm.currentThread->stack;
            vm.currentThread->frameCount = 0;
        }
    }

    // Optional: Save history to file
    // linenoiseHistorySave(".mylang_history");

    printf("\n");
    clearCompletionState();
    freeVM(&vm);
}
