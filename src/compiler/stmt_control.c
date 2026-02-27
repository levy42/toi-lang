#include <string.h>

#include "internal.h"
#include "stmt_control.h"
#include "stmt.h"

static void print_statement() {
    uint8_t arg_count = 0;

    if (match(TOKEN_LEFT_PAREN)) {
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                type_stack_top = 0;
                expression();
                if (arg_count == 255) {
                    error("Can't print more than 255 values.");
                    return;
                }
                arg_count++;
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after print arguments.");
    } else {
        do {
            type_stack_top = 0;
            expression();
            if (arg_count == 255) {
                error("Can't print more than 255 values.");
                return;
            }
            arg_count++;
        } while (match(TOKEN_COMMA));
    }

    emit_bytes(OP_PRINT, arg_count);
}

static void delete_variable(Token name) {
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        emit_byte(OP_NIL);
        emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
        emit_byte(OP_POP);
        return;
    }
    arg = resolve_upvalue(current, &name);
    if (arg != -1) {
        emit_byte(OP_NIL);
        emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
        emit_byte(OP_POP);
        return;
    }
    uint8_t global = identifier_constant(&name);
    emit_bytes(OP_DELETE_GLOBAL, global);
}

static void delete_access_chain(void) {
    int deleted = 0;
    for (;;) {
        if (match(TOKEN_DOT)) {
            consume_property_name_after_dot();
            uint8_t name = identifier_constant(&parser.previous);
            emit_bytes(OP_CONSTANT, name);
        } else if (match(TOKEN_LEFT_BRACKET)) {
            expression();
            consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
        } else {
            if (!deleted) error("Expect property or index to delete.");
            return;
        }

        if (check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET)) {
            emit_byte(OP_GET_TABLE);
        } else {
            emit_byte(OP_DELETE_TABLE);
            deleted = 1;
            return;
        }
    }
}

static void del_statement() {
    do {
        if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            if (check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET)) {
                named_variable(name, 0);
                delete_access_chain();
            } else {
                delete_variable(name);
            }
        } else if (match(TOKEN_LEFT_PAREN)) {
            expression();
            consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
            if (!(check(TOKEN_DOT) || check(TOKEN_LEFT_BRACKET))) {
                error("Expect property or index to delete.");
                return;
            }
            delete_access_chain();
        } else {
            error("Expect variable or table access after 'del'.");
            return;
        }
    } while (match(TOKEN_COMMA));
}

static void expression_statement() {
    type_stack_top = 0;
    expression();
    // In REPL mode, leave the last expression result on stack so it can be printed
    // In normal mode, pop the result
    if (!is_repl_mode) {
        // For multi-return functions, we need to clean up all return values
        // Pop one value, then adjust stack to correct position
        emit_byte(OP_POP);
        // Ensure stack matches our local count
        if (current->scope_depth > 0) {
            emit_bytes(OP_ADJUST_STACK, current->local_count);
        }
    }
}

void block(void) {
    while (!check(TOKEN_ELSE) && !check(TOKEN_ELIF) && 
           !check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        declaration();
    }
}

void block(void);
void statement(void);
static void try_statement();
static void throw_statement();
static void yield_statement();
static void with_statement();
static void match_statement();
static Token synthetic_token(const char* name);
static int is_multi_assignment_statement(void);
static void multi_assignment_statement(void);

static int match_identifier_keyword(const char* keyword) {
    int length = (int)strlen(keyword);
    if (!check(TOKEN_IDENTIFIER)) return 0;
    if (parser.current.length != length) return 0;
    if (memcmp(parser.current.start, keyword, (size_t)length) != 0) return 0;
    advance();
    return 1;
}

static int tokens_equal(Token a, Token b) {
    return a.length == b.length && memcmp(a.start, b.start, (size_t)a.length) == 0;
}

