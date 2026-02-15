#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "opt.h"
#include "lexer.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
    Token current;
    Token previous;
    int hadError;
    int panicMode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_TERNARY,     // ? :
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(int canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    int isCaptured;  // Is this local captured by a closure?
    uint8_t type;
} Local;

typedef struct {
    uint8_t index;
    int isLocal;
} Upvalue;

typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT
} FunctionType;

typedef struct LoopContext {
    int start;                  // Loop start offset
    int scopeDepth;             // Scope depth at loop entry
    int breakJumps[256];        // Offsets of break jumps to patch
    int breakCount;             // Number of break jumps
    int continueJumps[256];     // Offsets of continue jumps to patch (for for loops)
    int continueCount;          // Number of continue jumps
    int isForLoop;               // Is this a for loop? (needs continue patching)
    int slotsToPop;             // Number of stack slots to pop on continue (for for-in loops)
    struct LoopContext* enclosing;  // Enclosing loop context
} LoopContext;

typedef struct Compiler {
    struct Compiler* enclosing;
    Local locals[UINT8_MAX + 1];
    int localCount;
    Upvalue upvalues[UINT8_MAX + 1];
    int upvalueCount;
    int scopeDepth;
    ObjFunction* function;
    FunctionType type;
    LoopContext* loopContext;   // Current loop context
} Compiler;

Parser parser;
Compiler* current = NULL;
Lexer lexer;
static int isREPLMode = 0;  // If 1, don't pop expression results 
static int lastExprEndsWithCall = 0;
static int lastExprWasRange = 0;
static int inForRangeHeader = 0;
static int inTableEntryExpression = 0;
static uint8_t typeStack[512];
static int typeStackTop = 0;

static void typePush(uint8_t type) {
    if (typeStackTop < (int)(sizeof(typeStack) / sizeof(typeStack[0]))) {
        typeStack[typeStackTop++] = type;
    }
}

static uint8_t typePop(void) {
    if (typeStackTop == 0) return TYPEHINT_ANY;
    return typeStack[--typeStackTop];
}

static int isNumericType(uint8_t type) {
    return type == TYPEHINT_INT || type == TYPEHINT_FLOAT;
}

static Chunk* currentChunk() {
    return &current->function->chunk;
}

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = 1;
    fprintf(stderr, COLOR_RED "[line %d] Error" COLOR_RESET, token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = 1;
}

static void error(const char* message) {
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
    errorAt(&parser.current, message);
}

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken(&lexer);
#ifdef DEBUG_COMPILER
        printf("Token: %d '%.*s'\n", parser.current.type, parser.current.length, parser.current.start);
#endif
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}



static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

static int match(TokenType type) {
    if (parser.current.type == type) {
        advance();
        return 1;
    }
    return 0;
}

static int check(TokenType type) {
    return parser.current.type == type;
}

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->count - 2;
}

typedef struct {
    int flagsOffset;
    int exceptOffset;
    int finallyOffset;
} TryPatch;

static TryPatch emitTry(uint8_t depth) {
    emitByte(OP_TRY);
    emitByte(depth);
    int flagsOffset = currentChunk()->count;
    emitByte(0); // flags
    int exceptOffset = currentChunk()->count;
    emitByte(0x00);
    emitByte(0x00);
    int finallyOffset = currentChunk()->count;
    emitByte(0x00);
    emitByte(0x00);
    return (TryPatch){flagsOffset, exceptOffset, finallyOffset};
}

static void patchJump(int offset) {
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void patchTry(int offset) {
    int jump = currentChunk()->count - offset - 4;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void patchTryFinally(int offset) {
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static void emitReturn() {
    emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > 255) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

static void emitConstant(Value value) {
    emitBytes(OP_CONSTANT, makeConstant(value));
}

static void initCompiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing = current;
    compiler->function = newFunction();
    compiler->type = type;

    compiler->localCount = 0;
    compiler->upvalueCount = 0;
    compiler->scopeDepth = 0;
    compiler->loopContext = NULL;

    // Claim stack slot 0
    Local* local = &compiler->locals[compiler->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->isCaptured = 0;
    local->type = TYPEHINT_ANY;

    current = compiler;

    if (type == TYPE_SCRIPT) {
        compiler->function->name = NULL;
        // Scripts are local-by-default (except REPL).
        if (!isREPLMode) {
            compiler->scopeDepth = 1;
        }
    } else {
        compiler->function->name = copyString(parser.previous.start, parser.previous.length);
    }
}

static ObjFunction* endCompiler() {
    emitReturn();
    ObjFunction* function = current->function;
    function->upvalueCount = current->upvalueCount;
#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        disassembleChunk(currentChunk(), function->name != NULL ? function->name->chars : "<script>");
    }
#endif
    return function;
}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        current->localCount--;
    }
}

static void expression();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);
static void emitGetNamed(Token name);
static void emitSetNamed(Token name);

static int tokenIndent(Token token) {
    const char* p = token.start;
    const char* lineStart = p;
    while (lineStart > lexer.sourceStart && lineStart[-1] != '\n') {
        lineStart--;
    }
    int indent = 0;
    while (lineStart < p) {
        if (*lineStart == ' ') indent++;
        else if (*lineStart == '\t') indent += 4;
        else break;
        lineStart++;
    }
    return indent;
}

static double parseNumberToken(Token token) {
    char* buf = (char*)malloc(token.length + 1);
    int w = 0;
    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];
        if (c != '_') buf[w++] = c;
    }
    buf[w] = '\0';
    double value = strtod(buf, NULL);
    free(buf);
    return value;
}

static int tokenIsInt(Token token) {
    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];
        if (c == '.' || c == 'e' || c == 'E') return 0;
    }
    return 1;
}

static void number(int canAssign) {
    (void)canAssign;
    double value = parseNumberToken(parser.previous);
    emitConstant(NUMBER_VAL(value));
    typePush(tokenIsInt(parser.previous) ? TYPEHINT_INT : TYPEHINT_FLOAT);
}

static void string(int canAssign) {
    (void)canAssign;

    // Check if this is a multiline string [[...]]
    if (parser.previous.length >= 4 &&
        parser.previous.start[0] == '[' &&
        parser.previous.start[1] == '[') {
        // Multiline string - no escape sequences, just copy content
        const char* src = parser.previous.start + 2;  // Skip [[
        int rawLen = parser.previous.length - 4;      // Exclude [[ and ]]
    ObjString* s = copyString(src, rawLen);
    emitConstant(OBJ_VAL(s));
    typePush(TYPEHINT_STR);
    return;
    }

    // Regular quoted string ("..." or '...') - process escape sequences
    char quote = parser.previous.start[0];
    const char* src = parser.previous.start + 1;
    int rawLen = parser.previous.length - 2;
    char* buf = (char*)malloc(rawLen + 1); // max size (shrinks after escapes)
    int w = 0;
    for (int i = 0; i < rawLen; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < rawLen) {
            char e = src[++i];
            switch (e) {
                case 'n': buf[w++] = '\n'; break;
                case 't': buf[w++] = '\t'; break;
                case 'r': buf[w++] = '\r'; break;
                case '\'': buf[w++] = '\''; break;
                case '"': buf[w++] = '"'; break;
                case '\\': buf[w++] = '\\'; break;
                default: // unknown escape, keep as-is
                    buf[w++] = '\\';
                    buf[w++] = e;
                    break;
            }
        } else {
            if (c == quote) {
                // Should not occur (lexer terminates on unescaped quote), but keep safe.
                continue;
            }
            buf[w++] = c;
        }
    }
    ObjString* s = copyString(buf, w);
    free(buf);
    emitConstant(OBJ_VAL(s));
    typePush(TYPEHINT_STR);
}

static void literal(int canAssign) {
    (void)canAssign;
    switch (parser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); typePush(TYPEHINT_BOOL); break;
        case TOKEN_NIL: emitByte(OP_NIL); typePush(TYPEHINT_ANY); break;
        case TOKEN_TRUE: emitByte(OP_TRUE); typePush(TYPEHINT_BOOL); break;
        default: return; // Unreachable.
    }
}

// Forward declarations for fstring
static void expression();

static void fstring(int canAssign) {
    (void)canAssign;
    int baseTop = typeStackTop;

    // Token is f"..." - extract content (skip f" at start and " at end)
    const char* src = parser.previous.start + 2;
    int rawLen = parser.previous.length - 3;

    int partCount = 0;
    int i = 0;

    while (i < rawLen) {
        // Find next { or end of string
        int start = i;
        while (i < rawLen && src[i] != '{') {
            if (src[i] == '\\' && i + 1 < rawLen) i++; // skip escape
            i++;
        }

        // Emit literal part (start to i)
        if (i > start) {
            // Process escape sequences in literal part
            char* buf = (char*)malloc(i - start + 1);
            int w = 0;
            for (int j = start; j < i; j++) {
                char c = src[j];
                if (c == '\\' && j + 1 < i) {
                    char e = src[++j];
                    switch (e) {
                        case 'n': buf[w++] = '\n'; break;
                        case 't': buf[w++] = '\t'; break;
                        case 'r': buf[w++] = '\r'; break;
                        case '"': buf[w++] = '"'; break;
                        case '\\': buf[w++] = '\\'; break;
                        case '{': buf[w++] = '{'; break;
                        case '}': buf[w++] = '}'; break;
                        default:
                            buf[w++] = '\\';
                            buf[w++] = e;
                            break;
                    }
                } else {
                    buf[w++] = c;
                }
            }
            ObjString* s = copyString(buf, w);
            free(buf);
            emitConstant(OBJ_VAL(s));
            partCount++;
        }

        if (i < rawLen && src[i] == '{') {
            i++; // skip {

            // Find matching }
            int exprStart = i;
            int braceDepth = 1;
            while (i < rawLen && braceDepth > 0) {
                // Check for multiline strings [[...]]
                if (src[i] == '[' && i + 1 < rawLen && src[i+1] == '[') {
                    i += 2;
                    while (i < rawLen && !(src[i] == ']' && i + 1 < rawLen && src[i+1] == ']')) {
                        i++;
                    }
                    if (i < rawLen) i += 2; // consume ]]
                    continue;
                }
                
                // Check for comments --...
                if (src[i] == '-' && i + 1 < rawLen && src[i+1] == '-') {
                    i += 2;
                    while (i < rawLen && src[i] != '\n') i++;
                    continue;
                }
                
                // Check for string start: \"
                if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '"') {
                    i += 2; // enter string
                    while (i < rawLen) {
                        // Check for escaped backslash (double backslash)
                        if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '\\') {
                            i += 2;
                            continue;
                        }

                        // Check for closing quote \"
                        if (src[i] == '\\' && i + 1 < rawLen && src[i+1] == '"') {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                    continue;
                }

                // Check for other escapes (skip them so we don't count \{ or \})
                if (src[i] == '\\') {
                    if (i + 1 < rawLen) i += 2;
                    else i++;
                    continue;
                }

                if (src[i] == '{') braceDepth++;
                else if (src[i] == '}') braceDepth--;
                if (braceDepth > 0) i++;
            }
            int exprLen = i - exprStart;
            i++; // skip closing }

            if (exprLen > 0) {
                // Save current parser/lexer state
                Parser savedParser = parser;
                Lexer savedLexer = lexer;

                // Create expression source: str(<expr>)
                // This wraps the expression in str() call for automatic conversion
                // We must unescape \" -> " and \\ -> \ etc.
                char* exprSrc = (char*)malloc(exprLen + 10);
                memcpy(exprSrc, "str(", 4);
                
                int w = 4;
                for (int j = exprStart; j < exprStart + exprLen; j++) {
                    if (src[j] == '\\' && j + 1 < exprStart + exprLen) {
                        if (src[j+1] == '"') {
                            exprSrc[w++] = '"';
                            j++;
                        } else if (src[j+1] == '\\') {
                            exprSrc[w++] = '\\';
                            j++;
                        } else if (src[j+1] == '{' || src[j+1] == '}') {
                            exprSrc[w++] = src[j+1];
                            j++;
                        } else {
                            // Keep other escapes as is (e.g. \n)
                            exprSrc[w++] = '\\';
                        }
                    } else {
                        exprSrc[w++] = src[j];
                    }
                }
                
                memcpy(exprSrc + w, ")", 2); // includes null terminator

                // Initialize new lexer for the expression



                initLexer(&lexer, exprSrc);

                // Reset parser for expression
                parser.hadError = 0;
                parser.panicMode = 0;
                advance();

                // Compile the expression (which is now str(original_expr))
                expression();

                free(exprSrc);

                // Restore parser/lexer state
                parser = savedParser;
                lexer = savedLexer;

                partCount++;
            }
        }
    }

    // Handle empty f-string
    if (partCount == 0) {
        ObjString* s = copyString("", 0);
        emitConstant(OBJ_VAL(s));
        typeStackTop = baseTop;
        typePush(TYPEHINT_STR);
        return;
    }

    // Concatenate all parts: emit (partCount - 1) OP_ADD instructions
    for (int j = 1; j < partCount; j++) {
        emitByte(OP_ADD);
    }
    typeStackTop = baseTop;
    typePush(TYPEHINT_STR);
}

