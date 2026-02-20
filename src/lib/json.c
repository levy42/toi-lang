#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// ============ JSON Encoder ============

typedef struct {
    char* buffer;
    int capacity;
    int length;
} StringBuilder;

static void sb_init(StringBuilder* sb) {
    sb->capacity = 256;
    sb->buffer = (char*)malloc(sb->capacity);
    sb->length = 0;
    sb->buffer[0] = '\0';
}

static void sb_append(StringBuilder* sb, const char* str, int len) {
    if (len < 0) len = strlen(str);
    while (sb->length + len + 1 > sb->capacity) {
        sb->capacity *= 2;
        sb->buffer = (char*)realloc(sb->buffer, sb->capacity);
    }
    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

static void sb_append_char(StringBuilder* sb, char c) {
    char s[2] = {c, '\0'};
    sb_append(sb, s, 1);
}

static void encode_value(StringBuilder* sb, Value value, int depth);

static void encode_string(StringBuilder* sb, const char* str, int len) {
    sb_append_char(sb, '"');
    for (int i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':  sb_append(sb, "\\\"", 2); break;
            case '\\': sb_append(sb, "\\\\", 2); break;
            case '\b': sb_append(sb, "\\b", 2); break;
            case '\f': sb_append(sb, "\\f", 2); break;
            case '\n': sb_append(sb, "\\n", 2); break;
            case '\r': sb_append(sb, "\\r", 2); break;
            case '\t': sb_append(sb, "\\t", 2); break;
            default:
                if ((unsigned char)c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    sb_append(sb, buf, 6);
                } else {
                    sb_append_char(sb, c);
                }
        }
    }
    sb_append_char(sb, '"');
}

static void encode_table(StringBuilder* sb, ObjTable* table, int depth) {
    if (depth > 100) {
        sb_append(sb, "null", 4); // Prevent infinite recursion
        return;
    }

    // Check if it's an array (consecutive integer keys starting at 1)
    int array_len = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) {
            array_len = i - 1;
            break;
        }
    }

    // Check if there are any string keys
    int has_string_keys = 0;
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            has_string_keys = 1;
            break;
        }
    }

    if (array_len > 0 && !has_string_keys) {
        // Encode as JSON array
        sb_append_char(sb, '[');
        for (int i = 1; i <= array_len; i++) {
            if (i > 1) sb_append_char(sb, ',');
            Value val;
            table_get_array(&table->table, i, &val);
            encode_value(sb, val, depth + 1);
        }
        sb_append_char(sb, ']');
    } else {
        // Encode as JSON object
        sb_append_char(sb, '{');
        int first = 1;

        // String keys
        for (int i = 0; i < table->table.capacity; i++) {
            Entry* entry = &table->table.entries[i];
            if (entry->key != NULL && !IS_NIL(entry->value)) {
                if (!first) sb_append_char(sb, ',');
                first = 0;
                encode_string(sb, entry->key->chars, entry->key->length);
                sb_append_char(sb, ':');
                encode_value(sb, entry->value, depth + 1);
            }
        }

        // Also include array part if mixed
        if (array_len > 0) {
            for (int i = 1; i <= array_len; i++) {
                if (!first) sb_append_char(sb, ',');
                first = 0;
                char key[32];
                snprintf(key, sizeof(key), "\"%d\"", i);
                sb_append(sb, key, -1);
                sb_append_char(sb, ':');
                Value val;
                table_get_array(&table->table, i, &val);
                encode_value(sb, val, depth + 1);
            }
        }

        sb_append_char(sb, '}');
    }
}

