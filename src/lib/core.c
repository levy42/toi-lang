#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// Core Natives -- These were previously defined in vm.c

typedef struct {
    char* buffer;
    int length;
    int capacity;
} StringBuilder;

static void sb_init(StringBuilder* sb) {
    sb->buffer = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static void sb_append(StringBuilder* sb, const char* str, int len) {
    if (len < 0) len = (int)strlen(str);
    if (sb->length + len > sb->capacity) {
        int new_capacity = sb->capacity < 8 ? 8 : sb->capacity * 2;
        while (new_capacity < sb->length + len) new_capacity *= 2;
        sb->buffer = (char*)realloc(sb->buffer, new_capacity + 1);
        sb->capacity = new_capacity;
    }
    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

static void sb_free(StringBuilder* sb) {
    if (sb->buffer != NULL) {
        free(sb->buffer);
    }
}

static int next_native(VM* vm, int arg_count, Value* args) {

    ASSERT_ARGC_EQ(2);
    Value state = args[0];
    Value current_key = args[1];

    if (IS_TABLE(state)) {
        ObjTable* obj_table = AS_TABLE(state);
        Table* table = &obj_table->table;

        // Array part first
        if (IS_NIL(current_key) || IS_NUMBER(current_key)) {
            double num = IS_NUMBER(current_key) ? GET_NUMBER(1) : 0;
            int start = 1;
            if (IS_NUMBER(current_key) && num >= 1 && (double)(int)num == num) {
                start = (int)num + 1;
            }
            for (int i = start; i <= table->array_capacity; i++) {
                Value val = NIL_VAL;
                if (table_get_array(table, i, &val) && !IS_NIL(val)) {
                    push(vm, NUMBER_VAL((double)i));
                    push(vm, val);
                    return 2;
                }
            }
            current_key = NIL_VAL; // Move to hash iteration
        }

        // Hash part
        int found_current = IS_NIL(current_key);
        ObjString* num_key = NULL;
        if (IS_NUMBER(current_key)) {
            num_key = number_key_string(GET_NUMBER(1));
        }

        for (int i = 0; i < table->capacity; i++) {
            Entry* entry = &table->entries[i];
            if (entry->key == NULL) continue;

            if (found_current) {
                push(vm, OBJ_VAL(entry->key));
                push(vm, entry->value);
                return 2;
            }

            if (IS_STRING(current_key)) {
                ObjString* s_key = GET_STRING(1);
                if (entry->key == s_key || 
                   (entry->key->length == s_key->length && 
                    memcmp(entry->key->chars, s_key->chars, entry->key->length) == 0)) {
                    found_current = 1;
                }
            } else if (IS_NUMBER(current_key)) {
                if (entry->key == num_key ||
                    (entry->key->length == num_key->length &&
                     memcmp(entry->key->chars, num_key->chars, entry->key->length) == 0)) {
                    found_current = 1;
                }
            }
        }

        // Return 2 nils to match expected return count for for-in loops
        push(vm, NIL_VAL);
        push(vm, NIL_VAL);
        return 2;
    }

    if (IS_STRING(state)) {
        ObjString* str = AS_STRING(state);
        int index = 1;
        if (IS_NUMBER(current_key)) {
            double n = GET_NUMBER(1);
            if (n >= 1 && (double)(int)n == n) {
                index = (int)n + 1;
            }
        } else if (!IS_NIL(current_key)) {
            vm_runtime_error(vm, "next() string control must be number or nil.");
            return 0;
        }

        if (index < 1 || index > str->length) {
            push(vm, NIL_VAL);
            push(vm, NIL_VAL);
            return 2;
        }

        push(vm, NUMBER_VAL((double)index));
        push(vm, OBJ_VAL(copy_string(str->chars + (index - 1), 1)));
        return 2;
    }

    vm_runtime_error(vm, "next expects table or string as first argument.");
    return 0;
}

static int inext_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    ASSERT_NUMBER(1);

    ObjTable* table = GET_TABLE(0);
    double index = GET_NUMBER(1);
    double next_index = index + 1;
    int i_next = (int)next_index;

    Value value = NIL_VAL;
    int found = 0;

    if ((double)i_next == next_index && i_next >= 1) {
        if (table_get_array(&table->table, i_next, &value)) {
            found = 1;
        }
    }

    if (!found) {
        ObjString* key = number_key_string(next_index);
        if (table_get(&table->table, key, &value) && !IS_NIL(value)) {
            found = 1;
        }
    }

    if (found) {
        push(vm, NUMBER_VAL(next_index));
        push(vm, value);
        return 2;
    }

    // Return 2 nils to match expected return count for for-in loops
    push(vm, NIL_VAL);
    push(vm, NIL_VAL);
    return 2;
}

static int gen_next_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_THREAD(0);

    ObjThread* thread = GET_THREAD(0);
    if (!thread->is_generator) {
        vm_runtime_error(vm, "gen_next expects a generator thread.");
        return 0;
    }
    if (thread->frame_count == 0) {
        push(vm, NIL_VAL);
        push(vm, NIL_VAL);
        return 2;
    }

    thread->caller = vm_current_thread(vm);
    thread->generator_mode = 1;
    vm_set_current_thread(vm, thread);
    return 1;
}

