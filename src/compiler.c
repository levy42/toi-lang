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

typedef void (*ParseFn)(int can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

Parser parser;
Compiler* current = NULL;
Lexer lexer;
int is_repl_mode = 0;  // If 1, don't pop expression results
int last_expr_ends_with_call = 0;
int last_expr_was_range = 0;
int in_for_range_header = 0;
int in_table_entry_expression = 0;
static uint8_t type_stack[512];
int type_stack_top = 0;

void type_push(uint8_t type) {
    if (type_stack_top < (int)(sizeof(type_stack) / sizeof(type_stack[0]))) {
        type_stack[type_stack_top++] = type;
    }
}

static uint8_t type_pop(void) {
    if (type_stack_top == 0) return TYPEHINT_ANY;
    return type_stack[--type_stack_top];
}

static int is_numeric_type(uint8_t type) {
    return type == TYPEHINT_INT || type == TYPEHINT_FLOAT;
}

Chunk* current_chunk(void) {
    return &current->function->chunk;
}

static void error_at(Token* token, const char* message) {
    if (parser.panic_mode) return;
    parser.panic_mode = 1;
    fprintf(stderr, COLOR_RED "[line %d] Error" COLOR_RESET, token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);

    // Show the source line with a caret marker, if we have source context
    if (token->type != TOKEN_EOF && token->type != TOKEN_ERROR && token->start != NULL) {
        const char* line_start = token->start;
        // Scan backward to find start of the line
        while (line_start > lexer.source_start && line_start[-1] != '\n') {
            line_start--;
        }
        const char* line_end = token->start;
        // Scan forward to find end of the line
        while (*line_end != '\0' && *line_end != '\n') {
            line_end++;
        }
        // Print the source line
        fprintf(stderr, "    %.*s\n", (int)(line_end - line_start), line_start);
        // Print caret marker under the token
        int before = (int)(token->start - line_start);
        int caret_len = token->length > 0 ? token->length : 1;
        if (caret_len > (int)(line_end - token->start)) {
            caret_len = (int)(line_end - token->start);
        }
        // Build caret string of appropriate length
        char caret[256];
        int clen = caret_len < 255 ? caret_len : 255;
        memset(caret, '~', (size_t)clen);
        caret[clen] = '\0';
        fprintf(stderr, "    %*s", before, "");
        fprintf(stderr, COLOR_RED "%s" COLOR_RESET "\n", caret);
    }

    parser.had_error = 1;
}

void error(const char* message) {
    error_at(&parser.previous, message);
}

void error_at_current(const char* message) {
    error_at(&parser.current, message);
}

void advance(void) {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scan_token(&lexer);
#ifdef DEBUG_COMPILER
        printf("Token: %d '%.*s'\n", parser.current.type, parser.current.length, parser.current.start);
#endif
        if (parser.current.type != TOKEN_ERROR) break;

        error_at_current(parser.current.start);
    }
}



void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    error_at_current(message);
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

void emit_byte(uint8_t byte) {
    write_chunk(current_chunk(), byte, parser.previous.line);
}

void emit_bytes(uint8_t byte1, uint8_t byte2) {
    emit_byte(byte1);
    emit_byte(byte2);
}

void emit_call(uint8_t arg_count) {
    switch (arg_count) {
        case 0: emit_byte(OP_CALL0); break;
        case 1: emit_byte(OP_CALL1); break;
        case 2: emit_byte(OP_CALL2); break;
        default: emit_bytes(OP_CALL, arg_count); break;
    }
}

int emit_jump(uint8_t instruction) {
    emit_byte(instruction);
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 2;
}

TryPatch emit_try(uint8_t depth) {
    emit_byte(OP_TRY);
    emit_byte(depth);
    int flags_offset = current_chunk()->count;
    emit_byte(0); // flags
    int except_offset = current_chunk()->count;
    emit_byte(0x00);
    emit_byte(0x00);
    int finally_offset = current_chunk()->count;
    emit_byte(0x00);
    emit_byte(0x00);
    return (TryPatch){flags_offset, except_offset, finally_offset};
}

