#include <string.h>

#include "internal.h"
#include "stmt.h"
#include "stmt_control.h"

static Token parse_module_path(char module_path[256], int* out_len, const char* first_component_error) {
    consume(TOKEN_IDENTIFIER, first_component_error);

    int len = 0;
    Token last_component = parser.previous;

    int first_len = parser.previous.length;
    if (first_len >= 256) first_len = 255;
    memcpy(module_path, parser.previous.start, first_len);
    len = first_len;

    while (match(TOKEN_DOT)) {
        if (len < 255) module_path[len++] = '.';
        consume(TOKEN_IDENTIFIER, "Expect module name after '.'.");
        last_component = parser.previous;
        int part_len = parser.previous.length;
        if (len + part_len >= 256) part_len = 255 - len;
        if (part_len > 0) {
            memcpy(module_path + len, parser.previous.start, part_len);
            len += part_len;
        }
    }

    module_path[len] = '\0';
    *out_len = len;
    return last_component;
}

static void import_statement() {
    // Parse: import module_name[.submodule...][, module_name[.submodule...]]
    do {
        char module_path[256];
        int len = 0;
        Token last_component = parse_module_path(
            module_path,
            &len,
            "Expect module name after 'import'."
        );

        // Use the last path component as the variable name.
        parser.previous = last_component;
        declare_variable();

        ObjString* path_string = copy_string(module_path, len);
        uint8_t path_constant = make_constant(OBJ_VAL(path_string));
        emit_bytes(OP_IMPORT, path_constant);

        if (current->scope_depth > 0) {
            mark_initialized();
        } else {
            uint8_t var_name = identifier_constant(&last_component);
            emit_bytes(OP_DEFINE_GLOBAL, var_name);
        }
    } while (match(TOKEN_COMMA));
}

static void from_import_statement(void) {
    // Parse: from module_name[.submodule...] import name[, name...]
    consume(TOKEN_IDENTIFIER, "Expect module name after 'from'.");

    char module_path[256];
    int len = 0;

    int first_len = parser.previous.length;
    if (first_len >= 256) first_len = 255;
    memcpy(module_path, parser.previous.start, first_len);
    len = first_len;

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

    consume(TOKEN_IMPORT, "Expect 'import' after module path.");

    // Prepare module path constant for repeated imports.
    ObjString* path_string = copy_string(module_path, len);
    uint8_t path_constant = make_constant(OBJ_VAL(path_string));
    if (match(TOKEN_STAR)) {
        emit_bytes(OP_IMPORT, path_constant);
        emit_byte(OP_IMPORT_STAR);
        return;
    }

    do {
        consume(TOKEN_IDENTIFIER, "Expect imported name.");
        Token imported_name = parser.previous;

        emit_bytes(OP_IMPORT, path_constant);

        // module[name]
        emit_constant(OBJ_VAL(copy_string(imported_name.start, imported_name.length)));
        emit_byte(OP_GET_TABLE);

        // Define imported symbol in current scope.
        parser.previous = imported_name;
        if (current->scope_depth > 0) {
            declare_variable();
            mark_initialized();
        } else {
            uint8_t var_name = identifier_constant(&imported_name);
            emit_bytes(OP_DEFINE_GLOBAL, var_name);
        }
    } while (match(TOKEN_COMMA));
}

void declaration(void) {
    if (match(TOKEN_AT)) {
        decorated_function_declaration();
    } else if (match(TOKEN_FN)) {
        function_declaration();
    } else if (match(TOKEN_IMPORT)) {
        import_statement();
    } else if (match(TOKEN_FROM)) {
        from_import_statement();
    } else if (match(TOKEN_GLOBAL)) {
        if (match(TOKEN_FN)) {
            global_function_declaration();
        } else {
            global_declaration();
        }
    } else if (match(TOKEN_LOCAL)) {
        // Check if this is "local fn name()" syntax
        if (match(TOKEN_FN)) {
            function_declaration();
        } else {
            variable_declaration();
        }
    } else {
        statement();
    }
}
