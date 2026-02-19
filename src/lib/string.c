#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int string_len(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);
    RETURN_NUMBER(GET_STRING(0)->length);
}

static int string_sub(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_NUMBER(1);
    
    ObjString* str = GET_STRING(0);
    int start = (int)GET_NUMBER(1);
    int end = str->length;
    
    if (argCount >= 3) { ASSERT_NUMBER(2); end = (int)GET_NUMBER(2); }
    
    // Adjust 1-based indexing to 0-based
    start--; 
    
    // Handle negative indices
    if (start < 0) start = str->length + start + 1; 
    if (end < 0) end = str->length + end + 1;
    
    // Clamp
    if (start < 0) start = 0;
    if (end > str->length) end = str->length;
    
    int len = end - start;
    if (len <= 0) {
        RETURN_STRING("", 0);
    }
    
    char* sub = (char*)malloc(len + 1);
    memcpy(sub, str->chars + start, len);
    sub[len] = '\0';
    
    RETURN_OBJ(takeString(sub, len));
}

static int string_lower(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    
    char* buf = (char*)malloc(str->length + 1);
    for (int i = 0; i < str->length; i++) {
        buf[i] = tolower(str->chars[i]);
    }
    buf[str->length] = '\0';
    
    RETURN_OBJ(takeString(buf, str->length));
}

static int string_upper(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    
    char* buf = (char*)malloc(str->length + 1);
    for (int i = 0; i < str->length; i++) {
        buf[i] = toupper(str->chars[i]);
    }
    buf[str->length] = '\0';
    
    RETURN_OBJ(takeString(buf, str->length));
}

static int string_char(VM* vm, int argCount, Value* args) {
    int len = argCount;
    char* buf = (char*)malloc(len + 1);
    
    for (int i = 0; i < len; i++) {
        ASSERT_NUMBER(i);
        buf[i] = (char)GET_NUMBER(i);
    }
    buf[len] = '\0';
    
    RETURN_OBJ(takeString(buf, len));
}

static int string_byte(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    int index = 1;
    if (argCount >= 2) { ASSERT_NUMBER(1); index = (int)GET_NUMBER(1); }

    index--; // 1-based
    if (index < 0 || index >= str->length) {
        RETURN_NIL;
    }

    RETURN_NUMBER((double)(unsigned char)str->chars[index]);
}

static int string_find(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_STRING(0);
    ASSERT_STRING(1);

    ObjString* str = GET_STRING(0);
    ObjString* pattern = GET_STRING(1);
    int start = 1;
    if (argCount >= 3) { ASSERT_NUMBER(2); start = (int)GET_NUMBER(2); }

    start--; // 1-based to 0-based
    if (start < 0) start = 0;
    if (start >= str->length) { RETURN_NIL; }

    char* found = strstr(str->chars + start, pattern->chars);
    if (found == NULL) { RETURN_NIL; }

    int pos = (int)(found - str->chars) + 1; // Back to 1-based
    push(vm, NUMBER_VAL(pos));
    push(vm, NUMBER_VAL(pos + pattern->length - 1));
    return 2;
}

static int string_trim(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* s = str->chars;
    int len = str->length;

    // Trim leading whitespace
    int start = 0;
    while (start < len && (s[start] == ' ' || s[start] == '\t' ||
           s[start] == '\n' || s[start] == '\r')) {
        start++;
    }

    // Trim trailing whitespace
    int end = len;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' ||
           s[end-1] == '\n' || s[end-1] == '\r')) {
        end--;
    }

    int newLen = end - start;
    if (newLen <= 0) {
        RETURN_STRING("", 0);
    }

    char* buf = (char*)malloc(newLen + 1);
    memcpy(buf, s + start, newLen);
    buf[newLen] = '\0';
    RETURN_OBJ(takeString(buf, newLen));
}

static int string_split(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    const char* sep = " ";
    int sepLen = 1;

    if (argCount >= 2) {
        ASSERT_STRING(1);
        sep = GET_CSTRING(1);
        sepLen = GET_STRING(1)->length;
    }

    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result)); // Protect from GC

    int index = 1;
    const char* s = str->chars;
    const char* end = s + str->length;

    if (sepLen == 0) {
        // Empty separator: split into characters
        for (int i = 0; i < str->length; i++) {
            Value val = OBJ_VAL(copyString(s + i, 1));
            tableSetArray(&result->table, index++, val);
        }
    } else {
        while (s < end) {
            const char* found = strstr(s, sep);
            if (found == NULL) {
                // No more separators, add rest of string
                int len = (int)(end - s);
                Value val = OBJ_VAL(copyString(s, len));
                tableSetArray(&result->table, index++, val);
                break;
            } else {
                // Add substring before separator
                int len = (int)(found - s);
                Value val = OBJ_VAL(copyString(s, len));
                tableSetArray(&result->table, index++, val);
                s = found + sepLen;
            }
        }
    }

    pop(vm); // Pop result table (will be returned)
    RETURN_OBJ(result);
}