static void grouping(int canAssign) {
    (void)canAssign;
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}


static void unary(int canAssign) {
    (void)canAssign;
    TokenType operatorType = parser.previous.type;
    parsePrecedence(PREC_UNARY);
    uint8_t rhsType = typePop();
    switch (operatorType) {
        case TOKEN_NOT:
            emitByte(OP_NOT);
            typePush(TYPEHINT_BOOL);
            break;
        case TOKEN_MINUS:
            emitByte(OP_NEGATE);
            typePush(isNumericType(rhsType) ? rhsType : TYPEHINT_ANY);
            break;
        case TOKEN_HASH:
            emitByte(OP_LENGTH);
            typePush(TYPEHINT_INT);
            break;
        default: return;
    }
    lastExprEndsWithCall = 0;
}

static void binary(int canAssign) {
    (void)canAssign;
    TokenType operatorType = parser.previous.type;
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));
    uint8_t rhsType = typePop();
    uint8_t lhsType = typePop();
    uint8_t outType = TYPEHINT_ANY;
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:
            emitBytes(OP_EQUAL, OP_NOT);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_EQUAL_EQUAL:
            emitByte(OP_EQUAL);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_GREATER:
            emitByte(OP_GREATER);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_GREATER_EQUAL:
            emitBytes(OP_LESS, OP_NOT);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_LESS:
            emitByte(OP_LESS);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_LESS_EQUAL:
            emitBytes(OP_GREATER, OP_NOT);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_HAS:
            emitByte(OP_HAS);
            outType = TYPEHINT_BOOL;
            break;
        case TOKEN_PLUS:
            if (isNumericType(lhsType) && isNumericType(rhsType)) {
                if (lhsType == TYPEHINT_INT && rhsType == TYPEHINT_INT) {
                    emitByte(OP_IADD);
                    outType = TYPEHINT_INT;
                } else {
                    emitByte(OP_FADD);
                    outType = TYPEHINT_FLOAT;
                }
            } else {
                emitByte(OP_ADD);
            }
            break;
        case TOKEN_MINUS:
            if (isNumericType(lhsType) && isNumericType(rhsType)) {
                if (lhsType == TYPEHINT_INT && rhsType == TYPEHINT_INT) {
                    emitByte(OP_ISUB);
                    outType = TYPEHINT_INT;
                } else {
                    emitByte(OP_FSUB);
                    outType = TYPEHINT_FLOAT;
                }
            } else {
                emitByte(OP_SUBTRACT);
            }
            break;
        case TOKEN_STAR:
            if (isNumericType(lhsType) && isNumericType(rhsType)) {
                if (lhsType == TYPEHINT_INT && rhsType == TYPEHINT_INT) {
                    emitByte(OP_IMUL);
                    outType = TYPEHINT_INT;
                } else {
                    emitByte(OP_FMUL);
                    outType = TYPEHINT_FLOAT;
                }
            } else {
                emitByte(OP_MULTIPLY);
            }
            break;
        case TOKEN_SLASH:
            if (isNumericType(lhsType) && isNumericType(rhsType)) {
                emitByte(OP_FDIV);
                outType = TYPEHINT_FLOAT;
            } else {
                emitByte(OP_DIVIDE);
            }
            break;
        case TOKEN_POWER:         emitByte(OP_POWER); break;
        case TOKEN_INT_DIV:       emitByte(OP_INT_DIV); break;
        case TOKEN_PERCENT:
            if (isNumericType(lhsType) && isNumericType(rhsType)) {
                if (lhsType == TYPEHINT_INT && rhsType == TYPEHINT_INT) {
                    emitByte(OP_IMOD);
                    outType = TYPEHINT_INT;
                } else {
                    emitByte(OP_FMOD);
                    outType = TYPEHINT_FLOAT;
                }
            } else {
                emitByte(OP_MODULO);
            }
            break;
        default: return;
    }
    typePush(outType);
    lastExprEndsWithCall = 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (name->length == local->name.length &&
            memcmp(name->start, local->name.start, name->length) == 0) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static int addUpvalue(Compiler* compiler, uint8_t index, int isLocal) {
    int upvalueCount = compiler->upvalueCount;

    // Check if upvalue already exists
    for (int i = 0; i < upvalueCount; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    if (upvalueCount == UINT8_MAX + 1) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index = index;
    return compiler->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    // Try to resolve in enclosing function's locals
    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = 1;
        return addUpvalue(compiler, (uint8_t)local, 1);
    }

    // Try to resolve in enclosing function's upvalues
    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, 0);
    }

    return -1;
}

static void addLocal(Token name) {
    if (current->localCount == UINT8_MAX + 1) {
        error("Too many local variables in function.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->isCaptured = 0;
    local->type = TYPEHINT_ANY;
}

static void markInitialized() {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void markInitializedCount(int count) {
    if (current->scopeDepth == 0) return;
    for (int i = 0; i < count; i++) {
        current->locals[current->localCount - 1 - i].depth = current->scopeDepth;
    }
}

static void declareVariable() {
    if (current->scopeDepth == 0 && !isREPLMode) return;
    Token* name = &parser.previous;
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break; 
        }
        if (name->length == local->name.length &&
            memcmp(name->start, local->name.start, name->length) == 0) {
            error("Already a variable with this name in this scope.");
        }
    }
    addLocal(*name);
}

static uint8_t identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static uint8_t parseTypeName(Token* name) {
    if (name->length == 3 && memcmp(name->start, "int", 3) == 0) return TYPEHINT_INT;
    if (name->length == 5 && memcmp(name->start, "float", 5) == 0) return TYPEHINT_FLOAT;
    if (name->length == 4 && memcmp(name->start, "bool", 4) == 0) return TYPEHINT_BOOL;
    if ((name->length == 3 && memcmp(name->start, "str", 3) == 0) ||
        (name->length == 6 && memcmp(name->start, "string", 6) == 0)) return TYPEHINT_STR;
    if (name->length == 5 && memcmp(name->start, "table", 5) == 0) return TYPEHINT_TABLE;
    return TYPEHINT_ANY;
}

static void setLocalType(int localIndex, uint8_t type) {
    if (localIndex < 0 || localIndex >= current->localCount) return;
    current->locals[localIndex].type = type;
}

static void updateLocalType(int localIndex, uint8_t rhsType) {
    if (localIndex < 0 || localIndex >= current->localCount) return;
    uint8_t currentType = current->locals[localIndex].type;
    if (rhsType == TYPEHINT_ANY) {
        current->locals[localIndex].type = TYPEHINT_ANY;
        return;
    }
    if (currentType == TYPEHINT_ANY) {
        current->locals[localIndex].type = rhsType;
        return;
    }
    if (currentType != rhsType) {
        current->locals[localIndex].type = TYPEHINT_ANY;
    }
}

static void setParamType(ObjFunction* function, int index, uint8_t type) {
    if (index < 0) return;
    if (function->paramTypesCount < function->arity) {
        int old = function->paramTypesCount;
        function->paramTypesCount = function->arity;
        function->paramTypes = (uint8_t*)realloc(function->paramTypes, sizeof(uint8_t) * function->paramTypesCount);
        for (int i = old; i < function->paramTypesCount; i++) {
            function->paramTypes[i] = TYPEHINT_ANY;
        }
    }
    if (index < function->paramTypesCount) {
        function->paramTypes[index] = type;
    }
}

static void setParamName(ObjFunction* function, int index, Token* name) {
    if (index < 0) return;
    if (function->paramNamesCount < function->arity) {
        int old = function->paramNamesCount;
        function->paramNamesCount = function->arity;
        function->paramNames = (ObjString**)realloc(function->paramNames, sizeof(ObjString*) * function->paramNamesCount);
        for (int i = old; i < function->paramNamesCount; i++) {
            function->paramNames[i] = NULL;
        }
    }
    if (index < function->paramNamesCount) {
        function->paramNames[index] = copyString(name->start, name->length);
    }
}

static uint8_t parseVariable(const char* errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);
    declareVariable();
    if (current->scopeDepth > 0) return 0;
    return identifierConstant(&parser.previous);
}

