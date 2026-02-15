#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void initLexer(Lexer* lexer, const char* source) {
    lexer->start = source;
    lexer->current = source;
    lexer->sourceStart = source;
    lexer->line = 1;
    lexer->indentTop = 0;
    lexer->indentStack[0] = 0; // Base level 0
    lexer->pendingDedents = 0;
    lexer->atStartOfLine = 1;
    lexer->insideTable = 0;
}

static int isAtEnd(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peekNext(Lexer* lexer) {
    if (isAtEnd(lexer)) return '\0';
    return lexer->current[1];
}

static Token makeToken(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static Token errorToken(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    return token;
}

typedef struct {
    const char* name;
    int length;
    TokenType type;
} Keyword;

static Keyword keywords[] = {
    {"and",      3, TOKEN_AND},
    {"or",       2, TOKEN_OR},
    {"not",      3, TOKEN_NOT},
    {"nil",      3, TOKEN_NIL},
    {"gc",       2, TOKEN_GC},
    {"print",    5, TOKEN_PRINT},
    {"local",    5, TOKEN_LOCAL},
    {"while",    5, TOKEN_WHILE},
    {"break",    5, TOKEN_BREAK},
    {"continue", 8, TOKEN_CONTINUE},
    {"with",     4, TOKEN_WITH},
    {"as",       2, TOKEN_AS},
    {"try",      3, TOKEN_TRY},
    {"except",   6, TOKEN_EXCEPT},
    {"finally",  7, TOKEN_FINALLY},
    {"throw",    5, TOKEN_THROW},
    {"else",     4, TOKEN_ELSE},
    {"elif",     4, TOKEN_ELIF},
    {"false",    5, TOKEN_FALSE},
    {"for",      3, TOKEN_FOR},
    {"fn",       2, TOKEN_FN},
    {"return",   6, TOKEN_RETURN},
    {"if",       2, TOKEN_IF},
    {"in",       2, TOKEN_IN},
    {"has",      3, TOKEN_HAS},
    {"global",   6, TOKEN_GLOBAL},
    {"import",   6, TOKEN_IMPORT},
    {"from",     4, TOKEN_FROM},
    {"del",      3, TOKEN_DEL},
    {"true",     4, TOKEN_TRUE},
    {NULL,       0, TOKEN_EOF} // Sentinel
};

static TokenType identifierType(Lexer* lexer) {
    int length = (int)(lexer->current - lexer->start);
    for (int i = 0; keywords[i].name != NULL; i++) {
        if (length == keywords[i].length &&
            memcmp(lexer->start, keywords[i].name, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer* lexer) {
    while (isalnum(peek(lexer)) || peek(lexer) == '_') advance(lexer);
    return makeToken(lexer, identifierType(lexer));
}

static Token number(Lexer* lexer) {
    int lastWasDigit = 1; // First digit already consumed by caller
    while (1) {
        char c = peek(lexer);
        if (isdigit(c)) {
            lastWasDigit = 1;
            advance(lexer);
            continue;
        }
        if (c == '_' && lastWasDigit && isdigit(peekNext(lexer))) {
            lastWasDigit = 0;
            advance(lexer);
            continue;
        }
        break;
    }
    if (peek(lexer) == '.' && isdigit(peekNext(lexer))) {
        advance(lexer);
        lastWasDigit = 0;
        while (1) {
            char c = peek(lexer);
            if (isdigit(c)) {
                lastWasDigit = 1;
                advance(lexer);
                continue;
            }
            if (c == '_' && lastWasDigit && isdigit(peekNext(lexer))) {
                lastWasDigit = 0;
                advance(lexer);
                continue;
            }
            break;
        }
    }
    return makeToken(lexer, TOKEN_NUMBER);
}

static Token string(Lexer* lexer, char quote) {
    while (peek(lexer) != quote && !isAtEnd(lexer)) {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\' && peekNext(lexer) != '\0') {
            advance(lexer); // Skip the backslash
            if (peek(lexer) == '\n') lexer->line++; // Handle escaped newline
        }
        advance(lexer);
    }
    if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated string.");
    advance(lexer);
    return makeToken(lexer, TOKEN_STRING);
}


static Token multilineString(Lexer* lexer) {
    // When called, lexer->start points at the first '['
    // lexer->current is past the first '[' (consumed by the case statement)
    // peek() has verified that the next char is also '['

    // Skip the second [
    advance(lexer);

    // Scan content until we find ]]
    // Keep lexer->start pointing at the first '[' so the token includes [[...]]
    while (!isAtEnd(lexer)) {
        if (peek(lexer) == ']' && peekNext(lexer) == ']') {
            // Found closing ]]
            // Skip ]] and create token
            advance(lexer); // Skip first ]
            advance(lexer); // Skip second ]
            // Token now includes [[...]] from start to current
            return makeToken(lexer, TOKEN_STRING);
        }
        // Not at closing yet, keep scanning
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }

    return errorToken(lexer, "Unterminated multiline string.");
}

Token scanToken(Lexer* lexer) {
    if (lexer->pendingDedents > 0) {
        lexer->pendingDedents--;
        return makeToken(lexer, TOKEN_DEDENT);
    }

    if (lexer->atStartOfLine && lexer->insideTable == 0) {
        lexer->atStartOfLine = 0;
        int indent = 0;
        lexer->start = lexer->current;
        while (peek(lexer) == ' ' || peek(lexer) == '\t') {
            if (advance(lexer) == ' ') indent++;
            else indent += 4;
        }

        if (peek(lexer) == '\n' || (peek(lexer) == '-' && peekNext(lexer) == '-')) {
            lexer->atStartOfLine = 1;
            if (peek(lexer) == '\n') {
                lexer->line++;
                advance(lexer);
            } else {
                while (peek(lexer) != '\n' && !isAtEnd(lexer)) advance(lexer);
            }
            return scanToken(lexer);
        }

        if (isAtEnd(lexer)) {
             // End of file usually means dedent back to 0
             indent = 0;
        }

        int currentIndent = lexer->indentStack[lexer->indentTop];
        if (indent > currentIndent) {
            lexer->indentTop++;
            lexer->indentStack[lexer->indentTop] = indent;
            return makeToken(lexer, TOKEN_INDENT);
        } else if (indent < currentIndent) {
            while (lexer->indentTop > 0 && lexer->indentStack[lexer->indentTop] > indent) {
                lexer->indentTop--;
                lexer->pendingDedents++;
            }
            if (lexer->indentStack[lexer->indentTop] != indent) {
                return errorToken(lexer, "Inconsistent indentation.");
            }
            if (lexer->pendingDedents > 0) {
                lexer->pendingDedents--;
                return makeToken(lexer, TOKEN_DEDENT);
            }
        }
    } else if (lexer->atStartOfLine) {
        lexer->atStartOfLine = 0;
    }

    // Skip spaces, tabs, and comments on the current line
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '-' && peekNext(lexer) == '-') {
            while (peek(lexer) != '\n' && !isAtEnd(lexer)) advance(lexer);
        } else {
            break;
        }
    }

    if (peek(lexer) == '\n') {
        lexer->line++;
        advance(lexer);
        lexer->atStartOfLine = 1;
        return scanToken(lexer);
    }

    lexer->start = lexer->current;

    if (isAtEnd(lexer)) {
        if (lexer->indentTop > 0) {
            lexer->pendingDedents = lexer->indentTop;
            lexer->indentTop = 0;
            return scanToken(lexer);
        }
        return makeToken(lexer, TOKEN_EOF);
    }

    char c = advance(lexer);
    // Check for f-string: f"..." or f'...'
    if (c == 'f' && (peek(lexer) == '"' || peek(lexer) == '\'')) {
        char quote = advance(lexer); // consume opening quote
        while (peek(lexer) != quote && !isAtEnd(lexer)) {
            if (peek(lexer) == '\n') lexer->line++;
            if (peek(lexer) == '\\' && peekNext(lexer) != '\0') {
                advance(lexer); // skip backslash
            }
            advance(lexer);
        }
        if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated f-string.");
        advance(lexer); // consume closing quote
        return makeToken(lexer, TOKEN_FSTRING);
    }
    if (isalpha(c) || c == '_') return identifier(lexer);
    if (isdigit(c)) return number(lexer);

    switch (c) {
        case '(': return makeToken(lexer, TOKEN_LEFT_PAREN);
        case ')': return makeToken(lexer, TOKEN_RIGHT_PAREN);
        case '{': 
            lexer->insideTable++;
            return makeToken(lexer, TOKEN_LEFT_BRACE);
        case '}':
            lexer->insideTable--;
            return makeToken(lexer, TOKEN_RIGHT_BRACE);
        case '[':
            // Check for multiline string [[...]]
            if (peek(lexer) == '[') {
                return multilineString(lexer);
            }
            return makeToken(lexer, TOKEN_LEFT_BRACKET);
        case ']': return makeToken(lexer, TOKEN_RIGHT_BRACKET);
        case ';': return makeToken(lexer, TOKEN_SEMICOLON);
        case ',': return makeToken(lexer, TOKEN_COMMA);
        case '.': 
            if (peek(lexer) == '.') { advance(lexer); return makeToken(lexer, TOKEN_DOT_DOT); }
            return makeToken(lexer, TOKEN_DOT);
        case '-': return makeToken(lexer, TOKEN_MINUS);
        case '+': return makeToken(lexer, TOKEN_PLUS);
        case '/':
            if (peek(lexer) == '/') { advance(lexer); return makeToken(lexer, TOKEN_INT_DIV); }
            return makeToken(lexer, TOKEN_SLASH);
        case '*':
            if (peek(lexer) == '*') { advance(lexer); return makeToken(lexer, TOKEN_POWER); }
            return makeToken(lexer, TOKEN_STAR);
        case '%': return makeToken(lexer, TOKEN_PERCENT);
        case '#': return makeToken(lexer, TOKEN_HASH);
        case '?': return makeToken(lexer, TOKEN_QUESTION);
        case ':': return makeToken(lexer, TOKEN_COLON);
        case '@': return makeToken(lexer, TOKEN_AT);
        case '=':
            if (peek(lexer) == '=') { advance(lexer); return makeToken(lexer, TOKEN_EQUAL_EQUAL); }
            return makeToken(lexer, TOKEN_EQUALS);
        case '!':
            if (peek(lexer) == '=') { advance(lexer); return makeToken(lexer, TOKEN_BANG_EQUAL); }
            break;
        case '<':
            if (peek(lexer) == '=') { advance(lexer); return makeToken(lexer, TOKEN_LESS_EQUAL); }
            return makeToken(lexer, TOKEN_LESS);
        case '>':
            if (peek(lexer) == '=') { advance(lexer); return makeToken(lexer, TOKEN_GREATER_EQUAL); }
            return makeToken(lexer, TOKEN_GREATER);
        case '"': return string(lexer, '"');
        case '\'': return string(lexer, '\'');
    }

    static char buffer[100];
    snprintf(buffer, sizeof(buffer), "Unexpected character: '%c' (ASCII %d).", c, c);
    return errorToken(lexer, buffer);
}
