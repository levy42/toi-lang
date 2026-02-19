#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>

#include "common.h"
#include "debug.h"
#include "table.h"
#include "vm.h"
#include "vm/build_string.h"
#include "vm/ops_arith.h"
#include "vm/ops_arith_const.h"
#include "vm/ops_compare.h"
#include "vm/ops_control.h"
#include "vm/ops_exception.h"
#include "vm/ops_float.h"
#include "vm/ops_has.h"
#include "vm/ops_import.h"
#include "vm/ops_import_star.h"
#include "vm/ops_iter.h"
#include "vm/ops_local_const.h"
#include "vm/ops_local_set.h"
#include "vm/ops_meta.h"
#include "vm/ops_mod.h"
#include "vm/ops_power.h"
#include "vm/ops_print.h"
#include "vm/ops_state.h"
#include "vm/ops_table.h"
#include "vm/ops_unary.h"
#include "lib/libs.h" // For registerLibs
#include "object.h" // For freeObject

// #define DEBUG_TABLE_KEYS

// Forward declaration for compile function
ObjFunction* compile(const char* source);
static void closeUpvalues(VM* vm, Value* last);

static inline int to_int64(double x, int64_t* out) {
    if (x < (double)INT64_MIN || x > (double)INT64_MAX) return 0;
    int64_t i = (int64_t)x;
    if ((double)i != x) return 0;
    *out = i;
    return 1;
}

static int valueMatchesType(Value v, uint8_t type) {
    switch (type) {
        case TYPEHINT_ANY:
            return 1;
        case TYPEHINT_INT: {
            if (!IS_NUMBER(v)) return 0;
            int64_t tmp;
            return to_int64(AS_NUMBER(v), &tmp);
        }
        case TYPEHINT_FLOAT:
            return IS_NUMBER(v);
        case TYPEHINT_BOOL:
            return IS_BOOL(v);
        case TYPEHINT_STR:
            return IS_STRING(v);
        case TYPEHINT_TABLE:
            return IS_TABLE(v);
        default:
            return 0;
    }
}

static void applyPendingSetLocal(VM* vm) {
    if (vm->pendingSetLocalCount == 0) return;
    int top = vm->pendingSetLocalCount - 1;
    int frameIndex = vm->pendingSetLocalFrames[top];
    if (frameIndex != vm->currentThread->frameCount - 1) return;
    CallFrame* target = &vm->currentThread->frames[frameIndex];
    target->slots[vm->pendingSetLocalSlots[top]] = peek(vm, 0);
    vm->pendingSetLocalCount--;
}

extern void registerLibs(VM* vm);
extern void freeObject(struct Obj* object);
void collectGarbage(VM* vm); // Defined later in this file

static void resetStack(VM* vm) {
    if (vm->currentThread != NULL) {
        vm->currentThread->stackTop = vm->currentThread->stack;
        vm->currentThread->frameCount = 0;
        vm->currentThread->openUpvalues = NULL;
    }
}

void vmRuntimeError(VM* vm, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    vm->hasException = 1;
    vm->exception = OBJ_VAL(copyString(buffer, (int)strlen(buffer)));
}