static void defineVariable(uint8_t global) {
    if (current->scopeDepth > 0) return;
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static int hasSliceRangeInSubscript(void);
static int rhsHasTopLevelComma(int startLine);
static void parseArrayLiteralFromCommaList(void);

static void namedVariable(Token name, int canAssign) {
    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((arg = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        arg = identifierConstant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQUALS)) {
        int startLine = parser.current.line;
        if (rhsHasTopLevelComma(startLine)) {
            parseArrayLiteralFromCommaList();
        } else {
            expression();
        }
        uint8_t rhsType = typePop();
        if (getOp == OP_GET_LOCAL) {
            emitBytes(setOp, (uint8_t)arg);
            updateLocalType(arg, rhsType);
        } else if (getOp == OP_GET_UPVALUE) {
            emitBytes(setOp, (uint8_t)arg);
        } else if (isREPLMode && current->type == TYPE_SCRIPT) {
            emitByte(OP_DUP);
            emitBytes(OP_DEFINE_GLOBAL, (uint8_t)arg);
        } else {
            // Local-by-default: assignment creates a new local if not resolved.
            int localIndex = current->localCount;
            addLocal(name);
            markInitialized();
            emitBytes(OP_SET_LOCAL, (uint8_t)localIndex);
            setLocalType(localIndex, rhsType);
        }
        typePush(rhsType);
    } else {
        emitBytes(getOp, (uint8_t)arg);
        if (getOp == OP_GET_LOCAL && arg >= 0 && arg < current->localCount) {
            typePush(current->locals[arg].type);
        } else {
            typePush(TYPEHINT_ANY);
        }
    }
}

static void emitGetNamed(Token name) {
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        emitBytes(OP_GET_LOCAL, (uint8_t)arg);
        return;
    }
    arg = resolveUpvalue(current, &name);
    if (arg != -1) {
        emitBytes(OP_GET_UPVALUE, (uint8_t)arg);
        return;
    }
    emitBytes(OP_GET_GLOBAL, identifierConstant(&name));
}

static void emitSetNamed(Token name) {
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        emitBytes(OP_SET_LOCAL, (uint8_t)arg);
        return;
    }
    arg = resolveUpvalue(current, &name);
    if (arg != -1) {
        emitBytes(OP_SET_UPVALUE, (uint8_t)arg);
        return;
    }
    emitBytes(OP_SET_GLOBAL, identifierConstant(&name));
}

static void variable(int canAssign) {
    namedVariable(parser.previous, canAssign);
}

static void dot(int canAssign) {
    lastExprEndsWithCall = 0;
    int baseTop = typeStackTop - 1;
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    uint8_t name = identifierConstant(&parser.previous);
    
    if (canAssign && match(TOKEN_EQUALS)) {
        emitBytes(OP_CONSTANT, name);
        int startLine = parser.current.line;
        if (rhsHasTopLevelComma(startLine)) {
            parseArrayLiteralFromCommaList();
        } else {
            expression();
        }
        emitByte(OP_SET_TABLE);
        {
            uint8_t rhsType = typePop();
            typeStackTop = baseTop;
            typePush(rhsType);
        }
    } else {
        emitBytes(OP_CONSTANT, name);
        emitByte(OP_GET_TABLE);
        typeStackTop = baseTop;
        typePush(TYPEHINT_ANY);
    }
}

static void subscript(int canAssign) {
    lastExprEndsWithCall = 0;
    int baseTop = typeStackTop - 1;
    if (hasSliceRangeInSubscript()) {
        if (check(TOKEN_DOT_DOT)) {
            advance();
            emitByte(OP_NIL); // start
        } else {
            parsePrecedence((Precedence)(PREC_TERM + 1));
            consume(TOKEN_DOT_DOT, "Expect '..' in slice.");
        }
        if (check(TOKEN_COLON) || check(TOKEN_RIGHT_BRACKET)) {
            emitByte(OP_NIL); // end
        } else {
            expression(); // end
        }
        if (match(TOKEN_COLON)) {
            if (check(TOKEN_RIGHT_BRACKET)) {
                emitConstant(NUMBER_VAL(1));
            } else {
                expression(); // step
            }
        } else {
            emitConstant(NUMBER_VAL(1));
        }
        consume(TOKEN_RIGHT_BRACKET, "Expect ']' after slice.");
        if (canAssign && match(TOKEN_EQUALS)) {
            error("Can't assign to a slice.");
            expression();
        }
        emitByte(OP_SLICE);
        typeStackTop = baseTop;
        typePush(TYPEHINT_ANY);
        lastExprEndsWithCall = 0;
        return;
    }
    expression();

    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

    if (canAssign && match(TOKEN_EQUALS)) {
        int startLine = parser.current.line;
        if (rhsHasTopLevelComma(startLine)) {
            parseArrayLiteralFromCommaList();
        } else {
            expression();
        }
        emitByte(OP_SET_TABLE);
        {
            uint8_t rhsType = typePop();
            typeStackTop = baseTop;
            typePush(rhsType);
        }
    } else {
        emitByte(OP_GET_TABLE);
        typeStackTop = baseTop;
        typePush(TYPEHINT_ANY);
    }
}

static void parseTableEntries();
static void table(int canAssign);
static int isTableComprehensionStart(int startLine);
static void tableComprehension(int canAssign);
static void compileExpressionFromString(const char* srcStart, size_t srcLen);
static int findComprehensionFor(const char** forStart);
static const char* findComprehensionAssign(Lexer base, const char* exprStart, const char* exprEnd);
static int isImplicitTableSeparator(void);

static int isTableEntryStart(TokenType type) {
    switch (type) {
        case TOKEN_LEFT_BRACKET:
        case TOKEN_LEFT_PAREN:
        case TOKEN_LEFT_BRACE:
        case TOKEN_IDENTIFIER:
        case TOKEN_STRING:
        case TOKEN_FSTRING:
        case TOKEN_NUMBER:
        case TOKEN_NIL:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_NOT:
        case TOKEN_MINUS:
        case TOKEN_HASH:
        case TOKEN_FN:
        case TOKEN_IMPORT:
            return 1;
        default:
            return 0;
    }
}

static int isImplicitTableSeparator(void) {
    if (parser.current.line <= parser.previous.line) return 0;
    return isTableEntryStart(parser.current.type);
}

static void tableEntryExpression(void) {
    int saved = inTableEntryExpression;
    inTableEntryExpression = 1;
    expression();
    inTableEntryExpression = saved;
}

static void parseTableEntries() {
    double arrayIndex = 1.0;
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        emitByte(OP_DUP);
        if (match(TOKEN_LEFT_BRACKET)) {
            tableEntryExpression();
            consume(TOKEN_RIGHT_BRACKET, "Expect ']' after key.");
            consume(TOKEN_EQUALS, "Expect '=' after key.");
            tableEntryExpression();
            emitByte(OP_SET_TABLE);
            emitByte(OP_POP);
        } else if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            if (match(TOKEN_EQUALS)) {
                emitConstant(OBJ_VAL(copyString(name.start, name.length)));
                tableEntryExpression();
                emitByte(OP_SET_TABLE);
                emitByte(OP_POP);
            } else {
                // Array item that happens to be an identifier
                emitConstant(NUMBER_VAL(arrayIndex++));
                namedVariable(name, false);
                emitByte(OP_SET_TABLE);
                emitByte(OP_POP);
            }
        } else {
            // Array item
            emitConstant(NUMBER_VAL(arrayIndex++));
            tableEntryExpression();
            emitByte(OP_SET_TABLE);
            emitByte(OP_POP);
        }
        if (match(TOKEN_COMMA) || isImplicitTableSeparator()) continue;
        break;
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after table.");
}

static void table(int canAssign) {
    (void)canAssign;
    int baseTop = typeStackTop;
    if (isTableComprehensionStart(parser.previous.line)) {
        tableComprehension(canAssign);
        typeStackTop = baseTop;
        typePush(TYPEHINT_TABLE);
        return;
    }
    emitByte(OP_NEW_TABLE);
    parseTableEntries();
    typeStackTop = baseTop;
    typePush(TYPEHINT_TABLE);
}

static void tableInfix(int canAssign) {
    (void)canAssign;
    int baseTop = typeStackTop;
    // Left side (metatable) is already on stack
    emitByte(OP_NEW_TABLE);
    parseTableEntries();
    emitByte(OP_SET_METATABLE);
    typeStackTop = baseTop - 1;
    typePush(TYPEHINT_TABLE);
}

static void grouping(int canAssign);
static void parseCall(int canAssign);
static void unary(int canAssign);
static void binary(int canAssign);
static void number(int canAssign);
static void literal(int canAssign);
static void string(int canAssign);
static void variable(int canAssign);
static void table(int canAssign);
static void dot(int canAssign);
static void subscript(int canAssign);

static void and_(int canAssign) {
    (void)canAssign;
    typePop();
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
    typePop();
    typePush(TYPEHINT_ANY);
    lastExprEndsWithCall = 0;
}

static void or_(int canAssign) {
    (void)canAssign;
    typePop();
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump = emitJump(OP_JUMP);

    patchJump(elseJump);
    emitByte(OP_POP);

    parsePrecedence(PREC_OR);
    patchJump(endJump);
    typePop();
    typePush(TYPEHINT_ANY);
    lastExprEndsWithCall = 0;
}

static void ternary(int canAssign) {
    (void)canAssign;

    // At this point, condition is on stack
    typePop();
    // Jump to false branch if condition is false
    int elseBranch = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Pop condition

    // Parse true expression (higher precedence to avoid consuming another ?)
    parsePrecedence(PREC_TERNARY + 1);

    consume(TOKEN_COLON, "Expect ':' after true branch of ternary operator.");

    // Jump over false branch
    int endJump = emitJump(OP_JUMP);

    // False branch
    patchJump(elseBranch);
    emitByte(OP_POP); // Pop condition

    // Parse false expression (same precedence for right-associativity)
    parsePrecedence(PREC_TERNARY);

    patchJump(endJump);
    {
        uint8_t falseType = typePop();
        uint8_t trueType = typePop();
        typePush(trueType == falseType ? trueType : TYPEHINT_ANY);
    }
    lastExprEndsWithCall = 0;
}

static void importExpression(int canAssign) {
    (void)canAssign;
    // Parse: import module_name[.submodule...]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'import'.");

    // Build the full dotted module path
    char modulePath[256];
    int len = 0;

    // Copy first identifier
    int firstLen = parser.previous.length;
    if (firstLen >= 256) firstLen = 255;
    memcpy(modulePath, parser.previous.start, firstLen);
    len = firstLen;

    // Parse additional .submodule components
    while (match(TOKEN_DOT)) {
        if (len < 255) modulePath[len++] = '.';
        consume(TOKEN_IDENTIFIER, "Expect module name after '.'.");
        int partLen = parser.previous.length;
        if (len + partLen >= 256) partLen = 255 - len;
        if (partLen > 0) {
            memcpy(modulePath + len, parser.previous.start, partLen);
            len += partLen;
        }
    }
    modulePath[len] = '\0';

    // Create a string constant with the full path
    ObjString* pathString = copyString(modulePath, len);
    uint8_t pathConstant = makeConstant(OBJ_VAL(pathString));

    // Emit OP_IMPORT with the full path
    emitBytes(OP_IMPORT, pathConstant);
    typePush(TYPEHINT_ANY);
}

static void anonymousFunction(int canAssign);

static void range_(int canAssign) {
    (void)canAssign;
    parsePrecedence(PREC_TERM);
    if (inForRangeHeader) {
        lastExprWasRange = 1;
        return;
    }
    emitByte(OP_RANGE);
    typePop();
    typePop();
    typePush(TYPEHINT_ANY);
    lastExprEndsWithCall = 0;
}