static int token_indent_local(Token token) {
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

static void parse_statement_suite(int header_line, const char* indent_error) {
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
        return;
    }

    if (parser.current.line > header_line) {
        if (!in_table_entry_expression) {
            error(indent_error);
            statement();
            return;
        }

        int header_indent = token_indent_local(parser.previous);
        int body_indent = token_indent_local(parser.current);
        if (body_indent <= header_indent) {
            error(indent_error);
            statement();
            return;
        }

        while (!check(TOKEN_EOF) &&
               !check(TOKEN_RIGHT_BRACE) &&
               !check(TOKEN_DEDENT) &&
               parser.current.line > header_line &&
               token_indent_local(parser.current) > header_indent) {
            statement();
        }
        return;
    }

    statement();
}

static void assign_name_from_stack(Token name, uint8_t rhs_type) {
    int arg = resolve_local(current, &name);
    if (arg != -1) {
        emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
        update_local_type(arg, rhs_type);
        return;
    }

    if (is_explicit_global_name(current, &name)) {
        emit_bytes(OP_SET_GLOBAL, identifier_constant(&name));
        return;
    }

    if (current->type == TYPE_FUNCTION) {
        arg = resolve_upvalue(current, &name);
        if (arg != -1) {
            emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
            return;
        }

        // Function assignment binds local unless explicit global.
        int local_index = current->local_count;
        add_local(name);
        mark_initialized();
        emit_bytes(OP_SET_LOCAL, (uint8_t)local_index);
        set_local_type(local_index, rhs_type);
        return;
    }

    arg = resolve_upvalue(current, &name);
    if (arg != -1) {
        emit_bytes(OP_SET_UPVALUE, (uint8_t)arg);
        return;
    }

    if (is_repl_mode && current->type == TYPE_SCRIPT) {
        arg = identifier_constant(&name);
        emit_byte(OP_DUP);
        emit_bytes(OP_DEFINE_GLOBAL, (uint8_t)arg);
        return;
    }

    int local_index = current->local_count;
    add_local(name);
    mark_initialized();
    emit_bytes(OP_SET_LOCAL, (uint8_t)local_index);
    set_local_type(local_index, rhs_type);
}

static int is_multi_assignment_statement(void) {
    if (!check(TOKEN_IDENTIFIER)) return 0;

    int start_line = parser.current.line;
    int target_count = 1;
    Lexer peek = lexer;

    for (;;) {
        Token tok = scan_token(&peek);
        if (tok.line > start_line) return 0;

        if (tok.type == TOKEN_COMMA) {
            tok = scan_token(&peek);
            if (tok.line > start_line) return 0;
            if (tok.type != TOKEN_IDENTIFIER) return 0;
            target_count++;
            continue;
        }

        return tok.type == TOKEN_EQUALS && target_count > 1;
    }
}

static void multi_assignment_statement(void) {
    Token targets[UINT8_MAX + 1];
    int target_count = 0;

    do {
        consume(TOKEN_IDENTIFIER, "Expect variable name.");
        targets[target_count++] = parser.previous;
        if (target_count > 255) {
            error("Too many variables in assignment.");
            return;
        }
    } while (match(TOKEN_COMMA));

    if (!(is_repl_mode && current->type == TYPE_SCRIPT)) {
        int declared = 0;
        for (int i = 0; i < target_count; i++) {
            if (is_explicit_global_name(current, &targets[i])) continue;
            if (resolve_local(current, &targets[i]) != -1) continue;

            int seen = 0;
            for (int j = 0; j < i; j++) {
                if (tokens_equal(targets[j], targets[i])) {
                    seen = 1;
                    break;
                }
            }
            if (seen) continue;

            add_local(targets[i]);
            declared++;
        }
        if (declared > 0) {
            mark_initialized_count(declared);
        }
    }

    consume(TOKEN_EQUALS, "Expect '=' in assignment.");

    // Normalize evaluation stack to local slot depth before RHS evaluation.
    emit_bytes(OP_ADJUST_STACK, (uint8_t)current->local_count);

    int expr_count = 0;
    do {
        type_stack_top = 0;
        expression();
        expr_count++;
    } while (match(TOKEN_COMMA));

    // For explicit multi-expression RHS, pad missing values with nil.
    if (expr_count > 1) {
        while (expr_count < target_count) {
            emit_byte(OP_NIL);
            expr_count++;
        }
    } else {
        // Normalize single-expression RHS for multi-assignment:
        // - preserve multi-return call values
        // - expand single table RHS into positional values
        // - pad missing values with nil
        emit_byte(OP_UNPACK);
        emit_byte((uint8_t)current->local_count);
        emit_byte((uint8_t)target_count);
    }

    // Assign from right to left so stack order lines up with targets.
    for (int i = target_count - 1; i >= 0; i--) {
        assign_name_from_stack(targets[i], TYPEHINT_ANY);
        emit_byte(OP_POP);
    }

    // Keep evaluation stack above local slots even when RHS returned fewer
    // values than targets (avoids clobbering live locals on later pushes).
    emit_bytes(OP_ADJUST_STACK, (uint8_t)current->local_count);
}

