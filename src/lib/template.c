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

static void bufInit(StrBuf* buf) {
    buf->cap = 256;
    buf->data = malloc(buf->cap);
    buf->data[0] = '\0';
    buf->len = 0;
}

static void bufFree(StrBuf* buf) {
    free(buf->data);
}

static void bufAppend(StrBuf* buf, const char* str, size_t len) {
    while (buf->len + len + 1 > buf->cap) {
        buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, str, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void bufAppendStr(StrBuf* buf, const char* str) {
    bufAppend(buf, str, strlen(str));
}

// Escape a string for Pua string literal
static void bufAppendEscaped(StrBuf* buf, const char* str, size_t len) {
    bufAppendStr(buf, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '\n': bufAppendStr(buf, "\\n"); break;
            case '\r': bufAppendStr(buf, "\\r"); break;
            case '\t': bufAppendStr(buf, "\\t"); break;
            case '"':  bufAppendStr(buf, "\\\""); break;
            case '\\': bufAppendStr(buf, "\\\\"); break;
            default:
                bufAppend(buf, &c, 1);
        }
    }
    bufAppendStr(buf, "\"");
}

// Template parser state
typedef struct {
    const char* src;
    size_t pos;
    size_t len;
    StrBuf code;
    int indentLevel;
    char* error;
} Parser;

static void parserInit(Parser* p, const char* src) {
    p->src = src;
    p->pos = 0;
    p->len = strlen(src);
    bufInit(&p->code);
    p->indentLevel = 1; // Start inside function
    p->error = NULL;
}

static void parserFree(Parser* p) {
    bufFree(&p->code);
    if (p->error) free(p->error);
}

static void emitIndent(Parser* p) {
    for (int i = 0; i < p->indentLevel; i++) {
        bufAppendStr(&p->code, "    ");
    }
}

static void emitLine(Parser* p, const char* line) {
    emitIndent(p);
    bufAppendStr(&p->code, line);
    bufAppendStr(&p->code, "\n");
}

static void emitText(Parser* p, const char* text, size_t len) {
    if (len == 0) return;
    emitIndent(p);
    bufAppendStr(&p->code, "table.insert(__out, ");
    bufAppendEscaped(&p->code, text, len);
    bufAppendStr(&p->code, ")\n");
}

static void emitExpr(Parser* p, const char* expr, size_t len) {
    // Trim whitespace
    while (len > 0 && isspace((unsigned char)*expr)) { expr++; len--; }
    while (len > 0 && isspace((unsigned char)expr[len-1])) { len--; }

    emitIndent(p);
    bufAppendStr(&p->code, "table.insert(__out, str(");
    bufAppend(&p->code, expr, len);
    bufAppendStr(&p->code, "))\n");
}

// Find next occurrence of str, return position or -1
static int findNext(Parser* p, const char* str) {
    const char* found = strstr(p->src + p->pos, str);
    if (!found) return -1;
    return (int)(found - p->src);
}

// Skip whitespace in tag content
static void skipSpaces(const char** s, size_t* len) {
    while (*len > 0 && isspace((unsigned char)**s)) { (*s)++; (*len)--; }
}

// Extract word (identifier)
static size_t extractWord(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
    return i;
}

// Parse block tags {% ... %}
static int parseTag(Parser* p) {
    p->pos += 2; // Skip {%

    int endPos = findNext(p, "%}");
    if (endPos < 0) {
        p->error = strdup("Unclosed {% tag");
        return 0;
    }

    const char* content = p->src + p->pos;
    size_t contentLen = endPos - p->pos;
    p->pos = endPos + 2; // Skip %}

    // Trim whitespace
    skipSpaces(&content, &contentLen);
    while (contentLen > 0 && isspace((unsigned char)content[contentLen-1])) contentLen--;

    // Parse keyword
    size_t kwLen = extractWord(content, contentLen);
    if (kwLen == 0) {
        p->error = strdup("Expected keyword in {% tag");
        return 0;
    }

    if (kwLen == 2 && strncmp(content, "if", 2) == 0) {
        const char* cond = content + 2;
        size_t condLen = contentLen - 2;
        skipSpaces(&cond, &condLen);

        emitIndent(p);
        bufAppendStr(&p->code, "if ");
        bufAppend(&p->code, cond, condLen);
        bufAppendStr(&p->code, "\n");
        p->indentLevel++;
    }
    else if (kwLen == 4 && strncmp(content, "elif", 4) == 0) {
        const char* cond = content + 4;
        size_t condLen = contentLen - 4;
        skipSpaces(&cond, &condLen);

        p->indentLevel--;
        emitIndent(p);
        bufAppendStr(&p->code, "elif ");
        bufAppend(&p->code, cond, condLen);
        bufAppendStr(&p->code, "\n");
        p->indentLevel++;
    }
    else if (kwLen == 4 && strncmp(content, "else", 4) == 0) {
        p->indentLevel--;
        emitLine(p, "else");
        p->indentLevel++;
    }
    else if (kwLen == 5 && strncmp(content, "endif", 5) == 0) {
        p->indentLevel--;
        // No explicit end marker needed for indentation-based syntax
    }
    else if (kwLen == 3 && strncmp(content, "for", 3) == 0) {
        const char* rest = content + 3;
        size_t restLen = contentLen - 3;
        skipSpaces(&rest, &restLen);

        // Parse: var in expr
        size_t varLen = extractWord(rest, restLen);
        if (varLen == 0) {
            p->error = strdup("Expected variable name after 'for'");
            return 0;
        }

        const char* varName = rest;
        rest += varLen;
        restLen -= varLen;
        skipSpaces(&rest, &restLen);

        // Expect 'in'
        if (restLen < 2 || strncmp(rest, "in", 2) != 0) {
            p->error = strdup("Expected 'in' in for loop");
            return 0;
        }
        rest += 2;
        restLen -= 2;
        skipSpaces(&rest, &restLen);

        // rest is the iterable expression
        emitIndent(p);
        bufAppendStr(&p->code, "for __idx#, ");
        bufAppend(&p->code, varName, varLen);
        bufAppendStr(&p->code, " in ");
        bufAppend(&p->code, rest, restLen);
        bufAppendStr(&p->code, "\n");
        p->indentLevel++;
    }
    else if (kwLen == 6 && strncmp(content, "endfor", 6) == 0) {
        p->indentLevel--;
    }
    else if (kwLen == 3 && strncmp(content, "set", 3) == 0) {
        const char* rest = content + 3;
        size_t restLen = contentLen - 3;
        skipSpaces(&rest, &restLen);

        // Parse: var = expr
        size_t varLen = extractWord(rest, restLen);
        if (varLen == 0) {
            p->error = strdup("Expected variable name after 'set'");
            return 0;
        }

        const char* varName = rest;
        rest += varLen;
        restLen -= varLen;
        skipSpaces(&rest, &restLen);

        // Expect '='
        if (restLen < 1 || *rest != '=') {
            p->error = strdup("Expected '=' in set");
            return 0;
        }
        rest++;
        restLen--;
        skipSpaces(&rest, &restLen);

        emitIndent(p);
        bufAppendStr(&p->code, "local ");
        bufAppend(&p->code, varName, varLen);
        bufAppendStr(&p->code, " = ");
        bufAppend(&p->code, rest, restLen);
        bufAppendStr(&p->code, "\n");
    }
    else {
        p->error = malloc(64);
        snprintf(p->error, 64, "Unknown tag: %.*s", (int)kwLen, content);
        return 0;
    }

    return 1;
}

// Main parse loop
static int parseTemplate(Parser* p) {
    // Function header - named function so we can return it
    bufAppendStr(&p->code, "fn __tmpl(__ctx)\n");
    emitLine(p, "local __out = {}");

    while (p->pos < p->len) {
        // Look for next tag
        int exprPos = findNext(p, "{{");
        int tagPos = findNext(p, "{%");

        int nextPos = -1;
        int isExpr = 0;

        if (exprPos >= 0 && (tagPos < 0 || exprPos < tagPos)) {
            nextPos = exprPos;
            isExpr = 1;
        } else if (tagPos >= 0) {
            nextPos = tagPos;
            isExpr = 0;
        }

        if (nextPos < 0) {
            // No more tags, emit rest as text
            emitText(p, p->src + p->pos, p->len - p->pos);
            p->pos = p->len;
            break;
        }

        // Emit text before tag
        if (nextPos > (int)p->pos) {
            emitText(p, p->src + p->pos, nextPos - p->pos);
        }
        p->pos = nextPos;

        if (isExpr) {
            // Parse {{ expr }}
            p->pos += 2; // Skip {{
            int endPos = findNext(p, "}}");
            if (endPos < 0) {
                p->error = strdup("Unclosed {{ expression");
                return 0;
            }
            emitExpr(p, p->src + p->pos, endPos - p->pos);
            p->pos = endPos + 2;
        } else {
            // Parse {% tag %}
            if (!parseTag(p)) return 0;
        }
    }

    // Function footer
    emitLine(p, "return table.concat(__out)");

    // End function and return it (blank line needed to close indentation block)
    bufAppendStr(&p->code, "\nreturn __tmpl\n");

    return 1;
}

static ObjTable* get_template_cache(VM* vm) {
    ObjString* moduleName = copyString("template", 8);
    Value moduleVal = NIL_VAL;
    if (!tableGet(&vm->globals, moduleName, &moduleVal) || !IS_TABLE(moduleVal)) {
        return NULL;
    }

    ObjTable* module = AS_TABLE(moduleVal);
    ObjString* cacheKey = copyString("_cache", 6);
    Value cacheVal = NIL_VAL;
    if (tableGet(&module->table, cacheKey, &cacheVal) && IS_TABLE(cacheVal)) {
        return AS_TABLE(cacheVal);
    }

    ObjTable* cache = newTable();
    push(vm, OBJ_VAL(cache));
    tableSet(&module->table, cacheKey, OBJ_VAL(cache));
    pop(vm);
    return cache;
}

// template.compile(str) -> function
static int template_compile(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* tmplStr = GET_STRING(0);

    Parser parser;
    parserInit(&parser, tmplStr->chars);

    if (!parseTemplate(&parser)) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "Template error: %s", parser.error ? parser.error : "unknown");
        parserFree(&parser);
        vmRuntimeError(vm, errMsg);
        return 0;
    }

    // Compile generated code (script that defines and returns __tmpl function)
    ObjFunction* scriptFn = compile(parser.code.data);
    parserFree(&parser);

    if (scriptFn == NULL) {
        vmRuntimeError(vm, "Failed to compile template");
        return 0;
    }

    // Create closure for the script and execute it to get the template function
    ObjClosure* scriptClosure = newClosure(scriptFn);
    push(vm, OBJ_VAL(scriptClosure));

    // Get current frame count
    int frameCount = vm->currentThread->frameCount;

    // Call the script
    if (!call(vm, scriptClosure, 0)) {
        return 0;
    }

    // Run until script returns
    InterpretResult result = vmRun(vm, frameCount);
    if (result != INTERPRET_OK) {
        vmRuntimeError(vm, "Failed to execute template compilation");
        return 0;
    }

    // The template function should now be on the stack
    // Return it (it's already there)
    return 1;
}

