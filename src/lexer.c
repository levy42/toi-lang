#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void init_lexer(Lexer* lexer, const char* source) {
    lexer->start = source;
    lexer->current = source;
    lexer->source_start = source;
    lexer->line = 1;
    lexer->indent_top = 0;
    lexer->indent_stack[0] = 0; // Base level 0
    lexer->pending_dedents = 0;
    lexer->at_start_of_line = 1;
    lexer->inside_table = 0;
}

static int is_at_end(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static Token error_token(Lexer* lexer, const char* message) {
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
    {"yield",    5, TOKEN_YIELD},
    {"if",       2, TOKEN_IF},
    {"in",       2, TOKEN_IN},
    {"has",      3, TOKEN_HAS},
    {"global",   6, TOKEN_GLOBAL},
    {"import",   6, TOKEN_IMPORT},
    {"from",     4, TOKEN_FROM},
    {"del",      3, TOKEN_DEL},
    {"assert",   6, TOKEN_ASSERT},
    {"true",     4, TOKEN_TRUE},
    {NULL,       0, TOKEN_EOF} // Sentinel
};

static TokenType identifier_type(Lexer* lexer) {
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
    return make_token(lexer, identifier_type(lexer));
}

static Token number(Lexer* lexer) {
    int last_was_digit = 1; // First digit already consumed by caller
    while (1) {
        char c = peek(lexer);
        if (isdigit(c)) {
            last_was_digit = 1;
            advance(lexer);
            continue;
        }
        if (c == '_' && last_was_digit && isdigit(peek_next(lexer))) {
            last_was_digit = 0;
            advance(lexer);
            continue;
        }
        break;
    }
    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        advance(lexer);
        last_was_digit = 0;
        while (1) {
            char c = peek(lexer);
            if (isdigit(c)) {
                last_was_digit = 1;
                advance(lexer);
                continue;
            }
            if (c == '_' && last_was_digit && isdigit(peek_next(lexer))) {
                last_was_digit = 0;
                advance(lexer);
                continue;
            }
            break;
        }
    }
    return make_token(lexer, TOKEN_NUMBER);
}

static Token string(Lexer* lexer, char quote) {
    while (peek(lexer) != quote && !is_at_end(lexer)) {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer); // Skip the backslash
            if (peek(lexer) == '\n') lexer->line++; // Handle escaped newline
        }
        advance(lexer);
    }
    if (is_at_end(lexer)) return error_token(lexer, "Unterminated string.");
    advance(lexer);
    return make_token(lexer, TOKEN_STRING);
}


static Token multiline_string(Lexer* lexer) {
    // When called, lexer->start points at the first '['
    // lexer->current is past the first '[' (consumed by the case statement)
    // peek() has verified that the next char is also '['

    // Skip the second [
    advance(lexer);

    // Scan content until we find ]]
    // Keep lexer->start pointing at the first '[' so the token includes [[...]]
    while (!is_at_end(lexer)) {
        if (peek(lexer) == ']' && peek_next(lexer) == ']') {
            // Found closing ]]
            // Skip ]] and create token
            advance(lexer); // Skip first ]
            advance(lexer); // Skip second ]
            // Token now includes [[...]] from start to current
            return make_token(lexer, TOKEN_STRING);
        }
        // Not at closing yet, keep scanning
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }

    return error_token(lexer, "Unterminated multiline string.");
}

static Token fstring_quoted(Lexer* lexer, char quote) {
    // lexer->start points at 'f', opening quote already consumed.
    while (peek(lexer) != quote && !is_at_end(lexer)) {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer); // skip backslash
        }
        advance(lexer);
    }
    if (is_at_end(lexer)) return error_token(lexer, "Unterminated f-string.");
    advance(lexer); // consume closing quote
    return make_token(lexer, TOKEN_FSTRING);
}

static Token fstring_multiline(Lexer* lexer) {
    // lexer->start points at 'f', current points at first '['.
    // consume [[ and then read until ]]
    advance(lexer); // consume first [
    advance(lexer); // consume second [

    while (!is_at_end(lexer)) {
        if (peek(lexer) == ']' && peek_next(lexer) == ']') {
            advance(lexer); // first ]
            advance(lexer); // second ]
            return make_token(lexer, TOKEN_FSTRING);
        }
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }

    return error_token(lexer, "Unterminated multiline f-string.");
}