static void compileExpressionFromString(const char* srcStart, size_t srcLen) {
    char* exprSrc = (char*)malloc(srcLen + 1);
    memcpy(exprSrc, srcStart, srcLen);
    exprSrc[srcLen] = '\0';

    Parser savedParser = parser;
    Lexer savedLexer = lexer;
    int savedLastCall = lastExprEndsWithCall;
    int savedTypeTop = typeStackTop;

    initLexer(&lexer, exprSrc);
    parser.hadError = 0;
    parser.panicMode = 0;
    advance();
    expression();

    free(exprSrc);
    parser = savedParser;
    lexer = savedLexer;
    lastExprEndsWithCall = savedLastCall;
    typeStackTop = savedTypeTop;
}

static int findComprehensionFor(const char** forStart) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scanToken(&peek);
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE:
                if (brace > 0) { brace--; break; }
                return 0;
            case TOKEN_FOR:
                if (paren == 0 && bracket == 0 && brace == 0) {
                    *forStart = tok.start;
                    return 1;
                }
                break;
            case TOKEN_EOF:
                return 0;
            default:
                break;
        }
    }
}

static const char* findComprehensionAssign(Lexer base, const char* exprStart, const char* exprEnd) {
    Lexer peek = base;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scanToken(&peek);
        if (tok.start < exprStart) continue;
        if (tok.start >= exprEnd) return NULL;
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE: if (brace > 0) brace--; break;
            case TOKEN_EQUALS:
                if (paren == 0 && bracket == 0 && brace == 0) {
                    return tok.start;
                }
                break;
            default:
                break;
        }
    }
}

static int hasSliceRangeInSubscript(void) {
    if (parser.current.type == TOKEN_DOT_DOT) return 1;
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scanToken(&peek);
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE: if (brace > 0) brace--; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET:
                if (bracket == 0 && paren == 0 && brace == 0) return 0;
                if (bracket > 0) bracket--;
                break;
            case TOKEN_DOT_DOT:
                if (paren == 0 && bracket == 0 && brace == 0) return 1;
                break;
            case TOKEN_EOF:
                return 0;
            default:
                break;
        }
    }
}

static int rhsHasTopLevelComma(int startLine) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    // The lexer is already past parser.current, so account for it first.
    Token tok = parser.current;
    if (tok.type == TOKEN_EOF) return 0;
    if (tok.line > startLine && paren == 0 && bracket == 0 && brace == 0) return 0;
    switch (tok.type) {
        case TOKEN_LEFT_PAREN: paren++; break;
        case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
        case TOKEN_LEFT_BRACKET: bracket++; break;
        case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
        case TOKEN_LEFT_BRACE: brace++; break;
        case TOKEN_RIGHT_BRACE: if (brace > 0) brace--; break;
        case TOKEN_COMMA:
            if (paren == 0 && bracket == 0 && brace == 0) return 1;
            break;
        case TOKEN_SEMICOLON:
        case TOKEN_DEDENT:
            if (paren == 0 && bracket == 0 && brace == 0) return 0;
            break;
        default:
            break;
    }
    for (;;) {
        tok = scanToken(&peek);
        if (tok.type == TOKEN_EOF) return 0;
        if (tok.line > startLine && paren == 0 && bracket == 0 && brace == 0) return 0;
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE: if (brace > 0) brace--; break;
            case TOKEN_COMMA:
                if (paren == 0 && bracket == 0 && brace == 0) return 1;
                break;
            case TOKEN_SEMICOLON:
            case TOKEN_DEDENT:
                if (paren == 0 && bracket == 0 && brace == 0) return 0;
                break;
            default:
                break;
        }
    }
}

static void parseArrayLiteralFromCommaList(void) {
    emitByte(OP_NEW_TABLE);
    int index = 1;
    do {
        emitByte(OP_DUP);
        emitConstant(NUMBER_VAL(index++));
        expression();
        emitByte(OP_SET_TABLE);
        emitByte(OP_POP);
    } while (match(TOKEN_COMMA));
}

static int isTableComprehensionStart(int startLine) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scanToken(&peek);
        if (tok.line > startLine && paren == 0 && bracket == 0 && brace == 0) {
            return 0;
        }
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN: if (paren > 0) paren--; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE:
                if (brace == 0 && paren == 0 && bracket == 0) return 0;
                if (brace > 0) brace--;
                break;
            case TOKEN_FOR:
                if (paren == 0 && bracket == 0 && brace == 0) return 1;
                break;
            case TOKEN_EOF:
                return 0;
            default:
                break;
        }
    }
}

static void tableComprehension(int canAssign) {
    (void)canAssign;

    const char* exprStart = parser.current.start;
    Lexer exprLexer = lexer;
    const char* forStart = NULL;
    if (!findComprehensionFor(&forStart)) {
        error("Expected table comprehension 'expr for ...'.");
        return;
    }
    size_t exprLen = (size_t)(forStart - exprStart);

    while (!(parser.current.type == TOKEN_FOR && parser.current.start == forStart)) {
        if (parser.current.type == TOKEN_EOF) {
            error("Expected 'for' in table comprehension.");
            return;
        }
        advance();
    }

    Compiler compiler;
    Compiler* enclosing = current;
    initCompiler(&compiler, TYPE_FUNCTION);
    beginScope();

    emitByte(OP_NEW_TABLE);
    Token listToken = {TOKEN_IDENTIFIER, "(list)", 6, parser.previous.line};
    addLocal(listToken);
    markInitialized();
    int listSlot = current->localCount - 1;

    emitConstant(NUMBER_VAL(1));
    Token idxToken = {TOKEN_IDENTIFIER, "(idx)", 5, parser.previous.line};
    addLocal(idxToken);
    markInitialized();
    int idxSlot = current->localCount - 1;

    consume(TOKEN_FOR, "Expect 'for' in table comprehension.");

    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    int hasIndexSigil = 0;
    if (check(TOKEN_HASH)) {
        const char* expected = name.start + name.length;
        if (parser.current.start == expected) {
            advance();
            hasIndexSigil = 1;
        } else {
            errorAtCurrent("Whitespace is not allowed before '#'.");
            advance();
            hasIndexSigil = 1;
        }
    }

    Token loopVars[2];
    int varCount = 1;
    loopVars[0] = name;

    if (match(TOKEN_COMMA)) {
        consume(TOKEN_IDENTIFIER, "Expect second variable name.");
        loopVars[varCount++] = parser.previous;
    }

    consume(TOKEN_IN, "Expect 'in'.");

    int exprCount = 0;
    do {
        expression();
        exprCount++;
    } while (match(TOKEN_COMMA) && exprCount < 3);

    if (exprCount > 1) {
        while (exprCount < 3) {
            emitByte(OP_NIL);
            exprCount++;
        }
    } else {
        if (!lastExprEndsWithCall) {
            if (hasIndexSigil) {
                emitByte(OP_ITER_PREP_IPAIRS);
            } else {
                emitByte(OP_ITER_PREP);
            }
        } else if (hasIndexSigil) {
            error("Index loop syntax 'i#' only works with implicit table iteration.");
        }
    }

    if (hasIndexSigil && exprCount > 1) {
        error("Index loop syntax 'i#' only works with implicit table iteration.");
    }

    if (varCount == 1 && !hasIndexSigil) {
        Token keyToken = {TOKEN_IDENTIFIER, "(key)", 5, parser.previous.line};
        loopVars[1] = loopVars[0];
        loopVars[0] = keyToken;
        varCount = 2;
    }

    Token iterToken = {TOKEN_IDENTIFIER, "(iter)", 6, parser.previous.line};
    Token stateToken = {TOKEN_IDENTIFIER, "(state)", 7, parser.previous.line};
    Token controlToken = {TOKEN_IDENTIFIER, "(control)", 9, parser.previous.line};

    int iterSlot = current->localCount;
    addLocal(iterToken);
    int stateSlot = current->localCount;
    addLocal(stateToken);
    int controlSlot = current->localCount;
    addLocal(controlToken);
    markInitializedCount(3);

    int loopStart = currentChunk()->count;

    emitBytes(OP_GET_LOCAL, (uint8_t)iterSlot);
    emitBytes(OP_GET_LOCAL, (uint8_t)stateSlot);
    emitBytes(OP_GET_LOCAL, (uint8_t)controlSlot);
    emitBytes(OP_CALL, 2);

    for (int i = varCount; i < 2; i++) {
        emitByte(OP_POP);
    }

    for (int i = 0; i < varCount; i++) {
        addLocal(loopVars[i]);
    }
    markInitializedCount(varCount);

    emitBytes(OP_GET_LOCAL, (uint8_t)(current->localCount - varCount));
    emitByte(OP_NIL);
    emitByte(OP_EQUAL);
    int exitJump = emitJump(OP_JUMP_IF_TRUE);
    emitByte(OP_POP);

    emitBytes(OP_GET_LOCAL, (uint8_t)(current->localCount - varCount));
    emitBytes(OP_SET_LOCAL, (uint8_t)(current->localCount - varCount - 1));
    emitByte(OP_POP);

    int hasIf = 0;
    int skipJump = -1;
    int endJump = -1;
    if (match(TOKEN_IF)) {
        hasIf = 1;
        expression();
        skipJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }

    const char* exprEnd = forStart;
    const char* assign = findComprehensionAssign(exprLexer, exprStart, exprEnd);
    if (assign != NULL) {
        emitBytes(OP_GET_LOCAL, (uint8_t)listSlot);
        compileExpressionFromString(exprStart, (size_t)(assign - exprStart));
        compileExpressionFromString(assign + 1, (size_t)(exprEnd - (assign + 1)));
        emitByte(OP_SET_TABLE);
        emitByte(OP_POP);
    } else {
        emitBytes(OP_GET_LOCAL, (uint8_t)listSlot);
        emitBytes(OP_GET_LOCAL, (uint8_t)idxSlot);
        compileExpressionFromString(exprStart, exprLen);
        emitByte(OP_SET_TABLE);
        emitByte(OP_POP);

        emitBytes(OP_GET_LOCAL, (uint8_t)idxSlot);
        emitConstant(NUMBER_VAL(1));
        emitByte(OP_ADD);
        emitBytes(OP_SET_LOCAL, (uint8_t)idxSlot);
        emitByte(OP_POP);
    }

    if (hasIf) {
        endJump = emitJump(OP_JUMP);
        patchJump(skipJump);
        emitByte(OP_POP);
        patchJump(endJump);
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after table comprehension.");

    for (int i = 0; i < varCount; i++) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        current->localCount--;
    }

    emitLoop(loopStart);

    patchJump(exitJump);
    for (int i = 0; i < varCount; i++) {
        emitByte(OP_POP);
    }
    emitByte(OP_POP);

    emitBytes(OP_GET_LOCAL, (uint8_t)listSlot);

    ObjFunction* function = endCompiler();
    current = enclosing;
    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));
    for (int i = 0; i < compiler.upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
    emitBytes(OP_CALL, 0);
    lastExprEndsWithCall = 1;
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, parseCall,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {table,    tableInfix, PREC_CALL},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACKET]  = {NULL,     subscript, PREC_CALL},
    [TOKEN_RIGHT_BRACKET] = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
    [TOKEN_DOT_DOT]       = {NULL,     range_, PREC_TERM},
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_EQUALS]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_HASH]          = {unary,    NULL,   PREC_NONE},
    [TOKEN_QUESTION]      = {NULL,     ternary, PREC_TERNARY},
    [TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_AT]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_POWER]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_INT_DIV]       = {NULL,     binary, PREC_FACTOR},
    [TOKEN_PERCENT]       = {NULL,     binary, PREC_FACTOR},
    [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_HAS]           = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
    [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
    [TOKEN_FSTRING]       = {fstring,  NULL,   PREC_NONE},
    [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
    [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOKEN_FN]            = {anonymousFunction, NULL, PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
    [TOKEN_NOT]           = {unary,    NULL,   PREC_NONE},
    [TOKEN_LOCAL]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_GLOBAL]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_WITH]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_AS]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_TRY]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EXCEPT]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FINALLY]       = {NULL,     NULL,   PREC_NONE},
    [TOKEN_THROW]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_GC]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_INDENT]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DEDENT]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_IMPORT]        = {importExpression, NULL, PREC_NONE},
    [TOKEN_FROM]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DEL]           = {NULL,     NULL,   PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }
    int canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);
    while (precedence <= getRule(parser.current.type)->precedence) {
        if (inTableEntryExpression &&
            parser.current.line > parser.previous.line &&
            isTableEntryStart(parser.current.type)) {
            break;
        }
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }
    if (canAssign && match(TOKEN_EQUALS)) {
        error("Invalid assignment target.");
    }
}