static void reportException(VM* vm) {
    if (!vm->hasException) return;
    Value ex = vm->exception;
    if (IS_STRING(ex)) {
        fprintf(stderr, COLOR_RED "Runtime Error: " COLOR_RESET "%s\n", AS_CSTRING(ex));
    } else {
        fprintf(stderr, COLOR_RED "Runtime Error: " COLOR_RESET "<exception>\n");
    }

    for (int i = vm->currentThread->frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &vm->currentThread->frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
}

Value peek(VM* vm, int distance) {
    return vm->currentThread->stackTop[-1 - distance];
}

static int handleException(VM* vm, CallFrame** frame, uint8_t** ip) {
    if (!vm->hasException) return 0;
    ObjThread* thread = vm->currentThread;
    while (thread->handlerCount > 0) {
        ExceptionHandler* handler = &thread->handlers[thread->handlerCount - 1];

        while (thread->frameCount > handler->frameCount) {
            CallFrame* f = &thread->frames[thread->frameCount - 1];
            closeUpvalues(vm, f->slots);
            thread->frameCount--;
        }

        if (thread->frameCount == 0) {
            reportException(vm);
            resetStack(vm);
            vm->hasException = 0;
            vm->exception = NIL_VAL;
            return 0;
        }

        *frame = &thread->frames[thread->frameCount - 1];
        thread->stackTop = handler->stackTop;

        if (handler->hasExcept && !handler->inExcept) {
            handler->inExcept = 1;
            *ip = handler->except_ip;
            push(vm, vm->exception);
            vm->hasException = 0;
            vm->exception = NIL_VAL;
            return 1;
        }

        if (handler->hasFinally && handler->finally_ip != NULL) {
            thread->handlerCount--;
            *ip = handler->finally_ip;
            return 1;
        }

        thread->handlerCount--;
    }

    reportException(vm);
    resetStack(vm);
    vm->hasException = 0;
    vm->exception = NIL_VAL;
    return 0;
}

static volatile sig_atomic_t interruptRequested = 0;

void vmRequestInterrupt(void) {
    interruptRequested = 1;
}

void push(VM* vm, Value value) {
    *vm->currentThread->stackTop = value;
    vm->currentThread->stackTop++;
}

Value pop(VM* vm) {
    vm->currentThread->stackTop--;
    return *vm->currentThread->stackTop;
}

static Value getMetamethodCached(VM* vm, Value val, ObjString* name);
static int callNamed(VM* vm, ObjClosure* closure, int argCount);

static int finishClosureCall(VM* vm, ObjClosure* closure, int argCount) {
    ObjFunction* function = closure->function;

    if (function->paramTypesCount > 0) {
        int checkCount = function->paramTypesCount < function->arity ? function->paramTypesCount : function->arity;
        Value* args = vm->currentThread->stackTop - argCount;
        for (int i = 0; i < checkCount; i++) {
            uint8_t type = function->paramTypes[i];
            if (type == TYPEHINT_ANY) continue;
            if (!valueMatchesType(args[i], type)) {
                vmRuntimeError(vm, "Type mismatch for parameter %d.", i + 1);
                return 0;
            }
        }
    }

    if (vm->currentThread->frameCount == FRAMES_MAX) {
        vmRuntimeError(vm, "Stack overflow.");
        return 0;
    }

    CallFrame* frame = &vm->currentThread->frames[vm->currentThread->frameCount++];
    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = vm->currentThread->stackTop - argCount - 1;
    return 1;
}

int callValue(VM* vm, Value callee, int argCount, CallFrame** frame, uint8_t** ip) {
    if (IS_NATIVE(callee)) {
        NativeFn native = AS_NATIVE(callee);
        Value* args = vm->currentThread->stackTop - argCount;
        vm->currentThread->stackTop -= argCount + 1; // Pop args and callee

        (*frame)->ip = *ip;
        ObjThread* current = vm->currentThread;
        if (!native(vm, argCount, args)) {
            return 0;
        }

        if (vm->currentThread != current) {
            *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
            *ip = (*frame)->ip;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        (*frame)->ip = *ip;
        if (!call(vm, AS_CLOSURE(callee), argCount)) {
            return 0;
        }
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    return 0;
}

static int invokeCallWithArgCount(VM* vm, int argCount, CallFrame** frame, uint8_t** ip) {
    Value callee = peek(vm, argCount);
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value methodVal = OBJ_VAL(bound->method);

        // Stack: [callee, arg1, ..., argN]
        // We want: [method, receiver, arg1, ..., argN]
        for (int i = 0; i < argCount; i++) {
            vm->currentThread->stackTop[0 - i] = vm->currentThread->stackTop[-1 - i];
        }
        vm->currentThread->stackTop[-argCount] = bound->receiver;
        vm->currentThread->stackTop[-argCount - 1] = methodVal;
        vm->currentThread->stackTop++;
        argCount++;
        callee = methodVal;
    }

    if (IS_NATIVE(callee) || IS_CLOSURE(callee)) {
        if (!callValue(vm, callee, argCount, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_TABLE(callee)) {
        // __call metamethod: __call(table, ...)
        Value mm = getMetamethodCached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            // Stack: [callee, arg1, ..., argN]
            // We want: [mm, callee, arg1, ..., argN]
            for (int i = 0; i < argCount; i++) {
                vm->currentThread->stackTop[0 - i] = vm->currentThread->stackTop[-1 - i];
            }

            vm->currentThread->stackTop[-argCount] = callee;      // Insert table as first arg
            vm->currentThread->stackTop[-argCount - 1] = mm;      // Replace callee slot with callable
            vm->currentThread->stackTop++;                        // Increase stack size

            argCount++;
            if (!callValue(vm, mm, argCount, frame, ip)) {
                return 0;
            }
            return 1;
        }
    }

    vmRuntimeError(vm, "Can only call functions.");
    return 0;
}

static int invokeCallWithNamedArgCount(VM* vm, int argCount, CallFrame** frame, uint8_t** ip) {
    Value callee = peek(vm, argCount);
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        Value methodVal = OBJ_VAL(bound->method);

        for (int i = 0; i < argCount; i++) {
            vm->currentThread->stackTop[0 - i] = vm->currentThread->stackTop[-1 - i];
        }
        vm->currentThread->stackTop[-argCount] = bound->receiver;
        vm->currentThread->stackTop[-argCount - 1] = methodVal;
        vm->currentThread->stackTop++;
        argCount++;
        callee = methodVal;
    }

    if (IS_NATIVE(callee)) {
        if (!callValue(vm, callee, argCount, frame, ip)) {
            return 0;
        }
        return 1;
    }

    if (IS_CLOSURE(callee)) {
        (*frame)->ip = *ip;
        if (!callNamed(vm, AS_CLOSURE(callee), argCount)) {
            return 0;
        }
        *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
        *ip = (*frame)->ip;
        return 1;
    }

    if (IS_TABLE(callee)) {
        Value mm = getMetamethodCached(vm, callee, vm->mm_call);
        if (IS_CLOSURE(mm) || IS_NATIVE(mm)) {
            for (int i = 0; i < argCount; i++) {
                vm->currentThread->stackTop[0 - i] = vm->currentThread->stackTop[-1 - i];
            }

            vm->currentThread->stackTop[-argCount] = callee;
            vm->currentThread->stackTop[-argCount - 1] = mm;
            vm->currentThread->stackTop++;

            argCount++;
            if (IS_CLOSURE(mm)) {
                (*frame)->ip = *ip;
                if (!callNamed(vm, AS_CLOSURE(mm), argCount)) {
                    return 0;
                }
                *frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                *ip = (*frame)->ip;
            } else {
                if (!callValue(vm, mm, argCount, frame, ip)) {
                    return 0;
                }
            }
            return 1;
        }
    }

    vmRuntimeError(vm, "Can only call functions.");
    return 0;
}

static void discardHandlersForFrameReturn(ObjThread* thread) {
    int currentFrameCount = thread->frameCount;
    while (thread->handlerCount > 0 &&
           thread->handlers[thread->handlerCount - 1].frameCount >= currentFrameCount) {
        thread->handlerCount--;
    }
}

static Value getMetamethodCached(VM* vm, Value val, ObjString* name) {
    (void)vm;
    Value method = NIL_VAL;
    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            tableGet(&table->metatable->table, name, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            tableGet(&udata->metatable->table, name, &method);
        }
    }
    return method;
}

static ObjUpvalue* captureUpvalue(VM* vm, Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue = vm->currentThread->openUpvalues;

    // Find existing upvalue or position to insert
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    // If we found an existing upvalue for this slot, reuse it
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    // Create new upvalue
    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm->currentThread->openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(VM* vm, Value* last) {

    while (vm->currentThread->openUpvalues != NULL &&

           vm->currentThread->openUpvalues->location >= last) {

        ObjUpvalue* upvalue = vm->currentThread->openUpvalues;

        upvalue->closed = *upvalue->location;

        upvalue->location = &upvalue->closed;

        vm->currentThread->openUpvalues = upvalue->next;

    }

}

int call(VM* vm, ObjClosure* closure, int argCount) {
    ObjFunction* function = closure->function;

    // Handle variadic functions
    if (function->isVariadic) {
        int requiredArgs = function->arity - 1;  // Minus the varargs parameter

        if (argCount < requiredArgs) {
            vmRuntimeError(vm, "Expected at least %d arguments but got %d.", requiredArgs, argCount);
            return 0;
        }

        // Collect extra arguments into a table
        int extraArgs = argCount - requiredArgs;
        ObjTable* varargs = newTable();

        // Pop extra args and put them in the varargs array part so
        // call-spread (`fn(*args)`) can iterate them via tableGetArray.
        for (int i = 0; i < extraArgs; i++) {
            Value arg = vm->currentThread->stackTop[-extraArgs + i];
            tableSetArray(&varargs->table, i + 1, arg);
            #ifdef DEBUG_VARIADIC
            printf("Vararg[%d] = ", i + 1);
            printValue(arg);
            printf("\n");
            #endif
        }

        // Remove the extra args from the stack
        vm->currentThread->stackTop -= extraArgs;

        // Push the varargs table as the last argument
        push(vm, OBJ_VAL(varargs));

        #ifdef DEBUG_VARIADIC
        printf("Created varargs table with %d args\n", extraArgs);
        #endif
        argCount = function->arity;
    } else {
        // Non-variadic function handling
        if (argCount > function->arity) {
            vmRuntimeError(vm, "Expected %d arguments but got %d.", function->arity, argCount);
            return 0;
        }

        if (argCount < function->arity) {
            if (function->defaultsCount == 0) {
                vmRuntimeError(vm, "Expected %d arguments but got %d.", function->arity, argCount);
                return 0;
            }

            int defaultStart = function->arity - function->defaultsCount;
            if (argCount < defaultStart) {
                vmRuntimeError(vm, "Expected at least %d arguments (non-default parameters) but got %d.", defaultStart, argCount);
                return 0;
            }

            for (int i = argCount; i < function->arity; i++) {
                push(vm, function->defaults[i - defaultStart]);
            }
            argCount = function->arity;
        }
    }

    return finishClosureCall(vm, closure, argCount);
}

static int findNamedParamIndex(ObjFunction* function, ObjString* key, int nonVariadicArity) {
    if (function->paramNames == NULL) return -1;
    int limit = function->paramNamesCount < nonVariadicArity ? function->paramNamesCount : nonVariadicArity;
    for (int i = 0; i < limit; i++) {
        ObjString* name = function->paramNames[i];
        if (name == NULL) continue;
        if (name == key) return i;
        if (name->hash == key->hash &&
            name->length == key->length &&
            memcmp(name->chars, key->chars, name->length) == 0) {
            return i;
        }
    }
    return -1;
}

static int isOptionsParamName(ObjString* name) {
    if (name == NULL) return 0;
    if (name->length == 4 && memcmp(name->chars, "opts", 4) == 0) return 1;
    if (name->length == 7 && memcmp(name->chars, "options", 7) == 0) return 1;
    if (name->length == 6 && memcmp(name->chars, "kwargs", 6) == 0) return 1;
    return 0;
}

static int callNamed(VM* vm, ObjClosure* closure, int argCount) {
    ObjFunction* function = closure->function;
    if (argCount < 1) {
        vmRuntimeError(vm, "Named call requires a named-arguments table.");
        return 0;
    }

    Value namedValue = vm->currentThread->stackTop[-1];
    if (!IS_TABLE(namedValue)) {
        vmRuntimeError(vm, "Named call requires a table as final argument.");
        return 0;
    }

    ObjTable* namedArgs = AS_TABLE(namedValue);
    int positionalCount = argCount - 1;
    int nonVariadicArity = function->isVariadic ? (function->arity - 1) : function->arity;
    if (nonVariadicArity < 0) nonVariadicArity = 0;

    Value* incoming = vm->currentThread->stackTop - argCount;
    Value boundArgs[256];
    uint8_t assigned[256];
    memset(assigned, 0, sizeof(assigned));

    ObjTable* varargs = NULL;
    int varargPos = 0;
    if (function->isVariadic) {
        varargs = newTable();
    }
    ObjTable* legacyOptions = NULL;
    ObjString* firstUnexpected = NULL;

    int positionalToBind = positionalCount < nonVariadicArity ? positionalCount : nonVariadicArity;
    for (int i = 0; i < positionalToBind; i++) {
        boundArgs[i] = incoming[i];
        assigned[i] = 1;
    }

    if (positionalCount > nonVariadicArity) {
        if (!function->isVariadic) {
            vmRuntimeError(vm, "Expected %d arguments but got %d.", function->arity, positionalCount);
            return 0;
        }
        for (int i = nonVariadicArity; i < positionalCount; i++) {
            tableSetArray(&varargs->table, ++varargPos, incoming[i]);
        }
    }

    for (int i = 0; i < namedArgs->table.capacity; i++) {
        Entry* entry = &namedArgs->table.entries[i];
        if (entry->key == NULL) continue;

        int index = findNamedParamIndex(function, entry->key, nonVariadicArity);
        if (index >= 0) {
            if (assigned[index]) {
                vmRuntimeError(vm, "Multiple values for argument '%s'.", entry->key->chars);
                return 0;
            }
            boundArgs[index] = entry->value;
            assigned[index] = 1;
            continue;
        }

        if (function->isVariadic) {
            tableSet(&varargs->table, entry->key, entry->value);
            continue;
        }

        if (legacyOptions == NULL) {
            legacyOptions = newTable();
            firstUnexpected = entry->key;
        }
        tableSet(&legacyOptions->table, entry->key, entry->value);
    }

    if (legacyOptions != NULL) {
        int target = -1;
        for (int i = 0; i < nonVariadicArity; i++) {
            if (assigned[i]) continue;
            if (function->paramNames != NULL && i < function->paramNamesCount &&
                isOptionsParamName(function->paramNames[i])) {
                target = i;
                break;
            }
        }
        if (target >= 0) {
            boundArgs[target] = OBJ_VAL(legacyOptions);
            assigned[target] = 1;
        } else {
            vmRuntimeError(vm, "Unexpected named argument '%s'.", firstUnexpected->chars);
            return 0;
        }
    }

    int defaultStart = nonVariadicArity - function->defaultsCount;
    for (int i = 0; i < nonVariadicArity; i++) {
        if (assigned[i]) continue;
        if (i >= defaultStart) {
            boundArgs[i] = function->defaults[i - defaultStart];
            assigned[i] = 1;
            continue;
        }
        if (function->paramNames != NULL && i < function->paramNamesCount && function->paramNames[i] != NULL) {
            vmRuntimeError(vm, "Missing required argument '%s'.", function->paramNames[i]->chars);
        } else {
            vmRuntimeError(vm, "Missing required argument %d.", i + 1);
        }
        return 0;
    }

    if (function->isVariadic) {
        boundArgs[nonVariadicArity] = OBJ_VAL(varargs);
    }

    vm->currentThread->stackTop -= argCount;
    for (int i = 0; i < function->arity; i++) {
        push(vm, boundArgs[i]);
    }

    return finishClosureCall(vm, closure, function->arity);
}

static void markRoots(VM* vm) {
    if (vm->currentThread != NULL) {
        markObject((struct Obj*)vm->currentThread);
    }
    if (vm->mm_index != NULL) markObject((struct Obj*)vm->mm_index);
    if (vm->mm_newindex != NULL) markObject((struct Obj*)vm->mm_newindex);
    if (vm->mm_str != NULL) markObject((struct Obj*)vm->mm_str);
    if (vm->mm_call != NULL) markObject((struct Obj*)vm->mm_call);
    if (vm->mm_new != NULL) markObject((struct Obj*)vm->mm_new);
    if (vm->mm_append != NULL) markObject((struct Obj*)vm->mm_append);
    if (vm->mm_next != NULL) markObject((struct Obj*)vm->mm_next);
    if (vm->hasException) markValue(vm->exception);

    // Mark globals
    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL) {
            markObject((struct Obj*)entry->key);
            markValue(entry->value);
        }
    }
}

