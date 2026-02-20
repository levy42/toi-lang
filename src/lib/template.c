#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"
#include "../compiler.h"

// Dynamic string buffer for code generation
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} StrBuf;

static void buf_init(StrBuf* buf) {
    buf->cap = 256;
    buf->data = malloc(buf->cap);
    buf->data[0] = '\0';
    buf->len = 0;
}

static void buf_free(StrBuf* buf) {
    free(buf->data);
}

static void buf_append(StrBuf* buf, const char* str, size_t len) {
    while (buf->len + len + 1 > buf->cap) {
        buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, str, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void buf_append_str(StrBuf* buf, const char* str) {
    buf_append(buf, str, strlen(str));
}

// Escape a string for Pua string literal
static void buf_append_escaped(StrBuf* buf, const char* str, size_t len) {
    buf_append_str(buf, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '\n': buf_append_str(buf, "\\n"); break;
            case '\r': buf_append_str(buf, "\\r"); break;
            case '\t': buf_append_str(buf, "\\t"); break;
            case '"':  buf_append_str(buf, "\\\""); break;
            case '\\': buf_append_str(buf, "\\\\"); break;
            default:
                buf_append(buf, &c, 1);
        }
    }
    buf_append_str(buf, "\"");
}

// Template parser state
typedef struct {
    const char* src;
    size_t pos;
    size_t len;
    StrBuf code;
    int indent_level;
    char* error;
} Parser;

static void parser_init(Parser* p, const char* src) {
    p->src = src;
    p->pos = 0;
    p->len = strlen(src);
    buf_init(&p->code);
    p->indent_level = 1; // Start inside function
    p->error = NULL;
}

static void parser_free(Parser* p) {
    buf_free(&p->code);
    if (p->error) free(p->error);
}

static void emit_indent(Parser* p) {
    for (int i = 0; i < p->indent_level; i++) {
        buf_append_str(&p->code, "    ");
    }
}

static void emit_line(Parser* p, const char* line) {
    emit_indent(p);
    buf_append_str(&p->code, line);
    buf_append_str(&p->code, "\n");
}

static void emit_text(Parser* p, const char* text, size_t len) {
    if (len == 0) return;
    emit_indent(p);
    buf_append_str(&p->code, "table.insert(__out, ");
    buf_append_escaped(&p->code, text, len);
    buf_append_str(&p->code, ")\n");
}

static void emit_expr(Parser* p, const char* expr, size_t len) {
    // Trim whitespace
    while (len > 0 && isspace((unsigned char)*expr)) { expr++; len--; }
    while (len > 0 && isspace((unsigned char)expr[len-1])) { len--; }

    emit_indent(p);
    buf_append_str(&p->code, "table.insert(__out, str(");
    buf_append(&p->code, expr, len);
    buf_append_str(&p->code, "))\n");
}

// Find next occurrence of str, return position or -1
static int find_next(Parser* p, const char* str) {
    const char* found = strstr(p->src + p->pos, str);
    if (!found) return -1;
    return (int)(found - p->src);
}

// Skip whitespace in tag content
static void skip_spaces(const char** s, size_t* len) {
    while (*len > 0 && isspace((unsigned char)**s)) { (*s)++; (*len)--; }
}

// Extract word (identifier)
static size_t extract_word(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
    return i;
}

// Parse block tags {% ... %}
static int parse_tag(Parser* p) {
    p->pos += 2; // Skip {%

    int end_pos = find_next(p, "%}");
    if (end_pos < 0) {
        p->error = strdup("Unclosed {% tag");
        return 0;
    }

    const char* content = p->src + p->pos;
    size_t content_len = end_pos - p->pos;
    p->pos = end_pos + 2; // Skip %}

    // Trim whitespace
    skip_spaces(&content, &content_len);
    while (content_len > 0 && isspace((unsigned char)content[content_len-1])) content_len--;

    // Parse keyword
    size_t kw_len = extract_word(content, content_len);
    if (kw_len == 0) {
        p->error = strdup("Expected keyword in {% tag");
        return 0;
    }

    if (kw_len == 2 && strncmp(content, "if", 2) == 0) {
        const char* cond = content + 2;
        size_t cond_len = content_len - 2;
        skip_spaces(&cond, &cond_len);

        emit_indent(p);
        buf_append_str(&p->code, "if ");
        buf_append(&p->code, cond, cond_len);
        buf_append_str(&p->code, "\n");
        p->indent_level++;
    }
    else if (kw_len == 4 && strncmp(content, "elif", 4) == 0) {
        const char* cond = content + 4;
        size_t cond_len = content_len - 4;
        skip_spaces(&cond, &cond_len);

        p->indent_level--;
        emit_indent(p);
        buf_append_str(&p->code, "elif ");
        buf_append(&p->code, cond, cond_len);
        buf_append_str(&p->code, "\n");
        p->indent_level++;
    }
    else if (kw_len == 4 && strncmp(content, "else", 4) == 0) {
        p->indent_level--;
        emit_line(p, "else");
        p->indent_level++;
    }
    else if (kw_len == 5 && strncmp(content, "endif", 5) == 0) {
        p->indent_level--;
        // No explicit end marker needed for indentation-based syntax
    }
    else if (kw_len == 3 && strncmp(content, "for", 3) == 0) {
        const char* rest = content + 3;
        size_t rest_len = content_len - 3;
        skip_spaces(&rest, &rest_len);

        // Parse: var in expr
        size_t var_len = extract_word(rest, rest_len);
        if (var_len == 0) {
            p->error = strdup("Expected variable name after 'for'");
            return 0;
        }

        const char* var_name = rest;
        rest += var_len;
        rest_len -= var_len;
        skip_spaces(&rest, &rest_len);

        // Expect 'in'
        if (rest_len < 2 || strncmp(rest, "in", 2) != 0) {
            p->error = strdup("Expected 'in' in for loop");
            return 0;
        }
        rest += 2;
        rest_len -= 2;
        skip_spaces(&rest, &rest_len);

        // rest is the iterable expression
        emit_indent(p);
        buf_append_str(&p->code, "for __idx#, ");
        buf_append(&p->code, var_name, var_len);
        buf_append_str(&p->code, " in ");
        buf_append(&p->code, rest, rest_len);
        buf_append_str(&p->code, "\n");
        p->indent_level++;
    }
    else if (kw_len == 6 && strncmp(content, "endfor", 6) == 0) {
        p->indent_level--;
    }
    else if (kw_len == 3 && strncmp(content, "set", 3) == 0) {
        const char* rest = content + 3;
        size_t rest_len = content_len - 3;
        skip_spaces(&rest, &rest_len);

        // Parse: var = expr
        size_t var_len = extract_word(rest, rest_len);
        if (var_len == 0) {
            p->error = strdup("Expected variable name after 'set'");
            return 0;
        }

        const char* var_name = rest;
        rest += var_len;
        rest_len -= var_len;
        skip_spaces(&rest, &rest_len);

        // Expect '='
        if (rest_len < 1 || *rest != '=') {
            p->error = strdup("Expected '=' in set");
            return 0;
        }
        rest++;
        rest_len--;
        skip_spaces(&rest, &rest_len);

        emit_indent(p);
        buf_append_str(&p->code, "local ");
        buf_append(&p->code, var_name, var_len);
        buf_append_str(&p->code, " = ");
        buf_append(&p->code, rest, rest_len);
        buf_append_str(&p->code, "\n");
    }
    else {
        p->error = malloc(64);
        snprintf(p->error, 64, "Unknown tag: %.*s", (int)kw_len, content);
        return 0;
    }

    return 1;
}

// Main parse loop
static int parse_template(Parser* p) {
    // Function header - named function so we can return it
    buf_append_str(&p->code, "fn __tmpl(__ctx)\n");
    emit_line(p, "local __out = {}");

    while (p->pos < p->len) {
        // Look for next tag
        int expr_pos = find_next(p, "{{");
        int tag_pos = find_next(p, "{%");

        int next_pos = -1;
        int is_expr = 0;

        if (expr_pos >= 0 && (tag_pos < 0 || expr_pos < tag_pos)) {
            next_pos = expr_pos;
            is_expr = 1;
        } else if (tag_pos >= 0) {
            next_pos = tag_pos;
            is_expr = 0;
        }

        if (next_pos < 0) {
            // No more tags, emit rest as text
            emit_text(p, p->src + p->pos, p->len - p->pos);
            p->pos = p->len;
            break;
        }

        // Emit text before tag
        if (next_pos > (int)p->pos) {
            emit_text(p, p->src + p->pos, next_pos - p->pos);
        }
        p->pos = next_pos;

        if (is_expr) {
            // Parse {{ expr }}
            p->pos += 2; // Skip {{
            int end_pos = find_next(p, "}}");
            if (end_pos < 0) {
                p->error = strdup("Unclosed {{ expression");
                return 0;
            }
            emit_expr(p, p->src + p->pos, end_pos - p->pos);
            p->pos = end_pos + 2;
        } else {
            // Parse {% tag %}
            if (!parse_tag(p)) return 0;
        }
    }

    // Function footer
    emit_line(p, "return table.concat(__out)");

    // End function and return it (blank line needed to close indentation block)
    buf_append_str(&p->code, "\nreturn __tmpl\n");

    return 1;
}

static ObjTable* get_template_cache(VM* vm) {
    ObjString* module_name = copy_string("template", 8);
    Value module_val = NIL_VAL;
    if (!table_get(&vm->globals, module_name, &module_val) || !IS_TABLE(module_val)) {
        return NULL;
    }

    ObjTable* module = AS_TABLE(module_val);
    ObjString* cache_key = copy_string("_cache", 6);
    Value cache_val = NIL_VAL;
    if (table_get(&module->table, cache_key, &cache_val) && IS_TABLE(cache_val)) {
        return AS_TABLE(cache_val);
    }

    ObjTable* cache = new_table();
    push(vm, OBJ_VAL(cache));
    table_set(&module->table, cache_key, OBJ_VAL(cache));
    pop(vm);
    return cache;
}

// template.compile(str) -> function
static int template_compile(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* tmpl_str = GET_STRING(0);

    Parser parser;
    parser_init(&parser, tmpl_str->chars);

    if (!parse_template(&parser)) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Template error: %s", parser.error ? parser.error : "unknown");
        parser_free(&parser);
        vm_runtime_error(vm, err_msg);
        return 0;
    }

    // Compile generated code (script that defines and returns __tmpl function)
    ObjFunction* script_fn = compile(parser.code.data);
    parser_free(&parser);

    if (script_fn == NULL) {
        vm_runtime_error(vm, "Failed to compile template");
        return 0;
    }

    // Create closure for the script and execute it to get the template function
    ObjClosure* script_closure = new_closure(script_fn);
    push(vm, OBJ_VAL(script_closure));

    // Get current frame count
    int frame_count = vm->current_thread->frame_count;

    // Call the script
    if (!call(vm, script_closure, 0)) {
        return 0;
    }

    // Run until script returns
    InterpretResult result = vm_run(vm, frame_count);
    if (result != INTERPRET_OK) {
        vm_runtime_error(vm, "Failed to execute template compilation");
        return 0;
    }

    // The template function should now be on the stack
    // Return it (it's already there)
    return 1;
}