static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void expression() {
    lastExprEndsWithCall = 0;
    lastExprWasRange = 0;
    parsePrecedence(PREC_ASSIGNMENT);
}

static void variableDeclaration() {
    // Collect variable names
    uint8_t globals[256];
    int varCount = 0;

    do {
        globals[varCount] = parseVariable("Expect variable name.");
        varCount++;
        if (varCount > 255) {
            error("Too many variables in declaration.");
            return;
        }
    } while (match(TOKEN_COMMA));

    // Now handle initialization
    if (match(TOKEN_EQUALS)) {
        // Emit the RHS expression(s)
        int exprCount = 0;
        int startLine = parser.current.line;
        if (varCount == 1 && rhsHasTopLevelComma(startLine)) {
            parseArrayLiteralFromCommaList();
            exprCount = 1;
        } else {
            do {
                typeStackTop = 0;
                expression();
                exprCount++;
            } while (match(TOKEN_COMMA));
        }

        // Pad with nils for missing values
        // Note: if exprCount==1 and it's a function call returning multiple values,
        // those values will naturally fill the slots. If it returns fewer than varCount,
        // the remaining slots will be undefined (implementation limitation for now).
        if (exprCount > 1) {
            // Multiple expressions: pad any missing values with nil
            while (exprCount < varCount) {
                emitByte(OP_NIL);
                exprCount++;
            }
        } else if (varCount == 1) {
            // Single variable: no padding needed
        }
        // else: single expression, multiple variables - trust it returns enough values
    } else {
        // No initialization - set all to nil
        for (int i = 0; i < varCount; i++) {
            emitByte(OP_NIL);
        }
    }

    if (current->scopeDepth > 0) {
        markInitializedCount(varCount);
    }

    // Define variables in order (reverse for globals because of popping)
    for (int i = varCount - 1; i >= 0; i--) {
        defineVariable(globals[i]);
    }
}

static void printStatement() {
    typeStackTop = 0;
    expression();
    emitByte(OP_PRINT);
}

static void deleteVariable(Token name) {
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        emitByte(OP_NIL);
        emitBytes(OP_SET_LOCAL, (uint8_t)arg);
        emitByte(OP_POP);
        return;
    }
    arg = resolveUpvalue(current, &name);
    if (arg != -1) {
        emitByte(OP_NIL);
        emitBytes(OP_SET_UPVALUE, (uint8_t)arg);
        emitByte(OP_POP);
        return;
    }
    uint8_t global = identifierConstant(&name);
    emitBytes(OP_DELETE_GLOBAL, global);
}

static void deleteAccessChain(void) {
    int deleted = 0;
    for (;;) {
        if (match(TOKEN_DOT)) {
            consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
            uint8_t name = identifierConstant(&parser.previous);
            emitBytes(OP_CONSTANT, name);
        } else if (match(TOKEN_LEFT_BRACKET)) {
            expression();
            consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
        } else {
            if (!deleted) error("Expect property or index to delete.");
            return;
        }

        if (check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET)) {
            emitByte(OP_GET_TABLE);
        } else {
            emitByte(OP_DELETE_TABLE);
            deleted = 1;
            return;
        }
    }
}

static void delStatement() {
    do {
        if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            if (check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET)) {
                namedVariable(name, 0);
                deleteAccessChain();
            } else {
                deleteVariable(name);
            }
        } else if (match(TOKEN_LEFT_PAREN)) {
            expression();
            consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
            if (!(check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET))) {
                error("Expect property or index to delete.");
                return;
            }
            deleteAccessChain();
        } else {
            error("Expect variable or table access after 'del'.");
            return;
        }
    } while (match(TOKEN_COMMA));
}

static void expressionStatement() {
    typeStackTop = 0;
    expression();
    // In REPL mode, leave the last expression result on stack so it can be printed
    // In normal mode, pop the result
    if (!isREPLMode) {
        // For multi-return functions, we need to clean up all return values
        // Pop one value, then adjust stack to correct position
        emitByte(OP_POP);
        // Ensure stack matches our local count
        if (current->scopeDepth > 0) {
            emitBytes(OP_ADJUST_STACK, current->localCount);
        }
    }
}

static void block() {
    while (!check(TOKEN_ELSE) && !check(TOKEN_ELIF) && 
           !check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        declaration();
    }
}

static void block();
static void statement();
static void tryStatement();
static void throwStatement();
static void withStatement();
static void fromImportStatement(void);
static int isMultiAssignmentStatement(void);
static void multiAssignmentStatement(void);

static void assignNameFromStack(Token name, uint8_t rhsType) {
    uint8_t setOp;
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        setOp = OP_SET_LOCAL;
        emitBytes(setOp, (uint8_t)arg);
        updateLocalType(arg, rhsType);
        return;
    }

    arg = resolveUpvalue(current, &name);
    if (arg != -1) {
        setOp = OP_SET_UPVALUE;
        emitBytes(setOp, (uint8_t)arg);
        return;
    }

    arg = identifierConstant(&name);
    if (current->type == TYPE_SCRIPT) {
        emitByte(OP_DUP);
        emitBytes(OP_DEFINE_GLOBAL, (uint8_t)arg);
    } else {
        // Local-by-default, consistent with single-target assignment semantics.
        int localIndex = current->localCount;
        addLocal(name);
        markInitialized();
        emitBytes(OP_SET_LOCAL, (uint8_t)localIndex);
        setLocalType(localIndex, rhsType);
    }
}

static int isMultiAssignmentStatement(void) {
    if (!check(TOKEN_IDENTIFIER)) return 0;

    int startLine = parser.current.line;
    int targetCount = 1;
    Lexer peek = lexer;

    for (;;) {
        Token tok = scanToken(&peek);
        if (tok.line > startLine) return 0;

        if (tok.type == TOKEN_COMMA) {
            tok = scanToken(&peek);
            if (tok.line > startLine) return 0;
            if (tok.type != TOKEN_IDENTIFIER) return 0;
            targetCount++;
            continue;
        }

        return tok.type == TOKEN_EQUALS && targetCount > 1;
    }
}

static void multiAssignmentStatement(void) {
    Token targets[256];
    int targetCount = 0;

    do {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        targets[targetCount++] = parser.previous;
        if (targetCount > 255) {
            error("Too many variables in assignment.");
            return;
        }
    } while (match(TOKEN_COMMA));

    consume(TOKEN_EQUALS, "Expect '=' in assignment.");

    // Evaluate RHS expressions into a temporary table in positional order.
    emitByte(OP_NEW_TABLE);
    int exprIndex = 1;
    do {
        emitByte(OP_DUP);
        emitConstant(NUMBER_VAL(exprIndex++));
        typeStackTop = 0;
        expression();
        emitByte(OP_SET_TABLE);
        emitByte(OP_POP);
    } while (match(TOKEN_COMMA));

    // Assign each target from temp table index 1..N.
    typeStackTop = 0;
    for (int i = 0; i < targetCount; i++) {
        emitByte(OP_DUP);
        emitConstant(NUMBER_VAL(i + 1));
        emitByte(OP_GET_TABLE);
        assignNameFromStack(targets[i], TYPEHINT_ANY);
        if (!isREPLMode) {
            emitByte(OP_POP);
        }
    }

    // Pop temp table.
    emitByte(OP_POP);
}

static void ifStatement() {
    typeStackTop = 0;
    expression();
    int headerLine = parser.previous.line;
    
    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); 
    
    beginScope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > headerLine) {
            error("Expected indented block after 'if'.");
        }
        statement();
    }
    endScope();
    int elseJump = emitJump(OP_JUMP);
    
    patchJump(thenJump);
    emitByte(OP_POP); 
    
    if (match(TOKEN_ELIF)) {
        ifStatement();
    } else {
        if (match(TOKEN_ELSE)) {
            int elseLine = parser.previous.line;
            beginScope();
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > elseLine) {
                    error("Expected indented block after 'else'.");
                }
                statement();
            }
            endScope();
        }
    }
    
    patchJump(elseJump);
}

static void tryStatement() {
    uint8_t depth = (uint8_t)current->localCount;
    TryPatch handlerOffset = emitTry(depth);
    int headerLine = parser.previous.line;

    beginScope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > headerLine) {
            error("Expected indented block after 'try'.");
        }
        statement();
    }
    endScope();

    if (!check(TOKEN_EXCEPT) && !check(TOKEN_FINALLY)) {
        error("Expect 'except' or 'finally' after try block.");
        return;
    }

    emitByte(OP_END_TRY);

    int hasExcept = 0;
    int hasFinally = 0;
    int afterTryJump = -1;

    if (match(TOKEN_EXCEPT)) {
        hasExcept = 1;
        afterTryJump = emitJump(OP_JUMP);

        patchTry(handlerOffset.exceptOffset);

        beginScope();
        if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            addLocal(name);
            markInitialized();
            emitBytes(OP_SET_LOCAL, (uint8_t)(current->localCount - 1));
        } else {
            emitByte(OP_POP);
        }

        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            int exceptLine = parser.previous.line;
            if (parser.current.line > exceptLine) {
                error("Expected indented block after 'except'.");
            }
            statement();
        }
        endScope();
        emitByte(OP_END_TRY);
    }

    if (match(TOKEN_FINALLY)) {
        hasFinally = 1;
        if (hasExcept && afterTryJump != -1) {
            patchJump(afterTryJump);
        }

        patchTryFinally(handlerOffset.finallyOffset);

        beginScope();
        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            int finallyLine = parser.previous.line;
            if (parser.current.line > finallyLine) {
                error("Expected indented block after 'finally'.");
            }
            statement();
        }
        endScope();
        emitByte(OP_END_FINALLY);
    } else if (hasExcept && afterTryJump != -1) {
        patchJump(afterTryJump);
    }

    currentChunk()->code[handlerOffset.flagsOffset] =
        (uint8_t)((hasExcept ? 1 : 0) | (hasFinally ? 2 : 0));
}

