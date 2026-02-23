#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "fstring.h"

static int is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void trim_slice(const char** start, const char** end) {
    while (*start < *end && is_space_char(**start)) (*start)++;
    while (*end > *start && is_space_char((*end)[-1])) (*end)--;
}

static int find_top_level_pipe(const char* src, int len) {
    int paren = 0;
    int bracket = 0;
    int brace = 0;

    for (int i = 0; i < len; i++) {
        if (src[i] == '[' && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len && !(src[i] == ']' && i + 1 < len && src[i + 1] == ']')) i++;
            if (i + 1 < len) i++;
            continue;
        }

        if (src[i] == '-' && i + 1 < len && src[i + 1] == '-') {
            i += 2;
            while (i < len && src[i] != '\n') i++;
            continue;
        }

        if (src[i] == '"' || src[i] == '\'') {
            char quote = src[i];
            i++;
            while (i < len) {
                if (src[i] == '\\' && i + 1 < len) {
                    i += 2;
                    continue;
                }
                if (src[i] == quote) break;
                i++;
            }
            continue;
        }

        if (src[i] == '\\') {
            if (i + 1 < len) i++;
            continue;
        }

        if (src[i] == '(') paren++;
        else if (src[i] == ')' && paren > 0) paren--;
        else if (src[i] == '[') bracket++;
        else if (src[i] == ']' && bracket > 0) bracket--;
        else if (src[i] == '{') brace++;
        else if (src[i] == '}' && brace > 0) brace--;
        else if (src[i] == '|' && paren == 0 && bracket == 0 && brace == 0) return i;
    }

    return -1;
}

static char* unescape_fstring_expr_slice(const char* src, int len, int* out_len) {
    char* out = (char*)malloc((size_t)len + 1);
    int w = 0;
    for (int j = 0; j < len; j++) {
        if (src[j] == '\\' && j + 1 < len) {
            if (src[j + 1] == '"') {
                out[w++] = '"';
                j++;
            } else if (src[j + 1] == '\\') {
                out[w++] = '\\';
                j++;
            } else if (src[j + 1] == '{' || src[j + 1] == '}') {
                out[w++] = src[j + 1];
                j++;
            } else {
                out[w++] = '\\';
            }
        } else {
            out[w++] = src[j];
        }
    }
    out[w] = '\0';
    *out_len = w;
    return out;
}

static void compile_expression_source(const char* expr_src) {
    Parser saved_parser = parser;
    Lexer saved_lexer = lexer;

    init_lexer(&lexer, expr_src);
    parser.had_error = 0;
    parser.panic_mode = 0;
    advance();
    expression();

    parser = saved_parser;
    lexer = saved_lexer;
}

static void compile_fstring_expression_slice(const char* src, int len) {
    const char* expr_start = src;
    const char* expr_end = src + len;
    trim_slice(&expr_start, &expr_end);
    if (expr_start >= expr_end) {
        error("f-string interpolation is empty.");
        return;
    }

    int expr_src_len = 0;
    char* expr_src = unescape_fstring_expr_slice(
        expr_start,
        (int)(expr_end - expr_start),
        &expr_src_len);
    compile_expression_source(expr_src);
    free(expr_src);
}

static int emit_fstring_format_call(const char* expr_src, int expr_len, const char* fmt_src, int fmt_len) {
    const char* expr_start = expr_src;
    const char* expr_end = expr_src + expr_len;
    const char* fmt_start = fmt_src;
    const char* fmt_end = fmt_src + fmt_len;

    trim_slice(&expr_start, &expr_end);
    trim_slice(&fmt_start, &fmt_end);
    if (expr_start >= expr_end) {
        error("f-string interpolation format: missing expression before '|'.");
        return 0;
    }
    if (fmt_start >= fmt_end) {
        error("f-string interpolation format: missing format specifier after '|'.");
        return 0;
    }

    int spec_len = (int)(fmt_end - fmt_start);
    int add_percent = fmt_start[0] != '%';
    int cfmt_len = spec_len + (add_percent ? 1 : 0);
    char* cfmt = (char*)malloc((size_t)cfmt_len + 1);
    int w = 0;
    if (add_percent) cfmt[w++] = '%';
    memcpy(cfmt + w, fmt_start, (size_t)spec_len);
    w += spec_len;
    cfmt[w] = '\0';

    int expr_code_len = 0;
    char* expr_code = unescape_fstring_expr_slice(expr_start, (int)(expr_end - expr_start), &expr_code_len);

    int escaped_cfmt_len = cfmt_len * 2 + 1;
    char* escaped_cfmt = (char*)malloc((size_t)escaped_cfmt_len);
    int ef = 0;
    for (int i = 0; i < cfmt_len; i++) {
        char ch = cfmt[i];
        if (ch == '\\' || ch == '"') escaped_cfmt[ef++] = '\\';
        escaped_cfmt[ef++] = ch;
    }
    escaped_cfmt[ef] = '\0';

    int generated_len = 24 + ef + expr_code_len + 8;
    char* generated = (char*)malloc((size_t)generated_len + 1);
    int glen = snprintf(
        generated,
        (size_t)generated_len + 1,
        "(import string).format(\"%s\", (%s))",
        escaped_cfmt,
        expr_code);

    free(cfmt);
    free(expr_code);
    free(escaped_cfmt);

    if (glen < 0 || glen > generated_len) {
        free(generated);
        error("f-string interpolation format: failed to build formatter expression.");
        return 0;
    }

    compile_expression_source(generated);
    free(generated);
    return 1;
}