void patch_jump(int offset) {
    int jump = current_chunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

void patch_try(int offset) {
    int jump = current_chunk()->count - offset - 4;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

void patch_try_finally(int offset) {
    int jump = current_chunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

void emit_loop(int loop_start) {
    emit_byte(OP_LOOP);

    int offset = current_chunk()->count - loop_start + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");

    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

void emit_return(void) {
    emit_byte(OP_RETURN);
}

uint8_t make_constant(Value value) {
    int constant = add_constant(current_chunk(), value);
    if (constant > 255) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

void emit_constant(Value value) {
    emit_bytes(OP_CONSTANT, make_constant(value));
}

static void init_compiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing = current;
    compiler->function = new_function();
    compiler->type = type;

    compiler->local_count = 0;
    compiler->explicit_global_count = 0;
    compiler->upvalue_count = 0;
    compiler->scope_depth = 0;
    compiler->loop_context = NULL;

    // Claim stack slot 0
    Local* local = &compiler->locals[compiler->local_count++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
    local->is_captured = 0;
    local->type = TYPEHINT_ANY;
    local->is_function_value = 0;
    local->function_return_type = TYPEHINT_ANY;

    current = compiler;

    if (type == TYPE_SCRIPT) {
        compiler->function->name = NULL;
        // Scripts are local-by-default (except REPL).
        if (!is_repl_mode) {
            compiler->scope_depth = 1;
        }
    } else {
        compiler->function->name = copy_string(parser.previous.start, parser.previous.length);
    }
}

static ObjFunction* end_compiler() {
    emit_return();
    ObjFunction* function = current->function;
    function->upvalue_count = current->upvalue_count;
    function->tiny_inline_kind = TINY_INLINE_NONE;
    function->source_base = lexer.source_start;
    function->source_len = (int)(lexer.source_start ? strlen(lexer.source_start) : 0);
    if (function->upvalue_count == 0 &&
        !function->is_variadic &&
        function->defaults_count == 0 &&
        function->arity == 2) {
        Chunk* chunk = &function->chunk;
        if (chunk->count >= 3) {
            uint8_t* code = chunk->code;
            int trailing_ret = (chunk->count == 3) || (chunk->count == 4 && code[3] == OP_RETURN);
            if (trailing_ret && code[0] == OP_GET_LOCAL && code[2] == OP_RETURN) {
                if (code[1] == 1) {
                    function->tiny_inline_kind = TINY_INLINE_RET0_2;
                } else if (code[1] == 2) {
                    function->tiny_inline_kind = TINY_INLINE_RET1_2;
                }
            }
        }
        if (function->tiny_inline_kind != TINY_INLINE_NONE) goto tiny_inline_done;
        // Two-parameter shims that only read an argument's length:
        //   return a + #b  -> [GET_LOCAL 1, GET_LOCAL 2, LENGTH, <add>, RETURN...]
        //   return #a/#b   -> [GET_LOCAL n, LENGTH, RETURN...]
        // Trailing OP_RETURN bytes are stripped: emit_return may append one and the
        // optimiser folds OP_ADD into OP_IADD/OP_FADD for typed arguments.
        {
            uint8_t* code = chunk->code;
            int n = chunk->count;
            while (n > 0 && code[n - 1] == OP_RETURN) n--;
            if (n == 6 &&
                code[0] == OP_GET_LOCAL && code[1] == 1 &&
                code[2] == OP_GET_LOCAL && code[3] == 2 &&
                code[4] == OP_LENGTH &&
                (code[5] == OP_ADD || code[5] == OP_IADD ||
                 code[5] == OP_FADD || code[5] == OP_QFADD)) {
                function->tiny_inline_kind = TINY_INLINE_LEN_ADD2;
            } else if (n == 3 && code[0] == OP_GET_LOCAL && code[2] == OP_LENGTH) {
                if (code[1] == 1) {
                    function->tiny_inline_kind = TINY_INLINE_LEN_ARG0_2;
                } else if (code[1] == 2) {
                    function->tiny_inline_kind = TINY_INLINE_LEN_ARG1_2;
                }
            }
        }
        if (function->tiny_inline_kind != TINY_INLINE_NONE) goto tiny_inline_done;
        if (chunk->count >= 6) {
            uint8_t* code = chunk->code;
            int trailing_ok = (chunk->count == 6) || (chunk->count == 7 && code[6] == OP_RETURN);
            if (trailing_ok &&
                code[0] == OP_GET_LOCAL && code[1] == 1 &&
                code[2] == OP_GET_LOCAL && code[3] == 2 &&
                code[5] == OP_RETURN) {
                if (code[4] == OP_IADD) {
                    function->tiny_inline_kind = TINY_INLINE_IADD2;
                } else if (code[4] == OP_FADD) {
                    function->tiny_inline_kind = TINY_INLINE_FADD2;
                } else if (code[4] == OP_ISUB) {
                    function->tiny_inline_kind = TINY_INLINE_ISUB2;
                } else if (code[4] == OP_FSUB) {
                    function->tiny_inline_kind = TINY_INLINE_FSUB2;
                } else if (code[4] == OP_IMUL) {
                    function->tiny_inline_kind = TINY_INLINE_IMUL2;
                } else if (code[4] == OP_FMUL) {
                    function->tiny_inline_kind = TINY_INLINE_FMUL2;
                } else if (code[4] == OP_IDIV) {
                    function->tiny_inline_kind = TINY_INLINE_IDIV2;
                } else if (code[4] == OP_FDIV) {
                    function->tiny_inline_kind = TINY_INLINE_FDIV2;
                } else if (code[4] == OP_ADD) {
                    function->tiny_inline_kind = TINY_INLINE_ADD2;
                } else if (code[4] == OP_SUBTRACT) {
                    function->tiny_inline_kind = TINY_INLINE_SUB2;
                } else if (code[4] == OP_MULTIPLY) {
                    function->tiny_inline_kind = TINY_INLINE_MUL2;
                } else if (code[4] == OP_DIVIDE) {
                    function->tiny_inline_kind = TINY_INLINE_DIV2;
                } else if (code[4] == OP_GREATER) {
                    function->tiny_inline_kind = TINY_INLINE_GT2;
                } else if (code[4] == OP_LESS) {
                    function->tiny_inline_kind = TINY_INLINE_LT2;
                } else if (code[4] == OP_EQUAL) {
                    function->tiny_inline_kind = TINY_INLINE_EQ2;
                }
            }
        }
    }
tiny_inline_done:
#ifdef DEBUG_PRINT_CODE
    if (!parser.had_error) {
        disassemble_chunk(current_chunk(), function->name != NULL ? function->name->chars : "<script>");
    }
#endif
    return function;
}

void begin_scope(void) {
    current->scope_depth++;
}

void end_scope(void) {
    current->scope_depth--;
    while (current->local_count > 0 &&
           current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }
}

void expression(void);
static ParseRule* get_rule(TokenType type);
static void parse_precedence(Precedence precedence);
static void emit_get_named(Token name);
static void emit_set_named(Token name);
static int match_compound_assign(TokenType* op);
static uint8_t emit_compound_op(TokenType op, uint8_t lhs_type, uint8_t rhs_type);
static void emit_assignment_store(Token name, uint8_t get_op, uint8_t set_op, int arg, uint8_t rhs_type);

static int token_indent(Token token) {
    const char* p = token.start;
    const char* line_start = p;
    while (line_start > lexer.source_start && line_start[-1] != '\n') {
        line_start--;
    }
    int indent = 0;
    while (line_start < p) {
        if (*line_start == ' ') indent++;
        else if (*line_start == '\t') indent += 4;
        else break;
        line_start++;
    }
    return indent;
}

static double parse_number_token(Token token) {
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

static ObjString* string_from_token(Token token) {
    // Multiline string [[...]]: raw content.
    if (token.length >= 4 && token.start[0] == '[' && token.start[1] == '[') {
        const char* src = token.start + 2;
        int raw_len = token.length - 4;
        return copy_string(src, raw_len);
    }

    // Quoted string ("..." or '...'): decode escapes.
    char quote = token.start[0];
    const char* src = token.start + 1;
    int raw_len = token.length - 2;
    char* buf = (char*)malloc((size_t)raw_len + 1);
    int w = 0;
    for (int i = 0; i < raw_len; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < raw_len) {
            char e = src[++i];
            switch (e) {
                case 'n': buf[w++] = '\n'; break;
                case 't': buf[w++] = '\t'; break;
                case 'r': buf[w++] = '\r'; break;
                case '\'': buf[w++] = '\''; break;
                case '"': buf[w++] = '"'; break;
                case '\\': buf[w++] = '\\'; break;
                default:
                    buf[w++] = '\\';
                    buf[w++] = e;
                    break;
            }
        } else {
            if (c == quote) continue;
            buf[w++] = c;
        }
    }
    ObjString* s = copy_string(buf, w);
    free(buf);
    return s;
}

static void maybe_capture_function_docstring(void) {
    if (!check(TOKEN_STRING)) return;
    Token first = parser.current;
    Lexer peek = lexer;
    Token next = scan_token(&peek);
    if (!(next.type == TOKEN_DEDENT || next.type == TOKEN_EOF || next.line > first.line)) return;
    advance();
    current->function->doc = string_from_token(parser.previous);
}

static int token_is_int(Token token) {
    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];
        if (c == '.' || c == 'e' || c == 'E') return 0;
    }
    return 1;
}

static void number(int can_assign) {
    (void)can_assign;
    double value = parse_number_token(parser.previous);
    emit_constant(NUMBER_VAL(value));
    type_push(token_is_int(parser.previous) ? TYPEHINT_INT : TYPEHINT_FLOAT);
}

static void string(int can_assign) {
    (void)can_assign;
    ObjString* s = string_from_token(parser.previous);
    emit_constant(OBJ_VAL(s));
    type_push(TYPEHINT_STR);
}

static void literal(int can_assign) {
    (void)can_assign;
    switch (parser.previous.type) {
        case TOKEN_FALSE: emit_byte(OP_FALSE); type_push(TYPEHINT_BOOL); break;
        case TOKEN_NIL: emit_byte(OP_NIL); type_push(TYPEHINT_ANY); break;
        case TOKEN_TRUE: emit_byte(OP_TRUE); type_push(TYPEHINT_BOOL); break;
        default: return; // Unreachable.
    }
}

// Forward declarations for fstring
void expression(void);
int emit_simple_fstring_expr(const char* expr_start, int expr_len);
static int is_generator_comprehension_start(int start_line);
static void generator_comprehension(int can_assign);

static void grouping(int can_assign) {
    (void)can_assign;
    if (parser.current.type != TOKEN_LEFT_PAREN &&
        is_generator_comprehension_start(parser.previous.line)) {
        generator_comprehension(can_assign);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
        return;
    }
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}


static void unary(int can_assign) {
    (void)can_assign;
    TokenType operator_type = parser.previous.type;
    parse_precedence(PREC_UNARY);
    uint8_t rhs_type = type_pop();
    switch (operator_type) {
        case TOKEN_NOT:
            emit_byte(OP_NOT);
            type_push(TYPEHINT_BOOL);
            break;
        case TOKEN_MINUS:
            emit_byte(OP_NEGATE);
            type_push(is_numeric_type(rhs_type) ? rhs_type : TYPEHINT_ANY);
            break;
        case TOKEN_HASH:
            emit_byte(OP_LENGTH);
            type_push(TYPEHINT_INT);
            break;
        default: return;
    }
    last_expr_ends_with_call = 0;
}

static void not_in(int can_assign) {
    (void)can_assign;
    consume(TOKEN_IN, "Expect 'in' after 'not'.");
    ParseRule* rule = get_rule(TOKEN_IN);
    parse_precedence((Precedence)(rule->precedence + 1));
    type_pop();
    type_pop();
    emit_byte(OP_IN);
    emit_byte(OP_NOT);
    type_push(TYPEHINT_BOOL);
    last_expr_ends_with_call = 0;
}

static void binary(int can_assign) {
    (void)can_assign;
    TokenType operator_type = parser.previous.type;
    ParseRule* rule = get_rule(operator_type);
    parse_precedence((Precedence)(rule->precedence + 1));
    uint8_t rhs_type = type_pop();
    uint8_t lhs_type = type_pop();
    uint8_t out_type = TYPEHINT_ANY;
    switch (operator_type) {
        case TOKEN_BANG_EQUAL:
            emit_bytes(OP_EQUAL, OP_NOT);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_EQUAL_EQUAL:
            emit_byte(OP_EQUAL);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_GREATER:
            emit_byte(OP_GREATER);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_GREATER_EQUAL:
            emit_bytes(OP_LESS, OP_NOT);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_LESS:
            emit_byte(OP_LESS);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_LESS_EQUAL:
            emit_bytes(OP_GREATER, OP_NOT);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_HAS:
            emit_byte(OP_HAS);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_IN:
            emit_byte(OP_IN);
            out_type = TYPEHINT_BOOL;
            break;
        case TOKEN_APPEND:
            emit_byte(OP_APPEND);
            out_type = TYPEHINT_ANY;
            break;
        case TOKEN_PLUS:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IADD);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FADD);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_ADD);
            }
            break;
        case TOKEN_MINUS:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_ISUB);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FSUB);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_SUBTRACT);
            }
            break;
        case TOKEN_STAR:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IMUL);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FMUL);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_MULTIPLY);
            }
            break;
        case TOKEN_SLASH:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                emit_byte(OP_FDIV);
                out_type = TYPEHINT_FLOAT;
            } else {
                emit_byte(OP_DIVIDE);
            }
            break;
        case TOKEN_POWER:         emit_byte(OP_POWER); break;
        case TOKEN_INT_DIV:       emit_byte(OP_INT_DIV); break;
        case TOKEN_PERCENT:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IMOD);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FMOD);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_MODULO);
            }
            break;
        default: return;
    }
    type_push(out_type);
    last_expr_ends_with_call = 0;
}

int resolve_local(Compiler* compiler, Token* name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
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

int is_explicit_global_name(Compiler* compiler, Token* name) {
    for (int i = 0; i < compiler->explicit_global_count; i++) {
        Token* declared = &compiler->explicit_globals[i];
        if (name->length == declared->length &&
            memcmp(name->start, declared->start, name->length) == 0) {
            return 1;
        }
    }
    return 0;
}

void register_explicit_global(Token name) {
    if (current == NULL || current->type == TYPE_SCRIPT) return;
    if (is_explicit_global_name(current, &name)) return;
    if (current->explicit_global_count == UINT8_MAX + 1) {
        error("Too many global declarations in function.");
        return;
    }
    current->explicit_globals[current->explicit_global_count++] = name;
}

static int add_upvalue(Compiler* compiler, uint8_t index, int is_local) {
    int upvalue_count = compiler->upvalue_count;

    // Check if upvalue already exists
    for (int i = 0; i < upvalue_count; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }

    if (upvalue_count == UINT8_MAX + 1) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].index = index;
    return compiler->upvalue_count++;
}

int resolve_upvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    // Try to resolve in enclosing function's locals
    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = 1;
        return add_upvalue(compiler, (uint8_t)local, 1);
    }

    // Try to resolve in enclosing function's upvalues
    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, 0);
    }

    return -1;
}

void add_local(Token name) {
    if (current->local_count == UINT8_MAX + 1) {
        error("Too many local variables in function.");
        return;
    }
    Local* local = &current->locals[current->local_count++];
    local->name = name;
    local->depth = -1;
    local->is_captured = 0;
    local->type = TYPEHINT_ANY;
    local->is_function_value = 0;
    local->function_return_type = TYPEHINT_ANY;
}

void mark_initialized(void) {
    if (current->scope_depth == 0) return;
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

void mark_initialized_count(int count) {
    if (current->scope_depth == 0) return;
    for (int i = 0; i < count; i++) {
        current->locals[current->local_count - 1 - i].depth = current->scope_depth;
    }
}

void declare_variable(void) {
    if (current->scope_depth == 0 && !is_repl_mode) return;
    Token* name = &parser.previous;
    for (int i = current->local_count - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scope_depth) {
            break; 
        }
        if (name->length == local->name.length &&
            memcmp(name->start, local->name.start, name->length) == 0) {
            error("Already a variable with this name in this scope.");
        }
    }
    add_local(*name);
}

uint8_t identifier_constant(Token* name) {
    Chunk* chunk = current_chunk();
    for (int i = 0; i < chunk->constants.count; i++) {
        Value v = chunk->constants.values[i];
        if (!IS_STRING(v)) continue;
        ObjString* s = AS_STRING(v);
        if (s->length != name->length) continue;
        if (memcmp(s->chars, name->start, (size_t)name->length) == 0) {
            return (uint8_t)i;
        }
    }
    return make_constant(OBJ_VAL(copy_string(name->start, name->length)));
}

static uint8_t parse_type_name(Token* name) {
    if (name->length == 3 && memcmp(name->start, "int", 3) == 0) return TYPEHINT_INT;
    if (name->length == 5 && memcmp(name->start, "float", 5) == 0) return TYPEHINT_FLOAT;
    if (name->length == 4 && memcmp(name->start, "bool", 4) == 0) return TYPEHINT_BOOL;
    if ((name->length == 3 && memcmp(name->start, "str", 3) == 0) ||
        (name->length == 6 && memcmp(name->start, "string", 6) == 0)) return TYPEHINT_STR;
    if (name->length == 5 && memcmp(name->start, "table", 5) == 0) return TYPEHINT_TABLE;
    return TYPEHINT_ANY;
}

void set_local_type(int local_index, uint8_t type) {
    if (local_index < 0 || local_index >= current->local_count) return;
    current->locals[local_index].type = type;
    current->locals[local_index].is_function_value = 0;
    current->locals[local_index].function_return_type = TYPEHINT_ANY;
}

