#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
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

static void sbInit(StringBuilder* sb) {
    sb->buffer = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static void sbAppend(StringBuilder* sb, const char* str, int len) {
    if (len < 0) len = (int)strlen(str);
    if (sb->length + len > sb->capacity) {
        int newCapacity = sb->capacity < 8 ? 8 : sb->capacity * 2;
        while (newCapacity < sb->length + len) newCapacity *= 2;
        sb->buffer = (char*)realloc(sb->buffer, newCapacity + 1);
        sb->capacity = newCapacity;
    }
    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

static void sbFree(StringBuilder* sb) {
    if (sb->buffer != NULL) {
        free(sb->buffer);
    }
}

static int nextNative(VM* vm, int argCount, Value* args) {

    ASSERT_ARGC_EQ(2);
    Value state = args[0];
    Value currentKey = args[1];

    if (IS_TABLE(state)) {
        ObjTable* objTable = AS_TABLE(state);
        Table* table = &objTable->table;

        // Array part first
        if (IS_NIL(currentKey) || IS_NUMBER(currentKey)) {
            double num = IS_NUMBER(currentKey) ? GET_NUMBER(1) : 0;
            int start = 1;
            if (IS_NUMBER(currentKey) && num >= 1 && (double)(int)num == num) {
                start = (int)num + 1;
            }
            for (int i = start; i <= table->arrayCapacity; i++) {
                Value val = NIL_VAL;
                if (tableGetArray(table, i, &val) && !IS_NIL(val)) {
                    push(vm, NUMBER_VAL((double)i));
                    push(vm, val);
                    return 2;
                }
            }
            currentKey = NIL_VAL; // Move to hash iteration
        }

        // Hash part
        int foundCurrent = IS_NIL(currentKey);
        ObjString* numKey = NULL;
        if (IS_NUMBER(currentKey)) {
            numKey = numberKeyString(GET_NUMBER(1));
        }

        for (int i = 0; i < table->capacity; i++) {
            Entry* entry = &table->entries[i];
            if (entry->key == NULL) continue;

            if (foundCurrent) {
                push(vm, OBJ_VAL(entry->key));
                push(vm, entry->value);
                return 2;
            }

            if (IS_STRING(currentKey)) {
                ObjString* sKey = GET_STRING(1);
                if (entry->key == sKey || 
                   (entry->key->length == sKey->length && 
                    memcmp(entry->key->chars, sKey->chars, entry->key->length) == 0)) {
                    foundCurrent = 1;
                }
            } else if (IS_NUMBER(currentKey)) {
                if (entry->key == numKey ||
                    (entry->key->length == numKey->length &&
                     memcmp(entry->key->chars, numKey->chars, entry->key->length) == 0)) {
                    foundCurrent = 1;
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
        if (IS_NUMBER(currentKey)) {
            double n = GET_NUMBER(1);
            if (n >= 1 && (double)(int)n == n) {
                index = (int)n + 1;
            }
        } else if (!IS_NIL(currentKey)) {
            vmRuntimeError(vm, "next() string control must be number or nil.");
            return 0;
        }

        if (index < 1 || index > str->length) {
            push(vm, NIL_VAL);
            push(vm, NIL_VAL);
            return 2;
        }

        push(vm, NUMBER_VAL((double)index));
        push(vm, OBJ_VAL(copyString(str->chars + (index - 1), 1)));
        return 2;
    }

    vmRuntimeError(vm, "next expects table or string as first argument.");
    return 0;
}

static int inextNative(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    ASSERT_NUMBER(1);

    ObjTable* table = GET_TABLE(0);
    double index = GET_NUMBER(1);
    double nextIndex = index + 1;
    int iNext = (int)nextIndex;

    Value value = NIL_VAL;
    int found = 0;

    if ((double)iNext == nextIndex && iNext >= 1) {
        if (tableGetArray(&table->table, iNext, &value)) {
            found = 1;
        }
    }

    if (!found) {
        ObjString* key = numberKeyString(nextIndex);
        if (tableGet(&table->table, key, &value) && !IS_NIL(value)) {
            found = 1;
        }
    }

    if (found) {
        push(vm, NUMBER_VAL(nextIndex));
        push(vm, value);
        return 2;
    }

    // Return 2 nils to match expected return count for for-in loops
    push(vm, NIL_VAL);
    push(vm, NIL_VAL);
    return 2;
}

static int setmetatableNative(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_TABLE(0);
    if (!IS_TABLE(args[1]) && !IS_NIL(args[1])) { RETURN_NIL; }

    GET_TABLE(0)->metatable = IS_NIL(args[1]) ? NULL : GET_TABLE(1);
    RETURN_VAL(args[0]);
}

static int getmetatableNative(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_TABLE(0);

    ObjTable* table = GET_TABLE(0);
    if (table->metatable == NULL) {
        RETURN_NIL;
    } else {
        RETURN_VAL(OBJ_VAL(table->metatable));
    }
}

static void formatValue(VM* vm, Value val, StringBuilder* sb, int depth) {
    if (depth > 5) {
        sbAppend(sb, "...", 3);
        return;
    }

    if (IS_STRING(val)) {
        ObjString* str = AS_STRING(val);
        sbAppend(sb, "\"", 1);
        sbAppend(sb, str->chars, str->length);
        sbAppend(sb, "\"", 1);
    } else if (IS_NUMBER(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.14g", AS_NUMBER(val));
        sbAppend(sb, buf, -1);
    } else if (IS_BOOL(val)) {
        sbAppend(sb, AS_BOOL(val) ? "true" : "false", -1);
    } else if (IS_NIL(val)) {
        sbAppend(sb, "nil", 3);
    } else if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        
        // Check for __str metamethod
        if (table->metatable != NULL) {
            Value strMethod;
            ObjString* strKey = copyString("__str", 5);
            if (tableGet(&table->metatable->table, strKey, &strMethod) && IS_CLOSURE(strMethod)) {
                // If we are formatting recursively, we can't easily call VM code without complex state management.
                // For now, just indicate it has custom string representation or fall back to default if depth > 0.
                if (depth == 0) {
                     // Let tostringNative handle the call
                     sbAppend(sb, "<table>", 7); 
                     return;
                }
                sbAppend(sb, "<custom>", 8);
                return;
            }
        }

        sbAppend(sb, "{", 1);
        int count = 0;
        
        // Iterate array part
        if (table->table.array != NULL) {
            // Find last non-nil index to avoid printing trailing nils
            int maxIndex = -1;
            for (int i = 0; i < table->table.arrayCapacity; i++) {
                if (!IS_NIL(table->table.array[i])) {
                    maxIndex = i;
                }
            }

            // Print sequence up to maxIndex
            for (int i = 0; i <= maxIndex; i++) {
                if (count > 0) sbAppend(sb, ", ", 2);
                formatValue(vm, table->table.array[i], sb, depth + 1);
                count++;
            }
        }

        // Iterate hash part
        for (int i = 0; i < table->table.capacity; i++) {
            Entry* entry = &table->table.entries[i];
            if (entry->key != NULL && !IS_NIL(entry->value)) {
                if (count > 0) sbAppend(sb, ", ", 2);
                
                sbAppend(sb, entry->key->chars, entry->key->length);
                sbAppend(sb, ": ", 2);
                
                formatValue(vm, entry->value, sb, depth + 1);
                count++;
            }
        }
        sbAppend(sb, "}", 1);


    } else if (IS_USERDATA(val)) {
        ObjUserdata* userdata = AS_USERDATA(val);
        ObjString* typeName = NULL;
        if (userdata->metatable != NULL) {
            for (int i = 0; i < userdata->metatable->table.capacity; i++) {
                Entry* entry = &userdata->metatable->table.entries[i];
                if (entry->key == NULL || IS_NIL(entry->value)) continue;
                if (entry->key->length == 6 &&
                    memcmp(entry->key->chars, "__name", 6) == 0 &&
                    IS_STRING(entry->value)) {
                    typeName = AS_STRING(entry->value);
                    break;
                }
            }
        }

        if (typeName != NULL) {
            sbAppend(sb, "<", 1);
            sbAppend(sb, typeName->chars, typeName->length);
            if (userdata->data == NULL) {
                sbAppend(sb, " closed>", 8);
            } else {
                sbAppend(sb, ">", 1);
            }
        } else if (userdata->data == NULL) {
            sbAppend(sb, "<userdata closed>", 17);
        } else {
            sbAppend(sb, "<userdata>", 10);
        }
    } else if (IS_NATIVE(val)) {
        ObjNative* native = (ObjNative*)AS_OBJ(val);
        if (native->name != NULL) {
            sbAppend(sb, "<native fn ", 11);
            sbAppend(sb, native->name->chars, native->name->length);
            sbAppend(sb, ">", 1);
        } else {
            sbAppend(sb, "<native fn>", 11);
        }
    } else {
        sbAppend(sb, "<object>", 8);
    }
}

int core_tostring(VM* vm, int argCount, Value* args) {
    Value val;
    if (argCount == 1) {
        val = args[0];
    } else if (argCount == 2) {
        // When called as string(x), args[0] is the string module table
        val = args[1];
    } else {
        vmRuntimeError(vm, "str() expects 1 argument.");
        return 0;
    }

    if (IS_STRING(val)) {
        RETURN_OBJ(AS_STRING(val));
    }

    // Check for __str on table first (top level)
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            Value strMethod;
            ObjString* strKey = copyString("__str", 5);
            if (tableGet(&table->metatable->table, strKey, &strMethod) && IS_CLOSURE(strMethod)) {
                int savedFrameCount = vm->currentThread->frameCount;

                push(vm, strMethod);
                push(vm, val);

                if (!call(vm, AS_CLOSURE(strMethod), 1)) {
                    RETURN_STRING("<table>", 7);
                }

                InterpretResult result = vmRun(vm, savedFrameCount);

                if (result != INTERPRET_OK) {
                    RETURN_STRING("<error>", 7);
                }
                return 1;
            }
        }
    }

    StringBuilder sb;
    sbInit(&sb);
    formatValue(vm, val, &sb, 0);
    
    // We can't return pointer to stack memory or malloc'd memory without managing it.
    // copyString makes a copy on the heap managed by GC.
    ObjString* str = copyString(sb.buffer, sb.length);
    sbFree(&sb);
    
    RETURN_OBJ(str);
}


static int globalError(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    vmRuntimeError(vm, "%s", GET_CSTRING(0));
    return 0; // Signal failure to vmRun
}

static int exitNative(VM* vm, int argCount, Value* args) {
    if (argCount == 0) {
        exit(0);
    }
    if (argCount == 1 && IS_NUMBER(args[0])) {
        exit((int)AS_NUMBER(args[0]));
    }
    vmRuntimeError(vm, "exit() expects no args or a numeric exit code.");
    return 0;
}

static int typeNative(VM* vm, int argCount, Value* args) {
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

static int isFalseySimple(Value v) {
    if (IS_NIL(v)) return 1;
    if (IS_BOOL(v)) return AS_BOOL(v) == 0;
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    if (IS_STRING(v)) return AS_STRING(v)->length == 0;
    if (IS_TABLE(v)) {
        ObjTable* t = AS_TABLE(v);
        if (t->table.count > 0) return 0;
        for (int i = 0; i < t->table.arrayCapacity; i++) {
            if (!IS_NIL(t->table.array[i])) return 0;
        }
        return 1;
    }
    return 0;
}

static int callBoolMetamethod(VM* vm, Value receiver, Value method, int* outBool) {
    if (IS_CLOSURE(method)) {
        int savedFrameCount = vm->currentThread->frameCount;

        push(vm, method);
        push(vm, receiver);

        if (!call(vm, AS_CLOSURE(method), 1)) {
            return 0;
        }

        InterpretResult result = vmRun(vm, savedFrameCount);
        if (result != INTERPRET_OK) {
            return 0;
        }

        Value ret = pop(vm);
        *outBool = !isFalseySimple(ret);
        return 1;
    }

    if (IS_NATIVE(method)) {
        push(vm, method);
        push(vm, receiver);

        Value* callArgs = vm->currentThread->stackTop - 1;
        vm->currentThread->stackTop -= 2;

        if (!AS_NATIVE(method)(vm, 1, callArgs)) {
            return 0;
        }

        Value ret = pop(vm);
        *outBool = !isFalseySimple(ret);
        return 1;
    }

    vmRuntimeError(vm, "__bool must be a function.");
    return 0;
}

static int boolNative(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_EQ(1);
    Value val = args[0];

    if (IS_TABLE(val)) {
        Value method = getMetamethod(vm, val, "__bool");
        if (!IS_NIL(method)) {
            int outBool = 0;
            if (!callBoolMetamethod(vm, val, method, &outBool)) {
                return 0;
            }
            RETURN_BOOL(outBool);
        }
    }

    RETURN_BOOL(!isFalseySimple(val));
}

static int intNative(VM* vm, int argCount, Value* args) {
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
            vmRuntimeError(vm, "int() expects a valid base-10 string.");
            return 0;
        }
        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        if (*end != '\0') {
            vmRuntimeError(vm, "int() expects a valid base-10 string.");
            return 0;
        }
        RETURN_NUMBER((double)num);
    }

    vmRuntimeError(vm, "int() expects number, string, or bool.");
    return 0;
}

static int floatNative(VM* vm, int argCount, Value* args) {
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
            vmRuntimeError(vm, "float() expects a valid number string.");
            return 0;
        }
        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        if (*end != '\0') {
            vmRuntimeError(vm, "float() expects a valid number string.");
            return 0;
        }
        RETURN_NUMBER(num);
    }

    vmRuntimeError(vm, "float() expects number, string, or bool.");
    return 0;
}

