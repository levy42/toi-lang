#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

typedef struct {
    const char* src;
    int pos;
    int len;
    int line;
    VM* vm;
} TomlParser;

typedef struct {
    char* data;
    int len;
    int cap;
} StrBuf;

static int toml_is_array_table(ObjTable* t);
static int set_value_at_path(TomlParser* p, ObjTable* base, ObjString** parts, int count, Value value);

static void toml_parse_error(TomlParser* p, const char* msg) {
    vm_runtime_error(p->vm, "toml.parse line %d: %s", p->line, msg);
}

static int sb_reserve(StrBuf* b, int extra) {
    int needed = b->len + extra + 1;
    if (needed <= b->cap) return 1;

    int new_cap = b->cap == 0 ? 128 : b->cap;
    while (new_cap < needed) new_cap *= 2;

    char* grown = (char*)realloc(b->data, (size_t)new_cap);
    if (grown == NULL) return 0;
    b->data = grown;
    b->cap = new_cap;
    return 1;
}

static int sb_append_char(StrBuf* b, char c) {
    if (!sb_reserve(b, 1)) return 0;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 1;
}

static int sb_append(StrBuf* b, const char* s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (!sb_reserve(b, n)) return 0;
    if (n > 0) memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static void parser_skip_inline_ws(TomlParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            p->pos++;
            continue;
        }
        break;
    }
}

static void parser_skip_ws_in_array(TomlParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            p->pos++;
            continue;
        }
        if (c == '\n') {
            p->pos++;
            p->line++;
            continue;
        }
        break;
    }
}

static void parser_skip_comment(TomlParser* p) {
    if (p->pos >= p->len || p->src[p->pos] != '#') return;
    while (p->pos < p->len && p->src[p->pos] != '\n') {
        p->pos++;
    }
}

static int parser_eat_line_end(TomlParser* p) {
    parser_skip_inline_ws(p);
    if (p->pos >= p->len) return 1;
    if (p->src[p->pos] == '#') {
        parser_skip_comment(p);
    }
    if (p->pos >= p->len) return 1;
    if (p->src[p->pos] == '\n') {
        p->pos++;
        p->line++;
        return 1;
    }
    toml_parse_error(p, "expected end of line.");
    return 0;
}

static int is_bare_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static int parse_bare_key(TomlParser* p, ObjString** out) {
    int start = p->pos;
    while (p->pos < p->len && is_bare_key_char(p->src[p->pos])) {
        p->pos++;
    }
    if (p->pos == start) {
        toml_parse_error(p, "expected bare key.");
        return 0;
    }
    *out = copy_string(p->src + start, p->pos - start);
    return 1;
}

static int parse_quoted_key(TomlParser* p, ObjString** out) {
    if (p->src[p->pos] != '"') {
        toml_parse_error(p, "expected quoted key.");
        return 0;
    }
    p->pos++;

    StrBuf b = {0};
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            *out = copy_string(b.data != NULL ? b.data : "", b.len);
            free(b.data);
            return 1;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                free(b.data);
                toml_parse_error(p, "unterminated escape in quoted key.");
                return 0;
            }
            char esc = p->src[p->pos++];
            char outc = 0;
            switch (esc) {
                case '"': outc = '"'; break;
                case '\\': outc = '\\'; break;
                case 'n': outc = '\n'; break;
                case 'r': outc = '\r'; break;
                case 't': outc = '\t'; break;
                default:
                    free(b.data);
                    toml_parse_error(p, "unsupported escape in quoted key.");
                    return 0;
            }
            if (!sb_append_char(&b, outc)) {
                free(b.data);
                vm_runtime_error(p->vm, "toml.parse: out of memory.");
                return 0;
            }
            continue;
        }
        if (c == '\n') p->line++;
        if (!sb_append_char(&b, c)) {
            free(b.data);
            vm_runtime_error(p->vm, "toml.parse: out of memory.");
            return 0;
        }
    }

    free(b.data);
    toml_parse_error(p, "unterminated quoted key.");
    return 0;
}