void update_local_type(int local_index, uint8_t rhs_type) {
    if (local_index < 0 || local_index >= current->local_count) return;
    current->locals[local_index].is_function_value = 0;
    current->locals[local_index].function_return_type = TYPEHINT_ANY;
    uint8_t current_type = current->locals[local_index].type;
    if (rhs_type == TYPEHINT_ANY) {
        current->locals[local_index].type = TYPEHINT_ANY;
        return;
    }
    if (current_type == TYPEHINT_ANY) {
        current->locals[local_index].type = rhs_type;
        return;
    }
    if (current_type != rhs_type) {
        current->locals[local_index].type = TYPEHINT_ANY;
    }
}

static void set_param_type(ObjFunction* function, int index, uint8_t type) {
    if (index < 0) return;
    if (function->param_types_count < function->arity) {
        int old = function->param_types_count;
        function->param_types_count = function->arity;
        function->param_types = (uint8_t*)realloc(function->param_types, sizeof(uint8_t) * function->param_types_count);
        for (int i = old; i < function->param_types_count; i++) {
            function->param_types[i] = TYPEHINT_ANY;
        }
    }
    if (index < function->param_types_count) {
        function->param_types[index] = type;
    }
}

static void set_param_name(ObjFunction* function, int index, Token* name) {
    if (index < 0) return;
    if (function->param_names_count < function->arity) {
        int old = function->param_names_count;
        function->param_names_count = function->arity;
        function->param_names = (ObjString**)realloc(function->param_names, sizeof(ObjString*) * function->param_names_count);
        for (int i = old; i < function->param_names_count; i++) {
            function->param_names[i] = NULL;
        }
    }
    if (index < function->param_names_count) {
        function->param_names[index] = copy_string(name->start, name->length);
    }
}

static void seed_parameter_local_types(Compiler* compiler) {
    // Slot 0 is reserved for VM call frame internals.
    int local_base = 1;
    ObjFunction* function = compiler->function;
    int param_count = function->arity;
    if (param_count <= 0) return;

    for (int i = 0; i < param_count; i++) {
        int local_index = local_base + i;
        if (local_index >= compiler->local_count) break;
        uint8_t type = TYPEHINT_ANY;
        if (i < function->param_types_count) {
            type = function->param_types[i];
        }
        compiler->locals[local_index].type = type;
    }
}

static int match_compound_assign(TokenType* op) {
    if (match(TOKEN_PLUS_EQUAL)) { *op = TOKEN_PLUS; return 1; }
    if (match(TOKEN_MINUS_EQUAL)) { *op = TOKEN_MINUS; return 1; }
    if (match(TOKEN_STAR_EQUAL)) { *op = TOKEN_STAR; return 1; }
    if (match(TOKEN_SLASH_EQUAL)) { *op = TOKEN_SLASH; return 1; }
    if (match(TOKEN_PERCENT_EQUAL)) { *op = TOKEN_PERCENT; return 1; }
    return 0;
}

static uint8_t emit_compound_op(TokenType op, uint8_t lhs_type, uint8_t rhs_type) {
    uint8_t out_type = TYPEHINT_ANY;
    switch (op) {
        case TOKEN_PLUS:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IADD);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FADD);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_ADD_INPLACE);
            }
            break;
        case TOKEN_MINUS:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_ISUB);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FSUB);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_SUBTRACT);
            }
            break;
        case TOKEN_STAR:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IMUL);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FMUL);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_MULTIPLY);
            }
            break;
        case TOKEN_SLASH:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                emit_byte(OP_FDIV);
                out_type = TYPEHINT_FLOAT;
            } else {
                emit_byte(OP_DIVIDE);
            }
            break;
        case TOKEN_PERCENT:
            if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
                if (lhs_type == TYPEHINT_INT && rhs_type == TYPEHINT_INT) {
                    emit_byte(OP_IMOD);
                    out_type = TYPEHINT_INT;
                } else {
                    emit_byte(OP_FMOD);
                    out_type = TYPEHINT_FLOAT;
                }
            } else {
                emit_byte(OP_MODULO);
            }
            break;
        default:
            out_type = TYPEHINT_ANY;
            break;
    }
    return out_type;
}

static void emit_assignment_store(Token name, uint8_t get_op, uint8_t set_op, int arg, uint8_t rhs_type) {
    if (get_op == OP_GET_LOCAL) {
        emit_bytes(set_op, (uint8_t)arg);
        update_local_type(arg, rhs_type);
    } else if (get_op == OP_GET_UPVALUE) {
        emit_bytes(set_op, (uint8_t)arg);
    } else if (is_repl_mode && current->type == TYPE_SCRIPT) {
        emit_byte(OP_DUP);
        emit_bytes(OP_DEFINE_GLOBAL, (uint8_t)arg);
    } else {
        // Local-by-default: assignment creates a new local if not resolved.
        int local_index = current->local_count;
        add_local(name);
        mark_initialized();
        emit_bytes(OP_SET_LOCAL, (uint8_t)local_index);
        set_local_type(local_index, rhs_type);
    }
}

static uint8_t parse_variable(const char* error_message) {
    consume(TOKEN_IDENTIFIER, error_message);
    declare_variable();
    if (current->scope_depth > 0) return 0;
    return identifier_constant(&parser.previous);
}

static void define_variable(uint8_t global) {
    if (current->scope_depth > 0) return;
    emit_bytes(OP_DEFINE_GLOBAL, global);
}

static int has_slice_range_in_subscript(void);
static int rhs_has_top_level_comma(int start_line);
static void parse_array_literal_from_comma_list(void);
static int can_emit_invoke_positional(void);

void named_variable(Token name, int can_assign) {
    uint8_t get_op, set_op;
    int declared_global = is_explicit_global_name(current, &name);
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else if (!declared_global && (arg = resolve_upvalue(current, &name)) != -1) {
        get_op = OP_GET_UPVALUE;
        set_op = OP_SET_UPVALUE;
    } else {
        arg = identifier_constant(&name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }

    if (can_assign && (match(TOKEN_EQUALS) || match(TOKEN_WALRUS))) {
        TokenType assign_tok = parser.previous.type;
        int predeclared_local = 0;
        int predeclared_local_index = -1;
        if (current->type == TYPE_FUNCTION &&
            !declared_global &&
            set_op == OP_SET_GLOBAL &&
            assign_tok == TOKEN_EQUALS) {
            predeclared_local_index = current->local_count;
            add_local(name);
            mark_initialized();
            emit_byte(OP_NIL);
            emit_bytes(OP_SET_LOCAL, (uint8_t)predeclared_local_index);
            emit_byte(OP_POP);
            get_op = OP_GET_LOCAL;
            set_op = OP_SET_LOCAL;
            arg = predeclared_local_index;
            predeclared_local = 1;
        }
        int start_line = parser.current.line;
        if (assign_tok == TOKEN_EQUALS && rhs_has_top_level_comma(start_line)) {
            parse_array_literal_from_comma_list();
        } else {
            expression();
        }
        uint8_t rhs_type = type_pop();

        if (declared_global) {
            emit_bytes(OP_SET_GLOBAL, identifier_constant(&name));
        } else if (set_op == OP_SET_LOCAL) {
            emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
            update_local_type(arg, rhs_type);
        } else if (set_op == OP_SET_UPVALUE) {
            emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
        } else if (current->type == TYPE_FUNCTION && !predeclared_local) {
            // Python-like function scoping: assignment binds local unless explicit global.
            int local_index = current->local_count;
            add_local(name);
            mark_initialized();
            emit_bytes(OP_SET_LOCAL, (uint8_t)local_index);
            set_local_type(local_index, rhs_type);
        } else if (assign_tok == TOKEN_WALRUS && set_op == OP_SET_GLOBAL) {
            emit_bytes(OP_SET_GLOBAL, identifier_constant(&name));
        } else {
            emit_assignment_store(name, get_op, set_op, arg, rhs_type);
        }
        type_push(rhs_type);
        return;
    }

    if (can_assign) {
        TokenType compound_op;
        if (match_compound_assign(&compound_op)) {
            uint8_t lhs_type = TYPEHINT_ANY;
            if (declared_global) {
                emit_bytes(OP_GET_GLOBAL, identifier_constant(&name));
            } else if (get_op == OP_GET_LOCAL) {
                emit_bytes(OP_GET_LOCAL, (uint8_t)arg);
                if (arg >= 0 && arg < current->local_count) {
                    lhs_type = current->locals[arg].type;
                }
            } else if (get_op == OP_GET_UPVALUE) {
                emit_bytes(OP_GET_UPVALUE, (uint8_t)arg);
            } else if (current->type == TYPE_FUNCTION) {
                // x += y in a function implicitly targets a local.
                // If not previously assigned, this behaves like an unbound local.
                emit_byte(OP_NIL);
            } else {
                emit_bytes(get_op, (uint8_t)arg);
            }
            type_push(lhs_type);
            expression();
            uint8_t rhs_type = type_pop();
            uint8_t lhs_popped = type_pop();
            uint8_t out_type = emit_compound_op(compound_op, lhs_popped, rhs_type);
            if (declared_global) {
                emit_bytes(OP_SET_GLOBAL, identifier_constant(&name));
            } else if (set_op == OP_SET_LOCAL) {
                emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
                update_local_type(arg, out_type);
            } else if (set_op == OP_SET_UPVALUE) {
                emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
            } else if (current->type == TYPE_FUNCTION) {
                int local_index = current->local_count;
                add_local(name);
                mark_initialized();
                emit_bytes(OP_SET_LOCAL, (uint8_t)local_index);
                set_local_type(local_index, out_type);
            } else {
                emit_assignment_store(name, get_op, set_op, arg, out_type);
            }
            type_push(out_type);
            return;
        }
    }

    emit_bytes(get_op, (uint8_t)arg);
    if (get_op == OP_GET_LOCAL && arg >= 0 && arg < current->local_count) {
        type_push(current->locals[arg].type);
    } else {
        type_push(TYPEHINT_ANY);
    }
}

static void emit_get_named(Token name) {
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        emit_bytes(OP_GET_LOCAL, (uint8_t)arg);
        return;
    }
    arg = resolve_upvalue(current, &name);
    if (arg != -1) {
        emit_bytes(OP_GET_UPVALUE, (uint8_t)arg);
        return;
    }
    emit_bytes(OP_GET_GLOBAL, identifier_constant(&name));
}