static Token syntheticToken(const char* name) {
    Token token;
    token.start = name;
    token.length = (int)strlen(name);
    token.line = parser.previous.line;
    token.type = TOKEN_IDENTIFIER;
    return token;
}

static void withStatement() {
    beginScope();
    typeStackTop = 0;

    expression();

    Token ctxToken = syntheticToken("$with_ctx");
    int ctxSlot = current->localCount;
    addLocal(ctxToken);
    markInitialized();

    Token enterToken = syntheticToken("__enter");
    emitBytes(OP_GET_LOCAL, (uint8_t)ctxSlot);
    emitBytes(OP_CONSTANT, identifierConstant(&enterToken));
    emitByte(OP_GET_TABLE);
    int skipEnter = emitJump(OP_JUMP_IF_FALSE);
    emitBytes(OP_CALL, 0);
    int afterEnter = emitJump(OP_JUMP);
    patchJump(skipEnter);
    emitByte(OP_POP);
    emitBytes(OP_GET_LOCAL, (uint8_t)ctxSlot);
    patchJump(afterEnter);

    if (match(TOKEN_AS)) {
        consume(TOKEN_IDENTIFIER, "Expect name after 'as'.");
        Token name = parser.previous;
        int local = resolveLocal(current, &name);
        if (local != -1) {
            emitBytes(OP_SET_LOCAL, (uint8_t)local);
            emitByte(OP_POP);
        } else {
            int upvalue = resolveUpvalue(current, &name);
            if (upvalue != -1) {
                emitBytes(OP_SET_UPVALUE, (uint8_t)upvalue);
                emitByte(OP_POP);
            } else {
                addLocal(name);
                markInitialized();
            }
        }
    } else {
        emitByte(OP_POP);
    }

    Token exToken = syntheticToken("$with_ex");
    int exSlot = current->localCount;
    emitByte(OP_NIL);
    addLocal(exToken);
    markInitialized();

    uint8_t depth = (uint8_t)current->localCount;
    TryPatch handler = emitTry(depth);
    int headerLine = parser.previous.line;

    beginScope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > headerLine) {
            error("Expected indented block after 'with'.");
        }
        statement();
    }
    endScope();

    emitByte(OP_END_TRY);
    int afterTryJump = emitJump(OP_JUMP);

    patchTry(handler.exceptOffset);
    emitBytes(OP_SET_LOCAL, (uint8_t)exSlot);
    emitBytes(OP_GET_LOCAL, (uint8_t)exSlot);
    emitByte(OP_THROW);

    patchJump(afterTryJump);
    patchTryFinally(handler.finallyOffset);

    Token exitToken = syntheticToken("__exit");
    emitBytes(OP_GET_LOCAL, (uint8_t)ctxSlot);
    emitBytes(OP_CONSTANT, identifierConstant(&exitToken));
    emitByte(OP_GET_TABLE);
    int skipExit = emitJump(OP_JUMP_IF_FALSE);
    emitBytes(OP_GET_LOCAL, (uint8_t)exSlot);
    emitBytes(OP_CALL, 1);
    emitByte(OP_POP);
    int afterExit = emitJump(OP_JUMP);
    patchJump(skipExit);
    emitByte(OP_POP);
    patchJump(afterExit);

    emitByte(OP_END_FINALLY);
    currentChunk()->code[handler.flagsOffset] = (uint8_t)(1 | 2);
    endScope();
}

static void whileStatement() {
    LoopContext loop;
    loop.start = currentChunk()->count;
    loop.scopeDepth = current->scopeDepth;
    loop.breakCount = 0;
    loop.continueCount = 0;
    loop.isForLoop = 0;
    loop.enclosing = current->loopContext;
    current->loopContext = &loop;

    typeStackTop = 0;
    expression();
    int headerLine = parser.previous.line;

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    beginScope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > headerLine) {
            error("Expected indented block after 'while'.");
        }
        statement();
    }
    endScope();

    emitLoop(loop.start);

    patchJump(exitJump);
    emitByte(OP_POP); // Pop condition

    // Patch all break jumps
    for (int i = 0; i < loop.breakCount; i++) {
        patchJump(loop.breakJumps[i]);
    }

    current->loopContext = loop.enclosing;
}

static void returnStatement() {
    if (current->type == TYPE_SCRIPT) {
        // Optional: allow return from script (exit)
        // error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON)) {
        emitReturn();
    } else {
        if (check(TOKEN_ELSE) || check(TOKEN_ELIF) || check(TOKEN_EOF)) {
             emitByte(OP_NIL);
             emitByte(OP_RETURN);
        } else {
            // Count return expressions
            int valueCount = 0;
            do {
                typeStackTop = 0;
                expression();
                valueCount++;
            } while (match(TOKEN_COMMA));

            if (valueCount == 1) {
                emitByte(OP_RETURN);
            } else {
                emitBytes(OP_RETURN_N, valueCount);
            }
        }
    }
}

static void forStatement() {
    LoopContext loop;
    loop.breakCount = 0;
    loop.continueCount = 0;
    loop.isForLoop = 1;
    loop.slotsToPop = 0;

    beginScope();
    loop.scopeDepth = current->scopeDepth;
    loop.enclosing = current->loopContext;

    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    int hasIndexSigil = 0;
    if (check(TOKEN_HASH)) {
        const char* expected = name.start + name.length;
        if (parser.current.start == expected) {
            advance();
            hasIndexSigil = 1;
        } else {
            errorAtCurrent("Whitespace is not allowed before '#'.");
            advance();
            hasIndexSigil = 1;
        }
    }

    // Check if this is a for-in loop or numeric for loop
    if (match(TOKEN_COMMA) || check(TOKEN_IN)) {
        // For-in loop: for k, v in ... or for k in ...
        // We already consumed the first identifier, need to handle it
        Token loopVars[2];
        int varCount = 1;
        loopVars[0] = name;

        if (parser.previous.type == TOKEN_COMMA) {
            consume(TOKEN_IDENTIFIER, "Expect second variable name.");
            loopVars[varCount++] = parser.previous;
        }

        consume(TOKEN_IN, "Expect 'in'.");

        // Parse iterator expression(s)
        // The iterator protocol expects 3 values: function, state, control
        // If one expression: assume it returns all 3 (e.g., custom iterator) - don't pad!
        // If two expressions: pad with one nil
        // If three expressions: use as-is
        int exprCount = 0;
        int singleExprIsCall = 0;
        int isRangeExpr = 0;
        int eligibleForRange = (varCount == 1 && !hasIndexSigil);
        // First expression
        inForRangeHeader = eligibleForRange;
        typeStackTop = 0;
        expression();
        inForRangeHeader = 0;
        exprCount = 1;
        singleExprIsCall = lastExprEndsWithCall;
        isRangeExpr = eligibleForRange && lastExprWasRange;

        if (isRangeExpr && check(TOKEN_COMMA)) {
            error("Range expression cannot be used with multiple iterator expressions.");
            current->loopContext = loop.enclosing;
            endScope();
            return;
        }

        while (match(TOKEN_COMMA) && exprCount < 3) {
            typeStackTop = 0;
            expression();
            exprCount++;
        }
        
        int headerLine = parser.previous.line;

        if (isRangeExpr && exprCount == 1) {
            // Stack has: start, end (end is on top)
            addLocal(name); // loop variable uses start (lower stack slot)
            Token endToken = {TOKEN_IDENTIFIER, "(end)", 5, parser.previous.line};
            addLocal(endToken); // end uses top of stack
            markInitializedCount(2);
            int varSlot = current->localCount - 2;
            int endSlot = current->localCount - 1;

            int loopStart = currentChunk()->count;
            loop.start = loopStart;
            current->loopContext = &loop;
            loop.slotsToPop = 0;

            // for-prep: jump out if start > end
            emitByte(OP_FOR_PREP);
            emitByte((uint8_t)varSlot);
            emitByte((uint8_t)endSlot);
            emitByte(0);
            emitByte(0);
            int exitJump = currentChunk()->count - 2;

            // Body
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > headerLine) {
                    error("Expected indented block after 'for'.");
                }
                statement();
            }

            int loopInstrOffset = currentChunk()->count;
            for (int i = 0; i < loop.continueCount; i++) {
                int jumpOffset = loop.continueJumps[i];
                int jump = loopInstrOffset - (jumpOffset + 2);
                currentChunk()->code[jumpOffset] = (uint8_t)((jump >> 8) & 0xff);
                currentChunk()->code[jumpOffset + 1] = (uint8_t)(jump & 0xff);
            }

            // for-loop: increment and jump back if <= end
            emitByte(OP_FOR_LOOP);
            emitByte((uint8_t)varSlot);
            emitByte((uint8_t)endSlot);
            emitByte(0);
            emitByte(0);
            // patch backward jump
            {
                int loopOffset = currentChunk()->count - loopStart;
                currentChunk()->code[currentChunk()->count - 2] = (uint8_t)((loopOffset >> 8) & 0xff);
                currentChunk()->code[currentChunk()->count - 1] = (uint8_t)(loopOffset & 0xff);
            }

            // patch exit jump
            {
                int jump = currentChunk()->count - (exitJump + 2);
                currentChunk()->code[exitJump] = (uint8_t)((jump >> 8) & 0xff);
                currentChunk()->code[exitJump + 1] = (uint8_t)(jump & 0xff);
            }

            for (int i = 0; i < loop.breakCount; i++) {
                patchJump(loop.breakJumps[i]);
            }

            current->loopContext = loop.enclosing;
            endScope();
            return;
        }

        // Only pad if we have 2 or 3 expressions
        // If exprCount == 1, assume the expression returns all 3 values
        if (exprCount > 1) {
            while (exprCount < 3) {
                emitByte(OP_NIL);
                exprCount++;
            }
        } else {
            // Single expression: might be a table that needs implicit iteration.
            // Avoid prepping function calls (ending in ')') which likely return triplet.
            if (!singleExprIsCall) {
                if (hasIndexSigil) {
                    emitByte(OP_ITER_PREP_IPAIRS);
                } else {
                    emitByte(OP_ITER_PREP);
                }
            } else if (hasIndexSigil) {
                error("Index loop syntax 'i#' only works with implicit table iteration.");
            }
        }

        if (hasIndexSigil && exprCount > 1) {
            error("Index loop syntax 'i#' only works with implicit table iteration.");
        }

        if (varCount == 1 && !hasIndexSigil) {
            Token keyToken = {TOKEN_IDENTIFIER, "(key)", 5, parser.previous.line};
            loopVars[1] = loopVars[0];
            loopVars[0] = keyToken;
            varCount = 2;
        }

        // Create hidden locals in the order values appear on stack
        // Iterator prep yields: iterator_function, state, control
        Token iterToken = {TOKEN_IDENTIFIER, "(iter)", 6, parser.previous.line};
        Token stateToken = {TOKEN_IDENTIFIER, "(state)", 7, parser.previous.line};
        Token controlToken = {TOKEN_IDENTIFIER, "(control)", 9, parser.previous.line};

                // Save the indices for hidden locals (before adding them)

                int iterSlot = current->localCount;

        addLocal(iterToken);    // First value: iterator function

        int stateSlot = current->localCount;

        addLocal(stateToken);   // Second value: state

        int controlSlot = current->localCount;

        addLocal(controlToken); // Third value: control variable

        markInitializedCount(3);
        

                // Loop start

        
        int loopStart = currentChunk()->count;
        loop.start = loopStart;
        current->loopContext = &loop;
        loop.slotsToPop = varCount;

        // Call iterator: iter(state, control)
        emitBytes(OP_GET_LOCAL, (uint8_t)iterSlot);
        emitBytes(OP_GET_LOCAL, (uint8_t)stateSlot);
        emitBytes(OP_GET_LOCAL, (uint8_t)controlSlot);
        emitBytes(OP_CALL, 2);

        // Iterators return 2 values (key, value). If we have fewer loop variables,
        // pop the extra values to keep the stack balanced.
        for (int i = varCount; i < 2; i++) {
            emitByte(OP_POP);
        }

        // Create loop variables
        for (int i = 0; i < varCount; i++) {
            addLocal(loopVars[i]);
        }
        markInitializedCount(varCount);

        // Check if first value is nil
        emitBytes(OP_GET_LOCAL, (uint8_t)(current->localCount - varCount));
        emitByte(OP_NIL);
        emitByte(OP_EQUAL);
        int exitJump = emitJump(OP_JUMP_IF_TRUE);
        emitByte(OP_POP);

        // Update control variable to first return value
        emitBytes(OP_GET_LOCAL, (uint8_t)(current->localCount - varCount));
        emitBytes(OP_SET_LOCAL, (uint8_t)(current->localCount - varCount - 1)); // control is just before loop vars
        emitByte(OP_POP);

        // Body
        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            if (parser.current.line > headerLine) {
                error("Expected indented block after 'for'.");
            }
            statement();
        }

        // Pop loop variables
        for (int i = 0; i < varCount; i++) {
            if (current->locals[current->localCount - 1].isCaptured) {
                emitByte(OP_CLOSE_UPVALUE);
            } else {
                emitByte(OP_POP);
            }
            current->localCount--;
        }

        // Patch continue jumps
        for (int i = 0; i < loop.continueCount; i++) {
            patchJump(loop.continueJumps[i]);
        }

        emitLoop(loopStart);

        patchJump(exitJump);
        // Pop loop variables that weren't popped because we jumped here
        for (int i = 0; i < varCount; i++) {
            emitByte(OP_POP);
        }
        emitByte(OP_POP);  // Pop comparison result

        // Patch break jumps
        for (int i = 0; i < loop.breakCount; i++) {
            patchJump(loop.breakJumps[i]);
        }

        current->loopContext = loop.enclosing;
        endScope();
        return;
    }

    error("Expect 'in' after loop variable.");
    return;
}

