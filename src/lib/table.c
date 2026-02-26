#include <stdlib.h>
#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int value_equals_for_find(Value a, Value b) {
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

typedef struct {
    ObjTable* src;
    ObjTable* dst;
} TableClonePair;

static int table_clone_recursive(VM* vm, ObjTable* source, int deep,
                                 TableClonePair** seen, int* seen_count, int* seen_capacity,
                                 ObjTable** out) {
    if (deep) {
        for (int i = 0; i < *seen_count; i++) {
            if ((*seen)[i].src == source) {
                *out = (*seen)[i].dst;
                return 1;
            }
        }
    }

    ObjTable* clone = new_table();
    push(vm, OBJ_VAL(clone)); // GC protection while populating.
    clone->metatable = source->metatable;
    clone->is_module = source->is_module;

    if (deep) {
        if (*seen_count >= *seen_capacity) {
            int new_capacity = *seen_capacity == 0 ? 8 : (*seen_capacity) * 2;
            TableClonePair* grown = (TableClonePair*)realloc(*seen, sizeof(TableClonePair) * (size_t)new_capacity);
            if (grown == NULL) {
                pop(vm);
                vm_runtime_error(vm, "table.clone: out of memory");
                return 0;
            }
            *seen = grown;
            *seen_capacity = new_capacity;
        }
        (*seen)[*seen_count].src = source;
        (*seen)[*seen_count].dst = clone;
        (*seen_count)++;
    }

    if (source->table.array_capacity > 0) {
        clone->table.array = (Value*)malloc(sizeof(Value) * (size_t)source->table.array_capacity);
        if (clone->table.array == NULL) {
            pop(vm);
            vm_runtime_error(vm, "table.clone: out of memory");
            return 0;
        }
        clone->table.array_capacity = source->table.array_capacity;
        clone->table.array_max = source->table.array_max;

        for (int i = 0; i < source->table.array_capacity; i++) {
            Value v = source->table.array[i];
            if (deep && IS_TABLE(v)) {
                ObjTable* child = NULL;
                if (!table_clone_recursive(vm, AS_TABLE(v), 1, seen, seen_count, seen_capacity, &child)) {
                    pop(vm);
                    return 0;
                }
                v = OBJ_VAL(child);
            }
            clone->table.array[i] = v;
        }
    }

    for (int i = 0; i < source->table.capacity; i++) {
        Entry* entry = &source->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;

        Value v = entry->value;
        if (deep && IS_TABLE(v)) {
            ObjTable* child = NULL;
            if (!table_clone_recursive(vm, AS_TABLE(v), 1, seen, seen_count, seen_capacity, &child)) {
                pop(vm);
                return 0;
            }
            v = OBJ_VAL(child);
        }

        table_set(&clone->table, entry->key, v);
    }

    pop(vm);
    *out = clone;
    return 1;
}

static int call_unary_lookup(VM* vm, Value fn, Value arg, Value* out) {
    if (IS_CLOSURE(fn)) {
        int saved_frame_count = vm_current_thread(vm)->frame_count;

        push(vm, fn);
        push(vm, arg);

        if (!call(vm, AS_CLOSURE(fn), 1)) {
            return 0;
        }

        InterpretResult result = vm_run(vm, saved_frame_count);
        if (result != INTERPRET_OK) {
            return 0;
        }

        *out = pop(vm);
        return 1;
    }

    if (IS_NATIVE(fn)) {
        push(vm, fn);
        push(vm, arg);

        Value* call_args = vm_current_thread(vm)->stack_top - 1;
        vm_current_thread(vm)->stack_top -= 2;

        if (!AS_NATIVE(fn)(vm, 1, call_args)) {
            return 0;
        }

        *out = pop(vm);
        return 1;
    }

    vm_runtime_error(vm, "table.find_index: lookup must be a function.");
    return 0;
}

static int call_binary_less(VM* vm, Value fn, Value a, Value b, int* out_less) {
    Value result = NIL_VAL;

    if (IS_CLOSURE(fn)) {
        int saved_frame_count = vm_current_thread(vm)->frame_count;

        push(vm, fn);
        push(vm, a);
        push(vm, b);

        if (!call(vm, AS_CLOSURE(fn), 2)) {
            return 0;
        }

        InterpretResult run_result = vm_run(vm, saved_frame_count);
        if (run_result != INTERPRET_OK) {
            return 0;
        }

        result = pop(vm);
    } else if (IS_NATIVE(fn)) {
        push(vm, fn);
        push(vm, a);
        push(vm, b);

        Value* call_args = vm_current_thread(vm)->stack_top - 2;
        vm_current_thread(vm)->stack_top -= 3;

        if (!AS_NATIVE(fn)(vm, 2, call_args)) {
            return 0;
        }

        result = pop(vm);
    } else {
        vm_runtime_error(vm, "table.sort: comparator must be a function.");
        return 0;
    }

    if (IS_BOOL(result)) {
        *out_less = AS_BOOL(result) ? 1 : 0;
        return 1;
    }
    if (IS_NUMBER(result)) {
        *out_less = AS_NUMBER(result) < 0 ? 1 : 0;
        return 1;
    }

    vm_runtime_error(vm, "table.sort: comparator must return bool or number.");
    return 0;
}