// template.render(str, ctx) -> string (convenience function)
static int template_render(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_TABLE(1);

    ObjString* tmplStr = GET_STRING(0);
    Value tmplFn = NIL_VAL;
    ObjTable* cache = get_template_cache(vm);

    if (cache != NULL && tableGet(&cache->table, tmplStr, &tmplFn) && IS_CLOSURE(tmplFn)) {
        // Cache hit: skip parse/compile.
    } else {
        Parser parser;
        parserInit(&parser, tmplStr->chars);

        if (!parseTemplate(&parser)) {
            char errMsg[256];
            snprintf(errMsg, sizeof(errMsg), "Template error: %s", parser.error ? parser.error : "unknown");
            parserFree(&parser);
            vmRuntimeError(vm, errMsg);
            return 0;
        }

        // Compile generated code (script that defines and returns __tmpl function)
        ObjFunction* scriptFn = compile(parser.code.data);
        parserFree(&parser);

        if (scriptFn == NULL) {
            vmRuntimeError(vm, "Failed to compile template");
            return 0;
        }

        // Step 1: Run the script to get the template function
        ObjClosure* scriptClosure = newClosure(scriptFn);
        push(vm, OBJ_VAL(scriptClosure));

        int frameCount = vm->currentThread->frameCount;
        if (!call(vm, scriptClosure, 0)) {
            return 0;
        }

        InterpretResult result = vmRun(vm, frameCount);
        if (result != INTERPRET_OK) {
            return 0;
        }

        // Now the template function (closure) is on the stack
        tmplFn = peek(vm, 0);
        if (!IS_CLOSURE(tmplFn)) {
            vmRuntimeError(vm, "Template compilation did not return a function");
            return 0;
        }

        // Optional cache write for future renders.
        if (cache != NULL) {
            tableSet(&cache->table, tmplStr, tmplFn);
        }
        pop(vm); // Pop cached template function from compilation step.
    }

    // Step 2: Call the template function with the context
    push(vm, tmplFn);   // callee
    push(vm, args[1]); // Push context table

    int frameCount = vm->currentThread->frameCount;
    if (!call(vm, AS_CLOSURE(tmplFn), 1)) {
        return 0;
    }

    InterpretResult result = vmRun(vm, frameCount);
    if (result != INTERPRET_OK) {
        return 0;
    }

    // Result string is on stack
    return 1;
}

// template.code(str) -> string (debug: show generated code)
static int template_code(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* tmplStr = GET_STRING(0);

    Parser parser;
    parserInit(&parser, tmplStr->chars);

    if (!parseTemplate(&parser)) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "Template error: %s", parser.error ? parser.error : "unknown");
        parserFree(&parser);
        vmRuntimeError(vm, errMsg);
        return 0;
    }

    ObjString* code = copyString(parser.code.data, (int)parser.code.len);
    parserFree(&parser);

    RETURN_OBJ(code);
}

void registerTemplate(VM* vm) {
    const NativeReg templateFuncs[] = {
        {"compile", template_compile},
        {"render", template_render},
        {"code", template_code},
        {NULL, NULL}
    };

    registerModule(vm, "template", templateFuncs);
    pop(vm); // Pop module from stack
}