static int string_rep(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_NUMBER(1);

    ObjString* str = GET_STRING(0);
    int n = (int)GET_NUMBER(1);

    if (n <= 0) {
        RETURN_STRING("", 0);
    }

    int newLen = str->length * n;
    char* buf = (char*)malloc(newLen + 1);

    for (int i = 0; i < n; i++) {
        memcpy(buf + i * str->length, str->chars, str->length);
    }
    buf[newLen] = '\0';

    RETURN_OBJ(takeString(buf, newLen));
}

static int string_reverse(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* str = GET_STRING(0);
    char* buf = (char*)malloc(str->length + 1);

    for (int i = 0; i < str->length; i++) {
        buf[i] = str->chars[str->length - 1 - i];
    }
    buf[str->length] = '\0';

    RETURN_OBJ(takeString(buf, str->length));
}

static int string_join(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_STRING(0);
    ASSERT_TABLE(1);

    ObjString* sep = GET_STRING(0);
    ObjTable* list = GET_TABLE(1);

    int totalLen = 0;
    int count = 0;

    // Iterate sequential integer keys starting from 1
    int i = 1;
    Value val;

    // We use a loop that continues as long as we find values at consecutive indices
    // Or should we use table->arrayCapacity?
    // Arrays in this language are 1-based.
    // Let's assume standard behavior: iterate 1..N until nil.
    
    // Note: iterating until nil might stop early for sparse arrays.
    // But join/split usually implies dense arrays.
    // Alternatively, iterate arrayCapacity? No, array part might be sparse too or have holes.
    // Iterating until nil is safer for sequence.
    
    while (1) {
        // Try array optimization first
        if (!tableGetArray(&list->table, i, &val)) {
            // Fallback to hash lookup (for numeric keys stored in hash part)
            ObjString* key = numberKeyString((double)i);
            if (!tableGet(&list->table, key, &val) || IS_NIL(val)) {
                // End of sequence
                break;
            }
        }

        if (IS_STRING(val)) {
            totalLen += AS_STRING(val)->length;
        } else if (IS_NUMBER(val)) {
            double num = AS_NUMBER(val);
            char numbuf[32];
            int slen;
            if (num == (int)num) {
                slen = snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
            } else {
                slen = snprintf(numbuf, sizeof(numbuf), "%g", num);
            }
            totalLen += slen;
        } else {
            vmRuntimeError(vm, "string.join: list contains non-string/number element");
            return 0;
        }

        count++;
        i++;
    }

    if (count == 0) {
        RETURN_STRING("", 0);
    }

    totalLen += sep->length * (count - 1);
    char* buffer = (char*)malloc(totalLen + 1);
    int length = 0;
    int first = 1;

    i = 1;
    while (1) {
        if (!tableGetArray(&list->table, i, &val)) {
            ObjString* key = numberKeyString((double)i);
            if (!tableGet(&list->table, key, &val) || IS_NIL(val)) {
                break;
            }
        }

        if (!first) {
            memcpy(buffer + length, sep->chars, sep->length);
            length += sep->length;
        }

        const char* s;
        int slen;
        char numbuf[32];

        if (IS_STRING(val)) {
            s = AS_STRING(val)->chars;
            slen = AS_STRING(val)->length;
        } else if (IS_NUMBER(val)) {
            double num = AS_NUMBER(val);
            if (num == (int)num) {
                slen = snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
            } else {
                slen = snprintf(numbuf, sizeof(numbuf), "%g", num);
            }
            s = numbuf;
        } else {
            free(buffer);
            vmRuntimeError(vm, "string.join: list contains non-string/number element");
            return 0;
        }

        memcpy(buffer + length, s, slen);
        length += slen;

        first = 0;
        i++;
    }

    buffer[length] = '\0';
    ObjString* result = takeString(buffer, length);
    RETURN_OBJ(result);
}

