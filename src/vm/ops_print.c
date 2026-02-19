#include <stdio.h>

#include "ops_print.h"

int vmHandleOpPrint(VM* vm, CallFrame** frame, uint8_t** ip, InterpretResult* outResult) {
    Value v = pop(vm);

    if (IS_TABLE(v)) {
        ObjTable* table = AS_TABLE(v);
        if (table->metatable != NULL) {
            Value strMethod;
            ObjString* strKey = vm->mm_str;
            if (tableGet(&table->metatable->table, strKey, &strMethod) &&
                (IS_CLOSURE(strMethod) || IS_NATIVE(strMethod))) {
                int savedFrameCount = vm->currentThread->frameCount;

                push(vm, strMethod);
                push(vm, v);

                (*frame)->ip = *ip;
                if (callValue(vm, strMethod, 1, frame, ip)) {
                    InterpretResult result = vmRun(vm, savedFrameCount);
                    if (result == INTERPRET_OK) {
                        Value strResult = pop(vm);
                        if (IS_STRING(strResult)) {
                            ObjString* s = AS_STRING(strResult);
                            fwrite(s->chars, 1, (size_t)s->length, stdout);
                        } else {
                            printValue(strResult);
                        }
                        printf("\n");
                        return 1;
                    }
                    *outResult = result;
                    return 0;
                }
                return -1;
            }
        }
    }

    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        fwrite(s->chars, 1, (size_t)s->length, stdout);
    } else {
        printValue(v);
    }
    printf("\n");
    return 1;
}