// table.remove(t, pos) - removes element at pos and shifts down
static int table_remove(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    int len = 0;

    // Find array length
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) {
            len = i - 1;
            break;
        }
    }

    int pos = len; // Default: remove last element
    if (arg_count >= 2) {
        ASSERT_NUMBER(1);
        pos = (int)GET_NUMBER(1);
    }

    if (pos < 1 || pos > len) {
        RETURN_NIL;
    }

    // Get the value being removed (to return it)
    Value removed;
    table_get_array(&table->table, pos, &removed);

    // Shift elements down
    for (int i = pos; i < len; i++) {
        Value val;
        table_get_array(&table->table, i + 1, &val);
        table_set_array(&table->table, i, val);
    }

    // Remove last element
    table_set_array(&table->table, len, NIL_VAL);

    RETURN_VAL(removed);
}

// table.push(t, value) - append value to array part
static int table_push(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    int index = table->table.array_max + 1;

    int raw_index = index - 1;
    if (raw_index >= table->table.array_capacity) {
        int new_capacity = table->table.array_capacity == 0 ? 8 : table->table.array_capacity * 2;
        while (raw_index >= new_capacity) new_capacity *= 2;

        table->table.array = (Value*)realloc(table->table.array, sizeof(Value) * new_capacity);
        for (int i = table->table.array_capacity; i < new_capacity; i++) {
            table->table.array[i] = NIL_VAL;
        }
        table->table.array_capacity = new_capacity;
    }

    table->table.array[raw_index] = args[1];
    table->table.array_max = index;
    RETURN_NUMBER((double)index);
}

// table.reserve(t, n) - pre-allocate array capacity
static int table_reserve(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    ASSERT_NUMBER(1);

    ObjTable* table = GET_TABLE(0);
    int n = (int)GET_NUMBER(1);
    if (n < 0) {
        vm_runtime_error(vm, "table.reserve: n must be non-negative");
        return 0;
    }
    if (n <= table->table.array_capacity) {
        RETURN_VAL(args[0]);
    }

    int new_capacity = table->table.array_capacity == 0 ? 8 : table->table.array_capacity;
    while (new_capacity < n) new_capacity *= 2;
    table->table.array = (Value*)realloc(table->table.array, sizeof(Value) * new_capacity);
    for (int i = table->table.array_capacity; i < new_capacity; i++) {
        table->table.array[i] = NIL_VAL;
    }
    table->table.array_capacity = new_capacity;
    RETURN_VAL(args[0]);
}

// table.concat(t, sep) - join array elements as string
static int table_concat(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    const char* sep = "";
    int sep_len = 0;

    if (arg_count >= 2) {
        ASSERT_STRING(1);
        sep = GET_CSTRING(1);
        sep_len = GET_STRING(1)->length;
    }

    // First pass: calculate total length
    int total_len = 0;
    int count = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) {
            break;
        }
        if (!IS_STRING(val)) {
            vm_runtime_error(vm, "table.concat: element %d is not a string", i);
            return 0;
        }
        total_len += AS_STRING(val)->length;
        count++;
    }

    if (count == 0) {
        RETURN_STRING("", 0);
    }

    total_len += sep_len * (count - 1);

    // Second pass: build string
    char* buf = (char*)malloc(total_len + 1);
    char* p = buf;

    for (int i = 1; i <= count; i++) {
        Value val;
        table_get_array(&table->table, i, &val);
        ObjString* s = AS_STRING(val);

        memcpy(p, s->chars, s->length);
        p += s->length;

        if (i < count && sep_len > 0) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
    }
    *p = '\0';

    RETURN_STRING(buf, total_len);
}

// Helper for qsort (default comparison)
static int compare_values(const void* a, const void* b) {
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
static int table_sort(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);

    // Find array length and collect elements
    int len = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) {
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
        table_get_array(&table->table, i + 1, &arr[i]);
    }

    if (arg_count >= 2 && !IS_NIL(args[1])) {
        Value cmp = args[1];
        if (!IS_CLOSURE(cmp) && !IS_NATIVE(cmp)) {
            free(arr);
            vm_runtime_error(vm, "table.sort: comparator must be a function.");
            return 0;
        }

        // Stable insertion sort using user comparator.
        for (int i = 1; i < len; i++) {
            Value key = arr[i];
            int j = i - 1;

            while (j >= 0) {
                int less = 0;
                if (!call_binary_less(vm, cmp, key, arr[j], &less)) {
                    free(arr);
                    return 0;
                }
                if (!less) break;
                arr[j + 1] = arr[j];
                j--;
            }
            arr[j + 1] = key;
        }
    } else {
        qsort(arr, len, sizeof(Value), compare_values);
    }

    // Copy back
    for (int i = 0; i < len; i++) {
        table_set_array(&table->table, i + 1, arr[i]);
    }

    free(arr);
    RETURN_VAL(args[0]);
}

