#include <stdlib.h>
#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int valueEqualsForFind(Value a, Value b) {
    if (a.type != b.type) return 0;
    if (IS_NIL(a)) return 1;
    if (IS_NUMBER(a)) return AS_NUMBER(a) == AS_NUMBER(b);
    if (IS_BOOL(a)) return AS_BOOL(a) == AS_BOOL(b);
    if (IS_OBJ(a) && IS_OBJ(b)) {
        if (AS_OBJ(a) == AS_OBJ(b)) return 1;
        if (IS_STRING(a) && IS_STRING(b)) {
            ObjString* sa = AS_STRING(a);
            ObjString* sb = AS_STRING(b);
            return sa->length == sb->length && memcmp(sa->chars, sb->chars, sa->length) == 0;
        }
    }
    return 0;
}

static int callUnaryLookup(VM* vm, Value fn, Value arg, Value* out) {
    if (IS_CLOSURE(fn)) {
        int savedFrameCount = vm->currentThread->frameCount;

        push(vm, fn);
        push(vm, arg);

        if (!call(vm, AS_CLOSURE(fn), 1)) {
            return 0;
        }

        InterpretResult result = vmRun(vm, savedFrameCount);
        if (result != INTERPRET_OK) {
            return 0;
        }

        *out = pop(vm);
        return 1;
    }

    if (IS_NATIVE(fn)) {
        push(vm, fn);
        push(vm, arg);

        Value* callArgs = vm->currentThread->stackTop - 1;
        vm->currentThread->stackTop -= 2;

        if (!AS_NATIVE(fn)(vm, 1, callArgs)) {
            return 0;
        }

        *out = pop(vm);
        return 1;
    }

    vmRuntimeError(vm, "table.find_index: lookup must be a function.");
    return 0;
}

// table.remove(t, pos) - removes element at pos and shifts down
static int table_remove(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    int len = 0;

    // Find array length
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) {
            len = i - 1;
            break;
        }
    }

    int pos = len; // Default: remove last element
    if (argCount >= 2) {
        ASSERT_NUMBER(1);
        pos = (int)GET_NUMBER(1);
    }

    if (pos < 1 || pos > len) {
        RETURN_NIL;
    }

    // Get the value being removed (to return it)
    Value removed;
    tableGetArray(&table->table, pos, &removed);

    // Shift elements down
    for (int i = pos; i < len; i++) {
        Value val;
        tableGetArray(&table->table, i + 1, &val);
        tableSetArray(&table->table, i, val);
    }

    // Remove last element
    tableSetArray(&table->table, len, NIL_VAL);

    RETURN_VAL(removed);
}

// table.push(t, value) - append value to array part
static int table_push(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    int index = table->table.arrayMax + 1;

    int rawIndex = index - 1;
    if (rawIndex >= table->table.arrayCapacity) {
        int newCapacity = table->table.arrayCapacity == 0 ? 8 : table->table.arrayCapacity * 2;
        while (rawIndex >= newCapacity) newCapacity *= 2;

        table->table.array = (Value*)realloc(table->table.array, sizeof(Value) * newCapacity);
        for (int i = table->table.arrayCapacity; i < newCapacity; i++) {
            table->table.array[i] = NIL_VAL;
        }
        table->table.arrayCapacity = newCapacity;
    }

    table->table.array[rawIndex] = args[1];
    table->table.arrayMax = index;
    RETURN_NUMBER((double)index);
}

// table.reserve(t, n) - pre-allocate array capacity
static int table_reserve(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    ASSERT_NUMBER(1);

    ObjTable* table = GET_TABLE(0);
    int n = (int)GET_NUMBER(1);
    if (n < 0) {
        vmRuntimeError(vm, "table.reserve: n must be non-negative");
        return 0;
    }
    if (n <= table->table.arrayCapacity) {
        RETURN_VAL(args[0]);
    }

    int newCapacity = table->table.arrayCapacity == 0 ? 8 : table->table.arrayCapacity;
    while (newCapacity < n) newCapacity *= 2;
    table->table.array = (Value*)realloc(table->table.array, sizeof(Value) * newCapacity);
    for (int i = table->table.arrayCapacity; i < newCapacity; i++) {
        table->table.array[i] = NIL_VAL;
    }
    table->table.arrayCapacity = newCapacity;
    RETURN_VAL(args[0]);
}

// table.concat(t, sep) - join array elements as string
static int table_concat(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    const char* sep = "";
    int sepLen = 0;

    if (argCount >= 2) {
        ASSERT_STRING(1);
        sep = GET_CSTRING(1);
        sepLen = GET_STRING(1)->length;
    }

    // First pass: calculate total length
    int totalLen = 0;
    int count = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) {
            break;
        }
        if (!IS_STRING(val)) {
            vmRuntimeError(vm, "table.concat: element %d is not a string", i);
            return 0;
        }
        totalLen += AS_STRING(val)->length;
        count++;
    }

    if (count == 0) {
        RETURN_STRING("", 0);
    }

    totalLen += sepLen * (count - 1);

    // Second pass: build string
    char* buf = (char*)malloc(totalLen + 1);
    char* p = buf;

    for (int i = 1; i <= count; i++) {
        Value val;
        tableGetArray(&table->table, i, &val);
        ObjString* s = AS_STRING(val);

        memcpy(p, s->chars, s->length);
        p += s->length;

        if (i < count && sepLen > 0) {
            memcpy(p, sep, sepLen);
            p += sepLen;
        }
    }
    *p = '\0';

    RETURN_STRING(buf, totalLen);
}