static int setmetatable_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    if (!IS_TABLE(args[1]) && !IS_NIL(args[1])) { RETURN_NIL; }

    GET_TABLE(0)->metatable = IS_NIL(args[1]) ? NULL : GET_TABLE(1);
    RETURN_VAL(args[0]);
}

static int getmetatable_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    if (table->metatable == NULL) {
        RETURN_NIL;
    } else {
        RETURN_VAL(OBJ_VAL(table->metatable));
    }
}

static void format_value(VM* vm, Value val, StringBuilder* sb, int depth) {
    if (depth > 5) {
        sb_append(sb, "...", 3);
        return;
    }

    if (IS_STRING(val)) {
        ObjString* str = AS_STRING(val);
        sb_append(sb, "\"", 1);
        sb_append(sb, str->chars, str->length);
        sb_append(sb, "\"", 1);
    } else if (IS_NUMBER(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.14g", AS_NUMBER(val));
        sb_append(sb, buf, -1);
    } else if (IS_BOOL(val)) {
        sb_append(sb, AS_BOOL(val) ? "true" : "false", -1);
    } else if (IS_NIL(val)) {
        sb_append(sb, "nil", 3);
    } else if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        
        // Check for __str metamethod
        if (table->metatable != NULL) {
            Value str_method;
            ObjString* str_key = copy_string("__str", 5);
            if (table_get(&table->metatable->table, str_key, &str_method) && IS_CLOSURE(str_method)) {
                // If we are formatting recursively, we can't easily call VM code without complex state management.
                // For now, just indicate it has custom string representation or fall back to default if depth > 0.
                if (depth == 0) {
                     // Let tostring_native handle the call
                     sb_append(sb, "<table>", 7); 
                     return;
                }
                sb_append(sb, "<custom>", 8);
                return;
            }
        }

        sb_append(sb, "{", 1);
        int count = 0;
        
        // Iterate array part
        if (table->table.array != NULL) {
            // Find last non-nil index to avoid printing trailing nils
            int max_index = -1;
            for (int i = 0; i < table->table.array_capacity; i++) {
                if (!IS_NIL(table->table.array[i])) {
                    max_index = i;
                }
            }

            // Print sequence up to max_index
            for (int i = 0; i <= max_index; i++) {
                if (count > 0) sb_append(sb, ", ", 2);
                format_value(vm, table->table.array[i], sb, depth + 1);
                count++;
            }
        }

        // Iterate hash part
        for (int i = 0; i < table->table.capacity; i++) {
            Entry* entry = &table->table.entries[i];
            if (entry->key != NULL && !IS_NIL(entry->value)) {
                if (entry->key->length == 7 &&
                    memcmp(entry->key->chars, "__index", 7) == 0) {
                    continue;
                }
                if (count > 0) sb_append(sb, ", ", 2);
                
                sb_append(sb, entry->key->chars, entry->key->length);
                sb_append(sb, ": ", 2);
                
                format_value(vm, entry->value, sb, depth + 1);
                count++;
            }
        }
        sb_append(sb, "}", 1);


    } else if (IS_USERDATA(val)) {
        ObjUserdata* userdata = AS_USERDATA(val);
        ObjString* type_name = NULL;
        if (userdata->metatable != NULL) {
            for (int i = 0; i < userdata->metatable->table.capacity; i++) {
                Entry* entry = &userdata->metatable->table.entries[i];
                if (entry->key == NULL || IS_NIL(entry->value)) continue;
                if (entry->key->length == 6 &&
                    memcmp(entry->key->chars, "__name", 6) == 0 &&
                    IS_STRING(entry->value)) {
                    type_name = AS_STRING(entry->value);
                    break;
                }
            }
        }

        if (type_name != NULL) {
            sb_append(sb, "<", 1);
            sb_append(sb, type_name->chars, type_name->length);
            if (userdata->data == NULL) {
                sb_append(sb, " closed>", 8);
            } else {
                sb_append(sb, ">", 1);
            }
        } else if (userdata->data == NULL) {
            sb_append(sb, "<userdata closed>", 17);
        } else {
            sb_append(sb, "<userdata>", 10);
        }
    } else if (IS_NATIVE(val)) {
        ObjNative* native = (ObjNative*)AS_OBJ(val);
        if (native->name != NULL) {
            sb_append(sb, "<native fn ", 11);
            sb_append(sb, native->name->chars, native->name->length);
            sb_append(sb, ">", 1);
        } else {
            sb_append(sb, "<native fn>", 11);
        }
    } else {
        sb_append(sb, "<object>", 8);
    }
}

