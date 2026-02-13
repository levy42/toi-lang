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

static void sbInit(StringBuilder* sb) {
    sb->capacity = 256;
    sb->buffer = (char*)malloc(sb->capacity);
    sb->length = 0;
    sb->buffer[0] = '\0';
}

static void sbAppend(StringBuilder* sb, const char* str, int len) {
    if (len < 0) len = strlen(str);
    while (sb->length + len + 1 > sb->capacity) {
        sb->capacity *= 2;
        sb->buffer = (char*)realloc(sb->buffer, sb->capacity);
    }
    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

static void sbAppendChar(StringBuilder* sb, char c) {
    char s[2] = {c, '\0'};
    sbAppend(sb, s, 1);
}

static void encodeValue(StringBuilder* sb, Value value, int depth);

static void encodeString(StringBuilder* sb, const char* str, int len) {
    sbAppendChar(sb, '"');
    for (int i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':  sbAppend(sb, "\\\"", 2); break;
            case '\\': sbAppend(sb, "\\\\", 2); break;
            case '\b': sbAppend(sb, "\\b", 2); break;
            case '\f': sbAppend(sb, "\\f", 2); break;
            case '\n': sbAppend(sb, "\\n", 2); break;
            case '\r': sbAppend(sb, "\\r", 2); break;
            case '\t': sbAppend(sb, "\\t", 2); break;
            default:
                if ((unsigned char)c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    sbAppend(sb, buf, 6);
                } else {
                    sbAppendChar(sb, c);
                }
        }
    }
    sbAppendChar(sb, '"');
}

static void encodeTable(StringBuilder* sb, ObjTable* table, int depth) {
    if (depth > 100) {
        sbAppend(sb, "null", 4); // Prevent infinite recursion
        return;
    }

    // Check if it's an array (consecutive integer keys starting at 1)
    int arrayLen = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) {
            arrayLen = i - 1;
            break;
        }
    }

    // Check if there are any string keys
    int hasStringKeys = 0;
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            hasStringKeys = 1;
            break;
        }
    }

    if (arrayLen > 0 && !hasStringKeys) {
        // Encode as JSON array
        sbAppendChar(sb, '[');
        for (int i = 1; i <= arrayLen; i++) {
            if (i > 1) sbAppendChar(sb, ',');
            Value val;
            tableGetArray(&table->table, i, &val);
            encodeValue(sb, val, depth + 1);
        }
        sbAppendChar(sb, ']');
    } else {
        // Encode as JSON object
        sbAppendChar(sb, '{');
        int first = 1;

        // String keys
        for (int i = 0; i < table->table.capacity; i++) {
            Entry* entry = &table->table.entries[i];
            if (entry->key != NULL && !IS_NIL(entry->value)) {
                if (!first) sbAppendChar(sb, ',');
                first = 0;
                encodeString(sb, entry->key->chars, entry->key->length);
                sbAppendChar(sb, ':');
                encodeValue(sb, entry->value, depth + 1);
            }
        }

        // Also include array part if mixed
        if (arrayLen > 0) {
            for (int i = 1; i <= arrayLen; i++) {
                if (!first) sbAppendChar(sb, ',');
                first = 0;
                char key[32];
                snprintf(key, sizeof(key), "\"%d\"", i);
                sbAppend(sb, key, -1);
                sbAppendChar(sb, ':');
                Value val;
                tableGetArray(&table->table, i, &val);
                encodeValue(sb, val, depth + 1);
            }
        }

        sbAppendChar(sb, '}');
    }
}

static void encodeValue(StringBuilder* sb, Value value, int depth) {
    if (IS_NIL(value)) {
        sbAppend(sb, "null", 4);
    } else if (IS_BOOL(value)) {
        if (AS_BOOL(value)) {
            sbAppend(sb, "true", 4);
        } else {
            sbAppend(sb, "false", 5);
        }
    } else if (IS_NUMBER(value)) {
        double num = AS_NUMBER(value);
        char buf[64];
        if (isinf(num) || isnan(num)) {
            sbAppend(sb, "null", 4); // JSON doesn't support inf/nan
        } else if (num == floor(num) && fabs(num) < 1e15) {
            snprintf(buf, sizeof(buf), "%.0f", num);
            sbAppend(sb, buf, -1);
        } else {
            snprintf(buf, sizeof(buf), "%.17g", num);
            sbAppend(sb, buf, -1);
        }
    } else if (IS_STRING(value)) {
        ObjString* str = AS_STRING(value);
        encodeString(sb, str->chars, str->length);
    } else if (IS_TABLE(value)) {
        encodeTable(sb, AS_TABLE(value), depth);
    } else {
        // Functions, userdata, etc. become null
        sbAppend(sb, "null", 4);
    }
}

// json.encode(value) -> string
static int json_encode(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);

    StringBuilder sb;
    sbInit(&sb);
    encodeValue(&sb, args[0], 0);

    ObjString* result = copyString(sb.buffer, sb.length);
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

static Value parseValue(Parser* p);

