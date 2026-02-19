#ifndef COMPILER_INTERNAL_H
#define COMPILER_INTERNAL_H

#include "../common.h"
#include "../compiler.h"
#include "../lexer.h"

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_TERNARY,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef struct {
    Token name;
    int depth;
    int isCaptured;
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
    int start;
    int scopeDepth;
    int breakJumps[256];
    int breakCount;
    int continueJumps[256];
    int continueCount;
    int isForLoop;
    int slotsToPop;
    struct LoopContext* enclosing;
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
    LoopContext* loopContext;
} Compiler;

typedef struct {
    Token current;
    Token previous;
    int hadError;
    int panicMode;
} Parser;

typedef struct {
    int flagsOffset;
    int exceptOffset;
    int finallyOffset;
} TryPatch;

extern Parser parser;
extern Compiler* current;
extern Lexer lexer;
extern int typeStackTop;
extern int isREPLMode;
extern int lastExprEndsWithCall;
extern int lastExprWasRange;
extern int inForRangeHeader;
extern int inTableEntryExpression;

void typePush(uint8_t type);
Chunk* currentChunk(void);
void expression(void);
void advance(void);
void consume(TokenType type, const char* message);
int match(TokenType type);
int check(TokenType type);
void emitByte(uint8_t byte);
void emitConstant(Value value);
void emitBytes(uint8_t byte1, uint8_t byte2);
int emitJump(uint8_t instruction);
TryPatch emitTry(uint8_t depth);
void patchJump(int offset);
void patchTry(int offset);
void patchTryFinally(int offset);
void emitLoop(int loopStart);
uint8_t makeConstant(Value value);
void beginScope(void);
void endScope(void);
int resolveLocal(Compiler* compiler, Token* name);
int resolveUpvalue(Compiler* compiler, Token* name);
void addLocal(Token name);
void markInitialized(void);
void markInitializedCount(int count);
void declareVariable(void);
uint8_t identifierConstant(Token* name);
void namedVariable(Token name, int canAssign);
void consumePropertyNameAfterDot(void);
void variableDeclaration(void);
void functionDeclaration(void);
void globalFunctionDeclaration(void);
void globalDeclaration(void);
void decoratedFunctionDeclaration(void);
void error(const char* message);
void errorAtCurrent(const char* message);
void emitReturn(void);
void setLocalType(int localIndex, uint8_t type);
void updateLocalType(int localIndex, uint8_t rhsType);
int emitSimpleFstringExpr(const char* exprStart, int exprLen);

#endif