static const char* skip_space_slice(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int is_ident_start_char(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_ident_char(char c) {
    return is_ident_start_char(c) || (c >= '0' && c <= '9');
}

static int parse_int_slice(const char* p, const char* end, double* out) {
    int saw_digit = 0;
    double value = 0.0;
    while (p < end) {
        char c = *p;
        if (c == '_') {
            p++;
            continue;
        }
        if (c < '0' || c > '9') break;
        saw_digit = 1;
        value = value * 10.0 + (double)(c - '0');
        p++;
    }
    if (!saw_digit) return 0;
    p = skip_space_slice(p, end);
    if (p != end) return 0;
    *out = value;
    return 1;
}

int emit_simple_fstring_expr(const char* expr_start, int expr_len) {
    const char* p = expr_start;
    const char* end = expr_start + expr_len;
    p = skip_space_slice(p, end);
    if (p >= end || !is_ident_start_char(*p)) return 0;

    const char* name_start = p;
    p++;
    while (p < end && is_ident_char(*p)) p++;
    Token name = {TOKEN_IDENTIFIER, name_start, (int)(p - name_start), parser.previous.line};

    p = skip_space_slice(p, end);
    if (p == end) {
        emit_get_named(name);
        type_push(TYPEHINT_ANY);
        return 1;
    }

    if (*p != '%') return 0;
    p++;
    p = skip_space_slice(p, end);

    double rhs = 0.0;
    if (!parse_int_slice(p, end, &rhs)) return 0;

    emit_get_named(name);
    emit_constant(NUMBER_VAL(rhs));
    emit_byte(OP_IMOD);
    type_push(TYPEHINT_INT);
    return 1;
}

static void emit_set_named(Token name) {
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
        return;
    }
    arg = resolve_upvalue(current, &name);
    if (arg != -1) {
        emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
        return;
    }
    emit_bytes(OP_SET_GLOBAL, identifier_constant(&name));
}

static void variable(int can_assign) {
    named_variable(parser.previous, can_assign);
}

void consume_property_name_after_dot(void) {
    if (check(TOKEN_IDENTIFIER) || check(TOKEN_YIELD)) {
        advance();
        return;
    }
    error_at_current("Expect property name after '.'.");
}

static void dot(int can_assign) {
    last_expr_ends_with_call = 0;
    int base_top = type_stack_top - 1;
    consume_property_name_after_dot();
    uint8_t name = identifier_constant(&parser.previous);
    
    if (can_assign && (match(TOKEN_EQUALS) || match(TOKEN_WALRUS))) {
        TokenType assign_tok = parser.previous.type;
        int start_line = parser.current.line;
        if (assign_tok == TOKEN_EQUALS && rhs_has_top_level_comma(start_line)) {
            parse_array_literal_from_comma_list();
        } else {
            expression();
        }
        emit_bytes(OP_SET_FIELD_CONST, name);
        {
            uint8_t rhs_type = type_pop();
            type_stack_top = base_top;
            type_push(rhs_type);
        }
    } else if (check(TOKEN_LEFT_PAREN) && can_emit_invoke_positional()) {
        advance(); // consume '('
        uint8_t arg_count = 0;
        int invoke_base_top = type_stack_top;
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                if (arg_count == 255) {
                    error("Can't have more than 255 arguments.");
                }
                expression();
                type_pop();
                arg_count++;
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
        emit_byte(OP_INVOKE);
        emit_byte(name);
        emit_byte(arg_count);
        last_expr_ends_with_call = 1;
        type_stack_top = invoke_base_top;
        type_pop();
        type_push(TYPEHINT_ANY);
    } else {
        emit_bytes(OP_GET_FIELD_CONST, name);
        type_stack_top = base_top;
        type_push(TYPEHINT_ANY);
    }
}

static void meta_dot(int can_assign) {
    last_expr_ends_with_call = 0;
    int base_top = type_stack_top - 1;
    consume_property_name_after_dot();
    uint8_t name = identifier_constant(&parser.previous);

    if (can_assign && (match(TOKEN_EQUALS) || match(TOKEN_WALRUS))) {
        error("Can't assign through metatable method access.");
        expression();
        type_stack_top = base_top;
        type_push(TYPEHINT_ANY);
        return;
    }

    emit_bytes(OP_CONSTANT, name);
    emit_byte(OP_GET_META_TABLE);
    type_stack_top = base_top;
    type_push(TYPEHINT_ANY);
}

static void subscript(int can_assign) {
    last_expr_ends_with_call = 0;
    int base_top = type_stack_top - 1;
    if (has_slice_range_in_subscript()) {
        if (check(TOKEN_DOT_DOT)) {
            advance();
            emit_byte(OP_NIL); // start
        } else {
            // Parse the start bound as a full arithmetic expression and stop before '..'.
            parse_precedence(PREC_TERM);
            consume(TOKEN_DOT_DOT, "Expect '..' in slice.");
        }
        if (check(TOKEN_COLON) || check(TOKEN_RIGHT_BRACKET)) {
            emit_byte(OP_NIL); // end
        } else {
            expression(); // end
        }
        if (match(TOKEN_COLON)) {
            if (check(TOKEN_RIGHT_BRACKET)) {
                emit_constant(NUMBER_VAL(1));
            } else {
                expression(); // step
            }
        } else {
            emit_constant(NUMBER_VAL(1));
        }
        consume(TOKEN_RIGHT_BRACKET, "Expect ']' after slice.");
        if (can_assign && (match(TOKEN_EQUALS) || match(TOKEN_WALRUS))) {
            error("Can't assign to a slice.");
            expression();
        }
        emit_byte(OP_SLICE);
        type_stack_top = base_top;
        type_push(TYPEHINT_ANY);
        last_expr_ends_with_call = 0;
        return;
    }
    expression();
    uint8_t index_type = type_pop();

    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

    if (can_assign && (match(TOKEN_EQUALS) || match(TOKEN_WALRUS))) {
        TokenType assign_tok = parser.previous.type;
        int start_line = parser.current.line;
        if (assign_tok == TOKEN_EQUALS && rhs_has_top_level_comma(start_line)) {
            parse_array_literal_from_comma_list();
        } else {
            expression();
        }
        if (is_numeric_type(index_type)) {
            emit_byte(OP_SET_TABLE_NUM);
        } else {
            emit_byte(OP_SET_TABLE);
        }
        {
            uint8_t rhs_type = type_pop();
            type_stack_top = base_top;
            type_push(rhs_type);
        }
    } else {
        if (is_numeric_type(index_type)) {
            emit_byte(OP_GET_TABLE_NUM);
        } else {
            emit_byte(OP_GET_TABLE);
        }
        type_stack_top = base_top;
        type_push(TYPEHINT_ANY);
    }
}

static void parse_table_entries();
static void table(int can_assign);
static int is_table_comprehension_start(int start_line);
static int is_generator_comprehension_start(int start_line);
static void table_comprehension(int can_assign);
static void generator_comprehension(int can_assign);
static void compile_expression_from_string(const char* src_start, size_t src_len);
static int find_comprehension_for_until(TokenType end_token, const char** for_start);
static const char* find_comprehension_assign(const char* expr_start, const char* expr_end);
static int is_implicit_table_separator(void);

static int is_table_entry_start(TokenType type) {
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

static int is_implicit_table_separator(void) {
    if (parser.current.line <= parser.previous.line) return 0;
    return is_table_entry_start(parser.current.type);
}

static void table_entry_expression(void) {
    int saved = in_table_entry_expression;
    in_table_entry_expression = 1;
    expression();
    in_table_entry_expression = saved;
}

static void parse_table_entries() {
    double array_index = 1.0;
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_LEFT_BRACKET)) {
            table_entry_expression();
            consume(TOKEN_RIGHT_BRACKET, "Expect ']' after key.");
            consume(TOKEN_EQUALS, "Expect '=' after key.");
            table_entry_expression();
            emit_byte(OP_SET_TABLE_KEEP);
        } else if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            if (match(TOKEN_EQUALS)) {
                uint8_t key = identifier_constant(&name);
                table_entry_expression();
                emit_bytes(OP_SET_FIELD_CONST_KEEP, key);
            } else {
                // Array item that happens to be an identifier
                emit_constant(NUMBER_VAL(array_index++));
                named_variable(name, false);
                emit_byte(OP_SET_TABLE_KEEP);
            }
        } else if (match(TOKEN_LESS)) {
            consume(TOKEN_IDENTIFIER, "Expect identifier after '<' in shorthand table fields.");
            for (;;) {
                Token name = parser.previous;
                uint8_t key = identifier_constant(&name);
                named_variable(name, false);
                emit_bytes(OP_SET_FIELD_CONST_KEEP, key);

                if (!match(TOKEN_COMMA)) break;
                if (check(TOKEN_GREATER)) break;
                consume(TOKEN_IDENTIFIER, "Expect identifier in shorthand table fields.");
            }
            consume(TOKEN_GREATER, "Expect '>' after shorthand table fields.");
        } else {
            // Array item
            emit_constant(NUMBER_VAL(array_index++));
            table_entry_expression();
            emit_byte(OP_SET_TABLE_KEEP);
        }
        if (match(TOKEN_COMMA) || is_implicit_table_separator()) continue;
        break;
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after table.");
}

static void table(int can_assign) {
    (void)can_assign;
    int base_top = type_stack_top;
    if (is_table_comprehension_start(parser.previous.line)) {
        table_comprehension(can_assign);
        type_stack_top = base_top;
        type_push(TYPEHINT_TABLE);
        return;
    }
    emit_byte(OP_NEW_TABLE);
    parse_table_entries();
    type_stack_top = base_top;
    type_push(TYPEHINT_TABLE);
}

static void table_infix(int can_assign) {
    (void)can_assign;
    int base_top = type_stack_top;
    // Left side (metatable) is already on stack
    emit_byte(OP_NEW_TABLE);
    parse_table_entries();
    emit_byte(OP_SET_METATABLE);
    type_stack_top = base_top - 1;
    type_push(TYPEHINT_TABLE);
}

static void grouping(int can_assign);
static void parse_call(int can_assign);
static void unary(int can_assign);
static void binary(int can_assign);
static void number(int can_assign);
static void literal(int can_assign);
static void string(int can_assign);
static void variable(int can_assign);
static void table(int can_assign);
static void dot(int can_assign);
static void subscript(int can_assign);

static void and_(int can_assign) {
    (void)can_assign;
    type_pop();
    int end_jump = emit_jump(OP_JUMP_IF_FALSE);

    emit_byte(OP_POP);
    parse_precedence(PREC_AND);

    patch_jump(end_jump);
    type_pop();
    type_push(TYPEHINT_ANY);
    last_expr_ends_with_call = 0;
}

static void or_(int can_assign) {
    (void)can_assign;
    type_pop();
    int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    int end_jump = emit_jump(OP_JUMP);

    patch_jump(else_jump);
    emit_byte(OP_POP);

    parse_precedence(PREC_OR);
    patch_jump(end_jump);
    type_pop();
    type_push(TYPEHINT_ANY);
    last_expr_ends_with_call = 0;
}