static void skipWhitespace(Parser* p) {
    while (p->pos < p->length) {
        char c = p->json[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static int parseChar(Parser* p, char expected) {
    skipWhitespace(p);
    if (p->pos < p->length && p->json[p->pos] == expected) {
        p->pos++;
        return 1;
    }
    return 0;
}

static Value parseString(Parser* p) {
    if (!parseChar(p, '"')) {
        snprintf(p->error, sizeof(p->error), "Expected '\"'");
        return NIL_VAL;
    }

    StringBuilder sb;
    sbInit(&sb);

    while (p->pos < p->length && p->json[p->pos] != '"') {
        char c = p->json[p->pos++];
        if (c == '\\' && p->pos < p->length) {
            char esc = p->json[p->pos++];
            switch (esc) {
                case '"':  sbAppendChar(&sb, '"'); break;
                case '\\': sbAppendChar(&sb, '\\'); break;
                case '/':  sbAppendChar(&sb, '/'); break;
                case 'b':  sbAppendChar(&sb, '\b'); break;
                case 'f':  sbAppendChar(&sb, '\f'); break;
                case 'n':  sbAppendChar(&sb, '\n'); break;
                case 'r':  sbAppendChar(&sb, '\r'); break;
                case 't':  sbAppendChar(&sb, '\t'); break;
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
                        sbAppendChar(&sb, (char)codepoint);
                    } else if (codepoint < 2048) {
                        sbAppendChar(&sb, (char)(0xC0 | (codepoint >> 6)));
                        sbAppendChar(&sb, (char)(0x80 | (codepoint & 0x3F)));
                    } else {
                        sbAppendChar(&sb, (char)(0xE0 | (codepoint >> 12)));
                        sbAppendChar(&sb, (char)(0x80 | ((codepoint >> 6) & 0x3F)));
                        sbAppendChar(&sb, (char)(0x80 | (codepoint & 0x3F)));
                    }
                    break;
                }
                default:
                    sbAppendChar(&sb, esc);
            }
        } else {
            sbAppendChar(&sb, c);
        }
    }

    if (!parseChar(p, '"')) {
        free(sb.buffer);
        snprintf(p->error, sizeof(p->error), "Unterminated string");
        return NIL_VAL;
    }

    ObjString* str = copyString(sb.buffer, sb.length);
    free(sb.buffer);
    return OBJ_VAL(str);
}

static Value parseNumber(Parser* p) {
    skipWhitespace(p);
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

    char* numStr = (char*)malloc(p->pos - start + 1);
    memcpy(numStr, p->json + start, p->pos - start);
    numStr[p->pos - start] = '\0';

    double num = strtod(numStr, NULL);
    free(numStr);

    return NUMBER_VAL(num);
}

static Value parseArray(Parser* p) {
    if (!parseChar(p, '[')) {
        snprintf(p->error, sizeof(p->error), "Expected '['");
        return NIL_VAL;
    }

    ObjTable* table = newTable();
    push(p->vm, OBJ_VAL(table)); // GC protection

    int index = 1;
    skipWhitespace(p);

    if (p->pos < p->length && p->json[p->pos] != ']') {
        do {
            Value val = parseValue(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }
            tableSetArray(&table->table, index++, val);
        } while (parseChar(p, ','));
    }

    if (!parseChar(p, ']')) {
        pop(p->vm);
        snprintf(p->error, sizeof(p->error), "Expected ']'");
        return NIL_VAL;
    }

    pop(p->vm);
    return OBJ_VAL(table);
}

static Value parseObject(Parser* p) {
    if (!parseChar(p, '{')) {
        snprintf(p->error, sizeof(p->error), "Expected '{'");
        return NIL_VAL;
    }

    ObjTable* table = newTable();
    push(p->vm, OBJ_VAL(table)); // GC protection

    skipWhitespace(p);

    if (p->pos < p->length && p->json[p->pos] != '}') {
        do {
            skipWhitespace(p);
            Value keyVal = parseString(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }

            if (!parseChar(p, ':')) {
                pop(p->vm);
                snprintf(p->error, sizeof(p->error), "Expected ':'");
                return NIL_VAL;
            }

            Value val = parseValue(p);
            if (p->error[0] != '\0') {
                pop(p->vm);
                return NIL_VAL;
            }

            tableSet(&table->table, AS_STRING(keyVal), val);
        } while (parseChar(p, ','));
    }

    if (!parseChar(p, '}')) {
        pop(p->vm);
        snprintf(p->error, sizeof(p->error), "Expected '}'");
        return NIL_VAL;
    }

    pop(p->vm);
    return OBJ_VAL(table);
}

static Value parseValue(Parser* p) {
    skipWhitespace(p);

    if (p->pos >= p->length) {
        snprintf(p->error, sizeof(p->error), "Unexpected end of input");
        return NIL_VAL;
    }

    char c = p->json[p->pos];

    if (c == '"') {
        return parseString(p);
    } else if (c == '{') {
        return parseObject(p);
    } else if (c == '[') {
        return parseArray(p);
    } else if (c == '-' || isdigit(c)) {
        return parseNumber(p);
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
static int json_decode(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);

    Parser p;
    p.json = str->chars;
    p.pos = 0;
    p.length = str->length;
    p.vm = vm;
    p.error[0] = '\0';

    Value result = parseValue(&p);

    if (p.error[0] != '\0') {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString(p.error, strlen(p.error))));
        return 2;
    }

    skipWhitespace(&p);
    if (p.pos < p.length) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copyString("Trailing content after JSON", 27)));
        return 2;
    }

    RETURN_VAL(result);
}

void registerJSON(VM* vm) {
    const NativeReg jsonFuncs[] = {
        {"encode", json_encode},
        {"decode", json_decode},
        {NULL, NULL}
    };
    registerModule(vm, "json", jsonFuncs);
    pop(vm); // Pop json module
}
