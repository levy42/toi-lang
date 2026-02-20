#include <string.h> // for memcmp if needed? No, generic.
#include <stdio.h>
#include <stdlib.h>
#include "value.h"
#include "object.h"

// We need forward decl for call_function or table_get if we want full __str support?
// `print_value` in `value.c` cannot easily call into the VM/Interpreter for `__str` without circular dependency or callback.
// For the Debug/VM phase, let's stick to a raw `print_value` that debugs the value type without executing code.
// The existing `print_value` in parser.c was rich (called __str).
// Let's implement a "raw" print_value here for debugging, and keep the rich one in the VM/Parser later.

void print_value(Value value) {
    if (IS_OBJ(value)) {
        print_object(value);
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

void init_value_array(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void write_value_array(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int old_capacity = array->capacity;
        array->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        array->values = (Value*)realloc(array->values, sizeof(Value) * array->capacity);
    }
    
    array->values[array->count] = value;
    array->count++;
}

void free_value_array(ValueArray* array) {
    free(array->values);
    init_value_array(array);
}