int core_tostring(VM* vm, int arg_count, Value* args) {
    Value val;
    if (arg_count == 1) {
        val = args[0];
    } else if (arg_count == 2) {
        // When called as string(x), args[0] is the string module table
        val = args[1];
    } else {
        vm_runtime_error(vm, "str() expects 1 argument.");
        return 0;
    }

    if (IS_STRING(val)) {
        RETURN_OBJ(AS_STRING(val));
    }

    // Check for __str on table/userdata first (top level).
    ObjTable* metatable = NULL;
    if (IS_TABLE(val)) {
        metatable = AS_TABLE(val)->metatable;
    } else if (IS_USERDATA(val)) {
        metatable = AS_USERDATA(val)->metatable;
    }
    if (metatable != NULL) {
        Value str_method;
        ObjString* str_key = vm->mm_str;
        if (table_get(&metatable->table, str_key, &str_method) &&
            (IS_CLOSURE(str_method) || IS_NATIVE(str_method))) {
            int saved_frame_count = vm_current_thread(vm)->frame_count;
            CallFrame* frame = &vm_current_thread(vm)->frames[saved_frame_count - 1];
            uint8_t* ip = frame->ip;

            push(vm, str_method);
            push(vm, val);

            if (!call_value(vm, str_method, 1, &frame, &ip)) {
                RETURN_STRING("<error>", 7);
            }

            if (IS_CLOSURE(str_method)) {
                InterpretResult result = vm_run(vm, saved_frame_count);
                if (result != INTERPRET_OK) {
                    RETURN_STRING("<error>", 7);
                }
            }
            return 1;
        }
    }

    StringBuilder sb;
    sb_init(&sb);
    format_value(vm, val, &sb, 0);
    
    // We can't return pointer to stack memory or malloc'd memory without managing it.
    // copy_string makes a copy on the heap managed by GC.
    ObjString* str = copy_string(sb.buffer, sb.length);
    sb_free(&sb);
    
    RETURN_OBJ(str);
}


static int global_error(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    vm_runtime_error(vm, "%s", GET_CSTRING(0));
    return 0; // Signal failure to vm_run
}

static int exit_native(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        exit(0);
    }
    if (arg_count == 1 && IS_NUMBER(args[0])) {
        exit((int)AS_NUMBER(args[0]));
    }
    vm_runtime_error(vm, "exit() expects no args or a numeric exit code.");
    return 0;
}

static int type_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    Value val = args[0];

    if (IS_NIL(val)) {
        RETURN_STRING("nil", 3);
    } else if (IS_BOOL(val)) {
        RETURN_STRING("boolean", 7);
    } else if (IS_NUMBER(val)) {
        RETURN_STRING("number", 6);
    } else if (IS_STRING(val)) {
        RETURN_STRING("string", 6);
    } else if (IS_TABLE(val)) {
        RETURN_STRING("table", 5);
    } else if (IS_CLOSURE(val) || IS_NATIVE(val)) {
        RETURN_STRING("function", 8);
    } else if (IS_THREAD(val)) {
        RETURN_STRING("thread", 6);
    } else if (IS_USERDATA(val)) {
        RETURN_STRING("userdata", 8);
    } else {
        RETURN_STRING("unknown", 7);
    }
}

