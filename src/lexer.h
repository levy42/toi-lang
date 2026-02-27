#ifndef LEXER_H
#define LEXER_H

#include <stdint.h>

#include "token.h"

typedef struct {
    const char* start;
    const char* current;
    const char* source_start;
    int line;
    int indent_stack[UINT8_MAX + 1]; // Stack of indentation levels (column counts)
    int indent_top;        // Current stack pointer
    int pending_dedents;   // Number of DEDENT tokens queued to emit
    int at_start_of_line;
    int inside_table;      // Track if we're inside a table literal
} Lexer;

void init_lexer(Lexer* lexer, const char* source);
Token scan_token(Lexer* lexer);

#endif