static void breakStatement() {
    if (current->loopContext == NULL) {
        error("Can't use 'break' outside a loop.");
        return;
    }

    // Pop locals back to loop scope
    for (int i = current->localCount - 1; i >= 0 &&
         current->locals[i].depth > current->loopContext->scopeDepth; i--) {
        if (current->locals[i].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
    }

    // Emit jump and save for patching
    int offset = emitJump(OP_JUMP);
    current->loopContext->breakJumps[current->loopContext->breakCount++] = offset;
}

static void continueStatement() {
    if (current->loopContext == NULL) {
        error("Can't use 'continue' outside a loop.");
        return;
    }

    // Pop locals back to loop scope
    for (int i = current->localCount - 1; i >= 0 &&
         current->locals[i].depth > current->loopContext->scopeDepth; i--) {
        if (current->locals[i].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
    }

    // Pop extra slots if needed (e.g. for-in loop variables)
    for (int i = 0; i < current->loopContext->slotsToPop; i++) {
        emitByte(OP_POP);
    }

    if (current->loopContext->isForLoop) {
        // For loops: emit forward jump to be patched later
        int offset = emitJump(OP_JUMP);
        current->loopContext->continueJumps[current->loopContext->continueCount++] = offset;
    } else {
        // While loops: jump back to start
        emitLoop(current->loopContext->start);
    }
}

static void throwStatement() {
    typeStackTop = 0;
    expression();
    emitByte(OP_THROW);
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_TRY)) {
        tryStatement();
    } else if (match(TOKEN_WITH)) {
        withStatement();
    } else if (match(TOKEN_THROW)) {
        throwStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_BREAK)) {
        breakStatement();
    } else if (match(TOKEN_CONTINUE)) {
        continueStatement();
    } else if (match(TOKEN_GC)) {
        emitByte(OP_GC);
    } else if (match(TOKEN_DEL)) {
        delStatement();
    } else if (isMultiAssignmentStatement()) {
        multiAssignmentStatement();
    } else {
        expressionStatement();
    }
}

static void functionBody(FunctionType type) {
    
    Compiler compiler;
    Compiler* enclosing = current; 
    initCompiler(&compiler, type);
    beginScope(); 

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    int defaultsStart = current->function->defaultsCount;
    int paramIndex = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            // Check for variadic parameter (*name)
            if (match(TOKEN_STAR)) {
                current->function->isVariadic = 1;
                current->function->arity++;  // The varargs table counts as one parameter

                // Parse the varargs parameter name
                uint8_t constant = parseVariable("Expect parameter name after '*'.");
                Token paramNameToken = parser.previous;
                if (paramIndex == 0 && parser.previous.length == 4 &&
                    memcmp(parser.previous.start, "self", 4) == 0) {
                    current->function->isSelf = 1;
                }
                paramIndex++;
                if (match(TOKEN_COLON)) {
                    consume(TOKEN_IDENTIFIER, "Expect type name after ':'.");
                    uint8_t type = parseTypeName(&parser.previous);
                    setLocalType(current->localCount - 1, type);
                    setParamType(current->function, current->function->arity - 1, type);
                }
                setParamName(current->function, current->function->arity - 1, &paramNameToken);
                defineVariable(constant);
                break;  // *args must be the last parameter
            }

            current->function->arity++;
#ifdef DEBUG_COMPILER
            printf("Parsing param, arity: %d\n", current->function->arity);
#endif
            if (current->function->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            uint8_t constant = parseVariable("Expect parameter name.");
            Token paramNameToken = parser.previous;
            if (paramIndex == 0 && parser.previous.length == 4 &&
                memcmp(parser.previous.start, "self", 4) == 0) {
                current->function->isSelf = 1;
            }
            paramIndex++;
            if (match(TOKEN_COLON)) {
                consume(TOKEN_IDENTIFIER, "Expect type name after ':'.");
                uint8_t type = parseTypeName(&parser.previous);
                setLocalType(current->localCount - 1, type);
                setParamType(current->function, current->function->arity - 1, type);
            }
            setParamName(current->function, current->function->arity - 1, &paramNameToken);
            
            if (match(TOKEN_EQUALS)) {
                if (match(TOKEN_NUMBER)) {
                    double num = parseNumberToken(parser.previous);
                    current->function->defaultsCount++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaultsCount);
                    current->function->defaults[current->function->defaultsCount - 1] = NUMBER_VAL(num);
                } else if (match(TOKEN_STRING)) {
                    ObjString* str = copyString(parser.previous.start + 1, parser.previous.length - 2);
                    current->function->defaultsCount++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaultsCount);
                    current->function->defaults[current->function->defaultsCount - 1] = OBJ_VAL(str);
                } else if (match(TOKEN_NIL)) {
                    current->function->defaultsCount++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaultsCount);
                    current->function->defaults[current->function->defaultsCount - 1] = NIL_VAL;
                } else if (match(TOKEN_TRUE)) {
                    current->function->defaultsCount++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaultsCount);
                    current->function->defaults[current->function->defaultsCount - 1] = NUMBER_VAL(1);
                } else if (match(TOKEN_FALSE)) {
                    current->function->defaultsCount++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaultsCount);
                    current->function->defaults[current->function->defaultsCount - 1] = NIL_VAL;
                } else {
                    error("Default value must be a constant (number, string, nil, true, false).");
                }
            } else if (defaultsStart < current->function->defaultsCount) {
                error("Parameters with defaults cannot be followed by parameters without defaults.");
            }
            
            defineVariable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    // Parameters are initialized at function entry.
    for (int i = 0; i < current->localCount; i++) {
        if (current->locals[i].depth == -1) {
            current->locals[i].depth = current->scopeDepth;
        }
    }
    int headerLine = parser.previous.line;
    
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > headerLine && !inTableEntryExpression) {
            error("Expected indented block for function body.");
        }
        if (parser.current.line > headerLine && inTableEntryExpression) {
            int headerIndent = tokenIndent(parser.previous);
            int bodyIndent = tokenIndent(parser.current);
            if (bodyIndent <= headerIndent) {
                error("Expected indented block for function body.");
            } else {
                while (!check(TOKEN_EOF) &&
                       !check(TOKEN_RIGHT_BRACE) &&
                       parser.current.line > headerLine &&
                       tokenIndent(parser.current) > headerIndent) {
                    statement();
                }
            }
        } else {
            statement();
        }
    }
    
    ObjFunction* function = endCompiler();
    current = enclosing;
    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

    // Emit upvalue information
    for (int i = 0; i < compiler.upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
}

typedef struct {
    const char* start;
    size_t length;
} DecoratorSpan;

static Token functionDeclarationNamed(void) {
    uint8_t global = parseVariable("Expect function name.");
    Token name = parser.previous;
    if (current->scopeDepth > 0) {
        markInitialized();
    }
    functionBody(TYPE_FUNCTION);
    defineVariable(global);
    return name;
}

static void functionDeclaration() {
    (void)functionDeclarationNamed();
}

static void anonymousFunction(int canAssign) {
    (void)canAssign;
    functionBody(TYPE_FUNCTION);
    typePush(TYPEHINT_ANY);
}

static void globalDeclaration(void) {
    uint8_t globals[256];
    int varCount = 0;

    do {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        globals[varCount] = identifierConstant(&parser.previous);
        varCount++;
        if (varCount > 255) {
            error("Too many variables in declaration.");
            return;
        }
    } while (match(TOKEN_COMMA));

    if (match(TOKEN_EQUALS)) {
        int exprCount = 0;
        int startLine = parser.current.line;
        if (varCount == 1 && rhsHasTopLevelComma(startLine)) {
            parseArrayLiteralFromCommaList();
            exprCount = 1;
        } else {
            do {
                typeStackTop = 0;
                expression();
                exprCount++;
            } while (match(TOKEN_COMMA));
        }

        if (exprCount > 1) {
            while (exprCount < varCount) {
                emitByte(OP_NIL);
                exprCount++;
            }
        }
    } else {
        for (int i = 0; i < varCount; i++) {
            emitByte(OP_NIL);
        }
    }

    for (int i = varCount - 1; i >= 0; i--) {
        emitBytes(OP_DEFINE_GLOBAL, globals[i]);
    }
}