static void ternary(int can_assign) {
    (void)can_assign;

    // At this point, condition is on stack
    type_pop();
    // Jump to false branch if condition is false
    int else_branch = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP); // Pop condition

    // Parse true expression (higher precedence to avoid consuming another ?)
    parse_precedence(PREC_TERNARY + 1);

    consume(TOKEN_COLON, "Expect ':' after true branch of ternary operator.");

    // Jump over false branch
    int end_jump = emit_jump(OP_JUMP);

    // False branch
    patch_jump(else_branch);
    emit_byte(OP_POP); // Pop condition

    // Parse false expression (same precedence for right-associativity)
    parse_precedence(PREC_TERNARY);

    patch_jump(end_jump);
    {
        uint8_t false_type = type_pop();
        uint8_t true_type = type_pop();
        type_push(true_type == false_type ? true_type : TYPEHINT_ANY);
    }
    last_expr_ends_with_call = 0;
}

static void import_expression(int can_assign) {
    (void)can_assign;
    // Parse: import module_name[.submodule...]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'import'.");

    // Build the full dotted module path
    char module_path[256];
    int len = 0;

    // Copy first identifier
    int first_len = parser.previous.length;
    if (first_len >= 256) first_len = 255;
    memcpy(module_path, parser.previous.start, first_len);
    len = first_len;

    // Parse additional .submodule components
    while (match(TOKEN_DOT)) {
        if (len < 255) module_path[len++] = '.';
        consume(TOKEN_IDENTIFIER, "Expect module name after '.'.");
        int part_len = parser.previous.length;
        if (len + part_len >= 256) part_len = 255 - len;
        if (part_len > 0) {
            memcpy(module_path + len, parser.previous.start, part_len);
            len += part_len;
        }
    }
    module_path[len] = '\0';

    // Create a string constant with the full path
    ObjString* path_string = copy_string(module_path, len);
    uint8_t path_constant = make_constant(OBJ_VAL(path_string));

    // Emit OP_IMPORT with the full path
    emit_bytes(OP_IMPORT, path_constant);
    type_push(TYPEHINT_ANY);
}

static void anonymous_function(int can_assign);

static void range_(int can_assign) {
    (void)can_assign;
    parse_precedence(PREC_TERM);
    if (in_for_range_header) {
        last_expr_was_range = 1;
        return;
    }
    emit_byte(OP_RANGE);
    type_pop();
    type_pop();
    type_push(TYPEHINT_ANY);
    last_expr_ends_with_call = 0;
}

static void compile_expression_from_string(const char* src_start, size_t src_len) {
    while (src_len > 0 && (*src_start == ' ' || *src_start == '\t' || *src_start == '\r' || *src_start == '\n')) {
        src_start++;
        src_len--;
    }
    while (src_len > 0) {
        char c = src_start[src_len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            src_len--;
            continue;
        }
        break;
    }

    char* expr_src = (char*)malloc(src_len + 1);
    memcpy(expr_src, src_start, src_len);
    expr_src[src_len] = '\0';

    Parser saved_parser = parser;
    Lexer saved_lexer = lexer;
    int saved_last_call = last_expr_ends_with_call;
    int saved_type_top = type_stack_top;

    init_lexer(&lexer, expr_src);
    parser.had_error = 0;
    parser.panic_mode = 0;
    advance();
    expression();

    free(expr_src);
    parser = saved_parser;
    lexer = saved_lexer;
    last_expr_ends_with_call = saved_last_call;
    type_stack_top = saved_type_top;
}