static int is_falsey_simple(Value v) {
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.array_capacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

static int call_bool_metamethod(VM* vm, Value receiver, Value method, int* out_bool) {
    if (IS_CLOSURE(method)) {
        int saved_frame_count = vm_current_thread(vm)->frame_count;

        push(vm, method);
        push(vm, receiver);

        if (!call(vm, AS_CLOSURE(method), 1)) {
            return 0;
        }

        InterpretResult result = vm_run(vm, saved_frame_count);
        if (result != INTERPRET_OK) {
            return 0;
        }

        Value ret = pop(vm);
        *out_bool = !is_falsey_simple(ret);
        return 1;
    }

    if (IS_NATIVE(method)) {
        push(vm, method);
        push(vm, receiver);

        Value* call_args = vm_current_thread(vm)->stack_top - 1;
        vm_current_thread(vm)->stack_top -= 2;

        if (!AS_NATIVE(method)(vm, 1, call_args)) {
            return 0;
        }

        Value ret = pop(vm);
        *out_bool = !is_falsey_simple(ret);
        return 1;
    }

    vm_runtime_error(vm, "__bool must be a function.");
    return 0;
}

static int bool_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    Value val = args[0];

    if (IS_TABLE(val)) {
        Value method = get_metamethod(vm, val, "__bool");
        if (!IS_NIL(method)) {
            int out_bool = 0;
            if (!call_bool_metamethod(vm, val, method, &out_bool)) {
                return 0;
            }
            RETURN_BOOL(out_bool);
        }
    }

    RETURN_BOOL(!is_falsey_simple(val));
}

static int int_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    Value val = args[0];

    if (IS_NUMBER(val)) {
        RETURN_NUMBER((double)(int)AS_NUMBER(val));
    }
    if (IS_BOOL(val)) {
        RETURN_NUMBER(AS_BOOL(val) ? 1 : 0);
    }
    if (IS_STRING(val)) {
        const char* str = GET_CSTRING(0);
        char* end = NULL;
        long num = strtol(str, &end, 10);
        if (end == str) {
            vm_runtime_error(vm, "int() expects a valid base-10 string.");
            return 0;
        }
        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        if (*end != '\0') {
            vm_runtime_error(vm, "int() expects a valid base-10 string.");
            return 0;
        }
        RETURN_NUMBER((double)num);
    }

    vm_runtime_error(vm, "int() expects number, string, or bool.");
    return 0;
}

static int float_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    Value val = args[0];

    if (IS_NUMBER(val)) {
        RETURN_VAL(val);
    }
    if (IS_BOOL(val)) {
        RETURN_NUMBER(AS_BOOL(val) ? 1.0 : 0.0);
    }
    if (IS_STRING(val)) {
        const char* str = GET_CSTRING(0);
        char* end = NULL;
        double num = strtod(str, &end);
        if (end == str) {
            vm_runtime_error(vm, "float() expects a valid number string.");
            return 0;
        }
        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        if (*end != '\0') {
            vm_runtime_error(vm, "float() expects a valid number string.");
            return 0;
        }
        RETURN_NUMBER(num);
    }

    vm_runtime_error(vm, "float() expects number, string, or bool.");
    return 0;
}

static int input_native(VM* vm, int arg_count, Value* args) {
    if (arg_count > 1) {
        vm_runtime_error(vm, "input() expects at most 1 argument.");
        return 0;
    }

    if (arg_count == 1 && !IS_NIL(args[0])) {
        if (!IS_STRING(args[0])) {
            vm_runtime_error(vm, "input() prompt must be string or nil.");
            return 0;
        }
        ObjString* prompt = AS_STRING(args[0]);
        if (prompt->length > 0) {
            fwrite(prompt->chars, 1, (size_t)prompt->length, stdout);
            fflush(stdout);
        }
    }

    int capacity = 128;
    int length = 0;
    char* buffer = (char*)malloc((size_t)capacity);
    if (!buffer) {
        vm_runtime_error(vm, "input(): out of memory.");
        return 0;
    }

    char chunk[256];
    while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
        int chunk_len = (int)strlen(chunk);
        while (length + chunk_len + 1 > capacity) {
            capacity *= 2;
            char* grown = (char*)realloc(buffer, (size_t)capacity);
            if (!grown) {
                free(buffer);
                vm_runtime_error(vm, "input(): out of memory.");
                return 0;
            }
            buffer = grown;
        }
        memcpy(buffer + length, chunk, (size_t)chunk_len);
        length += chunk_len;
        if (chunk_len > 0 && chunk[chunk_len - 1] == '\n') break;
    }

    if (length == 0) {
        free(buffer);
        RETURN_NIL;
    }

    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
        length--;
    }
    buffer[length] = '\0';
    RETURN_OBJ(take_string(buffer, length));
}