static int minNative(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double min = GET_NUMBER(0);
    for (int i = 1; i < argCount; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val < min) min = val;
    }
    RETURN_NUMBER(min);
}

static int maxNative(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }
    ASSERT_NUMBER(0);
    double max = GET_NUMBER(0);
    for (int i = 1; i < argCount; i++) {
        ASSERT_NUMBER(i);
        double val = GET_NUMBER(i);
        if (val > max) max = val;
    }
    RETURN_NUMBER(max);
}

static int sumNative(VM* vm, int argCount, Value* args) {
    if (argCount == 0) { RETURN_NIL; }

    if (argCount == 1 && IS_TABLE(args[0])) {
        ObjTable* table = GET_TABLE(0);
        double sum = 0.0;
        for (int i = 1; ; i++) {
            Value val;
            if (!tableGetArray(&table->table, i, &val) || IS_NIL(val)) break;
            if (!IS_NUMBER(val)) {
                vmRuntimeError(vm, "sum: element %d is not a number", i);
                return 0;
            }
            sum += AS_NUMBER(val);
        }
        RETURN_NUMBER(sum);
    }

    double sum = 0.0;
    for (int i = 0; i < argCount; i++) {
        ASSERT_NUMBER(i);
        sum += GET_NUMBER(i);
    }
    RETURN_NUMBER(sum);
}