static int parse_key_part(TomlParser* p, ObjString** out) {
    if (p->pos < p->len && p->src[p->pos] == '"') {
        return parse_quoted_key(p, out);
    }
    return parse_bare_key(p, out);
}

static int parse_key_path(TomlParser* p, ObjString** parts, int* out_count) {
    int count = 0;
    for (;;) {
        parser_skip_inline_ws(p);
        if (count >= 32) {
            toml_parse_error(p, "key path is too deep.");
            return 0;
        }
        if (!parse_key_part(p, &parts[count++])) return 0;
        parser_skip_inline_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '.') {
            p->pos++;
            continue;
        }
        break;
    }
    *out_count = count;
    return 1;
}

static int parse_value(TomlParser* p, Value* out);

static int toml_token_looks_datetime(const char* tok, int n) {
    if (n < 10) return 0;
    if (!isdigit((unsigned char)tok[0])) return 0;

    int has_dash = 0;
    int has_time = 0;
    for (int i = 0; i < n; i++) {
        char c = tok[i];
        if (c == '-') has_dash = 1;
        if (c == 'T' || c == 't' || c == ':') has_time = 1;
        if (!(isdigit((unsigned char)c) || c == '-' || c == ':' || c == 'T' || c == 't' ||
              c == 'Z' || c == 'z' || c == '+' || c == '.')) {
            return 0;
        }
    }
    return has_dash && has_time;
}

static int parse_string_value(TomlParser* p, Value* out) {
    if (p->src[p->pos] != '"') {
        toml_parse_error(p, "expected string value.");
        return 0;
    }
    p->pos++;

    StrBuf b = {0};
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            ObjString* s = copy_string(b.data != NULL ? b.data : "", b.len);
            free(b.data);
            *out = OBJ_VAL(s);
            return 1;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                free(b.data);
                toml_parse_error(p, "unterminated string escape.");
                return 0;
            }
            char esc = p->src[p->pos++];
            char outc = 0;
            switch (esc) {
                case '"': outc = '"'; break;
                case '\\': outc = '\\'; break;
                case 'n': outc = '\n'; break;
                case 'r': outc = '\r'; break;
                case 't': outc = '\t'; break;
                default:
                    free(b.data);
                    toml_parse_error(p, "unsupported string escape.");
                    return 0;
            }
            if (!sb_append_char(&b, outc)) {
                free(b.data);
                vm_runtime_error(p->vm, "toml.parse: out of memory.");
                return 0;
            }
            continue;
        }
        if (c == '\n') {
            free(b.data);
            toml_parse_error(p, "unterminated string value.");
            return 0;
        }
        if (!sb_append_char(&b, c)) {
            free(b.data);
            vm_runtime_error(p->vm, "toml.parse: out of memory.");
            return 0;
        }
    }

    free(b.data);
    toml_parse_error(p, "unterminated string value.");
    return 0;
}

static int token_is_delim(char c) {
    return c == '\0' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '#';
}

static int parse_bool_or_number(TomlParser* p, Value* out) {
    int start = p->pos;
    while (p->pos < p->len && !token_is_delim(p->src[p->pos])) {
        p->pos++;
    }
    int n = p->pos - start;
    if (n <= 0) {
        toml_parse_error(p, "expected value.");
        return 0;
    }

    const char* tok = p->src + start;
    if (n == 4 && memcmp(tok, "true", 4) == 0) {
        *out = BOOL_VAL(1);
        return 1;
    }
    if (n == 5 && memcmp(tok, "false", 5) == 0) {
        *out = BOOL_VAL(0);
        return 1;
    }

    if (toml_token_looks_datetime(tok, n)) {
        *out = OBJ_VAL(copy_string(tok, n));
        return 1;
    }

    char buf[128];
    if (n >= (int)sizeof(buf)) {
        toml_parse_error(p, "numeric token is too long.");
        return 0;
    }
    memcpy(buf, tok, (size_t)n);
    buf[n] = '\0';

    char* end = NULL;
    double d = strtod(buf, &end);
    if (end == buf || *end != '\0') {
        toml_parse_error(p, "unsupported value token.");
        return 0;
    }
    *out = NUMBER_VAL(d);
    return 1;
}