void fstring(int can_assign) {
    (void)can_assign;
    int base_top = type_stack_top;
    const char* tok = parser.previous.start;
    int tok_len = parser.previous.length;
    const char* src = NULL;
    int raw_len = 0;

    // Token is either f"..." / f'...' / f[[...]]
    if (tok_len >= 5 && tok[0] == 'f' && tok[1] == '[' && tok[2] == '[' &&
        tok[tok_len - 2] == ']' && tok[tok_len - 1] == ']') {
        src = tok + 3;       // after f[[
        raw_len = tok_len - 5; // strip f[[ and ]]
    } else if (tok_len >= 4 && tok[0] == 'f' &&
               (tok[1] == '"' || tok[1] == '\'')) {
        src = tok + 2;       // after f"
        raw_len = tok_len - 3; // strip f" and trailing "
    } else {
        error("Invalid f-string token.");
        type_stack_top = base_top;
        type_push(TYPEHINT_STR);
        return;
    }

    int part_count = 0;
    int i = 0;

    while (i < raw_len) {
        // Find next { or end of string
        int start = i;
        while (i < raw_len && src[i] != '{') {
            if (src[i] == '\\' && i + 1 < raw_len) i++; // skip escape
            i++;
        }

        // Emit literal part (start to i)
        if (i > start) {
            // Process escape sequences in literal part
            char* buf = (char*)malloc(i - start + 1);
            int w = 0;
            for (int j = start; j < i; j++) {
                char c = src[j];
                if (c == '\\' && j + 1 < i) {
                    char e = src[++j];
                    switch (e) {
                        case 'n': buf[w++] = '\n'; break;
                        case 't': buf[w++] = '\t'; break;
                        case 'r': buf[w++] = '\r'; break;
                        case '"': buf[w++] = '"'; break;
                        case '\\': buf[w++] = '\\'; break;
                        case '{': buf[w++] = '{'; break;
                        case '}': buf[w++] = '}'; break;
                        default:
                            buf[w++] = '\\';
                            buf[w++] = e;
                            break;
                    }
                } else {
                    buf[w++] = c;
                }
            }
            ObjString* s = copy_string(buf, w);
            free(buf);
            emit_constant(OBJ_VAL(s));
            part_count++;
        }

        if (i < raw_len && src[i] == '{') {
            i++; // skip {

            // Find matching }
            int expr_start = i;
            int brace_depth = 1;
            while (i < raw_len && brace_depth > 0) {
                // Check for multiline strings [[...]]
                if (src[i] == '[' && i + 1 < raw_len && src[i+1] == '[') {
                    i += 2;
                    while (i < raw_len && !(src[i] == ']' && i + 1 < raw_len && src[i+1] == ']')) {
                        i++;
                    }
                    if (i < raw_len) i += 2; // consume ]]
                    continue;
                }

                // Check for comments --...
                if (src[i] == '-' && i + 1 < raw_len && src[i+1] == '-') {
                    i += 2;
                    while (i < raw_len && src[i] != '\n') i++;
                    continue;
                }

                // Check for string start: \"
                if (src[i] == '"' || src[i] == '\'') {
                    char quote = src[i++];
                    while (i < raw_len) {
                        if (src[i] == '\\' && i + 1 < raw_len) {
                            i += 2; // escaped char inside expression string
                            continue;
                        }
                        if (src[i] == quote) { i++; break; }
                        i++;
                    }
                    continue;
                }

                // Check for other escapes (skip them so we don't count \{ or \})
                if (src[i] == '\\') {
                    if (i + 1 < raw_len) i += 2;
                    else i++;
                    continue;
                }

                if (src[i] == '{') brace_depth++;
                else if (src[i] == '}') brace_depth--;
                if (brace_depth > 0) i++;
            }
            int expr_len = i - expr_start;
            i++; // skip closing }

            if (expr_len > 0) {
                int format_split = find_top_level_pipe(src + expr_start, expr_len);
                if (format_split >= 0) {
                    if (emit_fstring_format_call(
                            src + expr_start,
                            format_split,
                            src + expr_start + format_split + 1,
                            expr_len - format_split - 1)) {
                        part_count++;
                    }
                    continue;
                }

                if (emit_simple_fstring_expr(src + expr_start, expr_len)) {
                    part_count++;
                    continue;
                }

                compile_fstring_expression_slice(src + expr_start, expr_len);
                part_count++;
            }
        }
    }

    // Handle empty f-string
    if (part_count == 0) {
        ObjString* s = copy_string("", 0);
        emit_constant(OBJ_VAL(s));
        type_stack_top = base_top;
        type_push(TYPEHINT_STR);
        return;
    }

    if (part_count > 255) {
        error("f-string has too many parts.");
        type_stack_top = base_top;
        type_push(TYPEHINT_STR);
        return;
    }

    emit_bytes(OP_BUILD_STRING, (uint8_t)part_count);
    type_stack_top = base_top;
    type_push(TYPEHINT_STR);
}
