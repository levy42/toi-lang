#include <string.h>

#include "internal.h"
#include "stmt.h"
#include "stmt_control.h"

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

void declaration(void) {
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