static int min_native(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double min = GET_NUMBER(0);
    for (int i = 1; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val < min) min = val;
    }
    RETURN_NUMBER(min);
}

static int max_native(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double max = GET_NUMBER(0);
    for (int i = 1; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val > max) max = val;
    }
    RETURN_NUMBER(max);
}

static int sum_native(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) { RETURN_NIL; }

    if (arg_count == 1 && IS_TABLE(args[0])) {
        ObjTable* table = GET_TABLE(0);
        double sum = 0.0;
        for (int i = 1; ; i++) {
            Value val;
            if (!table_get_array(&table->table, i, &val) || IS_NIL(val)) break;
            if (!IS_NUMBER(val)) {
                vm_runtime_error(vm, "sum: element %d is not a number", i);
                return 0;
            }
            sum += AS_NUMBER(val);
        }
        RETURN_NUMBER(sum);
    }

    double sum = 0.0;
    for (int i = 0; i < arg_count; i++) {
        ASSERT_NUMBER(i);
        sum += GET_NUMBER(i);
    }
    RETURN_NUMBER(sum);
}

static int divmod_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_NUMBER(0);
    ASSERT_NUMBER(1);
    double a = GET_NUMBER(0);
    double b = GET_NUMBER(1);
    if (b == 0.0) {
        vm_runtime_error(vm, "divmod: division by zero");
        return 0;
    }
    double q = floor(a / b);
    double r = a - (q * b);
    ObjTable* out = new_table();
    if (!table_set_array(&out->table, 1, NUMBER_VAL(q))) {
        vm_runtime_error(vm, "divmod: failed to set quotient");
        return 0;
    }
    if (!table_set_array(&out->table, 2, NUMBER_VAL(r))) {
        vm_runtime_error(vm, "divmod: failed to set remainder");
        return 0;
    }
    RETURN_OBJ(out);
}

static int range_iter(VM* vm, int arg_count, Value* args) {
    Value state = args[0];
    double current = AS_NUMBER(args[1]);
    
    double stop, step;
    if (IS_TABLE(state)) {
        Value v_stop, v_step;
        table_get_array(&AS_TABLE(state)->table, 1, &v_stop);
        table_get_array(&AS_TABLE(state)->table, 2, &v_step);
        stop = AS_NUMBER(v_stop);
        step = AS_NUMBER(v_step);
    } else {
        stop = AS_NUMBER(state);
        step = 1;
    }
    
    double next = current + step;
    if ((step > 0 && next > stop) || (step < 0 && next < stop)) {
        push(vm, NIL_VAL);
        push(vm, NIL_VAL);
        return 1;
    }
    
    push(vm, NUMBER_VAL(next));
    push(vm, NUMBER_VAL(next));
    return 1;
}


static int range_native(VM* vm, int arg_count, Value* args) {
    double start = 1, stop, step = 1;
    
    if (arg_count == 1) {
        stop = GET_NUMBER(0);
    } else if (arg_count == 2) {
        start = GET_NUMBER(0);
        stop = GET_NUMBER(1);
    } else if (arg_count >= 3) {
        start = GET_NUMBER(0);
        stop = GET_NUMBER(1);
        step = GET_NUMBER(2);
    } else {
        vm_runtime_error(vm, "range() expects 1-3 arguments");
        return 0;
    }
    
    // Get range_iter function
    Value iter_fn;
    ObjString* iter_name = copy_string("range_iter", 10);
    if (!table_get(&vm->globals, iter_name, &iter_fn)) {
        vm_runtime_error(vm, "range_iter not found");
        return 0;
    }
    
    // Create state table {stop, step}
    ObjTable* state = new_table();
    push(vm, OBJ_VAL(state));
    table_set_array(&state->table, 1, NUMBER_VAL(stop));
    table_set_array(&state->table, 2, NUMBER_VAL(step));
    pop(vm); // Pop protection
    
    push(vm, iter_fn);
    push(vm, OBJ_VAL(state));
    push(vm, NUMBER_VAL(start - step));
    return 1; // NativeFn should return 1 on success in this codebase?

}

