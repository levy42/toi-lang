#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/internal.h"
#include "opt.h"
#include "compiler/fstring.h"
#include "compiler/stmt.h"
#include "compiler/stmt_control.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef void (*ParseFn)(int canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

Parser parser;
Compiler* current = NULL;
Lexer lexer;
int isREPLMode = 0;  // If 1, don't pop expression results
int lastExprEndsWithCall = 0;
int lastExprWasRange = 0;
int inForRangeHeader = 0;
int inTableEntryExpression = 0;
static uint8_t typeStack[512];
int typeStackTop = 0;

void typePush(uint8_t type) {
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

Chunk* currentChunk(void) {
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

void error(const char* message) {
    errorAt(&parser.previous, message);
}

void errorAtCurrent(const char* message) {
    errorAt(&parser.current, message);
}

void advance(void) {
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



void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

int match(TokenType type) {
    if (parser.current.type == type) {
        advance();
        return 1;
    }
    return 0;
}

int check(TokenType type) {
    return parser.current.type == type;
}

void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->count - 2;
}

TryPatch emitTry(uint8_t depth) {
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

void patchJump(int offset) {
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

void patchTry(int offset) {
    int jump = currentChunk()->count - offset - 4;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

void patchTryFinally(int offset) {
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

void emitReturn(void) {
    emitByte(OP_RETURN);
}

uint8_t makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > 255) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

void emitConstant(Value value) {
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

void beginScope(void) {
    current->scopeDepth++;
}

void endScope(void) {
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

void expression(void);
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
void expression(void);
int emitSimpleFstringExpr(const char* exprStart, int exprLen);

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
        case TOKEN_APPEND:
            emitByte(OP_APPEND);
            outType = TYPEHINT_ANY;
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

int resolveLocal(Compiler* compiler, Token* name) {
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

int resolveUpvalue(Compiler* compiler, Token* name) {
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

void addLocal(Token name) {
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

void markInitialized(void) {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

void markInitializedCount(int count) {
    if (current->scopeDepth == 0) return;
    for (int i = 0; i < count; i++) {
        current->locals[current->localCount - 1 - i].depth = current->scopeDepth;
    }
}

void declareVariable(void) {
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

uint8_t identifierConstant(Token* name) {
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

void setLocalType(int localIndex, uint8_t type) {
    if (localIndex < 0 || localIndex >= current->localCount) return;
    current->locals[localIndex].type = type;
}

void updateLocalType(int localIndex, uint8_t rhsType) {
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

void namedVariable(Token name, int canAssign) {
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

static const char* skipSpaceSlice(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int isIdentStartChar(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int isIdentChar(char c) {
    return isIdentStartChar(c) || (c >= '0' && c <= '9');
}

static int parseIntSlice(const char* p, const char* end, double* out) {
    int sawDigit = 0;
    double value = 0.0;
    while (p < end) {
        char c = *p;
        if (c == '_') {
            p++;
            continue;
        }
        if (c < '0' || c > '9') break;
        sawDigit = 1;
        value = value * 10.0 + (double)(c - '0');
        p++;
    }
    if (!sawDigit) return 0;
    p = skipSpaceSlice(p, end);
    if (p != end) return 0;
    *out = value;
    return 1;
}

int emitSimpleFstringExpr(const char* exprStart, int exprLen) {
    const char* p = exprStart;
    const char* end = exprStart + exprLen;
    p = skipSpaceSlice(p, end);
    if (p >= end || !isIdentStartChar(*p)) return 0;

    const char* nameStart = p;
    p++;
    while (p < end && isIdentChar(*p)) p++;
    Token name = {TOKEN_IDENTIFIER, nameStart, (int)(p - nameStart), parser.previous.line};

    p = skipSpaceSlice(p, end);
    if (p == end) {
        emitGetNamed(name);
        typePush(TYPEHINT_ANY);
        return 1;
    }

    if (*p != '%') return 0;
    p++;
    p = skipSpaceSlice(p, end);

    double rhs = 0.0;
    if (!parseIntSlice(p, end, &rhs)) return 0;

    emitGetNamed(name);
    emitConstant(NUMBER_VAL(rhs));
    emitByte(OP_IMOD);
    typePush(TYPEHINT_INT);
    return 1;
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

void consumePropertyNameAfterDot(void) {
    if (check(TOKEN_IDENTIFIER) || check(TOKEN_YIELD)) {
        advance();
        return;
    }
    errorAtCurrent("Expect property name after '.'.");
}

static void dot(int canAssign) {
    lastExprEndsWithCall = 0;
    int baseTop = typeStackTop - 1;
    consumePropertyNameAfterDot();
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
    [TOKEN_APPEND]        = {NULL,     binary, PREC_TERM},
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
    [TOKEN_YIELD]         = {NULL,     NULL,   PREC_NONE},
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

void expression(void) {
    lastExprEndsWithCall = 0;
    lastExprWasRange = 0;
    parsePrecedence(PREC_ASSIGNMENT);
}

void variableDeclaration(void) {
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

void functionDeclaration(void) {
    (void)functionDeclarationNamed();
}

static void anonymousFunction(int canAssign) {
    (void)canAssign;
    functionBody(TYPE_FUNCTION);
    typePush(TYPEHINT_ANY);
}

void globalDeclaration(void) {
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

void globalFunctionDeclaration(void) {
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

void decoratedFunctionDeclaration(void) {
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
    } else if (inNamedArgs) {
        emitBytes(OP_CALL_NAMED, argCount);
    } else {
        emitBytes(OP_CALL, argCount);
    }
    lastExprEndsWithCall = 1;
    typeStackTop = baseTop;
    typePop();
    typePush(TYPEHINT_ANY);
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