static int find_comprehension_for_until(TokenType end_token, const char** for_start) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scan_token(&peek);
        switch (tok.type) {
            case TOKEN_FN:
                // Skip function literal: fn(params) body.
                // The body creates a new scope; 'for' inside it should not be
                // treated as a generator comprehension at the outer level.
                // 1. Skip fn( params )
                {
                    int fn_paren = 1;  // Expect '(' next
                    for (;;) {
                        Token t = scan_token(&peek);
                        if (t.type == TOKEN_LEFT_PAREN) {
                            fn_paren++;
                        } else if (t.type == TOKEN_RIGHT_PAREN) {
                            fn_paren--;
                            if (fn_paren == 0) break;
                        } else if (t.type == TOKEN_EOF) {
                            return 0;
                        }
                    }
                }
                // 2. Skip function body (indented block or same-line expression)
                {
                    int body_depth = 0;
                    for (;;) {
                        Token t = scan_token(&peek);
                        switch (t.type) {
                            case TOKEN_EOF:
                                return 0;
                            case TOKEN_INDENT:
                                body_depth++;
                                break;
                            case TOKEN_DEDENT:
                                if (body_depth > 0) {
                                    body_depth--;
                                    if (body_depth == 0) goto after_fn_body;
                                } else {
                                    // DEDENT without matching INDENT means we've
                                    // left the function's scope entirely
                                    goto after_fn_body;
                                }
                                break;
                            default:
                                if (body_depth == 0) {
                                    // Same-line expression body; first token read.
                                    // Stop and let the outer loop continue scanning.
                                    goto after_fn_body;
                                }
                                break;
                        }
                    }
                }
                after_fn_body:
                // Fall through to outer loop; next iteration calls scan_token.
                break;
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_PAREN:
                if (paren > 0) {
                    paren--;
                    break;
                }
                if (end_token == TOKEN_RIGHT_PAREN && bracket == 0 && brace == 0) {
                    return 0;
                }
                break;
            case TOKEN_RIGHT_BRACE:
                if (brace > 0) {
                    brace--;
                    break;
                }
                if (end_token == TOKEN_RIGHT_BRACE && paren == 0 && bracket == 0) {
                    return 0;
                }
                break;
            case TOKEN_FOR:
                if (paren == 0 && bracket == 0 && brace == 0) {
                    *for_start = tok.start;
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

static const char* find_comprehension_assign(const char* expr_start, const char* expr_end) {
    int paren = 0, bracket = 0, brace = 0;
    int in_single = 0;
    int in_double = 0;
    int escaped = 0;

    for (const char* p = expr_start; p < expr_end; p++) {
        char c = *p;
        if (escaped) {
            escaped = 0;
            continue;
        }
        if ((in_single || in_double) && c == '\\') {
            escaped = 1;
            continue;
        }
        if (in_single) {
            if (c == '\'') in_single = 0;
            continue;
        }
        if (in_double) {
            if (c == '"') in_double = 0;
            continue;
        }

        if (c == '\'') {
            in_single = 1;
            continue;
        }
        if (c == '"') {
            in_double = 1;
            continue;
        }

        if (c == '(') {
            paren++;
            continue;
        }
        if (c == ')' && paren > 0) {
            paren--;
            continue;
        }
        if (c == '[') {
            bracket++;
            continue;
        }
        if (c == ']' && bracket > 0) {
            bracket--;
            continue;
        }
        if (c == '{') {
            brace++;
            continue;
        }
        if (c == '}' && brace > 0) {
            brace--;
            continue;
        }

        if (c == '=' && paren == 0 && bracket == 0 && brace == 0) {
            return p;
        }
    }
    return NULL;
}

static int has_slice_range_in_subscript(void) {
    if (parser.current.type == TOKEN_DOT_DOT) return 1;
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scan_token(&peek);
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

static int rhs_has_top_level_comma(int start_line) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    // The lexer is already past parser.current, so account for it first.
    Token tok = parser.current;
    if (tok.type == TOKEN_EOF) return 0;
    if (tok.line > start_line && paren == 0 && bracket == 0 && brace == 0) return 0;
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
        tok = scan_token(&peek);
        if (tok.type == TOKEN_EOF) return 0;
        if (tok.line > start_line && paren == 0 && bracket == 0 && brace == 0) return 0;
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

static int can_emit_invoke_positional(void) {
    if (!check(TOKEN_LEFT_PAREN)) return 0;
    Lexer peek = lexer;
    Token tok;
    int depth = 0;
    int prev_identifier = 0;
    int expect_arg_start = 1;
    for (;;) {
        tok = scan_token(&peek);
        if (tok.type == TOKEN_EOF) return 0;
        if (tok.type == TOKEN_LEFT_PAREN) {
            depth++;
            prev_identifier = 0;
            continue;
        }
        if (tok.type == TOKEN_RIGHT_PAREN) {
            if (depth == 0) return 1;
            depth--;
            prev_identifier = 0;
            continue;
        }
        if (depth == 0) {
            if (tok.type == TOKEN_COMMA) {
                expect_arg_start = 1;
                prev_identifier = 0;
                continue;
            }
            if (tok.type == TOKEN_STAR && expect_arg_start) return 0;
            if (tok.type == TOKEN_EQUALS && prev_identifier) return 0;
            expect_arg_start = 0;
        }
        prev_identifier = (tok.type == TOKEN_IDENTIFIER);
    }
}

static void parse_array_literal_from_comma_list(void) {
    emit_byte(OP_NEW_TABLE);
    int index = 1;
    do {
        emit_byte(OP_DUP);
        emit_constant(NUMBER_VAL(index++));
        expression();
        emit_byte(OP_SET_TABLE);
        emit_byte(OP_POP);
    } while (match(TOKEN_COMMA));
}

static int is_table_comprehension_start(int start_line) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scan_token(&peek);
        if (tok.line > start_line && paren == 0 && bracket == 0 && brace == 0) {
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

static int is_generator_comprehension_start(int start_line) {
    Lexer peek = lexer;
    int paren = 0, bracket = 0, brace = 0;
    for (;;) {
        Token tok = scan_token(&peek);
        if (tok.line > start_line && paren == 0 && bracket == 0 && brace == 0) {
            return 0;
        }
        switch (tok.type) {
            case TOKEN_LEFT_PAREN: paren++; break;
            case TOKEN_RIGHT_PAREN:
                if (paren == 0 && bracket == 0 && brace == 0) return 0;
                if (paren > 0) paren--;
                break;
            case TOKEN_LEFT_BRACKET: bracket++; break;
            case TOKEN_RIGHT_BRACKET: if (bracket > 0) bracket--; break;
            case TOKEN_LEFT_BRACE: brace++; break;
            case TOKEN_RIGHT_BRACE: if (brace > 0) brace--; break;
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

static void generator_comprehension(int can_assign) {
    (void)can_assign;

    const char* expr_start = parser.current.start;
    const char* for_start = NULL;
    if (!find_comprehension_for_until(TOKEN_RIGHT_PAREN, &for_start)) {
        error("Expected generator comprehension 'expr for ...'.");
        return;
    }
    size_t expr_len = (size_t)(for_start - expr_start);

    while (!(parser.current.type == TOKEN_FOR && parser.current.start == for_start)) {
        if (parser.current.type == TOKEN_EOF) {
            error("Expected 'for' in generator comprehension.");
            return;
        }
        advance();
    }

    Compiler compiler;
    Compiler* enclosing = current;
    init_compiler(&compiler, TYPE_FUNCTION);
    begin_scope();
    current->function->is_generator = 1;

    consume(TOKEN_FOR, "Expect 'for' in generator comprehension.");

    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    int has_index_sigil = 0;
    if (check(TOKEN_HASH)) {
        const char* expected = name.start + name.length;
        if (parser.current.start == expected) {
            advance();
            has_index_sigil = 1;
        } else {
            error_at_current("Whitespace is not allowed before '#'.");
            advance();
            has_index_sigil = 1;
        }
    }

    Token loop_vars[2];
    int var_count = 1;
    loop_vars[0] = name;

    if (match(TOKEN_COMMA)) {
        consume(TOKEN_IDENTIFIER, "Expect second variable name.");
        loop_vars[var_count++] = parser.previous;
    }

    consume(TOKEN_IN, "Expect 'in'.");

    int expr_count = 0;
    int is_range_expr = 0;
    int eligible_for_range = (var_count == 1 && !has_index_sigil);
    in_for_range_header = eligible_for_range;
    expression();
    in_for_range_header = 0;
    expr_count = 1;
    is_range_expr = eligible_for_range && last_expr_was_range;

    if (is_range_expr && check(TOKEN_COMMA)) {
        error("Range expression cannot be used with multiple iterator expressions.");
        return;
    }
    if (is_range_expr) {
        emit_byte(OP_RANGE);
    }

    while (match(TOKEN_COMMA) && expr_count < 3) {
        expression();
        expr_count++;
    }

    if (expr_count == 1 && !is_range_expr) {
        Token iterable_token = {TOKEN_IDENTIFIER, "(iterable)", 10, parser.previous.line};
        add_local(iterable_token);
        mark_initialized();
        int iterable_slot = current->local_count - 1;
        emit_bytes(OP_GET_LOCAL, (uint8_t)iterable_slot);
    }

    if (is_range_expr) {
        // OP_RANGE already produced iter/state/control.
    } else if (expr_count > 1) {
        while (expr_count < 3) {
            emit_byte(OP_NIL);
            expr_count++;
        }
    } else {
        if (has_index_sigil) {
            emit_byte(OP_ITER_PREP_IPAIRS);
        } else {
            emit_byte(OP_ITER_PREP);
        }
    }

    if (has_index_sigil && expr_count > 1) {
        error("Index loop syntax 'i#' only works with implicit table iteration.");
    }

    if (var_count == 1 && !has_index_sigil) {
        Token key_token = {TOKEN_IDENTIFIER, "(key)", 5, parser.previous.line};
        loop_vars[1] = loop_vars[0];
        loop_vars[0] = key_token;
        var_count = 2;
    }

    Token iter_token = {TOKEN_IDENTIFIER, "(iter)", 6, parser.previous.line};
    Token state_token = {TOKEN_IDENTIFIER, "(state)", 7, parser.previous.line};
    Token control_token = {TOKEN_IDENTIFIER, "(control)", 9, parser.previous.line};

    int iter_slot = current->local_count;
    add_local(iter_token);
    int state_slot = current->local_count;
    add_local(state_token);
    int control_slot = current->local_count;
    add_local(control_token);
    mark_initialized_count(3);

    int loop_start = current_chunk()->count;

    emit_bytes(OP_GET_LOCAL, (uint8_t)iter_slot);
    emit_bytes(OP_GET_LOCAL, (uint8_t)state_slot);
    emit_bytes(OP_GET_LOCAL, (uint8_t)control_slot);
    emit_call(2);

    for (int i = var_count; i < 2; i++) {
        emit_byte(OP_POP);
    }

    for (int i = 0; i < var_count; i++) {
        add_local(loop_vars[i]);
    }
    mark_initialized_count(var_count);

    emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
    emit_byte(OP_NIL);
    emit_byte(OP_EQUAL);
    int exit_jump = emit_jump(OP_JUMP_IF_TRUE);
    emit_byte(OP_POP);

    emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
    emit_bytes(OP_SET_LOCAL, (uint8_t)(current->local_count - var_count - 1));
    emit_byte(OP_POP);

    int has_if = 0;
    int skip_jump = -1;
    int end_jump = -1;
    if (match(TOKEN_IF)) {
        has_if = 1;
        expression();
        skip_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP);
    }

    ObjString* coroutine_module = copy_string("coroutine", 9);
    Token yield_token = {TOKEN_IDENTIFIER, "yield", 5, parser.previous.line};
    emit_bytes(OP_IMPORT, make_constant(OBJ_VAL(coroutine_module)));
    emit_bytes(OP_CONSTANT, identifier_constant(&yield_token));
    emit_byte(OP_GET_TABLE);
    compile_expression_from_string(expr_start, expr_len);
    emit_call(1);

    if (has_if) {
        end_jump = emit_jump(OP_JUMP);
        patch_jump(skip_jump);
        emit_byte(OP_POP);
        patch_jump(end_jump);
    }

    for (int i = 0; i < var_count; i++) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }

    emit_loop(loop_start);

    patch_jump(exit_jump);
    for (int i = 0; i < var_count; i++) {
        emit_byte(OP_POP);
    }
    emit_byte(OP_POP);

    ObjFunction* function = end_compiler();
    current = enclosing;
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
    for (int i = 0; i < compiler.upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }
    emit_call(0);
    type_push(TYPEHINT_ANY);
    last_expr_ends_with_call = 1;
}

static void table_comprehension(int can_assign) {
    (void)can_assign;

    const char* expr_start = parser.current.start;
    const char* for_start = NULL;
    if (!find_comprehension_for_until(TOKEN_RIGHT_BRACE, &for_start)) {
        error("Expected table comprehension 'expr for ...'.");
        return;
    }
    size_t expr_len = (size_t)(for_start - expr_start);

    while (!(parser.current.type == TOKEN_FOR && parser.current.start == for_start)) {
        if (parser.current.type == TOKEN_EOF) {
            error("Expected 'for' in table comprehension.");
            return;
        }
        advance();
    }

    Compiler compiler;
    Compiler* enclosing = current;
    init_compiler(&compiler, TYPE_FUNCTION);
    begin_scope();

    emit_byte(OP_NEW_TABLE);
    Token list_token = {TOKEN_IDENTIFIER, "(list)", 6, parser.previous.line};
    add_local(list_token);
    mark_initialized();
    int list_slot = current->local_count - 1;

    emit_constant(NUMBER_VAL(1));
    Token idx_token = {TOKEN_IDENTIFIER, "(idx)", 5, parser.previous.line};
    add_local(idx_token);
    mark_initialized();
    int idx_slot = current->local_count - 1;

    consume(TOKEN_FOR, "Expect 'for' in table comprehension.");

    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    int has_index_sigil = 0;
    if (check(TOKEN_HASH)) {
        const char* expected = name.start + name.length;
        if (parser.current.start == expected) {
            advance();
            has_index_sigil = 1;
        } else {
            error_at_current("Whitespace is not allowed before '#'.");
            advance();
            has_index_sigil = 1;
        }
    }

    Token loop_vars[2];
    int var_count = 1;
    loop_vars[0] = name;

    if (match(TOKEN_COMMA)) {
        consume(TOKEN_IDENTIFIER, "Expect second variable name.");
        loop_vars[var_count++] = parser.previous;
    }

    consume(TOKEN_IN, "Expect 'in'.");

    int expr_count = 0;
    int is_range_expr = 0;
    int eligible_for_range = (var_count == 1 && !has_index_sigil);
    in_for_range_header = eligible_for_range;
    expression();
    in_for_range_header = 0;
    expr_count = 1;
    is_range_expr = eligible_for_range && last_expr_was_range;

    if (is_range_expr && check(TOKEN_COMMA)) {
        error("Range expression cannot be used with multiple iterator expressions.");
        return;
    }
    if (is_range_expr && expr_count == 1) {
        // Fast path for `{expr for i in a..b [if cond]}`:
        // lower to numeric for-loop opcodes so quickening/JIT can trigger.
        add_local(name);
        Token end_token = {TOKEN_IDENTIFIER, "(end)", 5, parser.previous.line};
        add_local(end_token);
        mark_initialized_count(2);
        int var_slot = current->local_count - 2;
        int end_slot = current->local_count - 1;
        set_local_type(var_slot, TYPEHINT_FLOAT);
        set_local_type(end_slot, TYPEHINT_FLOAT);

        int loop_start = current_chunk()->count;

        emit_byte(OP_FOR_PREP);
        emit_byte((uint8_t)var_slot);
        emit_byte((uint8_t)end_slot);
        emit_byte(0);
        emit_byte(0);
        int exit_jump = current_chunk()->count - 2;

        int has_if = 0;
        int skip_jump = -1;
        int end_jump = -1;
        if (match(TOKEN_IF)) {
            has_if = 1;
            expression();
            skip_jump = emit_jump(OP_JUMP_IF_FALSE);
            emit_byte(OP_POP);
        }

        const char* expr_end = for_start;
        const char* assign = find_comprehension_assign(expr_start, expr_end);
        if (assign != NULL) {
            emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
            compile_expression_from_string(expr_start, (size_t)(assign - expr_start));
            compile_expression_from_string(assign + 1, (size_t)(expr_end - (assign + 1)));
            emit_byte(OP_SET_TABLE);
            emit_byte(OP_POP);
        } else {
            emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
            emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
            compile_expression_from_string(expr_start, expr_len);
            emit_byte(OP_SET_TABLE_NUM);
            emit_byte(OP_POP);

            emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
            emit_constant(NUMBER_VAL(1));
            emit_byte(OP_ADD);
            emit_bytes(OP_SET_LOCAL, (uint8_t)idx_slot);
            emit_byte(OP_POP);
        }

        if (has_if) {
            end_jump = emit_jump(OP_JUMP);
            patch_jump(skip_jump);
            emit_byte(OP_POP);
            patch_jump(end_jump);
        }

        consume(TOKEN_RIGHT_BRACE, "Expect '}' after table comprehension.");

        emit_byte(OP_FOR_LOOP);
        emit_byte((uint8_t)var_slot);
        emit_byte((uint8_t)end_slot);
        emit_byte(0);
        emit_byte(0);
        {
            int loop_offset = current_chunk()->count - loop_start;
            current_chunk()->code[current_chunk()->count - 2] = (uint8_t)((loop_offset >> 8) & 0xff);
            current_chunk()->code[current_chunk()->count - 1] = (uint8_t)(loop_offset & 0xff);
        }
        {
            int jump = current_chunk()->count - (exit_jump + 2);
            current_chunk()->code[exit_jump] = (uint8_t)((jump >> 8) & 0xff);
            current_chunk()->code[exit_jump + 1] = (uint8_t)(jump & 0xff);
        }

        emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);

        ObjFunction* function = end_compiler();
        current = enclosing;
        emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
        for (int i = 0; i < compiler.upvalue_count; i++) {
            emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
            emit_byte(compiler.upvalues[i].index);
        }
        emit_call(0);
        last_expr_ends_with_call = 1;
        return;
    }
    if (is_range_expr) {
        // In range-aware headers, `1..n` leaves start/end on stack.
        // Convert that pair into iterator triplet for generic for-in path.
        emit_byte(OP_RANGE);
    }

    while (match(TOKEN_COMMA) && expr_count < 3) {
        expression();
        expr_count++;
    }

    if (expr_count == 1 && !is_range_expr) {
        Token iterable_token = {TOKEN_IDENTIFIER, "(iterable)", 10, parser.previous.line};
        add_local(iterable_token);
        mark_initialized();
        int iterable_slot = current->local_count - 1;
        emit_bytes(OP_GET_LOCAL, (uint8_t)iterable_slot);
    }

    if (is_range_expr) {
        // Already have iterator triplet from OP_RANGE.
    } else if (expr_count > 1) {
        while (expr_count < 3) {
            emit_byte(OP_NIL);
            expr_count++;
        }
    } else {
        if (has_index_sigil) {
            emit_byte(OP_ITER_PREP_IPAIRS);
        } else {
            emit_byte(OP_ITER_PREP);
        }
    }

    if (has_index_sigil && expr_count > 1) {
        error("Index loop syntax 'i#' only works with implicit table iteration.");
    }

    if (var_count == 1 && !has_index_sigil) {
        Token key_token = {TOKEN_IDENTIFIER, "(key)", 5, parser.previous.line};
        loop_vars[1] = loop_vars[0];
        loop_vars[0] = key_token;
        var_count = 2;
    }

    Token iter_token = {TOKEN_IDENTIFIER, "(iter)", 6, parser.previous.line};
    Token state_token = {TOKEN_IDENTIFIER, "(state)", 7, parser.previous.line};
    Token control_token = {TOKEN_IDENTIFIER, "(control)", 9, parser.previous.line};

    int iter_slot = current->local_count;
    add_local(iter_token);
    int state_slot = current->local_count;
    add_local(state_token);
    int control_slot = current->local_count;
    add_local(control_token);
    mark_initialized_count(3);

    int loop_start = current_chunk()->count;

    emit_bytes(OP_GET_LOCAL, (uint8_t)iter_slot);
    emit_bytes(OP_GET_LOCAL, (uint8_t)state_slot);
    emit_bytes(OP_GET_LOCAL, (uint8_t)control_slot);
    emit_call(2);

    for (int i = var_count; i < 2; i++) {
        emit_byte(OP_POP);
    }

    for (int i = 0; i < var_count; i++) {
        add_local(loop_vars[i]);
    }
    mark_initialized_count(var_count);

    emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
    emit_byte(OP_NIL);
    emit_byte(OP_EQUAL);
    int exit_jump = emit_jump(OP_JUMP_IF_TRUE);
    emit_byte(OP_POP);

    emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
    emit_bytes(OP_SET_LOCAL, (uint8_t)(current->local_count - var_count - 1));
    emit_byte(OP_POP);

    int has_if = 0;
    int skip_jump = -1;
    int end_jump = -1;
    if (match(TOKEN_IF)) {
        has_if = 1;
        expression();
        skip_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP);
    }

    const char* expr_end = for_start;
    const char* assign = find_comprehension_assign(expr_start, expr_end);
    if (assign != NULL) {
        emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
        compile_expression_from_string(expr_start, (size_t)(assign - expr_start));
        compile_expression_from_string(assign + 1, (size_t)(expr_end - (assign + 1)));
        emit_byte(OP_SET_TABLE);
        emit_byte(OP_POP);
    } else {
        emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
        emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
        compile_expression_from_string(expr_start, expr_len);
        emit_byte(OP_SET_TABLE);
        emit_byte(OP_POP);

        emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
        emit_constant(NUMBER_VAL(1));
        emit_byte(OP_ADD);
        emit_bytes(OP_SET_LOCAL, (uint8_t)idx_slot);
        emit_byte(OP_POP);
    }

    if (has_if) {
        end_jump = emit_jump(OP_JUMP);
        patch_jump(skip_jump);
        emit_byte(OP_POP);
        patch_jump(end_jump);
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after table comprehension.");

    for (int i = 0; i < var_count; i++) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }

    emit_loop(loop_start);

    patch_jump(exit_jump);
    for (int i = 0; i < var_count; i++) {
        emit_byte(OP_POP);
    }
    emit_byte(OP_POP);

    emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);

    ObjFunction* function = end_compiler();
    current = enclosing;
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
    for (int i = 0; i < compiler.upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }
    emit_call(0);
    last_expr_ends_with_call = 1;
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, parse_call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {table,    table_infix, PREC_CALL},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACKET]  = {NULL,     subscript, PREC_CALL},
    [TOKEN_RIGHT_BRACKET] = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
    [TOKEN_DOT_DOT]       = {NULL,     range_, PREC_RANGE},
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
    [TOKEN_ARROW]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_EQUALS]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_HASH]          = {unary,    NULL,   PREC_NONE},
    [TOKEN_QUESTION]      = {NULL,     ternary, PREC_TERNARY},
    [TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COLON_COLON]   = {NULL,     meta_dot, PREC_CALL},
    [TOKEN_WALRUS]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_PLUS_EQUAL]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_MINUS_EQUAL]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_STAR_EQUAL]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH_EQUAL]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_PERCENT_EQUAL] = {NULL,     NULL,   PREC_NONE},
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
    [TOKEN_IN]            = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_HAS]           = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
    [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
    [TOKEN_FSTRING]       = {fstring,  NULL,   PREC_NONE},
    [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
    [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOKEN_FN]            = {anonymous_function, NULL, PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_YIELD]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
    [TOKEN_NOT]           = {unary,    not_in, PREC_COMPARISON},
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
    [TOKEN_IMPORT]        = {import_expression, NULL, PREC_NONE},
    [TOKEN_FROM]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DEL]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ASSERT]        = {NULL,     NULL,   PREC_NONE},
};