static void encode_value(StringBuilder* sb, Value value, int depth) {
    if (IS_NIL(value)) {
        sb_append(sb, "null", 4);
    } else if (IS_BOOL(value)) {
        if (AS_BOOL(value)) {
            sb_append(sb, "true", 4);
        } else {
            sb_append(sb, "false", 5);
        }
    } else if (IS_NUMBER(value)) {
        double num = AS_NUMBER(value);
        char buf[64];
        if (isinf(num) || isnan(num)) {
            sb_append(sb, "null", 4); // JSON doesn't support inf/nan
        } else if (num == floor(num) && fabs(num) < 1e15) {
            snprintf(buf, sizeof(buf), "%.0f", num);
            sb_append(sb, buf, -1);
        } else {
            snprintf(buf, sizeof(buf), "%.17g", num);
            sb_append(sb, buf, -1);
        }
    } else if (IS_STRING(value)) {
        ObjString* str = AS_STRING(value);
        encode_string(sb, str->chars, str->length);
    } else if (IS_TABLE(value)) {
        encode_table(sb, AS_TABLE(value), depth);
    } else {
        // Functions, userdata, etc. become null
        sb_append(sb, "null", 4);
    }
}

// json.encode(value) -> string
static int json_encode(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);

    StringBuilder sb;
    sb_init(&sb);
    encode_value(&sb, args[0], 0);

    ObjString* result = copy_string(sb.buffer, sb.length);
    free(sb.buffer);

    RETURN_OBJ(result);
}

// ============ JSON Decoder ============

typedef struct {
    const char* json;
    int pos;
    int length;
    VM* vm;
    char error[256];
} Parser;

static Value parse_value(Parser* p);