static int parse_array_value(TomlParser* p, Value* out) {
    if (p->src[p->pos] != '[') {
        toml_parse_error(p, "expected array value.");
        return 0;
    }
    p->pos++;

    ObjTable* arr = new_table();
    push(p->vm, OBJ_VAL(arr));

    int idx = 1;
    for (;;) {
        parser_skip_ws_in_array(p);
        if (p->pos >= p->len) {
            pop(p->vm);
            toml_parse_error(p, "unterminated array.");
            return 0;
        }
        if (p->src[p->pos] == ']') {
            p->pos++;
            break;
        }

        Value v = NIL_VAL;
        if (!parse_value(p, &v)) {
            pop(p->vm);
            return 0;
        }
        if (!table_set_array(&arr->table, idx++, v)) {
            pop(p->vm);
            vm_runtime_error(p->vm, "toml.parse: out of memory.");
            return 0;
        }

        parser_skip_ws_in_array(p);
        if (p->pos >= p->len) {
            pop(p->vm);
            toml_parse_error(p, "unterminated array.");
            return 0;
        }
        if (p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->src[p->pos] == ']') {
            p->pos++;
            break;
        }
        pop(p->vm);
        toml_parse_error(p, "expected ',' or ']' in array.");
        return 0;
    }

    pop(p->vm);
    *out = OBJ_VAL(arr);
    return 1;
}

static int parse_inline_table_value(TomlParser* p, Value* out) {
    if (p->src[p->pos] != '{') {
        toml_parse_error(p, "expected inline table.");
        return 0;
    }
    p->pos++;

    ObjTable* tbl = new_table();
    push(p->vm, OBJ_VAL(tbl));

    for (;;) {
        parser_skip_inline_ws(p);
        if (p->pos >= p->len) {
            pop(p->vm);
            toml_parse_error(p, "unterminated inline table.");
            return 0;
        }
        if (p->src[p->pos] == '}') {
            p->pos++;
            break;
        }

        ObjString* parts[32];
        int count = 0;
        if (!parse_key_path(p, parts, &count)) {
            pop(p->vm);
            return 0;
        }

        parser_skip_inline_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != '=') {
            pop(p->vm);
            toml_parse_error(p, "expected '=' in inline table.");
            return 0;
        }
        p->pos++;

        Value value = NIL_VAL;
        if (!parse_value(p, &value)) {
            pop(p->vm);
            return 0;
        }

        if (!set_value_at_path(p, tbl, parts, count, value)) {
            pop(p->vm);
            return 0;
        }

        parser_skip_inline_ws(p);
        if (p->pos >= p->len) {
            pop(p->vm);
            toml_parse_error(p, "unterminated inline table.");
            return 0;
        }
        if (p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->src[p->pos] == '}') {
            p->pos++;
            break;
        }
        pop(p->vm);
        toml_parse_error(p, "expected ',' or '}' in inline table.");
        return 0;
    }

    pop(p->vm);
    *out = OBJ_VAL(tbl);
    return 1;
}

static int parse_value(TomlParser* p, Value* out) {
    parser_skip_inline_ws(p);
    if (p->pos >= p->len) {
        toml_parse_error(p, "expected value.");
        return 0;
    }

    char c = p->src[p->pos];
    if (c == '"') return parse_string_value(p, out);
    if (c == '[') return parse_array_value(p, out);
    if (c == '{') return parse_inline_table_value(p, out);
    return parse_bool_or_number(p, out);
}

