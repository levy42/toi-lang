#include <string.h> // for memcmp if needed? No, generic.
#include <stdio.h>
#include <stdlib.h>
#include "value.h"
#include "object.h"

// We need forward decl for callFunction or tableGet if we want full __str support?
// `printValue` in `value.c` cannot easily call into the VM/Interpreter for `__str` without circular dependency or callback.
// For the Debug/VM phase, let's stick to a raw `printValue` that debugs the value type without executing code.
// The existing `printValue` in parser.c was rich (called __str).
// Let's implement a "raw" printValue here for debugging, and keep the rich one in the VM/Parser later.

void printValue(Value value) {
    if (IS_OBJ(value)) {
        printObject(value);
    } else if (IS_NIL(value)) {
        printf("nil");
    } else if (IS_BOOL(value)) {
        printf(AS_BOOL(value) ? "true" : "false");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", AS_NUMBER(value));
        int len = (int)strlen(buf);
        while (len > 0 && buf[len - 1] == '0') {
            buf[--len] = '\0';
        }
        if (len > 0 && buf[len - 1] == '.') {
            buf[--len] = '\0';
        }
        if (len == 0) {
            strcpy(buf, "0");
        }
        printf("%s", buf);
    }
}

void initValueArray(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeValueArray(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        array->values = (Value*)realloc(array->values, sizeof(Value) * array->capacity);
    }
    
    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray* array) {
    free(array->values);
    initValueArray(array);
}