static void if_statement() {
    type_stack_top = 0;
    expression();
    int header_line = parser.previous.line;
    
    int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP); 
    
    begin_scope();
    parse_statement_suite(header_line, "Expected indented block after 'if'.");
    end_scope();
    int else_jump = emit_jump(OP_JUMP);
    
    patch_jump(then_jump);
    emit_byte(OP_POP); 
    
    if (match(TOKEN_ELIF)) {
        if_statement();
    } else {
        if (match(TOKEN_ELSE)) {
            int else_line = parser.previous.line;
            begin_scope();
            parse_statement_suite(else_line, "Expected indented block after 'else'.");
            end_scope();
        }
    }
    
    patch_jump(else_jump);
}

static void match_statement() {
    begin_scope();

    type_stack_top = 0;
    expression();
    Token match_value_token = synthetic_token("$match_value");
    int match_slot = current->local_count;
    add_local(match_value_token);
    mark_initialized();
    type_stack_top = 0;

    consume(TOKEN_INDENT, "Expected indented block after 'match'.");

    int clause_end_jumps[UINT8_MAX + 1];
    int clause_end_count = 0;
    int pending_case_fail_jump = -1;
    int saw_clause = 0;
    int saw_else = 0;

    while (!check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        if (pending_case_fail_jump != -1) {
            patch_jump(pending_case_fail_jump);
            emit_byte(OP_POP); // Pop failed comparison result.
            pending_case_fail_jump = -1;
        }

        if (match_identifier_keyword("case")) {
            if (saw_else) {
                error("Can't have 'case' after 'else' in match.");
                break;
            }

            saw_clause = 1;
            type_stack_top = 0;
            emit_bytes(OP_GET_LOCAL, (uint8_t)match_slot);
            expression();
            emit_byte(OP_EQUAL);

            int case_fail_jump = emit_jump(OP_JUMP_IF_FALSE);
            emit_byte(OP_POP); // Pop successful comparison result.

            int case_line = parser.previous.line;
            begin_scope();
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > case_line) {
                    error("Expected indented block after 'case'.");
                }
                statement();
            }
            end_scope();

            if (clause_end_count >= UINT8_MAX + 1) {
                error("Too many clauses in match statement.");
            } else {
                clause_end_jumps[clause_end_count++] = emit_jump(OP_JUMP);
            }
            pending_case_fail_jump = case_fail_jump;
            continue;
        }

        if (match(TOKEN_ELSE)) {
            if (saw_else) {
                error("Can't have multiple 'else' clauses in match.");
                break;
            }

            saw_clause = 1;
            saw_else = 1;

            int else_line = parser.previous.line;
            begin_scope();
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > else_line) {
                    error("Expected indented block after 'else'.");
                }
                statement();
            }
            end_scope();
            break;
        }

        error("Expect 'case' or 'else' in match block.");
        break;
    }

    if (pending_case_fail_jump != -1) {
        patch_jump(pending_case_fail_jump);
        emit_byte(OP_POP); // Pop failed comparison result.
    }

    consume(TOKEN_DEDENT, "Expect end of match block.");

    for (int i = 0; i < clause_end_count; i++) {
        patch_jump(clause_end_jumps[i]);
    }

    end_scope();

    if (!saw_clause) {
        error("Match block must contain at least one clause.");
    }
}