static void parse_precedence(Precedence precedence) {
    advance();
    ParseFn prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expect expression.");
        return;
    }
    int can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(can_assign);
    while (precedence <= get_rule(parser.current.type)->precedence) {
        if (in_table_entry_expression &&
            parser.current.line > parser.previous.line &&
            is_table_entry_start(parser.current.type)) {
            break;
        }
        advance();
        ParseFn infix_rule = get_rule(parser.previous.type)->infix;
        infix_rule(can_assign);
    }
    if (can_assign) {
        TokenType ignored;
        if (match(TOKEN_EQUALS) || match(TOKEN_WALRUS) || match_compound_assign(&ignored)) {
            error("Invalid assignment target.");
        }
    }
}

static ParseRule* get_rule(TokenType type) {
    return &rules[type];
}

void expression(void) {
    last_expr_ends_with_call = 0;
    last_expr_was_range = 0;
    parse_precedence(PREC_ASSIGNMENT);
}

void variable_declaration(void) {
    // Collect variable names
    uint8_t globals[UINT8_MAX + 1];
    int var_count = 0;

    do {
        globals[var_count] = parse_variable("Expect variable name.");
        var_count++;
        if (var_count > 255) {
            error("Too many variables in declaration.");
            return;
        }
    } while (match(TOKEN_COMMA));

    // Now handle initialization
    if (match(TOKEN_EQUALS)) {
        // Emit the RHS expression(s)
        int expr_count = 0;
        int start_line = parser.current.line;
        if (var_count == 1 && rhs_has_top_level_comma(start_line)) {
            parse_array_literal_from_comma_list();
            expr_count = 1;
        } else {
            do {
                type_stack_top = 0;
                expression();
                expr_count++;
            } while (match(TOKEN_COMMA));
        }

        // Pad with nils for missing values
        // Note: if expr_count==1 and it's a function call returning multiple values,
        // those values will naturally fill the slots. If it returns fewer than var_count,
        // the remaining slots will be undefined (implementation limitation for now).
        if (expr_count > 1) {
            // Multiple expressions: pad any missing values with nil
            while (expr_count < var_count) {
                emit_byte(OP_NIL);
                expr_count++;
            }
        } else if (var_count == 1) {
            // Single variable: no padding needed
        }
        // else: single expression, multiple variables - trust it returns enough values
    } else {
        // No initialization - set all to nil
        for (int i = 0; i < var_count; i++) {
            emit_byte(OP_NIL);
        }
    }

    if (current->scope_depth > 0) {
        mark_initialized_count(var_count);
    }

    // Define variables in order (reverse for globals because of popping)
    for (int i = var_count - 1; i >= 0; i--) {
        define_variable(globals[i]);
    }
}

static ObjFunction* function_body(FunctionType type) {
    
    Compiler compiler;
    Compiler* enclosing = current; 
    init_compiler(&compiler, type);
    begin_scope(); 

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    int defaults_start = current->function->defaults_count;
    int param_index = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            // Check for variadic parameter (*name)
            if (match(TOKEN_STAR)) {
                current->function->is_variadic = 1;
                current->function->arity++;  // The varargs table counts as one parameter

                // Parse the varargs parameter name
                uint8_t constant = parse_variable("Expect parameter name after '*'.");
                Token param_name_token = parser.previous;
                if (param_index == 0 && parser.previous.length == 4 &&
                    memcmp(parser.previous.start, "self", 4) == 0) {
                    current->function->is_self = 1;
                }
                param_index++;
                if (match(TOKEN_COLON)) {
                    consume(TOKEN_IDENTIFIER, "Expect type name after ':'.");
                    uint8_t type = parse_type_name(&parser.previous);
                    set_local_type(current->local_count - 1, type);
                    set_param_type(current->function, current->function->arity - 1, type);
                }
                set_param_name(current->function, current->function->arity - 1, &param_name_token);
                define_variable(constant);
                break;  // *args must be the last parameter
            }

            current->function->arity++;
#ifdef DEBUG_COMPILER
            printf("Parsing param, arity: %d\n", current->function->arity);
#endif
            if (current->function->arity > 255) {
                error_at_current("Can't have more than 255 parameters.");
            }
            uint8_t constant = parse_variable("Expect parameter name.");
            Token param_name_token = parser.previous;
            if (param_index == 0 && parser.previous.length == 4 &&
                memcmp(parser.previous.start, "self", 4) == 0) {
                current->function->is_self = 1;
            }
            param_index++;
            if (match(TOKEN_COLON)) {
                consume(TOKEN_IDENTIFIER, "Expect type name after ':'.");
                uint8_t type = parse_type_name(&parser.previous);
                set_local_type(current->local_count - 1, type);
                set_param_type(current->function, current->function->arity - 1, type);
            }
            set_param_name(current->function, current->function->arity - 1, &param_name_token);
            
            if (match(TOKEN_EQUALS)) {
                if (match(TOKEN_NUMBER)) {
                    double num = parse_number_token(parser.previous);
                    current->function->defaults_count++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaults_count);
                    current->function->defaults[current->function->defaults_count - 1] = NUMBER_VAL(num);
                } else if (match(TOKEN_STRING)) {
                    ObjString* str = string_from_token(parser.previous);
                    current->function->defaults_count++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaults_count);
                    current->function->defaults[current->function->defaults_count - 1] = OBJ_VAL(str);
                } else if (match(TOKEN_NIL)) {
                    current->function->defaults_count++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaults_count);
                    current->function->defaults[current->function->defaults_count - 1] = NIL_VAL;
                } else if (match(TOKEN_TRUE)) {
                    current->function->defaults_count++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaults_count);
                    current->function->defaults[current->function->defaults_count - 1] = NUMBER_VAL(1);
                } else if (match(TOKEN_FALSE)) {
                    current->function->defaults_count++;
                    current->function->defaults = (Value*)realloc(current->function->defaults, sizeof(Value) * current->function->defaults_count);
                    current->function->defaults[current->function->defaults_count - 1] = NIL_VAL;
                } else {
                    error("Default value must be a constant (number, string, nil, true, false).");
                }
            } else if (defaults_start < current->function->defaults_count) {
                error("Parameters with defaults cannot be followed by parameters without defaults.");
            }
            
            define_variable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    if (match(TOKEN_ARROW)) {
        consume(TOKEN_IDENTIFIER, "Expect return type name after '->'.");
        current->function->return_type = parse_type_name(&parser.previous);
    }
    seed_parameter_local_types(current);

    // Parameters are initialized at function entry.
    for (int i = 0; i < current->local_count; i++) {
        if (current->locals[i].depth == -1) {
            current->locals[i].depth = current->scope_depth;
        }
    }
    int header_line = parser.previous.line;
    
    if (match(TOKEN_INDENT)) {
        maybe_capture_function_docstring();
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > header_line && !in_table_entry_expression) {
            error("Expected indented block for function body.");
        }
        if (parser.current.line > header_line && in_table_entry_expression) {
            int header_indent = token_indent(parser.previous);
            int body_indent = token_indent(parser.current);
            if (body_indent <= header_indent) {
                error("Expected indented block for function body.");
            } else {
                maybe_capture_function_docstring();
                while (!check(TOKEN_EOF) &&
                       !check(TOKEN_RIGHT_BRACE) &&
                       parser.current.line > header_line &&
                       token_indent(parser.current) > header_indent) {
                    statement();
                }
            }
        } else {
            maybe_capture_function_docstring();
            if (!check(TOKEN_EOF) && !check(TOKEN_DEDENT) && !check(TOKEN_RIGHT_BRACE)) {
                statement();
            }
        }
    }
    
    ObjFunction* function = end_compiler();
    current = enclosing;
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));

    // Emit upvalue information
    for (int i = 0; i < compiler.upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }
    return function;
}