void initVM(VM* vm) {
   vm->currentThread = newThread(); // Create initial (main) thread
   vm->currentThread->vm = vm;
   vm->disableGC = 0;
   vm->isREPL = 0;
   vm->pendingSetLocalCount = 0;
   vm->hasException = 0;
   vm->exception = NIL_VAL;
   vm->mm_index = NULL;
   vm->mm_newindex = NULL;
   vm->mm_str = NULL;
   vm->mm_call = NULL;
   vm->mm_new = NULL;
   vm->mm_append = NULL;
   vm->mm_next = NULL;

    initTable(&vm->globals);
    initTable(&vm->modules);
    vm->cliArgc = 0;
    vm->cliArgv = NULL;
   vm->currentThread->openUpvalues = NULL;

    vm->mm_index = copyString("__index", 7);
    vm->mm_newindex = copyString("__newindex", 10);
    vm->mm_str = copyString("__str", 5);
    vm->mm_call = copyString("__call", 6);
    vm->mm_new = copyString("__new", 5);
    vm->mm_append = copyString("__append", 8);
    vm->mm_next = copyString("__next", 6);

    // Register built-in native functions (from libs module)
    registerLibs(vm);
}

void freeVM(VM* vm) {
   freeTable(&vm->globals);
   freeTable(&vm->modules);
   // The currentThread will be freed as part of GC if it's reachable.
    // We manually free the main thread if it's not part of GC collection.
    // Since we explicitly control it, we can free it here.
    // freeObject((struct Obj*)vm->currentThread);
    vm->currentThread = NULL;

    collectGarbage(vm); // Final garbage collection
#ifdef DEBUG_LOG_GC
    printf("-- GC DONE --\n");
#endif
}