static Token globalFunctionDeclarationNamed(void) {
    consume(TOKEN_IDENTIFIER, "Expect function name.");
    Token name = parser.previous;
    uint8_t global = identifierConstant(&parser.previous);
    functionBody(TYPE_FUNCTION);
    emitBytes(OP_DEFINE_GLOBAL, global);
    return name;
}

static void globalFunctionDeclaration(void) {
    (void)globalFunctionDeclarationNamed();
}

static void applyDecorators(Token functionName, DecoratorSpan* decorators, int decoratorCount) {
    for (int i = decoratorCount - 1; i >= 0; i--) {
        compileExpressionFromString(decorators[i].start, decorators[i].length);
        emitGetNamed(functionName);
        emitBytes(OP_CALL, 1);
        emitSetNamed(functionName);
        emitByte(OP_POP);
    }
}

static void decoratedFunctionDeclaration(void) {
    DecoratorSpan decorators[64];
    int decoratorCount = 0;

    for (;;) {
        if (parser.current.line != parser.previous.line) {
            error("Expect decorator expression after '@'.");
            return;
        }
        if (parser.current.type == TOKEN_EOF) {
            error("Expect decorator expression after '@'.");
            return;
        }

        const char* start = parser.current.start;
        const char* end = start;
        int line = parser.previous.line;
        while (parser.current.type != TOKEN_EOF && parser.current.line == line) {
            end = parser.current.start + parser.current.length;
            advance();
        }

        if (decoratorCount == 64) {
            error("Too many decorators on function.");
            return;
        }
        decorators[decoratorCount].start = start;
        decorators[decoratorCount].length = (size_t)(end - start);
        decoratorCount++;

        if (!match(TOKEN_AT)) break;
    }

    Token functionName;
    if (match(TOKEN_FN)) {
        functionName = functionDeclarationNamed();
    } else if (match(TOKEN_LOCAL)) {
        consume(TOKEN_FN, "Expect 'fn' after 'local' in decorated declaration.");
        functionName = functionDeclarationNamed();
    } else if (match(TOKEN_GLOBAL)) {
        consume(TOKEN_FN, "Expect 'fn' after 'global' in decorated declaration.");
        functionName = globalFunctionDeclarationNamed();
    } else {
        error("Decorators can only be applied to function declarations.");
        return;
    }

    applyDecorators(functionName, decorators, decoratorCount);
}

static void parseCall(int canAssign) {
    (void)canAssign;
    uint8_t argCount = 0;
    int inNamedArgs = 0;
    int hasSpreadArg = 0;
    int baseTop = typeStackTop;
    
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (match(TOKEN_STAR)) {
                if (inNamedArgs) {
                    error("Spread argument cannot be used with named arguments.");
                }
                if (hasSpreadArg) {
                    error("Can't use more than one spread argument.");
                }
                if (argCount == 255) {
                    error("Can't have more than 255 arguments.");
                }
                expression();
                typePop();
                hasSpreadArg = 1;
                if (check(TOKEN_COMMA)) {
                    error("Spread argument must be last.");
                }
                continue;
            }

            // Check if current argument is named: identifier = ...
            int isNamed = 0;
            if (parser.current.type == TOKEN_IDENTIFIER) {
                Lexer peekLexer = lexer;
                Token next = scanToken(&peekLexer);
                if (next.type == TOKEN_EQUALS) {
                    isNamed = 1;
                }
            }

            if (isNamed) {
                if (hasSpreadArg) {
                    error("Named arguments cannot follow spread argument.");
                }
                if (!inNamedArgs) {
                    emitByte(OP_NEW_TABLE); // Start the options table
                    inNamedArgs = 1;
                }
                
                // Parse named arg: name = expr
                consume(TOKEN_IDENTIFIER, "Expect parameter name.");
                Token name = parser.previous;
                consume(TOKEN_EQUALS, "Expect '=' after parameter name.");
                
                emitByte(OP_DUP); // Duplicate table for set operation
                emitConstant(OBJ_VAL(copyString(name.start, name.length))); // Key
                expression(); // Value
                typePop();
                emitByte(OP_SET_TABLE);
                emitByte(OP_POP); // Pop the value pushed by SET_TABLE
                
            } else {
                if (inNamedArgs) {
                    error("Positional arguments cannot follow named arguments.");
                }
                if (hasSpreadArg) {
                    error("Positional arguments cannot follow spread argument.");
                }
                expression();
                typePop();
                if (argCount == 255) {
                    error("Can't have more than 255 arguments.");
                }
                argCount++;
            }
        } while (match(TOKEN_COMMA));
    }
    
    // If we collected named args, the table is on the stack as the last argument
    if (inNamedArgs) {
        if (argCount == 255) {
             error("Can't have more than 255 arguments.");
        }
        argCount++;
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    if (hasSpreadArg) {
        emitBytes(OP_CALL_EXPAND, argCount);
    } else {
        emitBytes(OP_CALL, argCount);
    }
    lastExprEndsWithCall = 1;
    typeStackTop = baseTop;
    typePop();
    typePush(TYPEHINT_ANY);
}

static void importStatement() {
    // Parse: import module_name[.submodule...]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'import'.");

    // Build the full dotted module path
    char modulePath[256];
    int len = 0;
    Token lastComponent = parser.previous;  // Track last component for variable name

    // Copy first identifier
    int firstLen = parser.previous.length;
    if (firstLen >= 256) firstLen = 255;
    memcpy(modulePath, parser.previous.start, firstLen);
    len = firstLen;

    // Parse additional .submodule components
    while (match(TOKEN_DOT)) {
        if (len < 255) modulePath[len++] = '.';
        consume(TOKEN_IDENTIFIER, "Expect module name after '.'.");
        lastComponent = parser.previous;  // Update to last component
        int partLen = parser.previous.length;
        if (len + partLen >= 256) partLen = 255 - len;
        if (partLen > 0) {
            memcpy(modulePath + len, parser.previous.start, partLen);
            len += partLen;
        }
    }
    modulePath[len] = '\0';

    // Use the last component as the variable name
    parser.previous = lastComponent;

    // Declare the local variable using last component
    declareVariable();

    // Create a string constant with the full path
    ObjString* pathString = copyString(modulePath, len);
    uint8_t pathConstant = makeConstant(OBJ_VAL(pathString));

    // Emit the import opcode with full path
    emitBytes(OP_IMPORT, pathConstant);

    // Define the variable.
    if (current->scopeDepth > 0) {
        markInitialized();
    } else {
        uint8_t varName = identifierConstant(&lastComponent);
        emitBytes(OP_DEFINE_GLOBAL, varName);
    }
}

static void fromImportStatement(void) {
    // Parse: from module_name[.submodule...] import name[, name...]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'from'.");

    char modulePath[256];
    int len = 0;

    int firstLen = parser.previous.length;
    if (firstLen >= 256) firstLen = 255;
    memcpy(modulePath, parser.previous.start, firstLen);
    len = firstLen;

    while (match(TOKEN_DOT)) {
        if (len < 255) modulePath[len++] = '.';
        consume(TOKEN_IDENTIFIER, "Expect module name after '.'.");
        int partLen = parser.previous.length;
        if (len + partLen >= 256) partLen = 255 - len;
        if (partLen > 0) {
            memcpy(modulePath + len, parser.previous.start, partLen);
            len += partLen;
        }
    }
    modulePath[len] = '\0';

    consume(TOKEN_IMPORT, "Expect 'import' after module path.");

    // Prepare module path constant for repeated imports.
    ObjString* pathString = copyString(modulePath, len);
    uint8_t pathConstant = makeConstant(OBJ_VAL(pathString));
    if (match(TOKEN_STAR)) {
        emitBytes(OP_IMPORT, pathConstant);
        emitByte(OP_IMPORT_STAR);
        return;
    }

    do {
        consume(TOKEN_IDENTIFIER, "Expect imported name.");
        Token importedName = parser.previous;

        emitBytes(OP_IMPORT, pathConstant);

        // module[name]
        emitConstant(OBJ_VAL(copyString(importedName.start, importedName.length)));
        emitByte(OP_GET_TABLE);

        // Define imported symbol in current scope.
        parser.previous = importedName;
        if (current->scopeDepth > 0) {
            declareVariable();
            markInitialized();
        } else {
            uint8_t varName = identifierConstant(&importedName);
            emitBytes(OP_DEFINE_GLOBAL, varName);
        }
    } while (match(TOKEN_COMMA));
}

static void declaration() {
    if (match(TOKEN_AT)) {
        decoratedFunctionDeclaration();
    } else if (match(TOKEN_FN)) {
        functionDeclaration();
    } else if (match(TOKEN_IMPORT)) {
        importStatement();
    } else if (match(TOKEN_FROM)) {
        fromImportStatement();
    } else if (match(TOKEN_GLOBAL)) {
        if (match(TOKEN_FN)) {
            globalFunctionDeclaration();
        } else {
            globalDeclaration();
        }
    } else if (match(TOKEN_LOCAL)) {
        // Check if this is "local fn name()" syntax
        if (match(TOKEN_FN)) {
            functionDeclaration();
        } else {
            variableDeclaration();
        }
    } else {
        statement();
    }
}

ObjFunction* compile(const char* source) {
    // CRITICAL: Reset current to NULL before starting a new compilation
    // Otherwise initCompiler will set compiler->enclosing to a dangling pointer
    // from the previous compilation
    current = NULL;
    isREPLMode = 0;  // Normal compilation mode

    initLexer(&lexer, source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);

    // Reset parser state completely to avoid stale pointers from previous compilation
    parser.hadError = 0;
    parser.panicMode = 0;
    parser.current.type = TOKEN_ERROR;
    parser.current.start = source;
    parser.current.length = 0;
    parser.current.line = 1;
    parser.previous.type = TOKEN_ERROR;
    parser.previous.start = source;
    parser.previous.length = 0;
    parser.previous.line = 1;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = endCompiler();
    if (!parser.hadError && function != NULL) {
        optimizeChunk(&function->chunk);
    }
    return parser.hadError ? NULL : function;
}

ObjFunction* compileREPL(const char* source) {
    // CRITICAL: Reset current to NULL before starting a new compilation
    // Otherwise initCompiler will set compiler->enclosing to a dangling pointer
    // from the previous compilation
    current = NULL;
    isREPLMode = 1;  // REPL mode - leave expression results on stack

    initLexer(&lexer, source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);

    // Reset parser state completely to avoid stale pointers from previous compilation
    parser.hadError = 0;
    parser.panicMode = 0;
    parser.current.type = TOKEN_ERROR;
    parser.current.start = source;
    parser.current.length = 0;
    parser.current.line = 1;
    parser.previous.type = TOKEN_ERROR;
    parser.previous.start = source;
    parser.previous.length = 0;
    parser.previous.line = 1;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = endCompiler();
    if (!parser.hadError && function != NULL) {
        optimizeChunk(&function->chunk);
    }
    isREPLMode = 0;  // Reset flag after compilation
    return parser.hadError ? NULL : function;
}
