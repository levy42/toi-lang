#include <string.h>

#include "internal.h"
#include "stmt_control.h"
#include "stmt.h"

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
            consumePropertyNameAfterDot();
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

void block(void) {
    while (!check(TOKEN_ELSE) && !check(TOKEN_ELIF) && 
           !check(TOKEN_DEDENT) && !check(TOKEN_EOF)) {
        declaration();
    }
}

void block(void);
void statement(void);
static void tryStatement();
static void throwStatement();
static void yieldStatement();
static void withStatement();
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
        // Always discard the assignment value in multi-assign to keep
        // the temporary RHS table at the top for the next target.
        emitByte(OP_POP);
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

            // Body executes in its own scope each iteration so locals don't
            // interfere with loop-control locals.
            beginScope();
            if (match(TOKEN_INDENT)) {
                block();
                match(TOKEN_DEDENT);
            } else {
                if (parser.current.line > headerLine) {
                    error("Expected indented block after 'for'.");
                }
                statement();
            }
            endScope();

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

        // Body executes in its own scope each iteration so locals don't
        // interfere with loop-control locals.
        beginScope();
        if (match(TOKEN_INDENT)) {
            block();
            match(TOKEN_DEDENT);
        } else {
            if (parser.current.line > headerLine) {
                error("Expected indented block after 'for'.");
            }
            statement();
        }
        endScope();

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

static void yieldStatement() {
    if (current->type == TYPE_SCRIPT) {
        error("Can't use 'yield' outside a function.");
        return;
    }

    Token coroutineToken = {TOKEN_IDENTIFIER, "coroutine", 9, parser.previous.line};
    Token yieldToken = {TOKEN_IDENTIFIER, "yield", 5, parser.previous.line};
    emitBytes(OP_GET_GLOBAL, identifierConstant(&coroutineToken));
    emitBytes(OP_CONSTANT, identifierConstant(&yieldToken));
    emitByte(OP_GET_TABLE);

    int valueCount = 0;
    if (!(check(TOKEN_ELSE) || check(TOKEN_ELIF) || check(TOKEN_DEDENT) || check(TOKEN_EOF))) {
        do {
            typeStackTop = 0;
            expression();
            valueCount++;
        } while (match(TOKEN_COMMA));
    }

    emitBytes(OP_CALL, (uint8_t)valueCount);
}

void statement(void) {
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
    } else if (match(TOKEN_YIELD)) {
        yieldStatement();
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