void collectGarbage(VM* vm) {
    markRoots(vm);
    sweepObjects();
}

void maybeCollectGarbage(VM* vm) {
    if (vm->disableGC) return;  // Skip GC if disabled

    extern size_t bytesAllocated;
    extern size_t nextGC;
    if (bytesAllocated > nextGC) {
        collectGarbage(vm);
    }
}

void defineNative(VM* vm, const char* name, NativeFn function) {
    ObjString* nameStr = copyString(name, (int)strlen(name));
    push(vm, OBJ_VAL(nameStr));
    push(vm, OBJ_VAL(newNative(function, nameStr)));
    tableSet(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm); // Native function.
    pop(vm); // Native name.
}

ObjString* numberKeyString(double num) {
    if (num == 0) num = 0.0; // normalize -0 to 0
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%.17g", num);
    char buffer[2 + 32];
    buffer[0] = '\x1F';
    buffer[1] = 'n';
    size_t len = strlen(numbuf);
    memcpy(buffer + 2, numbuf, len);
    return copyString(buffer, (int)(2 + len));
}

Value getMetamethod(VM* vm, Value val, const char* name) {
    ObjString* methodName = copyString(name, (int)strlen(name));
    push(vm, OBJ_VAL(methodName)); // Protect from GC
    Value method = NIL_VAL;

    if (IS_TABLE(val)) {
        ObjTable* table = AS_TABLE(val);
        if (table->metatable != NULL) {
            tableGet(&table->metatable->table, methodName, &method);
        }
    } else if (IS_USERDATA(val)) {
        ObjUserdata* udata = AS_USERDATA(val);
        if (udata->metatable != NULL) {
            tableGet(&udata->metatable->table, methodName, &method);
        }
    }
    pop(vm); // Pop methodName
    return method;
}