static int rangeIter(VM* vm, int argCount, Value* args) {
    Value state = args[0];
    double current = AS_NUMBER(args[1]);
    
    double stop, step;
    if (IS_TABLE(state)) {
        Value vStop, vStep;
        tableGetArray(&AS_TABLE(state)->table, 1, &vStop);
        tableGetArray(&AS_TABLE(state)->table, 2, &vStep);
        stop = AS_NUMBER(vStop);
        step = AS_NUMBER(vStep);
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


static int rangeNative(VM* vm, int argCount, Value* args) {
    double start = 1, stop, step = 1;
    
    if (argCount == 1) {
        stop = GET_NUMBER(0);
    } else if (argCount == 2) {
        start = GET_NUMBER(0);
        stop = GET_NUMBER(1);
    } else if (argCount >= 3) {
        start = GET_NUMBER(0);
        stop = GET_NUMBER(1);
        step = GET_NUMBER(2);
    } else {
        vmRuntimeError(vm, "range() expects 1-3 arguments");
        return 0;
    }
    
    // Get rangeIter function
    Value iterFn;
    ObjString* iterName = copyString("range_iter", 10);
    if (!tableGet(&vm->globals, iterName, &iterFn)) {
        vmRuntimeError(vm, "range_iter not found");
        return 0;
    }
    
    // Create state table {stop, step}
    ObjTable* state = newTable();
    push(vm, OBJ_VAL(state));
    tableSetArray(&state->table, 1, NUMBER_VAL(stop));
    tableSetArray(&state->table, 2, NUMBER_VAL(step));
    pop(vm); // Pop protection
    
    push(vm, iterFn);
    push(vm, OBJ_VAL(state));
    push(vm, NUMBER_VAL(start - step));
    return 1; // NativeFn should return 1 on success in this codebase?

}

static int sliceNative(VM* vm, int argCount, Value* args) {
    ASSERT_ARGC_GE(3);
    if (!IS_TABLE(args[0]) && !IS_STRING(args[0])) {
        vmRuntimeError(vm, "slice() expects table or string");
        return 0;
    }
    ASSERT_NUMBER(1);
    ASSERT_NUMBER(2);

    double startD = GET_NUMBER(1);
    double endD = GET_NUMBER(2);
    double stepD = 1;
    if (argCount >= 4) {
        ASSERT_NUMBER(3);
        stepD = GET_NUMBER(3);
    }

    if (stepD == 0) {
        vmRuntimeError(vm, "slice() step cannot be 0");
        return 0;
    }

    int start = (int)startD;
    int end = (int)endD;
    int step = (int)stepD;
    if ((double)start != startD || (double)end != endD || (double)step != stepD) {
        vmRuntimeError(vm, "slice() expects integer start/end/step");
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
            int outLen = 0;
            for (int i = s0; i <= e0; i += step) outLen++;
            char* buf = (char*)malloc(outLen + 1);
            int w = 0;
            for (int i = s0; i <= e0; i += step) {
                buf[w++] = s->chars[i - 1];
            }
            buf[w] = '\0';
            RETURN_STRING(buf, w);
        } else {
            if (s0 < e0) { RETURN_STRING("", 0); }
            int outLen = 0;
            for (int i = s0; i >= e0; i += step) outLen++;
            char* buf = (char*)malloc(outLen + 1);
            int w = 0;
            for (int i = s0; i >= e0; i += step) {
                buf[w++] = s->chars[i - 1];
            }
            buf[w] = '\0';
            RETURN_STRING(buf, w);
        }
    }

    ObjTable* src = GET_TABLE(0);
    ObjTable* result = newTable();
    push(vm, OBJ_VAL(result)); // GC protection

    int outIndex = 1;
    if (step > 0) {
        for (int i = start; i <= end; i += step) {
            Value val = NIL_VAL;
            int found = 0;
            if (i >= 1 && tableGetArray(&src->table, i, &val) && !IS_NIL(val)) {
                found = 1;
            } else {
                ObjString* key = numberKeyString((double)i);
                if (tableGet(&src->table, key, &val) && !IS_NIL(val)) {
                    found = 1;
                }
            }
            if (found) {
                tableSetArray(&result->table, outIndex, val);
            }
            outIndex++;
        }
    } else {
        for (int i = start; i >= end; i += step) {
            Value val = NIL_VAL;
            int found = 0;
            if (i >= 1 && tableGetArray(&src->table, i, &val) && !IS_NIL(val)) {
                found = 1;
            } else {
                ObjString* key = numberKeyString((double)i);
                if (tableGet(&src->table, key, &val) && !IS_NIL(val)) {
                    found = 1;
                }
            }
            if (found) {
                tableSetArray(&result->table, outIndex, val);
            }
            outIndex++;
        }
    }

    return 1;
}

static int memNative(VM* vm, int argCount, Value* args) {
    (void)vm;
    (void)args;
    ASSERT_ARGC_EQ(0);
    extern size_t bytesAllocated;
    RETURN_NUMBER((double)bytesAllocated);
}

void registerCore(VM* vm) {
    const NativeReg coreFuncs[] = {
        {"exit", exitNative},
        {"bool", boolNative},
        {"int", intNative},
        {"float", floatNative},
        {"mem", memNative},
        {"next", nextNative},
        {"inext", inextNative},
        {"range_iter", rangeIter},
        {"range", rangeNative},
        {"slice", sliceNative},
        {"min", minNative},
        {"max", maxNative},
        {"sum", sumNative},
        {"setmetatable", setmetatableNative},
        {"getmetatable", getmetatableNative},
        {"error", globalError},
        {"type", typeNative},
        {NULL, NULL}
    };
    registerModule(vm, NULL, coreFuncs); // Register as global functions
}