// Helper for qsort (default comparison)
static int compareValues(const void* a, const void* b) {
    Value va = *(const Value*)a;
    Value vb = *(const Value*)b;

    // Default comparison for numbers
    if (IS_NUMBER(va) && IS_NUMBER(vb)) {
        double da = AS_NUMBER(va);
        double db = AS_NUMBER(vb);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    // Default comparison for strings
    if (IS_STRING(va) && IS_STRING(vb)) {
        return strcmp(AS_STRING(va)->chars, AS_STRING(vb)->chars);
    }

    // Can't compare different types
    return 0;
}

// table.sort(t, cmp?) - sort array in place
static int table_sort(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);

    // Find array length and collect elements
    int len = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) {
            len = i - 1;
            break;
        }
    }

    if (len <= 1) {
        RETURN_VAL(args[0]);
    }

    // Copy to temp array
    Value* arr = (Value*)malloc(len * sizeof(Value));
    for (int i = 0; i < len; i++) {
        tableGetArray(&table->table, i + 1, &arr[i]);
    }

    // Sort (using default comparison for now - custom comparator is complex)
    qsort(arr, len, sizeof(Value), compareValues);

    // Copy back
    for (int i = 0; i < len; i++) {
        tableSetArray(&table->table, i + 1, arr[i]);
    }

    free(arr);
    RETURN_VAL(args[0]);
}

// table.insert(t, [pos,] value) - insert value at pos (default: end)
static int table_insert(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);

    // Find array length
    int len = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) {
            len = i - 1;
            break;
        }
    }

    int pos;
    Value value;

    if (argCount == 2) {
        // table.insert(t, value) - append
        pos = len + 1;
        value = args[1];
    } else {
        // table.insert(t, pos, value)
        ASSERT_NUMBER(1);
        pos = (int)GET_NUMBER(1);
        value = args[2];
        if (pos < 1) pos = 1;
        if (pos > len + 1) pos = len + 1;

        // Shift elements up
        for (int i = len; i >= pos; i--) {
            Value val;
            tableGetArray(&table->table, i, &val);
            tableSetArray(&table->table, i + 1, val);
        }
    }

    tableSetArray(&table->table, pos, value);
    RETURN_VAL(args[0]); // Return table for chaining
}

// table.keys(t) - return array of keys
static int table_keys(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result)); // GC protection

    int index = 1;

    // String keys
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            tableSetArray(&result->table, index++, OBJ_VAL(entry->key));
        }
    }

    // Array keys
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) break;
        tableSetArray(&result->table, index++, NUMBER_VAL(i));
    }

    return 1; // result already on stack
}

// table.values(t) - return array of values
static int table_values(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result)); // GC protection

    int index = 1;

    // String key values
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            tableSetArray(&result->table, index++, entry->value);
        }
    }

    // Array values
    for (int i = 1; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) break;
        tableSetArray(&result->table, index++, val);
    }

    return 1;
}

// table.find_index(t, value[, start][, lookup]) - find first matching array value.
// lookup(element) can be used to compare by derived key.
// Returns 1-based index, or 0 if not found.
static int table_find_index(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_TABLE(0);
    if (argCount > 4) {
        vmRuntimeError(vm, "table.find_index: expected 2 to 4 arguments.");
        return 0;
    }

    ObjTable* table = GET_TABLE(0);
    Value needle = args[1];
    int start = 1;
    Value lookup = NIL_VAL;

    if (argCount >= 3) {
        if (IS_NUMBER(args[2])) {
            start = (int)GET_NUMBER(2);
            if (start < 1) start = 1;
        } else {
            lookup = args[2];
        }
    }

    if (argCount >= 4) {
        ASSERT_NUMBER(2);
        start = (int)GET_NUMBER(2);
        if (start < 1) start = 1;
        lookup = args[3];
    }

    for (int i = start; ; i++) {
        Value val;
        if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) break;

        Value candidate = val;
        if (!IS_NIL(lookup)) {
            if (!callUnaryLookup(vm, lookup, val, &candidate)) {
                return 0;
            }
        }

        if (valueEqualsForFind(candidate, needle)) {
            RETURN_NUMBER((double)i);
        }
    }

    RETURN_NUMBER(0);
}

void registerTable(VM* vm) {
    const NativeReg tableFuncs[] = {
        {"remove", table_remove},
        {"push", table_push},
        {"reserve", table_reserve},
        {"concat", table_concat},
        {"sort", table_sort},
        {"insert", table_insert},
        {"keys", table_keys},
        {"values", table_values},
        {"find_index", table_find_index},
        {NULL, NULL}
    };
    registerModule(vm, "table", tableFuncs);
    pop(vm); // Pop table module
}