static void try_statement() {
    uint8_t depth = (uint8_t)current->local_count;
    TryPatch handler_offset = emit_try(depth);
    int header_line = parser.previous.line;

    begin_scope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > header_line) {
            error("Expected indented block after 'try'.");
        }
        statement();
    }
    end_scope();

    if (!check(TOKEN_EXCEPT) && !check(TOKEN_FINALLY)) {
        error("Expect 'except' or 'finally' after try block.");
        return;
    }

    emit_byte(OP_END_TRY);

    int has_except = 0;
    int has_finally = 0;
    int after_try_jump = -1;

    if (match(TOKEN_EXCEPT)) {
        has_except = 1;
        after_try_jump = emit_jump(OP_JUMP);

        patch_try(handler_offset.except_offset);

        begin_scope();
        int except_local = -1;
        int has_filter = 0;
        int filter_fail_jump = -1;
        int after_except_jump = -1;
        if (match(TOKEN_IDENTIFIER)) {
            Token name = parser.previous;
            add_local(name);
            mark_initialized();
            except_local = current->local_count - 1;
            emit_bytes(OP_SET_LOCAL, (uint8_t)except_local);
        } else {
            emit_byte(OP_POP);
        }

        if (match(TOKEN_IF)) {
            if (except_local < 0) {
                error("Filtered except requires an exception variable: use 'except e if ...'.");
            } else {
                has_filter = 1;
                type_stack_top = 0;
                expression();
                filter_fail_jump = emit_jump(OP_JUMP_IF_FALSE);
                emit_byte(OP_POP); // condition when true
            }
        }

        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            int except_line = parser.previous.line;
            if (parser.current.line > except_line) {
                error("Expected indented block after 'except'.");
            }
            statement();
        }

        if (has_filter) {
            after_except_jump = emit_jump(OP_JUMP);
            patch_jump(filter_fail_jump);
            emit_byte(OP_POP); // condition when false
            emit_bytes(OP_GET_LOCAL, (uint8_t)except_local);
            emit_byte(OP_THROW);
            patch_jump(after_except_jump);
        }

        end_scope();
        emit_byte(OP_END_TRY);
    }

    if (match(TOKEN_FINALLY)) {
        has_finally = 1;
        if (has_except && after_try_jump != -1) {
            patch_jump(after_try_jump);
        }

        patch_try_finally(handler_offset.finally_offset);

        begin_scope();
        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            int finally_line = parser.previous.line;
            if (parser.current.line > finally_line) {
                error("Expected indented block after 'finally'.");
            }
            statement();
        }
        end_scope();
        emit_byte(OP_END_FINALLY);
    } else if (has_except && after_try_jump != -1) {
        patch_jump(after_try_jump);
    }

    current_chunk()->code[handler_offset.flags_offset] =
        (uint8_t)((has_except ? 1 : 0) | (has_finally ? 2 : 0));
}

static Token synthetic_token(const char* name) {
    Token token;
    token.start = name;
    token.length = (int)strlen(name);
    token.line = parser.previous.line;
    token.type = TOKEN_IDENTIFIER;
    return token;
}