static int table_get_or_create_child(TomlParser* p, ObjTable* parent, ObjString* key, ObjTable** out_child) {
    Value existing = NIL_VAL;
    if (table_get(&parent->table, key, &existing) && !IS_NIL(existing)) {
        if (!IS_TABLE(existing)) {
            toml_parse_error(p, "key path collides with non-table value.");
            return 0;
        }
        *out_child = AS_TABLE(existing);
        return 1;
    }

    ObjTable* child = new_table();
    push(p->vm, OBJ_VAL(child));
    if (!table_set(&parent->table, key, OBJ_VAL(child))) {
        pop(p->vm);
        vm_runtime_error(p->vm, "toml.parse: out of memory.");
        return 0;
    }
    pop(p->vm);
    *out_child = child;
    return 1;
}

static int set_value_at_path(TomlParser* p, ObjTable* base, ObjString** parts, int count, Value value) {
    if (count <= 0) {
        toml_parse_error(p, "empty key path.");
        return 0;
    }

    ObjTable* cur = base;
    for (int i = 0; i < count - 1; i++) {
        ObjTable* next = NULL;
        if (!table_get_or_create_child(p, cur, parts[i], &next)) return 0;
        cur = next;
    }

    Value existing = NIL_VAL;
    if (table_get(&cur->table, parts[count - 1], &existing) && !IS_NIL(existing)) {
        toml_parse_error(p, "duplicate key.");
        return 0;
    }
    if (!table_set(&cur->table, parts[count - 1], value)) {
        vm_runtime_error(p->vm, "toml.parse: out of memory.");
        return 0;
    }
    return 1;
}

static int toml_array_contains_only_tables(ObjTable* arr) {
    if (!toml_is_array_table(arr)) return 0;
    for (int i = 1; i <= arr->table.array_max; i++) {
        Value v = NIL_VAL;
        if (!table_get_array(&arr->table, i, &v) || IS_NIL(v) || !IS_TABLE(v)) return 0;
    }
    return 1;
}

static int parse_standard_table_header(TomlParser* p, ObjTable* root, ObjTable** out_current) {
    parser_skip_inline_ws(p);

    ObjString* parts[32];
    int count = 0;
    if (!parse_key_path(p, parts, &count)) return 0;

    parser_skip_inline_ws(p);
    if (p->pos >= p->len || p->src[p->pos] != ']') {
        toml_parse_error(p, "expected ']'.");
        return 0;
    }
    p->pos++;

    if (!parser_eat_line_end(p)) return 0;

    ObjTable* cur = root;
    for (int i = 0; i < count; i++) {
        ObjTable* next = NULL;
        if (!table_get_or_create_child(p, cur, parts[i], &next)) return 0;
        cur = next;
    }
    *out_current = cur;
    return 1;
}

static int parse_array_table_header(TomlParser* p, ObjTable* root, ObjTable** out_current) {
    parser_skip_inline_ws(p);

    ObjString* parts[32];
    int count = 0;
    if (!parse_key_path(p, parts, &count)) return 0;
    if (count <= 0) {
        toml_parse_error(p, "empty array-of-tables path.");
        return 0;
    }

    parser_skip_inline_ws(p);
    if (p->pos + 1 >= p->len || p->src[p->pos] != ']' || p->src[p->pos + 1] != ']') {
        toml_parse_error(p, "expected ']]'.");
        return 0;
    }
    p->pos += 2;

    if (!parser_eat_line_end(p)) return 0;

    ObjTable* parent = root;
    for (int i = 0; i < count - 1; i++) {
        ObjTable* next = NULL;
        if (!table_get_or_create_child(p, parent, parts[i], &next)) return 0;
        parent = next;
    }

    ObjString* arr_key = parts[count - 1];
    Value existing = NIL_VAL;
    ObjTable* arr = NULL;
    if (table_get(&parent->table, arr_key, &existing) && !IS_NIL(existing)) {
        if (!IS_TABLE(existing) || !toml_array_contains_only_tables(AS_TABLE(existing))) {
            toml_parse_error(p, "array-of-tables collides with non-array-of-tables value.");
            return 0;
        }
        arr = AS_TABLE(existing);
    } else {
        arr = new_table();
        push(p->vm, OBJ_VAL(arr));
        if (!table_set(&parent->table, arr_key, OBJ_VAL(arr))) {
            pop(p->vm);
            vm_runtime_error(p->vm, "toml.parse: out of memory.");
            return 0;
        }
        pop(p->vm);
    }

    ObjTable* row = new_table();
    push(p->vm, OBJ_VAL(row));
    if (!table_set_array(&arr->table, arr->table.array_max + 1, OBJ_VAL(row))) {
        pop(p->vm);
        vm_runtime_error(p->vm, "toml.parse: out of memory.");
        return 0;
    }
    pop(p->vm);
    *out_current = row;
    return 1;
}