static void skip_whitespace(Parser* p) {
    while (p->pos < p->length) {
        char c = p->json[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static int parse_char(Parser* p, char expected) {
    skip_whitespace(p);
    if (p->pos < p->length && p->json[p->pos] == expected) {
        p->pos++;
        return 1;
    }
    return 0;
}

static Value parse_string(Parser* p) {
    if (!parse_char(p, '"')) {
        snprintf(p->error, sizeof(p->error), "Expected '\"'");
        return NIL_VAL;
    }

    StringBuilder sb;
    sb_init(&sb);

    while (p->pos < p->length && p->json[p->pos] != '"') {
        char c = p->json[p->pos++];
        if (c == '\\' && p->pos < p->length) {
            char esc = p->json[p->pos++];
            switch (esc) {
                case '"':  sb_append_char(&sb, '"'); break;
                case '\\': sb_append_char(&sb, '\\'); break;
                case '/':  sb_append_char(&sb, '/'); break;
                case 'b':  sb_append_char(&sb, '\b'); break;
                case 'f':  sb_append_char(&sb, '\f'); break;
                case 'n':  sb_append_char(&sb, '\n'); break;
                case 'r':  sb_append_char(&sb, '\r'); break;
                case 't':  sb_append_char(&sb, '\t'); break;
                case 'u': {
                    // Parse 4 hex digits
                    if (p->pos + 4 > p->length) {
                        free(sb.buffer);
                        snprintf(p->error, sizeof(p->error), "Invalid unicode escape");
                        return NIL_VAL;
                    }
                    char hex[5] = {p->json[p->pos], p->json[p->pos+1],
                                   p->json[p->pos+2], p->json[p->pos+3], '\0'};
                    p->pos += 4;
                    int codepoint = (int)strtol(hex, NULL, 16);
                    if (codepoint < 128) {
                        sb_append_char(&sb, (char)codepoint);
                    } else if (codepoint < 2048) {
                        sb_append_char(&sb, (char)(0xC0 | (codepoint >> 6)));
                        sb_append_char(&sb, (char)(0x80 | (codepoint & 0x3F)));
                    } else {
                        sb_append_char(&sb, (char)(0xE0 | (codepoint >> 12)));
                        sb_append_char(&sb, (char)(0x80 | ((codepoint >> 6) & 0x3F)));
                        sb_append_char(&sb, (char)(0x80 | (codepoint & 0x3F)));
                    }
                    break;
                }
                default:
                    sb_append_char(&sb, esc);
            }
        } else {
            sb_append_char(&sb, c);
        }
    }

    if (!parse_char(p, '"')) {
        free(sb.buffer);
        snprintf(p->error, sizeof(p->error), "Unterminated string");
        return NIL_VAL;
    }

    ObjString* str = copy_string(sb.buffer, sb.length);
    free(sb.buffer);
    return OBJ_VAL(str);
}

static Value parse_number(Parser* p) {
    skip_whitespace(p);
    int start = p->pos;

    if (p->json[p->pos] == '-') p->pos++;

    while (p->pos < p->length && isdigit(p->json[p->pos])) p->pos++;

    if (p->pos < p->length && p->json[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->length && isdigit(p->json[p->pos])) p->pos++;
    }

    if (p->pos < p->length && (p->json[p->pos] == 'e' || p->json[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->length && (p->json[p->pos] == '+' || p->json[p->pos] == '-')) {
            p->pos++;
        }
        while (p->pos < p->length && isdigit(p->json[p->pos])) p->pos++;
    }

    char* num_str = (char*)malloc(p->pos - start + 1);
    memcpy(num_str, p->json + start, p->pos - start);
    num_str[p->pos - start] = '\0';

    double num = strtod(num_str, NULL);
    free(num_str);

    return NUMBER_VAL(num);
}

static Value parse_array(Parser* p) {
    if (!parse_char(p, '[')) {
        snprintf(p->error, sizeof(p->error), "Expected '['");
        return NIL_VAL;
    }

    ObjTable* table = new_table();
    push(p->vm, OBJ_VAL(table)); // GC protection

    int index = 1;
    skip_whitespace(p);

    if (p->pos < p->length && p->json[p->pos] != ']') {
        do {
            Value val = parse_value(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }
            table_set_array(&table->table, index++, val);
        } while (parse_char(p, ','));
    }

    if (!parse_char(p, ']')) {
        pop(p->vm);
        snprintf(p->error, sizeof(p->error), "Expected ']'");
        return NIL_VAL;
    }

    pop(p->vm);
    return OBJ_VAL(table);
}

static Value parse_object(Parser* p) {
    if (!parse_char(p, '{')) {
        snprintf(p->error, sizeof(p->error), "Expected '{'");
        return NIL_VAL;
    }

    ObjTable* table = new_table();
    push(p->vm, OBJ_VAL(table)); // GC protection

    skip_whitespace(p);

    if (p->pos < p->length && p->json[p->pos] != '}') {
        do {
            skip_whitespace(p);
            Value key_val = parse_string(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }

            if (!parse_char(p, ':')) {
                pop(p->vm);
                snprintf(p->error, sizeof(p->error), "Expected ':'");
                return NIL_VAL;
            }

            Value val = parse_value(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }

            table_set(&table->table, AS_STRING(key_val), val);
        } while (parse_char(p, ','));
    }

    if (!parse_char(p, '}')) {
        pop(p->vm);
        snprintf(p->error, sizeof(p->error), "Expected '}'");
        return NIL_VAL;
    }

    pop(p->vm);
    return OBJ_VAL(table);
}

static Value parse_value(Parser* p) {
    skip_whitespace(p);

    if (p->pos >= p->length) {
        snprintf(p->error, sizeof(p->error), "Unexpected end of input");
        return NIL_VAL;
    }

    char c = p->json[p->pos];

    if (c == '"') {
        return parse_string(p);
    } else if (c == '{') {
        return parse_object(p);
    } else if (c == '[') {
        return parse_array(p);
    } else if (c == '-' || isdigit(c)) {
        return parse_number(p);
    } else if (strncmp(p->json + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return BOOL_VAL(1);
    } else if (strncmp(p->json + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return BOOL_VAL(0);
    } else if (strncmp(p->json + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return NIL_VAL;
    } else {
        snprintf(p->error, sizeof(p->error), "Unexpected character '%c'", c);
        return NIL_VAL;
    }
}

// json.decode(string) -> value
static int json_decode(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);

    Parser p;
    p.json = str->chars;
    p.pos = 0;
    p.length = str->length;
    p.vm = vm;
    p.error[0] = '\0';

    Value result = parse_value(&p);

    if (p.error[0] != '\0') {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(p.error, strlen(p.error))));
        return 2;
    }

    skip_whitespace(&p);
    if (p.pos < p.length) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("Trailing content after JSON", 27)));
        return 2;
    }

    RETURN_VAL(result);
}

void register_json(VM* vm) {
    const NativeReg json_funcs[] = {
        {"encode", json_encode},
        {"decode", json_decode},
        {NULL, NULL}
    };
    register_module(vm, "json", json_funcs);
    pop(vm); // Pop json module
}