static void with_statement() {
    begin_scope();
    type_stack_top = 0;

    expression();

    Token ctx_token = synthetic_token("$with_ctx");
    int ctx_slot = current->local_count;
    add_local(ctx_token);
    mark_initialized();

    Token enter_token = synthetic_token("__enter");
    emit_bytes(OP_GET_LOCAL, (uint8_t)ctx_slot);
    emit_bytes(OP_CONSTANT, identifier_constant(&enter_token));
    emit_byte(OP_GET_TABLE);
    int skip_enter = emit_jump(OP_JUMP_IF_FALSE);
    emit_call(0);
    int after_enter = emit_jump(OP_JUMP);
    patch_jump(skip_enter);
    emit_byte(OP_POP);
    emit_bytes(OP_GET_LOCAL, (uint8_t)ctx_slot);
    patch_jump(after_enter);

    if (match(TOKEN_AS)) {
        consume(TOKEN_IDENTIFIER, "Expect name after 'as'.");
        Token name = parser.previous;
        int local = resolve_local(current, &name);
        if (local != -1) {
            emit_bytes(OP_SET_LOCAL, (uint8_t)local);
            emit_byte(OP_POP);
        } else {
            int upvalue = resolve_upvalue(current, &name);
            if (upvalue != -1) {
                emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue);
                emit_byte(OP_POP);
            } else {
                add_local(name);
                mark_initialized();
            }
        }
    } else {
        emit_byte(OP_POP);
    }

    Token ex_token = synthetic_token("$with_ex");
    int ex_slot = current->local_count;
    emit_byte(OP_NIL);
    add_local(ex_token);
    mark_initialized();

    uint8_t depth = (uint8_t)current->local_count;
    TryPatch handler = emit_try(depth);
    int header_line = parser.previous.line;

    begin_scope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > header_line) {
            error("Expected indented block after 'with'.");
        }
        statement();
    }
    end_scope();

    emit_byte(OP_END_TRY);
    int after_try_jump = emit_jump(OP_JUMP);

    patch_try(handler.except_offset);
    emit_bytes(OP_SET_LOCAL, (uint8_t)ex_slot);
    emit_bytes(OP_GET_LOCAL, (uint8_t)ex_slot);
    emit_byte(OP_THROW);

    patch_jump(after_try_jump);
    patch_try_finally(handler.finally_offset);

    Token exit_token = synthetic_token("__exit");
    emit_bytes(OP_GET_LOCAL, (uint8_t)ctx_slot);
    emit_bytes(OP_CONSTANT, identifier_constant(&exit_token));
    emit_byte(OP_GET_TABLE);
    int skip_exit = emit_jump(OP_JUMP_IF_FALSE);
    emit_bytes(OP_GET_LOCAL, (uint8_t)ex_slot);
    emit_call(1);
    emit_byte(OP_POP);
    int after_exit = emit_jump(OP_JUMP);
    patch_jump(skip_exit);
    emit_byte(OP_POP);
    patch_jump(after_exit);

    emit_byte(OP_END_FINALLY);
    current_chunk()->code[handler.flags_offset] = (uint8_t)(1 | 2);
    end_scope();
}

static void while_statement() {
    LoopContext loop;
    loop.start = current_chunk()->count;
    loop.scope_depth = current->scope_depth;
    loop.break_count = 0;
    loop.continue_count = 0;
    loop.is_for_loop = 0;
    loop.enclosing = current->loop_context;
    current->loop_context = &loop;

    type_stack_top = 0;
    expression();
    int header_line = parser.previous.line;

    int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);

    begin_scope();
    if (match(TOKEN_INDENT)) {
        block();
        match(TOKEN_DEDENT);
    } else {
        if (parser.current.line > header_line) {
            error("Expected indented block after 'while'.");
        }
        statement();
    }
    end_scope();

    emit_loop(loop.start);

    patch_jump(exit_jump);
    emit_byte(OP_POP); // Pop condition

    // Patch all break jumps
    for (int i = 0; i < loop.break_count; i++) {
        patch_jump(loop.break_jumps[i]);
    }

    current->loop_context = loop.enclosing;
}

static void return_statement() {
    if (current->type == TYPE_SCRIPT) {
        // Optional: allow return from script (exit)
        // error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON)) {
        emit_return();
    } else {
        if (check(TOKEN_ELSE) || check(TOKEN_ELIF) || check(TOKEN_EOF)) {
             emit_byte(OP_NIL);
             emit_byte(OP_RETURN);
        } else {
            // Count return expressions
            int value_count = 0;
            do {
                type_stack_top = 0;
                expression();
                value_count++;
            } while (match(TOKEN_COMMA));

            if (value_count == 1) {
                emit_byte(OP_RETURN);
            } else {
                emit_bytes(OP_RETURN_N, value_count);
            }
        }
    }
}