static int parse_table_header(TomlParser* p, ObjTable* root, ObjTable** out_current) {
    if (p->src[p->pos] != '[') {
        toml_parse_error(p, "expected table header.");
        return 0;
    }
    p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '[') {
        p->pos++;
        return parse_array_table_header(p, root, out_current);
    }
    return parse_standard_table_header(p, root, out_current);
}

static int parse_key_value_line(TomlParser* p, ObjTable* current) {
    ObjString* parts[32];
    int count = 0;
    if (!parse_key_path(p, parts, &count)) return 0;

    parser_skip_inline_ws(p);
    if (p->pos >= p->len || p->src[p->pos] != '=') {
        toml_parse_error(p, "expected '=' after key.");
        return 0;
    }
    p->pos++;

    Value value = NIL_VAL;
    if (!parse_value(p, &value)) return 0;
    if (!parser_eat_line_end(p)) return 0;

    return set_value_at_path(p, current, parts, count, value);
}

static int toml_parse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* in = GET_STRING(0);
    TomlParser p = {
        .src = in->chars,
        .pos = 0,
        .len = in->length,
        .line = 1,
        .vm = vm
    };

    ObjTable* root = new_table();
    push(vm, OBJ_VAL(root));
    ObjTable* current = root;

    while (p.pos < p.len) {
        parser_skip_inline_ws(&p);
        if (p.pos >= p.len) break;

        char c = p.src[p.pos];
        if (c == '\n') {
            p.pos++;
            p.line++;
            continue;
        }
        if (c == '#') {
            parser_skip_comment(&p);
            continue;
        }
        if (c == '[') {
            if (!parse_table_header(&p, root, &current)) {
                pop(vm);
                return 0;
            }
            continue;
        }
        if (!parse_key_value_line(&p, current)) {
            pop(vm);
            return 0;
        }
    }

    pop(vm);
    RETURN_OBJ(root);
}

static int toml_is_bare_key(ObjString* s) {
    if (s->length <= 0) return 0;
    for (int i = 0; i < s->length; i++) {
        if (!is_bare_key_char(s->chars[i])) return 0;
    }
    return 1;
}

static int toml_emit_key(StrBuf* out, ObjString* key) {
    if (toml_is_bare_key(key)) {
        return sb_append(out, key->chars, key->length);
    }

    if (!sb_append_char(out, '"')) return 0;
    for (int i = 0; i < key->length; i++) {
        char c = key->chars[i];
        if (c == '"' || c == '\\') {
            if (!sb_append_char(out, '\\')) return 0;
            if (!sb_append_char(out, c)) return 0;
            continue;
        }
        if (c == '\n') {
            if (!sb_append(out, "\\n", 2)) return 0;
            continue;
        }
        if (c == '\r') {
            if (!sb_append(out, "\\r", 2)) return 0;
            continue;
        }
        if (c == '\t') {
            if (!sb_append(out, "\\t", 2)) return 0;
            continue;
        }
        if (!sb_append_char(out, c)) return 0;
    }
    if (!sb_append_char(out, '"')) return 0;
    return 1;
}

static int toml_emit_value(StrBuf* out, Value v, int depth);