Token scan_token(Lexer* lexer) {
restart:
    if (lexer->pending_dedents > 0) {
        lexer->pending_dedents--;
        return make_token(lexer, TOKEN_DEDENT);
    }

    if (lexer->at_start_of_line && lexer->inside_table == 0) {
        lexer->at_start_of_line = 0;
        int indent = 0;
        lexer->start = lexer->current;
        while (peek(lexer) == ' ' || peek(lexer) == '\t') {
            if (advance(lexer) == ' ') indent++;
            else indent += 4;
        }

        if (peek(lexer) == '\n' || (peek(lexer) == '-' && peek_next(lexer) == '-')) {
            lexer->at_start_of_line = 1;
            if (peek(lexer) == '\n') {
                lexer->line++;
                advance(lexer);
            } else {
                while (peek(lexer) != '\n' && !is_at_end(lexer)) advance(lexer);
            }
            goto restart;
        }

        if (is_at_end(lexer)) {
             // End of file usually means dedent back to 0
             indent = 0;
        }

        int current_indent = lexer->indent_stack[lexer->indent_top];
        if (indent > current_indent) {
            lexer->indent_top++;
            lexer->indent_stack[lexer->indent_top] = indent;
            return make_token(lexer, TOKEN_INDENT);
        } else if (indent < current_indent) {
            while (lexer->indent_top > 0 && lexer->indent_stack[lexer->indent_top] > indent) {
                lexer->indent_top--;
                lexer->pending_dedents++;
            }
            if (lexer->indent_stack[lexer->indent_top] != indent) {
                return error_token(lexer, "Inconsistent indentation.");
            }
            if (lexer->pending_dedents > 0) {
                lexer->pending_dedents--;
                return make_token(lexer, TOKEN_DEDENT);
            }
        }
    } else if (lexer->at_start_of_line) {
        lexer->at_start_of_line = 0;
    }

    // Skip spaces, tabs, and comments on the current line
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '-' && peek_next(lexer) == '-') {
            while (peek(lexer) != '\n' && !is_at_end(lexer)) advance(lexer);
        } else {
            break;
        }
    }

    if (peek(lexer) == '\n') {
        lexer->line++;
        advance(lexer);
        lexer->at_start_of_line = 1;
        goto restart;
    }

    lexer->start = lexer->current;

    if (is_at_end(lexer)) {
        if (lexer->indent_top > 0) {
            lexer->pending_dedents = lexer->indent_top;
            lexer->indent_top = 0;
            goto restart;
        }
        return make_token(lexer, TOKEN_EOF);
    }

    char c = advance(lexer);
    // Check for f-string: f"...", f'...', or f[[...]]
    if (c == 'f') {
        if (peek(lexer) == '"' || peek(lexer) == '\'') {
            char quote = advance(lexer); // consume opening quote
            return fstring_quoted(lexer, quote);
        }
        if (peek(lexer) == '[' && peek_next(lexer) == '[') {
            return fstring_multiline(lexer);
        }
    }
    if (isalpha(c) || c == '_') return identifier(lexer);
    if (isdigit(c)) return number(lexer);

    switch (c) {
        case '(': return make_token(lexer, TOKEN_LEFT_PAREN);
        case ')': return make_token(lexer, TOKEN_RIGHT_PAREN);
        case '{': 
            lexer->inside_table++;
            return make_token(lexer, TOKEN_LEFT_BRACE);
        case '}':
            if (lexer->inside_table > 0) lexer->inside_table--;
            return make_token(lexer, TOKEN_RIGHT_BRACE);
        case '[':
            // Check for multiline string [[...]]
            if (peek(lexer) == '[') {
                return multiline_string(lexer);
            }
            return make_token(lexer, TOKEN_LEFT_BRACKET);
        case ']': return make_token(lexer, TOKEN_RIGHT_BRACKET);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case ',': return make_token(lexer, TOKEN_COMMA);
        case '.': 
            if (peek(lexer) == '.') { advance(lexer); return make_token(lexer, TOKEN_DOT_DOT); }
            return make_token(lexer, TOKEN_DOT);
        case '-':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_MINUS_EQUAL); }
            return make_token(lexer, TOKEN_MINUS);
        case '+':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_PLUS_EQUAL); }
            return make_token(lexer, TOKEN_PLUS);
        case '/':
            if (peek(lexer) == '/') { advance(lexer); return make_token(lexer, TOKEN_INT_DIV); }
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_SLASH_EQUAL); }
            return make_token(lexer, TOKEN_SLASH);
        case '*':
            if (peek(lexer) == '*') { advance(lexer); return make_token(lexer, TOKEN_POWER); }
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_STAR_EQUAL); }
            return make_token(lexer, TOKEN_STAR);
        case '%':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_PERCENT_EQUAL); }
            return make_token(lexer, TOKEN_PERCENT);
        case '#': return make_token(lexer, TOKEN_HASH);
        case '?': return make_token(lexer, TOKEN_QUESTION);
        case ':':
            if (peek(lexer) == ':') { advance(lexer); return make_token(lexer, TOKEN_COLON_COLON); }
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_WALRUS); }
            return make_token(lexer, TOKEN_COLON);
        case '@': return make_token(lexer, TOKEN_AT);
        case '=':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_EQUAL_EQUAL); }
            return make_token(lexer, TOKEN_EQUALS);
        case '!':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_BANG_EQUAL); }
            break;
        case '<':
            if (peek(lexer) == '+') { advance(lexer); return make_token(lexer, TOKEN_APPEND); }
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_LESS_EQUAL); }
            return make_token(lexer, TOKEN_LESS);
        case '>':
            if (peek(lexer) == '=') { advance(lexer); return make_token(lexer, TOKEN_GREATER_EQUAL); }
            return make_token(lexer, TOKEN_GREATER);
        case '"': return string(lexer, '"');
        case '\'': return string(lexer, '\'');
    }

    static char buffer[100];
    snprintf(buffer, sizeof(buffer), "Unexpected character: '%c' (ASCII %d).", c, c);
    return error_token(lexer, buffer);
}