InterpretResult vmRun(VM* vm, int minFrameCount) {
    CallFrame* frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
    uint8_t* ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_SHORT() \
    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

    for (;;) {
        if (interruptRequested) {
            interruptRequested = 0;
            vmRuntimeError(vm, "Interrupted.");
            goto runtime_error;
        }
#ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value* slot = vm->currentThread->stack; slot < vm->currentThread->stackTop; slot++) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        disassembleInstruction(&frame->closure->function->chunk, (int)(ip - frame->closure->function->chunk.code));
#endif

        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_TRY: {
                if (!vmHandleOpTry(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_END_TRY: {
                vmHandleOpEndTry(vm);
                break;
            }
            case OP_END_FINALLY: {
                if (!vmHandleOpEndFinally(vm)) goto runtime_error;
                break;
            }
            case OP_THROW: {
                vmHandleOpThrow(vm);
                goto runtime_error;
            }
            case OP_CONSTANT: {
                vmHandleOpConstant(vm, frame, &ip);
                break;
            }
            case OP_BUILD_STRING: {
                uint8_t partCount = READ_BYTE();
                if (!vmBuildString(vm, partCount)) goto runtime_error;
                break;
            }
            case OP_NIL: push(vm, NIL_VAL); break;
            case OP_TRUE: push(vm, BOOL_VAL(1)); break;
            case OP_FALSE: push(vm, BOOL_VAL(0)); break;
            case OP_POP: pop(vm); break;
            case OP_GET_GLOBAL: {
                if (!vmHandleOpGetGlobal(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DEFINE_GLOBAL: {
                vmHandleOpDefineGlobal(vm, frame, &ip);
                break;
            }
            case OP_SET_GLOBAL: {
                vmHandleOpSetGlobal(vm, frame, &ip);
                break;
            }
            case OP_DELETE_GLOBAL: {
                if (!vmHandleOpDeleteGlobal(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GET_LOCAL: {
                vmHandleOpGetLocal(vm, frame, &ip);
                break;
            }
            case OP_SET_LOCAL: {
                vmHandleOpSetLocal(vm, frame, &ip);
                break;
            }
            case OP_ADD_SET_LOCAL: {
                if (!vmHandleOpAddSetLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SUB_SET_LOCAL: {
                if (!vmHandleOpSubSetLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MUL_SET_LOCAL: {
                if (!vmHandleOpMulSetLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DIV_SET_LOCAL: {
                if (!vmHandleOpDivSetLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MOD_SET_LOCAL: {
                if (!vmHandleOpModSetLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_INC_LOCAL: {
                if (!vmHandleOpIncLocal(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SUB_LOCAL_CONST: {
                if (!vmHandleOpSubLocalConst(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MUL_LOCAL_CONST: {
                if (!vmHandleOpMulLocalConst(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DIV_LOCAL_CONST: {
                if (!vmHandleOpDivLocalConst(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_MOD_LOCAL_CONST: {
                if (!vmHandleOpModLocalConst(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GET_UPVALUE: {
                vmHandleOpGetUpvalue(vm, frame, &ip);
                break;
            }
            case OP_SET_UPVALUE: {
                vmHandleOpSetUpvalue(vm, frame, &ip);
                break;
            }
            case OP_CLOSE_UPVALUE: {
                closeUpvalues(vm, vm->currentThread->stackTop - 1);
                pop(vm);
                break;
            }
            case OP_NEW_TABLE: {
                if (!vmHandleOpNewTable(vm)) goto runtime_error;
                break;
            }
            case OP_SET_METATABLE: {
                if (!vmHandleOpSetMetatable(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DUP: push(vm, peek(vm, 0)); break;
            case OP_GET_TABLE: {
                if (!vmHandleOpGetTable(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SET_TABLE: {
                if (!vmHandleOpSetTable(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_DELETE_TABLE: {
                if (!vmHandleOpDeleteTable(vm)) goto runtime_error;
                break;
            }
            case OP_PRINT: {
                InterpretResult printResult = INTERPRET_OK;
                int printStatus = vmHandleOpPrint(vm, &frame, &ip, &printResult);
                if (printStatus < 0) goto runtime_error;
                if (printStatus == 0) return printResult;
                break;
            }
            case OP_JUMP: {
                vmHandleOpJump(&ip);
                break;
            }
            case OP_JUMP_IF_FALSE: {
                vmHandleOpJumpIfFalse(vm, &ip);
                break;
            }
            case OP_JUMP_IF_TRUE: {
                vmHandleOpJumpIfTrue(vm, &ip);
                break;
            }
            case OP_LOOP: {
                vmHandleOpLoop(&ip);
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!invokeCallWithArgCount(vm, argCount, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL_NAMED: {
                int argCount = READ_BYTE();
                if (!invokeCallWithNamedArgCount(vm, argCount, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_CALL_EXPAND: {
                int fixedArgCount = READ_BYTE();
                Value spread = peek(vm, 0);
                if (!IS_TABLE(spread)) {
                    vmRuntimeError(vm, "Spread argument must be a table.");
                    goto runtime_error;
                }

                ObjTable* spreadTable = AS_TABLE(spread);
                int spreadCount = 0;
                int hasNamed = spreadTable->table.count > 0;
                for (int i = 1; ; i++) {
                    Value val = NIL_VAL;
                    if (!tableGetArray(&spreadTable->table, i, &val) || IS_NIL(val)) {
                        break;
                    }
                    spreadCount++;
                    if (fixedArgCount + spreadCount > 255) {
                        vmRuntimeError(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                }

                pop(vm); // Remove spread table
                for (int i = 1; i <= spreadCount; i++) {
                    Value val = NIL_VAL;
                    tableGetArray(&spreadTable->table, i, &val);
                    push(vm, val);
                }

                int argCount = fixedArgCount + spreadCount;
                if (hasNamed) {
                    ObjTable* named = newTable();
                    tableAddAll(&spreadTable->table, &named->table);
                    push(vm, OBJ_VAL(named));
                    if (argCount + 1 > 255) {
                        vmRuntimeError(vm, "Can't have more than 255 arguments.");
                        goto runtime_error;
                    }
                    if (!invokeCallWithNamedArgCount(vm, argCount + 1, &frame, &ip)) {
                        goto runtime_error;
                    }
                } else {
                    if (!invokeCallWithArgCount(vm, argCount, &frame, &ip)) {
                        goto runtime_error;
                    }
                }
                break;
            }
            case OP_ITER_PREP: {
                if (!vmHandleOpIterPrep(vm)) goto runtime_error;
                break;
            }
            case OP_ITER_PREP_IPAIRS: {
                if (!vmHandleOpIterPrepIPairs(vm)) goto runtime_error;
                break;
            }
            case OP_RANGE: {
                if (!vmHandleOpRange(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_FOR_PREP: {
                if (!vmHandleOpForPrep(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_FOR_LOOP: {
                if (!vmHandleOpForLoop(vm, frame, &ip)) goto runtime_error;
                break;
            }
            case OP_SLICE: {
                if (!vmHandleOpSlice(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = newClosure(function);
                push(vm, OBJ_VAL(closure));

                // Read upvalue information
                for (int i = 0; i < function->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (isLocal) {
                        closure->upvalues[i] = captureUpvalue(vm, frame->slots + index);
                    }
                    else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                closure->upvalueCount = function->upvalueCount;
                break;
            }
            case OP_RETURN: {
                Value result = pop(vm);
                closeUpvalues(vm, frame->slots);
                discardHandlersForFrameReturn(vm->currentThread);
                vm->currentThread->frameCount--;

                // Restore stack and push result
                vm->currentThread->stackTop = frame->slots;
                push(vm, result);
                applyPendingSetLocal(vm);

                if (vm->currentThread->frameCount <= minFrameCount) {
                    if (vm->currentThread->caller != NULL) {
                        ObjThread* caller = vm->currentThread->caller;
                        vm->currentThread->caller = NULL;
                        
                        // Check stack overflow
                        if (caller->stackTop + 2 >= caller->stack + STACK_MAX) {
                            vmRuntimeError(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        *caller->stackTop = BOOL_VAL(1);
                        caller->stackTop++;
                        *caller->stackTop = result;
                        caller->stackTop++;
                        
                        vm->currentThread = caller;
                        frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                        ip = frame->ip;
                        break;
                    }

                    // In REPL mode, leave the result on stack so it can be printed
                    // In normal mode, pop the script closure
                    if (minFrameCount == 0 && !vm->isREPL) {
                        pop(vm);  // Pop the script closure when completely done
                    }
                    return INTERPRET_OK;
                }

                frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_RETURN_N: {
                uint8_t count = READ_BYTE();
                Value* results = vm->currentThread->stackTop - count;
                closeUpvalues(vm, frame->slots);
                discardHandlersForFrameReturn(vm->currentThread);
                vm->currentThread->frameCount--;

                // Copy results to where the function was called (frame->slots)
                // This replaces the function + args with the return values
                Value* dest = frame->slots;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_RETURN_N: count=%d, dest offset=%ld\n", count, dest - vm->currentThread->stack);
                for (int i = 0; i < count; i++) {
                    printf("  result[%d] = ", i);
                    printValue(results[i]);
                    printf("\n");
                }
                #endif
                for (int i = 0; i < count; i++) {
                    dest[i] = results[i];
                }
                vm->currentThread->stackTop = dest + count;
                applyPendingSetLocal(vm);

                if (vm->currentThread->frameCount <= minFrameCount) {
                    if (vm->currentThread->caller != NULL) {
                        ObjThread* caller = vm->currentThread->caller;
                        vm->currentThread->caller = NULL;

                        if (caller->stackTop + 1 + count >= caller->stack + STACK_MAX) {
                            vmRuntimeError(vm, "Stack overflow in caller.");
                            goto runtime_error;
                        }

                        *caller->stackTop = BOOL_VAL(1);
                        caller->stackTop++;

                        Value* results = vm->currentThread->stackTop - count;
                        for (int i = 0; i < count; i++) {
                            *caller->stackTop = results[i];
                            caller->stackTop++;
                        }

                        vm->currentThread = caller;
                        frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                        ip = frame->ip;
                        break;
                    }

                    if (minFrameCount == 0) {
                        vm->currentThread->stackTop -= count;
                    }
                    return INTERPRET_OK;
                }

                frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_ADJUST_STACK: {
                uint8_t targetDepth = READ_BYTE();
                vm->currentThread->stackTop = frame->slots + targetDepth;
                #ifdef DEBUG_MULTI_RETURN
                printf("OP_ADJUST_STACK: target depth=%d, new stackTop offset=%ld\n",
                       targetDepth, vm->currentThread->stackTop - vm->currentThread->stack);
                #endif
                break;
            }
            case OP_ADD_CONST: {
                Value b = READ_CONSTANT();
                if (!vmHandleOpAddConst(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_ADD:
                if (!vmHandleOpAdd(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_SUBTRACT:
                if (!vmHandleOpSubtract(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_MULTIPLY:
                if (!vmHandleOpMultiply(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_DIVIDE:
                if (!vmHandleOpDivide(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_MODULO:
                if (!vmHandleOpModulo(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_APPEND:
                if (!vmHandleOpAppend(vm, &frame, &ip)) goto runtime_error;
                break;
            case OP_IADD:
                vmHandleOpIAdd(vm);
                break;
            case OP_SUB_CONST: {
                Value b = READ_CONSTANT();
                if (!vmHandleOpSubConst(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_ISUB:
                vmHandleOpISub(vm);
                break;
            case OP_MUL_CONST: {
                Value b = READ_CONSTANT();
                if (!vmHandleOpMulConst(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_IMUL:
                vmHandleOpIMul(vm);
                break;
            case OP_DIV_CONST: {
                Value b = READ_CONSTANT();
                if (!vmHandleOpDivConst(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_IDIV:
                vmHandleOpIDiv(vm);
                break;
            case OP_NEGATE:
                vmHandleOpNegate(vm);
                break;
            case OP_NOT:
                vmHandleOpNot(vm);
                break;
            case OP_LENGTH:
                if (!vmHandleOpLength(vm)) goto runtime_error;
                break;
            case OP_EQUAL: {
                if (!vmHandleOpEqual(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_GREATER: {
                if (!vmHandleOpGreater(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_LESS: {
                if (!vmHandleOpLess(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_HAS: {
                if (!vmHandleOpHas(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_POWER: {
                if (!vmHandleOpPower(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_INT_DIV: {
                if (!vmHandleOpIntDiv(vm, &frame, &ip)) goto runtime_error;
                break;
            }
            case OP_IMOD: {
                vmHandleOpIMod(vm);
                break;
            }
            case OP_FADD: {
                vmHandleOpFAdd(vm);
                break;
            }
            case OP_FSUB: {
                vmHandleOpFSub(vm);
                break;
            }
            case OP_FMUL: {
                vmHandleOpFMul(vm);
                break;
            }
            case OP_FDIV: {
                vmHandleOpFDiv(vm);
                break;
            }
            case OP_FMOD: {
                vmHandleOpFMod(vm);
                break;
            }
            case OP_MOD_CONST: {
                Value b = READ_CONSTANT();
                if (!vmHandleOpModConst(vm, &frame, &ip, b)) goto runtime_error;
                break;
            }
            case OP_GC: {
                collectGarbage(vm);
                break;
            }
            case OP_IMPORT: {
               ObjString* moduleName = READ_STRING();
               InterpretResult importResult = vmHandleOpImport(vm, moduleName, &frame, &ip);
               if (importResult == INTERPRET_RUNTIME_ERROR) goto runtime_error;
               if (importResult == INTERPRET_COMPILE_ERROR) return INTERPRET_COMPILE_ERROR;
                break;
            }
            case OP_IMPORT_STAR: {
                if (!vmHandleOpImportStar(vm)) goto runtime_error;
                break;
            }
        }
        continue;
runtime_error:
        if (handleException(vm, &frame, &ip)) {
            continue;
        }
        return INTERPRET_RUNTIME_ERROR;
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
}

InterpretResult interpret(VM* vm, ObjFunction* function) {
    ObjClosure* closure = newClosure(function);
    push(vm, OBJ_VAL(closure));
    call(vm, closure, 0);
    return vmRun(vm, 0);  // Run until all frames complete
}