static int toml_is_array_table(ObjTable* t) {
    if (t->table.array_max <= 0) return 0;
    for (int i = 0; i < t->table.capacity; i++) {
        Entry* e = &t->table.entries[i];
        if (e->key != NULL && !IS_NIL(e->value)) return 0;
    }
    return 1;
}

static int toml_emit_array(StrBuf* out, ObjTable* arr, int depth) {
    if (!sb_append_char(out, '[')) return 0;
    int first = 1;
    for (int i = 1; i <= arr->table.array_max; i++) {
        Value v = NIL_VAL;
        if (!table_get_array(&arr->table, i, &v) || IS_NIL(v)) {
            return 0;
        }
        if (!first && !sb_append(out, ", ", 2)) return 0;
        if (!toml_emit_value(out, v, depth + 1)) return 0;
        first = 0;
    }
    if (!sb_append_char(out, ']')) return 0;
    return 1;
}

static int toml_is_datetime_literal(ObjString* s) {
    return toml_token_looks_datetime(s->chars, s->length);
}

static int toml_is_array_of_tables(ObjTable* t) {
    if (!toml_is_array_table(t)) return 0;
    if (t->table.array_max <= 0) return 0;
    for (int i = 1; i <= t->table.array_max; i++) {
        Value v = NIL_VAL;
        if (!table_get_array(&t->table, i, &v) || IS_NIL(v) || !IS_TABLE(v)) return 0;
    }
    return 1;
}

static int toml_emit_inline_table(StrBuf* out, ObjTable* t, int depth);

static int toml_emit_value(StrBuf* out, Value v, int depth) {
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (toml_is_datetime_literal(s)) {
            return sb_append(out, s->chars, s->length);
        }
        if (!sb_append_char(out, '"')) return 0;
        for (int i = 0; i < s->length; i++) {
            char c = s->chars[i];
            if (c == '"' || c == '\\') {
                if (!sb_append_char(out, '\\')) return 0;
                if (!sb_append_char(out, c)) return 0;
                continue;
            }
            if (c == '\n') {
                if (!sb_append(out, "\\n", 2)) return 0;
                continue;
            }
            if (c == '\r') {
                if (!sb_append(out, "\\r", 2)) return 0;
                continue;
            }
            if (c == '\t') {
                if (!sb_append(out, "\\t", 2)) return 0;
                continue;
            }
            if (!sb_append_char(out, c)) return 0;
        }
        if (!sb_append_char(out, '"')) return 0;
        return 1;
    }

    if (IS_NUMBER(v)) {
        char num[64];
        snprintf(num, sizeof(num), "%.17g", AS_NUMBER(v));
        return sb_append(out, num, -1);
    }

    if (IS_BOOL(v)) {
        return sb_append(out, AS_BOOL(v) ? "true" : "false", -1);
    }

    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (toml_is_array_table(t)) {
            return toml_emit_array(out, t, depth + 1);
        }
        return toml_emit_inline_table(out, t, depth + 1);
    }

    return 0;
}

static int toml_emit_inline_table(StrBuf* out, ObjTable* t, int depth) {
    if (depth > 64) return 0;
    if (t->table.array_max > 0) return 0;

    if (!sb_append_char(out, '{')) return 0;
    int first = 1;
    for (int i = 0; i < t->table.capacity; i++) {
        Entry* e = &t->table.entries[i];
        if (e->key == NULL || IS_NIL(e->value)) continue;
        if (!first && !sb_append(out, ", ", 2)) return 0;
        if (!toml_emit_key(out, e->key)) return 0;
        if (!sb_append(out, " = ", 3)) return 0;
        if (!toml_emit_value(out, e->value, depth + 1)) return 0;
        first = 0;
    }
    if (!sb_append_char(out, '}')) return 0;
    return 1;
}

static int toml_emit_table(StrBuf* out, ObjTable* table, ObjString** path, int path_len, int depth);
static int toml_emit_array_of_tables(StrBuf* out, ObjTable* arr, ObjString** path, int path_len, int depth);