static int slice_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(3);
    if (!IS_TABLE(args[0]) && !IS_STRING(args[0])) {
        vm_runtime_error(vm, "slice() expects table or string");
        return 0;
    }
    ASSERT_NUMBER(1);
    ASSERT_NUMBER(2);

    double start_d = GET_NUMBER(1);
    double end_d = GET_NUMBER(2);
    double step_d = 1;
    if (arg_count >= 4) {
        ASSERT_NUMBER(3);
        step_d = GET_NUMBER(3);
    }

    if (step_d == 0) {
        vm_runtime_error(vm, "slice() step cannot be 0");
        return 0;
    }

    int start = (int)start_d;
    int end = (int)end_d;
    int step = (int)step_d;
    if ((double)start != start_d || (double)end != end_d || (double)step != step_d) {
        vm_runtime_error(vm, "slice() expects integer start/end/step");
        return 0;
    }

    if (IS_STRING(args[0])) {
        ObjString* s = GET_STRING(0);
        int len = s->length;
        int s0 = start;
        int e0 = end;
        if (s0 < 1) s0 = 1;
        if (e0 > len) e0 = len;
        if (step > 0) {
            if (s0 > e0) { RETURN_STRING("", 0); }
            int out_len = 0;
            for (int i = s0; i <= e0; i += step) out_len++;
            char* buf = (char*)malloc(out_len + 1);
            int w = 0;
            for (int i = s0; i <= e0; i += step) {
                buf[w++] = s->chars[i - 1];
            }
            buf[w] = '\0';
            RETURN_STRING(buf, w);
        } else {
            if (s0 < e0) { RETURN_STRING("", 0); }
            int out_len = 0;
            for (int i = s0; i >= e0; i += step) out_len++;
            char* buf = (char*)malloc(out_len + 1);
            int w = 0;
            for (int i = s0; i >= e0; i += step) {
                buf[w++] = s->chars[i - 1];
            }
            buf[w] = '\0';
            RETURN_STRING(buf, w);
        }
    }

    ObjTable* src = GET_TABLE(0);
    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result)); // GC protection

    int out_index = 1;
    if (step > 0) {
        for (int i = start; i <= end; i += step) {
            Value val = NIL_VAL;
            int found = 0;
            if (i >= 1 && table_get_array(&src->table, i, &val) && !IS_NIL(val)) {
                found = 1;
            } else {
                ObjString* key = number_key_string((double)i);
                if (table_get(&src->table, key, &val) && !IS_NIL(val)) {
                    found = 1;
                }
            }
            if (found) {
                table_set_array(&result->table, out_index, val);
            }
            out_index++;
        }
    } else {
        for (int i = start; i >= end; i += step) {
            Value val = NIL_VAL;
            int found = 0;
            if (i >= 1 && table_get_array(&src->table, i, &val) && !IS_NIL(val)) {
                found = 1;
            } else {
                ObjString* key = number_key_string((double)i);
                if (table_get(&src->table, key, &val) && !IS_NIL(val)) {
                    found = 1;
                }
            }
            if (found) {
                table_set_array(&result->table, out_index, val);
            }
            out_index++;
        }
    }

    return 1;
}

static int mem_native(VM* vm, int arg_count, Value* args) {
    (void)vm;
    (void)args;
    ASSERT_ARGC_EQ(0);
    extern size_t bytes_allocated;
    RETURN_NUMBER((double)bytes_allocated);
}

void register_core(VM* vm) {
    const NativeReg core_funcs[] = {
        {"str", core_tostring},
        {"tostring", core_tostring},
        {"exit", exit_native},
        {"bool", bool_native},
        {"int", int_native},
        {"float", float_native},
        {"input", input_native},
        {"mem", mem_native},
        {"next", next_native},
        {"inext", inext_native},
        {"gen_next", gen_next_native},
        {"range_iter", range_iter},
        {"range", range_native},
        {"slice", slice_native},
        {"min", min_native},
        {"max", max_native},
        {"sum", sum_native},
        {"divmod", divmod_native},
        {"setmetatable", setmetatable_native},
        {"getmetatable", getmetatable_native},
        {"error", global_error},
        {"type", type_native},
        {NULL, NULL}
    };
    register_module(vm, NULL, core_funcs); // Register as global functions
}
