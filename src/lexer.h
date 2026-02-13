#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char* start;
    const char* current;
    const char* sourceStart;
    int line;
    int indentStack[256]; // Stack of indentation levels (column counts)
    int indentTop;        // Current stack pointer
    int pendingDedents;   // Number of DEDENT tokens queued to emit
    int atStartOfLine;
    int insideTable;      // Track if we're inside a table literal
} Lexer;

void initLexer(Lexer* lexer, const char* source);
Token scanToken(Lexer* lexer);

#endif