static int toml_emit_table_body(StrBuf* out, ObjTable* table, ObjString** path, int path_len, int depth) {
    if (depth > 64) return 0;

    for (int i = 0; i < table->table.capacity; i++) {
        Entry* e = &table->table.entries[i];
        if (e->key == NULL || IS_NIL(e->value)) continue;
        if (IS_TABLE(e->value)) {
            ObjTable* t = AS_TABLE(e->value);
            if (toml_is_array_of_tables(t)) continue;
            if (!toml_is_array_table(t)) continue;
        }

        if (!toml_emit_key(out, e->key)) return 0;
        if (!sb_append(out, " = ", 3)) return 0;
        if (!toml_emit_value(out, e->value, depth + 1)) return 0;
        if (!sb_append_char(out, '\n')) return 0;
    }

    for (int i = 0; i < table->table.capacity; i++) {
        Entry* e = &table->table.entries[i];
        if (e->key == NULL || IS_NIL(e->value) || !IS_TABLE(e->value)) continue;
        ObjTable* t = AS_TABLE(e->value);
        if (toml_is_array_table(t)) continue;

        if (!sb_append_char(out, '\n')) return 0;
        path[path_len] = e->key;
        if (!toml_emit_table(out, t, path, path_len + 1, depth + 1)) return 0;
    }

    for (int i = 0; i < table->table.capacity; i++) {
        Entry* e = &table->table.entries[i];
        if (e->key == NULL || IS_NIL(e->value) || !IS_TABLE(e->value)) continue;
        ObjTable* t = AS_TABLE(e->value);
        if (!toml_is_array_of_tables(t)) continue;

        path[path_len] = e->key;
        if (!toml_emit_array_of_tables(out, t, path, path_len + 1, depth + 1)) return 0;
    }

    return 1;
}

static int toml_emit_table(StrBuf* out, ObjTable* table, ObjString** path, int path_len, int depth) {
    if (depth > 64) return 0;

    if (path_len > 0) {
        if (!sb_append_char(out, '[')) return 0;
        for (int i = 0; i < path_len; i++) {
            if (i > 0 && !sb_append_char(out, '.')) return 0;
            if (!toml_emit_key(out, path[i])) return 0;
        }
        if (!sb_append(out, "]\n", 2)) return 0;
    }

    return toml_emit_table_body(out, table, path, path_len, depth + 1);
}

static int toml_emit_array_of_tables(StrBuf* out, ObjTable* arr, ObjString** path, int path_len, int depth) {
    if (depth > 64) return 0;
    for (int i = 1; i <= arr->table.array_max; i++) {
        Value row = NIL_VAL;
        if (!table_get_array(&arr->table, i, &row) || IS_NIL(row) || !IS_TABLE(row)) return 0;

        if (!sb_append(out, "\n[[", 3)) return 0;
        for (int j = 0; j < path_len; j++) {
            if (j > 0 && !sb_append_char(out, '.')) return 0;
            if (!toml_emit_key(out, path[j])) return 0;
        }
        if (!sb_append(out, "]]\n", 3)) return 0;

        if (!toml_emit_table_body(out, AS_TABLE(row), path, path_len, depth + 1)) return 0;
    }
    return 1;
}

static int toml_stringify(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* root = GET_TABLE(0);
    StrBuf out = {0};
    ObjString* path[64];

    if (!toml_emit_table(&out, root, path, 0, 0)) {
        free(out.data);
        vm_runtime_error(vm, "toml.stringify: unsupported value (requires table with scalar/array values).");
        return 0;
    }

    ObjString* s = copy_string(out.data != NULL ? out.data : "", out.len);
    free(out.data);
    RETURN_OBJ(s);
}

void register_toml(VM* vm) {
    static const NativeReg funcs[] = {
        {"parse", toml_parse},
        {"stringify", toml_stringify},
        {NULL, NULL}
    };
    register_module(vm, "toml", funcs);
}