static void for_statement() {
    LoopContext loop;
    loop.break_count = 0;
    loop.continue_count = 0;
    loop.is_for_loop = 1;
    loop.slots_to_pop = 0;

    begin_scope();
    loop.scope_depth = current->scope_depth;
    loop.enclosing = current->loop_context;

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

    // Check if this is a for-in loop or numeric for loop
    if (match(TOKEN_COMMA) || check(TOKEN_IN)) {
        // For-in loop: for k, v in ... or for k in ...
        // We already consumed the first identifier, need to handle it
        Token loop_vars[2];
        int var_count = 1;
        loop_vars[0] = name;

        if (parser.previous.type == TOKEN_COMMA) {
            consume(TOKEN_IDENTIFIER, "Expect second variable name.");
            loop_vars[var_count++] = parser.previous;
        }

        consume(TOKEN_IN, "Expect 'in'.");

        // Parse iterator expression(s)
        // The iterator protocol expects 3 values: function, state, control
        // If one expression: assume it returns all 3 (e.g., custom iterator) - don't pad!
        // If two expressions: pad with one nil
        // If three expressions: use as-is
        int expr_count = 0;
        int is_range_expr = 0;
        int eligible_for_range = (var_count == 1 && !has_index_sigil);
        // First expression
        in_for_range_header = eligible_for_range;
        type_stack_top = 0;
        expression();
        in_for_range_header = 0;
        expr_count = 1;
        is_range_expr = eligible_for_range && last_expr_was_range;

        if (is_range_expr && check(TOKEN_COMMA)) {
            error("Range expression cannot be used with multiple iterator expressions.");
            current->loop_context = loop.enclosing;
            end_scope();
            return;
        }

        while (match(TOKEN_COMMA) && expr_count < 3) {
            type_stack_top = 0;
            expression();
            expr_count++;
        }
        
        int header_line = parser.previous.line;

        if (is_range_expr && expr_count == 1) {
            // Stack has: start, end (end is on top)
            add_local(name); // loop variable uses start (lower stack slot)
            Token end_token = {TOKEN_IDENTIFIER, "(end)", 5, parser.previous.line};
            add_local(end_token); // end uses top of stack
            mark_initialized_count(2);
            int var_slot = current->local_count - 2;
            int end_slot = current->local_count - 1;

            int loop_start = current_chunk()->count;
            loop.start = loop_start;
            current->loop_context = &loop;
            loop.slots_to_pop = 0;

            // for-prep: jump out if start > end
            emit_byte(OP_FOR_PREP);
            emit_byte((uint8_t)var_slot);
            emit_byte((uint8_t)end_slot);
            emit_byte(0);
            emit_byte(0);
            int exit_jump = current_chunk()->count - 2;

            // Body executes in its own scope each iteration so locals don't
            // interfere with loop-control locals.
            begin_scope();
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > header_line) {
                    error("Expected indented block after 'for'.");
                }
                statement();
            }
            end_scope();

            int loop_instr_offset = current_chunk()->count;
            for (int i = 0; i < loop.continue_count; i++) {
                int jump_offset = loop.continue_jumps[i];
                int jump = loop_instr_offset - (jump_offset + 2);
                current_chunk()->code[jump_offset] = (uint8_t)((jump >> 8) & 0xff);
                current_chunk()->code[jump_offset + 1] = (uint8_t)(jump & 0xff);
            }

            // for-loop: increment and jump back if <= end
            emit_byte(OP_FOR_LOOP);
            emit_byte((uint8_t)var_slot);
            emit_byte((uint8_t)end_slot);
            emit_byte(0);
            emit_byte(0);
            // patch backward jump
            {
                int loop_offset = current_chunk()->count - loop_start;
                current_chunk()->code[current_chunk()->count - 2] = (uint8_t)((loop_offset >> 8) & 0xff);
                current_chunk()->code[current_chunk()->count - 1] = (uint8_t)(loop_offset & 0xff);
            }

            // patch exit jump
            {
                int jump = current_chunk()->count - (exit_jump + 2);
                current_chunk()->code[exit_jump] = (uint8_t)((jump >> 8) & 0xff);
                current_chunk()->code[exit_jump + 1] = (uint8_t)(jump & 0xff);
            }

            for (int i = 0; i < loop.break_count; i++) {
                patch_jump(loop.break_jumps[i]);
            }

            current->loop_context = loop.enclosing;
            end_scope();
            return;
        }

        // Normalize single-expression iteration by materializing the iterable
        // into a hidden local first, then preparing iterator triple from that.
        if (expr_count == 1) {
            Token iterable_token = {TOKEN_IDENTIFIER, "(iterable)", 10, parser.previous.line};
            add_local(iterable_token);
            mark_initialized();
            int iterable_slot = current->local_count - 1;
            emit_bytes(OP_GET_LOCAL, (uint8_t)iterable_slot);
        }

        // Only pad if we have 2 or 3 expressions
        // If expr_count == 1, iterate that single value via OP_ITER_PREP.
        if (expr_count > 1) {
            while (expr_count < 3) {
                emit_byte(OP_NIL);
                expr_count++;
            }
        } else {
            // Single-expression for-in always means iterate that value.
            // Triplet iterator protocol remains available via explicit
            // `for ... in iter_fn, state, control`.
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

        // Create hidden locals in the order values appear on stack
        // Iterator prep yields: iterator_function, state, control
        Token iter_token = {TOKEN_IDENTIFIER, "(iter)", 6, parser.previous.line};
        Token state_token = {TOKEN_IDENTIFIER, "(state)", 7, parser.previous.line};
        Token control_token = {TOKEN_IDENTIFIER, "(control)", 9, parser.previous.line};

                // Save the indices for hidden locals (before adding them)

                int iter_slot = current->local_count;

        add_local(iter_token);    // First value: iterator function

        int state_slot = current->local_count;

        add_local(state_token);   // Second value: state

        int control_slot = current->local_count;

        add_local(control_token); // Third value: control variable

        mark_initialized_count(3);
        

                // Loop start

        
        int loop_start = current_chunk()->count;
        loop.start = loop_start;
        current->loop_context = &loop;
        loop.slots_to_pop = var_count;

        // Call iterator: iter(state, control)
        emit_bytes(OP_GET_LOCAL, (uint8_t)iter_slot);
        emit_bytes(OP_GET_LOCAL, (uint8_t)state_slot);
        emit_bytes(OP_GET_LOCAL, (uint8_t)control_slot);
        emit_call(2);

        // Iterators return 2 values (key, value). If we have fewer loop variables,
        // pop the extra values to keep the stack balanced.
        for (int i = var_count; i < 2; i++) {
            emit_byte(OP_POP);
        }

        // Create loop variables
        for (int i = 0; i < var_count; i++) {
            add_local(loop_vars[i]);
        }
        mark_initialized_count(var_count);

        // Check if first value is nil
        emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
        emit_byte(OP_NIL);
        emit_byte(OP_EQUAL);
        int exit_jump = emit_jump(OP_JUMP_IF_TRUE);
        emit_byte(OP_POP);

        // Update control variable to first return value
        emit_bytes(OP_GET_LOCAL, (uint8_t)(current->local_count - var_count));
        emit_bytes(OP_SET_LOCAL, (uint8_t)(current->local_count - var_count - 1)); // control is just before loop vars
        emit_byte(OP_POP);

        // Body executes in its own scope each iteration so locals don't
        // interfere with loop-control locals.
        begin_scope();
        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            if (parser.current.line > header_line) {
                error("Expected indented block after 'for'.");
            }
            statement();
        }
        end_scope();

        // Pop loop variables
        for (int i = 0; i < var_count; i++) {
            if (current->locals[current->local_count - 1].is_captured) {
                emit_byte(OP_CLOSE_UPVALUE);
            } else {
                emit_byte(OP_POP);
            }
            current->local_count--;
        }

        // Patch continue jumps
        for (int i = 0; i < loop.continue_count; i++) {
            patch_jump(loop.continue_jumps[i]);
        }

        emit_loop(loop_start);

        patch_jump(exit_jump);
        // Pop loop variables that weren't popped because we jumped here
        for (int i = 0; i < var_count; i++) {
            emit_byte(OP_POP);
        }
        emit_byte(OP_POP);  // Pop comparison result

        // Patch break jumps
        for (int i = 0; i < loop.break_count; i++) {
            patch_jump(loop.break_jumps[i]);
        }

        current->loop_context = loop.enclosing;
        end_scope();
        return;
    }

    error("Expect 'in' after loop variable.");
    return;
}