// template.render(str, ctx) -> string (convenience function)
static int template_render(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_TABLE(1);

    ObjString* tmpl_str = GET_STRING(0);
    Value tmpl_fn = NIL_VAL;
    ObjTable* cache = get_template_cache(vm);

    if (cache != NULL && table_get(&cache->table, tmpl_str, &tmpl_fn) && IS_CLOSURE(tmpl_fn)) {
        // Cache hit: skip parse/compile.
    } else {
        Parser parser;
        parser_init(&parser, tmpl_str->chars);

        if (!parse_template(&parser)) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Template error: %s", parser.error ? parser.error : "unknown");
            parser_free(&parser);
            vm_runtime_error(vm, err_msg);
            return 0;
        }

        // Compile generated code (script that defines and returns __tmpl function)
        ObjFunction* script_fn = compile(parser.code.data);
        parser_free(&parser);

        if (script_fn == NULL) {
            vm_runtime_error(vm, "Failed to compile template");
            return 0;
        }

        // Step 1: Run the script to get the template function
        ObjClosure* script_closure = new_closure(script_fn);
        push(vm, OBJ_VAL(script_closure));

        int frame_count = vm->current_thread->frame_count;
        if (!call(vm, script_closure, 0)) {
            return 0;
        }

        InterpretResult result = vm_run(vm, frame_count);
        if (result != INTERPRET_OK) {
            return 0;
        }

        // Now the template function (closure) is on the stack
        tmpl_fn = peek(vm, 0);
        if (!IS_CLOSURE(tmpl_fn)) {
            vm_runtime_error(vm, "Template compilation did not return a function");
            return 0;
        }

        // Optional cache write for future renders.
        if (cache != NULL) {
            table_set(&cache->table, tmpl_str, tmpl_fn);
        }
        pop(vm); // Pop cached template function from compilation step.
    }

    // Step 2: Call the template function with the context
    push(vm, tmpl_fn);   // callee
    push(vm, args[1]); // Push context table

    int frame_count = vm->current_thread->frame_count;
    if (!call(vm, AS_CLOSURE(tmpl_fn), 1)) {
        return 0;
    }

    InterpretResult result = vm_run(vm, frame_count);
    if (result != INTERPRET_OK) {
        return 0;
    }

    // Result string is on stack
    return 1;
}

// template.code(str) -> string (debug: show generated code)
static int template_code(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* tmpl_str = GET_STRING(0);

    Parser parser;
    parser_init(&parser, tmpl_str->chars);

    if (!parse_template(&parser)) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Template error: %s", parser.error ? parser.error : "unknown");
        parser_free(&parser);
        vm_runtime_error(vm, err_msg);
        return 0;
    }

    ObjString* code = copy_string(parser.code.data, (int)parser.code.len);
    parser_free(&parser);

    RETURN_OBJ(code);
}

void register_template(VM* vm) {
    const NativeReg template_funcs[] = {
        {"compile", template_compile},
        {"render", template_render},
        {"code", template_code},
        {NULL, NULL}
    };

    register_module(vm, "template", template_funcs);
    pop(vm); // Pop module from stack
}