// string.format(fmt, ...) - printf-style formatting
static int string_format(VM* vm, int argCount, Value* args) {

    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* fmt = GET_STRING(0);
    const char* f = fmt->chars;
    int flen = fmt->length;

    // Allocate result buffer
    int capacity = flen * 2 + 128;
    char* result = (char*)malloc(capacity);
    int rlen = 0;

    int argIdx = 1;

    for (int i = 0; i < flen; i++) {
        if (f[i] == '%' && i + 1 < flen) {
            char spec = f[i + 1];
            i++;

            if (spec == '%') {
                // Literal %
                if (rlen + 1 >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                result[rlen++] = '%';
            } else if (spec == 's') {
                // String
                if (argIdx >= argCount) {
                    free(result);
                    vmRuntimeError(vm, "string.format: not enough arguments");
                    return 0;
                }
                Value v = args[argIdx++];
                const char* s;
                int slen;
                char numbuf[64];
                if (IS_STRING(v)) {
                    s = AS_STRING(v)->chars;
                    slen = AS_STRING(v)->length;
                } else if (IS_NUMBER(v)) {
                    double num = AS_NUMBER(v);
                    if (num == (int)num) {
                        slen = snprintf(numbuf, sizeof(numbuf), "%d", (int)num);
                    } else {
                        slen = snprintf(numbuf, sizeof(numbuf), "%g", num);
                    }
                    s = numbuf;
                } else if (IS_NIL(v)) {
                    s = "nil"; slen = 3;
                } else if (IS_BOOL(v)) {
                    s = AS_BOOL(v) ? "true" : "false";
                    slen = AS_BOOL(v) ? 4 : 5;
                } else {
                    s = "<value>"; slen = 7;
                }
                while (rlen + slen >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                memcpy(result + rlen, s, slen);
                rlen += slen;
            } else if (spec == 'd' || spec == 'i') {
                // Integer
                if (argIdx >= argCount) {
                    free(result);
                    vmRuntimeError(vm, "string.format: not enough arguments");
                    return 0;
                }
                Value v = args[argIdx++];
                if (!IS_NUMBER(v)) {
                    free(result);
                    vmRuntimeError(vm, "string.format: %%d expects number");
                    return 0;
                }
                char buf[64];
                int len = snprintf(buf, sizeof(buf), "%d", (int)AS_NUMBER(v));
                while (rlen + len >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                memcpy(result + rlen, buf, len);
                rlen += len;
            } else if (spec == 'f') {
                // Float
                if (argIdx >= argCount) {
                    free(result);
                    vmRuntimeError(vm, "string.format: not enough arguments");
                    return 0;
                }
                Value v = args[argIdx++];
                if (!IS_NUMBER(v)) {
                    free(result);
                    vmRuntimeError(vm, "string.format: %%f expects number");
                    return 0;
                }
                char buf[64];
                int len = snprintf(buf, sizeof(buf), "%f", AS_NUMBER(v));
                while (rlen + len >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                memcpy(result + rlen, buf, len);
                rlen += len;
            } else if (spec == 'g') {
                // General float
                if (argIdx >= argCount) {
                    free(result);
                    vmRuntimeError(vm, "string.format: not enough arguments");
                    return 0;
                }
                Value v = args[argIdx++];
                if (!IS_NUMBER(v)) {
                    free(result);
                    vmRuntimeError(vm, "string.format: %%g expects number");
                    return 0;
                }
                char buf[64];
                int len = snprintf(buf, sizeof(buf), "%g", AS_NUMBER(v));
                while (rlen + len >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                memcpy(result + rlen, buf, len);
                rlen += len;
            } else if (spec == 'x') {
                // Hex
                if (argIdx >= argCount) {
                    free(result);
                    vmRuntimeError(vm, "string.format: not enough arguments");
                    return 0;
                }
                Value v = args[argIdx++];
                if (!IS_NUMBER(v)) {
                    free(result);
                    vmRuntimeError(vm, "string.format: %%x expects number");
                    return 0;
                }
                char buf[64];
                int len = snprintf(buf, sizeof(buf), "%x", (unsigned int)AS_NUMBER(v));
                while (rlen + len >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                memcpy(result + rlen, buf, len);
                rlen += len;
            } else {
                // Unknown specifier, output as-is
                if (rlen + 2 >= capacity) { capacity *= 2; result = realloc(result, capacity); }
                result[rlen++] = '%';
                result[rlen++] = spec;
            }
        } else {
            if (rlen + 1 >= capacity) { capacity *= 2; result = realloc(result, capacity); }
            result[rlen++] = f[i];
        }
    }

    result[rlen] = '\0';
    ObjString* str = takeString(result, rlen);
    RETURN_OBJ(str);
}

void registerString(VM* vm) {
    const NativeReg stringFuncs[] = {
        {"len", string_len},
        {"sub", string_sub},
        {"lower", string_lower},
        {"upper", string_upper},
        {"char", string_char},
        {"byte", string_byte},
        {"find", string_find},
        {"trim", string_trim},
        {"split", string_split},
        {"join", string_join},
        {"rep", string_rep},

        {"reverse", string_reverse},
        {"format", string_format},
        {NULL, NULL}
    };
    registerModule(vm, "string", stringFuncs);

    ObjTable* stringModuleTable = AS_TABLE(peek(vm, 0));
    for (int i = 0; stringFuncs[i].name != NULL; i++) {
        ObjString* nameStr = copyString(stringFuncs[i].name, (int)strlen(stringFuncs[i].name));
        Value method = NIL_VAL;
        if (tableGet(&stringModuleTable->table, nameStr, &method) && IS_NATIVE(method)) {
            AS_NATIVE_OBJ(method)->isSelf = 1;
        }
    }
    
    // Add __call metamethod to string module to act as str() constructor
    Value stringModule = peek(vm, 0); // peek string module
    ObjTable* mt = newTable();
    push(vm, OBJ_VAL(mt)); // protect
    
    ObjString* callStr = copyString("__call", 6);
    push(vm, OBJ_VAL(callStr));
    push(vm, OBJ_VAL(newNative(core_tostring, callStr)));
    tableSet(&mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // native
    pop(vm); // callStr
    
    AS_TABLE(stringModule)->metatable = mt;
    
    // Alias 'str' global to 'string' module so str(x) works via __call
    ObjString* strName = copyString("str", 3);
    push(vm, OBJ_VAL(strName));
    push(vm, stringModule);
    tableSet(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // stringModule
    pop(vm); // strName
    
    pop(vm); // pop mt
    pop(vm); // Pop string module
}