static void break_statement() {
    if (current->loop_context == NULL) {
        error("Can't use 'break' outside a loop.");
        return;
    }

    // Pop locals back to loop scope
    for (int i = current->local_count - 1; i >= 0 &&
         current->locals[i].depth > current->loop_context->scope_depth; i--) {
        if (current->locals[i].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
    }

    // Emit jump and save for patching
    int offset = emit_jump(OP_JUMP);
    current->loop_context->break_jumps[current->loop_context->break_count++] = offset;
}

static void continue_statement() {
    if (current->loop_context == NULL) {
        error("Can't use 'continue' outside a loop.");
        return;
    }

    // Pop locals back to loop scope
    for (int i = current->local_count - 1; i >= 0 &&
         current->locals[i].depth > current->loop_context->scope_depth; i--) {
        if (current->locals[i].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
    }

    // Pop extra slots if needed (e.g. for-in loop variables)
    for (int i = 0; i < current->loop_context->slots_to_pop; i++) {
        emit_byte(OP_POP);
    }

    if (current->loop_context->is_for_loop) {
        // For loops: emit forward jump to be patched later
        int offset = emit_jump(OP_JUMP);
        current->loop_context->continue_jumps[current->loop_context->continue_count++] = offset;
    } else {
        // While loops: jump back to start
        emit_loop(current->loop_context->start);
    }
}

static void throw_statement() {
    type_stack_top = 0;
    expression();
    emit_byte(OP_THROW);
}

static void yield_statement() {
    if (current->type == TYPE_SCRIPT) {
        error("Can't use 'yield' outside a function.");
        return;
    }
    current->function->is_generator = 1;

    Token coroutine_token = {TOKEN_IDENTIFIER, "coroutine", 9, parser.previous.line};
    Token yield_token = {TOKEN_IDENTIFIER, "yield", 5, parser.previous.line};
    emit_bytes(OP_GET_GLOBAL, identifier_constant(&coroutine_token));
    emit_bytes(OP_CONSTANT, identifier_constant(&yield_token));
    emit_byte(OP_GET_TABLE);

    int value_count = 0;
    if (!(check(TOKEN_ELSE) || check(TOKEN_ELIF) || check(TOKEN_DEDENT) || check(TOKEN_EOF))) {
        do {
            type_stack_top = 0;
            expression();
            value_count++;
        } while (match(TOKEN_COMMA));
    }

    emit_call((uint8_t)value_count);
}

static void assert_statement() {
    type_stack_top = 0;
    expression();

    int fail_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP); // condition when true
    int done_jump = emit_jump(OP_JUMP);

    patch_jump(fail_jump);
    emit_byte(OP_POP); // condition when false

    if (match(TOKEN_COMMA)) {
        type_stack_top = 0;
        expression();
    } else {
        emit_constant(OBJ_VAL(copy_string("assert failed", 13)));
    }
    emit_byte(OP_THROW);

    patch_jump(done_jump);
}

void statement(void) {
    if (match(TOKEN_PRINT)) {
        print_statement();
    } else if (match(TOKEN_IF)) {
        if_statement();
    } else if (match_identifier_keyword("match")) {
        match_statement();
    } else if (match(TOKEN_TRY)) {
        try_statement();
    } else if (match(TOKEN_WITH)) {
        with_statement();
    } else if (match(TOKEN_THROW)) {
        throw_statement();
    } else if (match(TOKEN_YIELD)) {
        yield_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_FOR)) {
        for_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_BREAK)) {
        break_statement();
    } else if (match(TOKEN_CONTINUE)) {
        continue_statement();
    } else if (match(TOKEN_GC)) {
        emit_byte(OP_GC);
    } else if (match(TOKEN_ASSERT)) {
        assert_statement();
    } else if (match(TOKEN_DEL)) {
        del_statement();
    } else if (is_multi_assignment_statement()) {
        multi_assignment_statement();
    } else {
        expression_statement();
    }
}