typedef struct {
    const char* start;
    size_t length;
} DecoratorSpan;

static Token function_declaration_named(void) {
    uint8_t global = parse_variable("Expect function name.");
    Token name = parser.previous;
    int local_index = -1;
    if (current->scope_depth > 0) {
        local_index = current->local_count - 1;
        mark_initialized();
    }
    ObjFunction* function = function_body(TYPE_FUNCTION);
    if (local_index >= 0 && local_index < current->local_count) {
        current->locals[local_index].is_function_value = 1;
        current->locals[local_index].function_return_type = function->return_type;
    }
    define_variable(global);
    return name;
}

void function_declaration(void) {
    (void)function_declaration_named();
}

static void anonymous_function(int can_assign) {
    (void)can_assign;
    function_body(TYPE_FUNCTION);
    type_push(TYPEHINT_ANY);
}

void global_declaration(void) {
    uint8_t globals[UINT8_MAX + 1];
    int var_count = 0;

    do {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        register_explicit_global(parser.previous);
        globals[var_count] = identifier_constant(&parser.previous);
        var_count++;
        if (var_count > 255) {
            error("Too many variables in declaration.");
            return;
        }
    } while (match(TOKEN_COMMA));

    if (current->type == TYPE_FUNCTION && !check(TOKEN_EQUALS)) {
        // Python-like `global x` only declares binding intent for this function.
        return;
    }

    if (match(TOKEN_EQUALS)) {
        int expr_count = 0;
        int start_line = parser.current.line;
        if (var_count == 1 && rhs_has_top_level_comma(start_line)) {
            parse_array_literal_from_comma_list();
            expr_count = 1;
        } else {
            do {
                type_stack_top = 0;
                expression();
                expr_count++;
            } while (match(TOKEN_COMMA));
        }

        if (expr_count > 1) {
            while (expr_count < var_count) {
                emit_byte(OP_NIL);
                expr_count++;
            }
        }
    } else {
        for (int i = 0; i < var_count; i++) {
            emit_byte(OP_NIL);
        }
    }

    for (int i = var_count - 1; i >= 0; i--) {
        emit_bytes(OP_DEFINE_GLOBAL, globals[i]);
    }
}

static Token global_function_declaration_named(void) {
    consume(TOKEN_IDENTIFIER, "Expect function name.");
    Token name = parser.previous;
    uint8_t global = identifier_constant(&parser.previous);
    function_body(TYPE_FUNCTION);
    emit_bytes(OP_DEFINE_GLOBAL, global);
    return name;
}

void global_function_declaration(void) {
    (void)global_function_declaration_named();
}

static void apply_decorators(Token function_name, DecoratorSpan* decorators, int decorator_count) {
    for (int i = decorator_count - 1; i >= 0; i--) {
        compile_expression_from_string(decorators[i].start, decorators[i].length);
        emit_get_named(function_name);
        emit_call(1);
        emit_set_named(function_name);
        emit_byte(OP_POP);
    }
}

void decorated_function_declaration(void) {
    DecoratorSpan decorators[64];
    int decorator_count = 0;

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

        if (decorator_count == 64) {
            error("Too many decorators on function.");
            return;
        }
        decorators[decorator_count].start = start;
        decorators[decorator_count].length = (size_t)(end - start);
        decorator_count++;

        if (!match(TOKEN_AT)) break;
    }

    Token function_name;
    if (match(TOKEN_FN)) {
        function_name = function_declaration_named();
    } else if (match(TOKEN_LOCAL)) {
        consume(TOKEN_FN, "Expect 'fn' after 'local' in decorated declaration.");
        function_name = function_declaration_named();
    } else if (match(TOKEN_GLOBAL)) {
        consume(TOKEN_FN, "Expect 'fn' after 'global' in decorated declaration.");
        function_name = global_function_declaration_named();
    } else {
        error("Decorators can only be applied to function declarations.");
        return;
    }

    apply_decorators(function_name, decorators, decorator_count);
}

static void parse_call(int can_assign) {
    (void)can_assign;
    uint8_t arg_count = 0;
    int in_named_args = 0;
    int has_spread_arg = 0;
    int base_top = type_stack_top;
    uint8_t callee_return_hint = TYPEHINT_ANY;
    Chunk* chunk = current_chunk();
    if (chunk->count >= 2) {
        uint8_t op = chunk->code[chunk->count - 2];
        if (op == OP_GET_LOCAL) {
            int local_index = chunk->code[chunk->count - 1];
            if (local_index >= 0 &&
                local_index < current->local_count &&
                current->locals[local_index].is_function_value) {
                callee_return_hint = current->locals[local_index].function_return_type;
            }
        }
    }
    
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (match(TOKEN_STAR)) {
                if (in_named_args) {
                    error("Spread argument cannot be used with named arguments.");
                }
                if (has_spread_arg) {
                    error("Can't use more than one spread argument.");
                }
                if (arg_count == 255) {
                    error("Can't have more than 255 arguments.");
                }
                expression();
                type_pop();
                has_spread_arg = 1;
                if (check(TOKEN_COMMA)) {
                    error("Spread argument must be last.");
                }
                continue;
            }

            // Check if current argument is named: identifier = ...
            int is_named = 0;
            if (parser.current.type == TOKEN_IDENTIFIER) {
                Lexer peek_lexer = lexer;
                Token next = scan_token(&peek_lexer);
                if (next.type == TOKEN_EQUALS) {
                    is_named = 1;
                }
            }

            if (is_named) {
                if (has_spread_arg) {
                    error("Named arguments cannot follow spread argument.");
                }
                if (!in_named_args) {
                    emit_byte(OP_NEW_TABLE); // Start the options table
                    in_named_args = 1;
                }
                
                // Parse named arg: name = expr
                consume(TOKEN_IDENTIFIER, "Expect parameter name.");
                Token name = parser.previous;
                consume(TOKEN_EQUALS, "Expect '=' after parameter name.");
                
                emit_byte(OP_DUP); // Duplicate table for set operation
                emit_constant(OBJ_VAL(copy_string(name.start, name.length))); // Key
                expression(); // Value
                type_pop();
                emit_byte(OP_SET_TABLE);
                emit_byte(OP_POP); // Pop the value pushed by SET_TABLE
                
            } else {
                if (in_named_args) {
                    error("Positional arguments cannot follow named arguments.");
                }
                if (has_spread_arg) {
                    error("Positional arguments cannot follow spread argument.");
                }
                if (arg_count == 0 && parser.current.type != TOKEN_LEFT_PAREN) {
                    const char* for_start = NULL;
                    if (find_comprehension_for_until(TOKEN_RIGHT_PAREN, &for_start)) {
                        generator_comprehension(can_assign);
                        type_pop();
                        arg_count++;
                        break;
                    }
                }
                expression();
                type_pop();
                if (arg_count == 255) {
                    error("Can't have more than 255 arguments.");
                }
                arg_count++;
            }
        } while (match(TOKEN_COMMA));
    }
    
    // If we collected named args, the table is on the stack as the last argument
    if (in_named_args) {
        if (arg_count == 255) {
             error("Can't have more than 255 arguments.");
        }
        arg_count++;
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    if (has_spread_arg) {
        emit_bytes(OP_CALL_EXPAND, arg_count);
    } else if (in_named_args) {
        emit_bytes(OP_CALL_NAMED, arg_count);
    } else {
        emit_call(arg_count);
    }
    last_expr_ends_with_call = 1;
    type_stack_top = base_top;
    type_pop();
    type_push(callee_return_hint);
}

static void reset_parser_state(const char* source) {
    parser.had_error = 0;
    parser.panic_mode = 0;
    parser.current.type = TOKEN_ERROR;
    parser.current.start = source;
    parser.current.length = 0;
    parser.current.line = 1;
    parser.previous.type = TOKEN_ERROR;
    parser.previous.start = source;
    parser.previous.length = 0;
    parser.previous.line = 1;
}

static ObjFunction* compile_with_mode(const char* source, int repl_mode) {
    // CRITICAL: Reset current to NULL before starting a new compilation
    // Otherwise init_compiler will set compiler->enclosing to a dangling pointer
    // from the previous compilation
    current = NULL;
    is_repl_mode = repl_mode;
    type_stack_top = 0;

    init_lexer(&lexer, source);
    Compiler compiler;
    init_compiler(&compiler, TYPE_SCRIPT);

    // Reset parser state completely to avoid stale pointers from previous compilation
    reset_parser_state(source);

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = end_compiler();
    if (!parser.had_error && function != NULL) {
        optimize_chunk(&function->chunk);
    }
    is_repl_mode = 0;
    return parser.had_error ? NULL : function;
}

ObjFunction* compile(const char* source) {
    return compile_with_mode(source, 0);
}

ObjFunction* compile_repl(const char* source) {
    return compile_with_mode(source, 1);
}
