#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build_string.h"
#include "../lib/libs.h"

static inline int to_int64_local(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

static int appendBytes(char** buffer, int* length, int* capacity, const char* src, int srcLen) {
    if (srcLen <= 0) return 1;
    if (*length + srcLen > *capacity) {
        int newCapacity = *capacity < 8 ? 8 : *capacity * 2;
        while (newCapacity < *length + srcLen) newCapacity *= 2;
        char* newBuffer = (char*)realloc(*buffer, (size_t)newCapacity + 1);
        if (newBuffer == NULL) return 0;
        *buffer = newBuffer;
        *capacity = newCapacity;
    }

    memcpy(*buffer + *length, src, (size_t)srcLen);
    *length += srcLen;
    (*buffer)[*length] = '\0';
    return 1;
}

static int appendNumber(char** buffer, int* length, int* capacity, double number) {
    int64_t i64;
    if (to_int64_local(number, &i64)) {
        char tmp[32];
        int idx = 0;
        uint64_t u = (i64 < 0) ? (uint64_t)(-(i64 + 1)) + 1 : (uint64_t)i64;
        do {
            tmp[idx++] = (char)('0' + (u % 10));
            u /= 10;
        } while (u > 0);
        if (i64 < 0) tmp[idx++] = '-';

        char out[32];
        for (int j = 0; j < idx; j++) out[j] = tmp[idx - 1 - j];
        return appendBytes(buffer, length, capacity, out, idx);
    }

    char numBuf[32];
    int numLen = snprintf(numBuf, sizeof(numBuf), "%.14g", number);
    if (numLen < 0) return 0;
    return appendBytes(buffer, length, capacity, numBuf, numLen);
}

int vmBuildString(VM* vm, uint8_t partCount) {
    if (partCount == 0) {
        push(vm, OBJ_VAL(copyString("", 0)));
        return 1;
    }

    char* buffer = NULL;
    int length = 0;
    int capacity = 0;

    for (int i = partCount - 1; i >= 0; i--) {
        Value part = peek(vm, i);
        if (IS_STRING(part)) {
            ObjString* s = AS_STRING(part);
            if (!appendBytes(&buffer, &length, &capacity, s->chars, s->length)) {
                free(buffer);
                vmRuntimeError(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_NUMBER(part)) {
            if (!appendNumber(&buffer, &length, &capacity, AS_NUMBER(part))) {
                free(buffer);
                vmRuntimeError(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_BOOL(part)) {
            const char* b = AS_BOOL(part) ? "true" : "false";
            int bLen = AS_BOOL(part) ? 4 : 5;
            if (!appendBytes(&buffer, &length, &capacity, b, bLen)) {
                free(buffer);
                vmRuntimeError(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        if (IS_NIL(part)) {
            if (!appendBytes(&buffer, &length, &capacity, "nil", 3)) {
                free(buffer);
                vmRuntimeError(vm, "Out of memory while building string.");
                return 0;
            }
            continue;
        }

        Value arg = part;
        if (!core_tostring(vm, 1, &arg)) {
            free(buffer);
            return 0;
        }

        Value strVal = pop(vm);
        if (!IS_STRING(strVal)) {
            free(buffer);
            vmRuntimeError(vm, "str() must return a string.");
            return 0;
        }
        ObjString* s = AS_STRING(strVal);
        if (!appendBytes(&buffer, &length, &capacity, s->chars, s->length)) {
            free(buffer);
            vmRuntimeError(vm, "Out of memory while building string.");
            return 0;
        }
    }

    for (int i = 0; i < partCount; i++) pop(vm);

    if (buffer == NULL) {
        push(vm, OBJ_VAL(copyString("", 0)));
    } else {
        push(vm, OBJ_VAL(takeString(buffer, length)));
    }
    return 1;
}