// table.insert(t, [pos,] value) - insert value at pos (default: end)
static int table_insert(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);

    // Find array length
    int len = 0;
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) {
            len = i - 1;
            break;
        }
    }

    int pos;
    Value value;

    if (arg_count == 2) {
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
            table_get_array(&table->table, i, &val);
            table_set_array(&table->table, i + 1, val);
        }
    }

    table_set_array(&table->table, pos, value);
    RETURN_VAL(args[0]); // Return table for chaining
}

// table.keys(t) - return array of keys
static int table_keys(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result)); // GC protection

    int index = 1;

    // String keys
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            table_set_array(&result->table, index++, OBJ_VAL(entry->key));
        }
    }

    // Array keys
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;
        table_set_array(&result->table, index++, NUMBER_VAL(i));
    }

    return 1; // result already on stack
}

// table.values(t) - return array of values
static int table_values(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result)); // GC protection

    int index = 1;

    // String key values
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            table_set_array(&result->table, index++, entry->value);
        }
    }

    // Array values
    for (int i = 1; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;
        table_set_array(&result->table, index++, val);
    }

    return 1;
}

// table.find_index(t, value[, start][, lookup]) - find first matching array value.
// lookup(element) can be used to compare by derived key.
// Returns 1-based index, or 0 if not found.
static int table_find_index(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(2);
    ASSERT_TABLE(0);
    if (arg_count > 4) {
        vm_runtime_error(vm, "table.find_index: expected 2 to 4 arguments.");
        return 0;
    }

    ObjTable* table = GET_TABLE(0);
    Value needle = args[1];
    int start = 1;
    Value lookup = NIL_VAL;

    if (arg_count >= 3) {
        if (IS_NUMBER(args[2])) {
            start = (int)GET_NUMBER(2);
            if (start < 1) start = 1;
        } else {
            lookup = args[2];
        }
    }

    if (arg_count >= 4) {
        ASSERT_NUMBER(2);
        start = (int)GET_NUMBER(2);
        if (start < 1) start = 1;
        lookup = args[3];
    }

    for (int i = start; ; i++) {
        Value val;
        if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;

        Value candidate = val;
        if (!IS_NIL(lookup)) {
            if (!call_unary_lookup(vm, lookup, val, &candidate)) {
                return 0;
            }
        }

        if (value_equals_for_find(candidate, needle)) {
            RETURN_NUMBER((double)i);
        }
    }

    RETURN_NUMBER(0);
}

// table.clone(t, deep?) - clone table shallowly by default, recursively when deep=true
static int table_clone(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_TABLE(0);
    if (arg_count > 2) {
        vm_runtime_error(vm, "table.clone: expected 1 to 2 arguments.");
        return 0;
    }

    int deep = 0;
    if (arg_count == 2) {
        if (IS_BOOL(args[1])) {
            deep = AS_BOOL(args[1]) ? 1 : 0;
        } else if (IS_TABLE(args[1])) {
            Value deep_value = NIL_VAL;
            ObjString* deep_key = copy_string("deep", 4);
            if (table_get(&GET_TABLE(1)->table, deep_key, &deep_value)) {
                if (!IS_BOOL(deep_value)) {
                    vm_runtime_error(vm, "table.clone: deep must be a bool.");
                    return 0;
                }
                deep = AS_BOOL(deep_value) ? 1 : 0;
            }
        } else {
            vm_runtime_error(vm, "table.clone: deep must be a bool.");
            return 0;
        }
    }

    TableClonePair* seen = NULL;
    int seen_count = 0;
    int seen_capacity = 0;
    ObjTable* clone = NULL;
    int ok = table_clone_recursive(vm, GET_TABLE(0), deep, &seen, &seen_count, &seen_capacity, &clone);
    free(seen);
    if (!ok) return 0;

    RETURN_OBJ(clone);
}

void register_table(VM* vm) {
    const NativeReg table_funcs[] = {
        {"remove", table_remove},
        {"push", table_push},
        {"reserve", table_reserve},
        {"clone", table_clone},
        {"concat", table_concat},
        {"sort", table_sort},
        {"insert", table_insert},
        {"keys", table_keys},
        {"values", table_values},
        {"find_index", table_find_index},
        {NULL, NULL}
    };
    register_module(vm, "table", table_funcs);
    pop(vm); // Pop table module
}
