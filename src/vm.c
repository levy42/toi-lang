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

static int valuesEqualSimple(Value a, Value b) {
    if (IS_NUMBER(a) && IS_NUMBER(b)) return AS_NUMBER(a) == AS_NUMBER(b);
    if (IS_BOOL(a) && IS_BOOL(b)) return AS_BOOL(a) == AS_BOOL(b);
    if (IS_NIL(a) && IS_NIL(b)) return 1;
    if (IS_OBJ(a) && IS_OBJ(b)) {
        if (AS_OBJ(a) == AS_OBJ(b)) return 1;
        if (IS_STRING(a) && IS_STRING(b)) {
            ObjString* sa = AS_STRING(a);
            ObjString* sb = AS_STRING(b);
            return (sa->hash == sb->hash &&
                    sa->length == sb->length &&
                    memcmp(sa->chars, sb->chars, sa->length) == 0);
        }
    }
    return 0;
}

static int isSelfCallable(Value v) {
    if (IS_CLOSURE(v)) return AS_CLOSURE(v)->function->isSelf;
    if (IS_NATIVE(v)) return AS_NATIVE_OBJ(v)->isSelf;
    return 0;
}

static Value maybeBindSelf(Value receiver, Value result) {
    if (IS_BOUND_METHOD(result)) return result;
    if (IS_TABLE(receiver) && AS_TABLE(receiver)->isModule) {
        return result;
    }
    if (isSelfCallable(result)) {
        return OBJ_VAL(newBoundMethod(receiver, AS_OBJ(result)));
    }
    return result;
}

static int stringContains(ObjString* haystack, ObjString* needle) {
    if (needle->length == 0) return 1;
    if (needle->length > haystack->length) return 0;
    int last = haystack->length - needle->length;
    for (int i = 0; i <= last; i++) {
        if (haystack->chars[i] == needle->chars[0] &&
            memcmp(haystack->chars + i, needle->chars, needle->length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int pushPendingSetLocal(VM* vm, int frameIndex, int slot) {
    if (vm->pendingSetLocalCount >= 8) {
        vmRuntimeError(vm, "Pending set-local stack overflow.");
        return 0;
    }
    int idx = vm->pendingSetLocalCount++;
    vm->pendingSetLocalFrames[idx] = frameIndex;
    vm->pendingSetLocalSlots[idx] = slot;
    return 1;
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

static int isFalsey(Value v) {
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
    return 0; // other objects are truthy
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

static void concatenate(VM* vm);
static int isFalsey(Value v);
static Value getMetamethodCached(VM* vm, Value val, ObjString* name);

static int callValue(VM* vm, Value callee, int argCount, CallFrame** frame, uint8_t** ip) {
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

static void discardHandlersForFrameReturn(ObjThread* thread) {
    int currentFrameCount = thread->frameCount;
    while (thread->handlerCount > 0 &&
           thread->handlers[thread->handlerCount - 1].frameCount >= currentFrameCount) {
        thread->handlerCount--;
    }
}

// Returns 1 if handled, 0 if no metamethod, -1 on runtime error.
static int handleIndexMetamethod(VM* vm, ObjTable* t, Value tableVal, Value key, Value* result,
                                 CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value idxVal = NIL_VAL;
    if (!tableGet(&t->metatable->table, vm->mm_index, &idxVal)) return 0;

    if (IS_CLOSURE(idxVal) || IS_NATIVE(idxVal)) {
        push(vm, idxVal);
        push(vm, tableVal);
        push(vm, key);
        if (!callValue(vm, idxVal, 2, frame, ip)) return -1;
        *result = pop(vm);
        return 1;
    }
    if (IS_TABLE(idxVal)) {
        *result = NIL_VAL;
        if (IS_STRING(key)) {
            tableGet(&AS_TABLE(idxVal)->table, AS_STRING(key), result);
        } else if (IS_NUMBER(key)) {
            ObjString* nKey = numberKeyString(AS_NUMBER(key));
            tableGet(&AS_TABLE(idxVal)->table, nKey, result);
        }
        return 1;
    }
    return 0;
}

// Returns 1 if handled, 0 if no metamethod, -1 on runtime error.
static int handleNewIndexMetamethod(VM* vm, ObjTable* t, Value tableVal, Value key, Value value,
                                    CallFrame** frame, uint8_t** ip) {
    if (!t->metatable) return 0;
    Value ni = NIL_VAL;
    if (!tableGet(&t->metatable->table, vm->mm_newindex, &ni)) return 0;

    if (IS_CLOSURE(ni) || IS_NATIVE(ni)) {
        push(vm, ni);
        push(vm, tableVal);
        push(vm, key);
        push(vm, value);
        if (!callValue(vm, ni, 3, frame, ip)) return -1;
        return 1;
    }
    if (IS_TABLE(ni)) {
        if (IS_STRING(key)) {
            tableSet(&AS_TABLE(ni)->table, AS_STRING(key), value);
        } else if (IS_NUMBER(key)) {
            ObjString* nKey = numberKeyString(AS_NUMBER(key));
            tableSet(&AS_TABLE(ni)->table, nKey, value);
        }
        return 1;
    }
    return 0;
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

        // Pop extra args and put them in the table with numeric keys
        for (int i = 0; i < extraArgs; i++) {
            Value arg = vm->currentThread->stackTop[-extraArgs + i];
            // Create numeric key (1-indexed like Lua)
            ObjString* key = numberKeyString(i + 1);
            tableSet(&varargs->table, key, arg);
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

static void markRoots(VM* vm) {
    if (vm->currentThread != NULL) {
        markObject((struct Obj*)vm->currentThread);
    }
    if (vm->mm_index != NULL) markObject((struct Obj*)vm->mm_index);
    if (vm->mm_newindex != NULL) markObject((struct Obj*)vm->mm_newindex);
    if (vm->mm_str != NULL) markObject((struct Obj*)vm->mm_str);
    if (vm->mm_call != NULL) markObject((struct Obj*)vm->mm_call);
    if (vm->mm_new != NULL) markObject((struct Obj*)vm->mm_new);
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

static void maybeCollectGarbage(VM* vm) {
    if (vm->disableGC) return;  // Skip GC if disabled

    extern size_t bytesAllocated;
    extern size_t nextGC;
    if (bytesAllocated > nextGC) {
        collectGarbage(vm);
    }
}

static void concatenate(VM* vm) {
    ObjString* b = AS_STRING(pop(vm));
    ObjString* a = AS_STRING(pop(vm));

    int length = a->length + b->length;
    char* chars = (char*)malloc(length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = takeString(chars, length);
    push(vm, OBJ_VAL(result));
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

static bool findProperty(VM* vm, ObjTable* table, ObjString* name, Value* result) {
    if (tableGet(&table->table, name, result)) return true;

    ObjTable* current = table;
    int depth = 0;
    ObjString* idxName = vm->mm_index;

    while (current->metatable && depth < 10) {
        Value idxVal = NIL_VAL;
        if (tableGet(&current->metatable->table, idxName, &idxVal)) {
            if (IS_TABLE(idxVal)) {
                current = AS_TABLE(idxVal);
                if (tableGet(&current->table, name, result)) {
                    return true;
                }
                depth++;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return false;
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
                uint8_t depth = READ_BYTE();
                uint8_t flags = READ_BYTE();
                uint16_t exJump = READ_SHORT();
                uint16_t finJump = READ_SHORT();
                ObjThread* thread = vm->currentThread;
                if (thread->handlerCount < 64) {
                    ExceptionHandler* handler = &thread->handlers[thread->handlerCount++];
                    handler->frameCount = thread->frameCount;
                    handler->stackTop = frame->slots + depth;
                    handler->hasExcept = (flags & 0x1) != 0;
                    handler->hasFinally = (flags & 0x2) != 0;
                    handler->inExcept = 0;
                    handler->except_ip = handler->hasExcept ? ip + exJump : NULL;
                    handler->finally_ip = handler->hasFinally ? ip + finJump : NULL;
                } else {
                    vmRuntimeError(vm, "Too many nested try blocks.");
                    goto runtime_error;
                }
                break;
            }
            case OP_END_TRY: {
                ObjThread* thread = vm->currentThread;
                if (thread->handlerCount > 0) thread->handlerCount--;
                break;
            }
            case OP_END_FINALLY: {
                if (vm->hasException) goto runtime_error;
                break;
            }
            case OP_THROW: {
                Value ex = pop(vm);
                vm->hasException = 1;
                vm->exception = ex;
                goto runtime_error;
            }
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(vm, constant);
                maybeCollectGarbage(vm);
                break;
            }
            case OP_NIL: push(vm, NIL_VAL); break;
            case OP_TRUE: push(vm, BOOL_VAL(1)); break; 
            case OP_FALSE: push(vm, BOOL_VAL(0)); break; 
            case OP_POP: pop(vm); break;
            case OP_GET_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value;
                if (!tableGet(&vm->globals, name, &value)) {
                    vmRuntimeError(vm, "Undefined variable '%s'.", name->chars);
                    goto runtime_error;
                }
                push(vm, value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = READ_STRING();
                tableSet(&vm->globals, name, peek(vm, 0));
                pop(vm);
                maybeCollectGarbage(vm);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = READ_STRING();
                tableSet(&vm->globals, name, peek(vm, 0));
                maybeCollectGarbage(vm);
                break;
            }
            case OP_DELETE_GLOBAL: {
                ObjString* name = READ_STRING();
                if (!tableDelete(&vm->globals, name)) {
                    vmRuntimeError(vm, "Undefined variable '%s'.", name->chars);
                    goto runtime_error;
                }
                break;
            }
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value val = frame->slots[slot];
                push(vm, val);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(vm, 0);
                break;
            }
            case OP_ADD_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_STRING(a) && IS_STRING(b)) {
                    push(vm, a);
                    push(vm, b);
                    concatenate(vm);
                    frame->slots[slot] = peek(vm, 0);
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else if (IS_TABLE(a) && IS_TABLE(b)) {
                    ObjTable* tb = AS_TABLE(b);
                    ObjTable* ta = AS_TABLE(a);
                    ObjTable* result = newTable();
                    push(vm, OBJ_VAL(result)); // GC protection

                    int lenA = 0, lenB = 0;
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&ta->table, i, &val) || IS_NIL(val)) {
                            lenA = i - 1;
                            break;
                        }
                    }
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&tb->table, i, &val) || IS_NIL(val)) {
                            lenB = i - 1;
                            break;
                        }
                    }

                    for (int i = 1; i <= lenA; i++) {
                        Value val;
                        tableGetArray(&ta->table, i, &val);
                        tableSetArray(&result->table, i, val);
                    }

                    for (int i = 1; i <= lenB; i++) {
                        Value val;
                        tableGetArray(&tb->table, i, &val);
                        tableSetArray(&result->table, lenA + i, val);
                    }

                    tableAddAll(&ta->table, &result->table);
                    tableAddAll(&tb->table, &result->table);

                    frame->slots[slot] = peek(vm, 0);
                } else {
                    Value method = getMetamethod(vm, a, "__add");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
                    if (IS_NIL(method)) {
                        vmRuntimeError(vm, "Operands must be two numbers or two strings.");
                        goto runtime_error;
                    }
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_SUB_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__sub");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__sub");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_MUL_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__mul");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mul");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_DIV_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__div");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__div");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_MOD_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    double ad = AS_NUMBER(a);
                    double bd = AS_NUMBER(b);
                    int64_t ia, ib;
                    Value out;
                    if (to_int64(ad, &ia) && to_int64(bd, &ib) && ib != 0) {
                        out = NUMBER_VAL((double)(ia % ib));
                    } else {
                        out = NUMBER_VAL(fmod(ad, bd));
                    }
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__mod");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mod");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_INC_LOCAL: {
                uint8_t slot = READ_BYTE();
                uint8_t constant = READ_BYTE();
                Value v = frame->slots[slot];
                Value c = frame->closure->function->chunk.constants.values[constant];
                if (!IS_NUMBER(v) || !IS_NUMBER(c)) {
                    vmRuntimeError(vm, "Operands must be two numbers.");
                    goto runtime_error;
                }
                double result = AS_NUMBER(v) + AS_NUMBER(c);
                Value out = NUMBER_VAL(result);
                frame->slots[slot] = out;
                push(vm, out);
                break;
            }
            case OP_SUB_LOCAL_CONST: {
                uint8_t slot = READ_BYTE();
                uint8_t constant = READ_BYTE();
                Value a = frame->slots[slot];
                Value b = frame->closure->function->chunk.constants.values[constant];
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__sub");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__sub");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_MUL_LOCAL_CONST: {
                uint8_t slot = READ_BYTE();
                uint8_t constant = READ_BYTE();
                Value a = frame->slots[slot];
                Value b = frame->closure->function->chunk.constants.values[constant];
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__mul");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mul");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_DIV_LOCAL_CONST: {
                uint8_t slot = READ_BYTE();
                uint8_t constant = READ_BYTE();
                Value a = frame->slots[slot];
                Value b = frame->closure->function->chunk.constants.values[constant];
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    Value out = NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b));
                    frame->slots[slot] = out;
                    push(vm, out);
                } else {
                    Value method = getMetamethod(vm, a, "__div");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__div");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_MOD_LOCAL_CONST: {
                uint8_t slot = READ_BYTE();
                uint8_t constant = READ_BYTE();
                Value a = frame->slots[slot];
                Value b = frame->closure->function->chunk.constants.values[constant];
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    double ad = AS_NUMBER(a);
                    double bd = AS_NUMBER(b);
                    int64_t ia, ib;
                    if (to_int64(ad, &ia) && to_int64(bd, &ib) && ib != 0) {
                        Value out = NUMBER_VAL((double)(ia % ib));
                        frame->slots[slot] = out;
                        push(vm, out);
                    } else {
                        Value out = NUMBER_VAL(fmod(ad, bd));
                        frame->slots[slot] = out;
                        push(vm, out);
                    }
                } else {
                    Value method = getMetamethod(vm, a, "__mod");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mod");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    if (!pushPendingSetLocal(vm, vm->currentThread->frameCount - 1, slot)) {
                        goto runtime_error;
                    }
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(vm, *frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(vm, 0);
                break;
            }
            case OP_CLOSE_UPVALUE: {
                closeUpvalues(vm, vm->currentThread->stackTop - 1);
                pop(vm);
                break;
            }
            case OP_NEW_TABLE: {
                push(vm, OBJ_VAL(newTable()));
                maybeCollectGarbage(vm);
                break;
            }
            case OP_SET_METATABLE: {
                Value table = peek(vm, 0);
                Value metatable = peek(vm, 1);
                if (!IS_TABLE(table) || (!IS_TABLE(metatable) && !IS_NIL(metatable))) {
                    vmRuntimeError(vm, "Invalid arguments to setmetatable.");
                    goto runtime_error;
                }
                AS_TABLE(table)->metatable = IS_NIL(metatable) ? NULL : AS_TABLE(metatable);
                
                bool constructorCalled = false;
                if (!IS_NIL(metatable)) {
                    Value initMethod = NIL_VAL;
                    ObjString* newStr = vm->mm_new;
                    
                    bool found = findProperty(vm, AS_TABLE(table), newStr, &initMethod);
                    if (!found) {
                        if (tableGet(&AS_TABLE(metatable)->table, newStr, &initMethod)) {
                             found = true;
                        }
                    }

                    if (found) {
                        if (IS_CLOSURE(initMethod) || IS_NATIVE(initMethod)) {
                             pop(vm); // Pop table
                             pop(vm); // Pop metatable
                             
                             push(vm, initMethod);
                             push(vm, metatable); // Self (the metatable/class)
                             push(vm, table); // Argument (the instance)
                             
                             constructorCalled = true;
                             int argCount = 2;
                             
                             if (!callValue(vm, initMethod, argCount, &frame, &ip)) {
                                 goto runtime_error;
                             }
                        }
                    }
                }

                if (!constructorCalled) {
                    pop(vm); // pop table
                    pop(vm); // pop metatable
                    push(vm, table); // push table back
                }
                break;
            }
            case OP_DUP: push(vm, peek(vm, 0)); break;
            case OP_GET_TABLE: {
                Value key = pop(vm);
                Value table = pop(vm);
                Value result = NIL_VAL;

                if (IS_TABLE(table)) {
                    ObjTable* t = AS_TABLE(table);
                    if (IS_STRING(key)) {
#ifdef DEBUG_TABLE_KEYS
                        fprintf(stderr, "GET key type: string\n");
#endif
                        if (tableGet(&t->table, AS_STRING(key), &result)) {
                            result = maybeBindSelf(table, result);
                        } else if (t->metatable) {
                            int handled = handleIndexMetamethod(vm, t, table, key, &result, &frame, &ip);
                            if (handled < 0) goto runtime_error;
                            if (handled == 0) result = NIL_VAL;
                            result = maybeBindSelf(table, result);
                        }
                    } else if (IS_NUMBER(key)) {
                        // ... numeric key logic ...
                        double numKey = AS_NUMBER(key);
                        int idx = (int)numKey;
                        if (numKey == (double)idx) {
                            if (idx < 0) {
                                int len = 0;
                                for (int i = 1; ; i++) {
                                    Value val;
                                    if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                                        len = i - 1;
                                        break;
                                    }
                                }
                                idx = len + idx + 1;
                            }
                            if (tableGetArray(&t->table, idx, &result)) {
                                result = maybeBindSelf(table, result);
                                push(vm, result);
                                maybeCollectGarbage(vm);
                                break;
                            }
                        }
                        
                        ObjString* nKey = numberKeyString(numKey);
                        if (tableGet(&t->table, nKey, &result)) {
                             result = maybeBindSelf(table, result);
                             push(vm, result);
                        } else if (t->metatable) {
                             int handled = handleIndexMetamethod(vm, t, table, key, &result, &frame, &ip);
                             if (handled < 0) goto runtime_error;
                             if (handled == 0) result = NIL_VAL;
                             result = maybeBindSelf(table, result);
                             push(vm, result);
                        } else {
                            push(vm, NIL_VAL);
                        }
                        maybeCollectGarbage(vm);
                        break;
                    }
                } else if (IS_USERDATA(table)) {
                    ObjUserdata* udata = AS_USERDATA(table);
                    if (udata->metatable) {
                        Value idx = NIL_VAL;
                        ObjString* idxName = vm->mm_index;
                        if (tableGet(&udata->metatable->table, idxName, &idx)) {
                            if (IS_CLOSURE(idx) || IS_NATIVE(idx)) {
                                push(vm, idx);
                                push(vm, table);
                                push(vm, key);
                                if (!callValue(vm, idx, 2, &frame, &ip)) goto runtime_error;
                                result = pop(vm);
                            } else if (IS_TABLE(idx)) {
                                if (IS_STRING(key)) {
                                    tableGet(&AS_TABLE(idx)->table, AS_STRING(key), &result);
                                }
                            }
                            result = maybeBindSelf(table, result);
                        }
                    }
                } else if (IS_STRING(table)) {
                    if (IS_STRING(key)) {
                        Value stringModule = NIL_VAL;
                        ObjString* stringName = copyString("string", 6);
                        push(vm, OBJ_VAL(stringName)); // Root string name
                        if ((!tableGet(&vm->globals, stringName, &stringModule) || !IS_TABLE(stringModule)) &&
                            loadNativeModule(vm, "string")) {
                            stringModule = peek(vm, 0);
                            pop(vm); // loaded module
                        }
                        if (IS_TABLE(stringModule)) {
                            if (tableGet(&AS_TABLE(stringModule)->table, AS_STRING(key), &result)) {
                                result = maybeBindSelf(table, result);
                            } else {
                                result = NIL_VAL;
                            }
                        }
                        pop(vm); // stringName
                    }
                    else if (IS_NUMBER(key)) {
                        double numKey = AS_NUMBER(key);
                        int idx = (int)numKey;
                        if (numKey == (double)idx) {
                            ObjString* s = AS_STRING(table);
                            if (idx < 0) idx = s->length + idx + 1;
                            if (idx >= 1 && idx <= s->length) {
                                char buf[2];
                                buf[0] = s->chars[idx - 1];
                                buf[1] = '\0';
                                push(vm, OBJ_VAL(copyString(buf, 1)));
                                maybeCollectGarbage(vm);
                                break;
                            }
                        }
                        push(vm, NIL_VAL);
                    }
                } else {
                    if (IS_OBJ(table)) {
                         printf("DEBUG: Attempt to index non-table object type: %d\n", OBJ_TYPE(table));
                    } else {
                         printf("DEBUG: Attempt to index non-table value type: %d\n", table.type);
                    }
                    vmRuntimeError(vm, "Attempt to index non-table.");
                    goto runtime_error;
                }
                push(vm, result);
                maybeCollectGarbage(vm);
                break;
            }
            case OP_SET_TABLE: {
                Value value = pop(vm);
                Value key = pop(vm);
                Value table = pop(vm);
                
                if (!IS_TABLE(table)) {
                    vmRuntimeError(vm, "Attempt to index non-table.");
                    goto runtime_error;
                }
                ObjTable* t = AS_TABLE(table);
                
                if (IS_STRING(key)) {
#ifdef DEBUG_TABLE_KEYS
                    fprintf(stderr, "SET key type: string\n");
#endif
                    // Try direct set; if key missing and __newindex present, respect it
                    Value dummy;
                    if (tableGet(&t->table, AS_STRING(key), &dummy)) {
                        tableSet(&t->table, AS_STRING(key), value);
                    } else if (t->metatable) {
                        int handled = handleNewIndexMetamethod(vm, t, table, key, value, &frame, &ip);
                        if (handled < 0) goto runtime_error;
                        if (handled == 0) tableSet(&t->table, AS_STRING(key), value);
                    } else {
                        tableSet(&t->table, AS_STRING(key), value);
                    }
                } else if (IS_NUMBER(key)) {
#ifdef DEBUG_TABLE_KEYS
                    fprintf(stderr, "SET key type: number\n");
#endif
                    // Try array optimization first
                    double numKey = AS_NUMBER(key);
                    int idx = (int)numKey;
                    int isArray = 0;
                    if (numKey == (double)idx) {
                        if (idx < 0) {
                            int len = 0;
                            for (int i = 1; ; i++) {
                                Value val;
                                if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                                    len = i - 1;
                                    break;
                                }
                            }
                            idx = len + idx + 1;
                        }
                        // Check if key exists in hash part first to avoid shadowing?
                        // Or just set in array if it fits. 
                        // If it's in array range, we prefer array.
                        // But if we have it in hash, we should probably move it?
                        // For simplicity: tableSetArray returns false if it refuses (too sparse).
                        // If it accepts, it sets it.
                        if (tableSetArray(&t->table, idx, value)) {
                            isArray = 1;
                        }
                    }

                    if (!isArray) {
                        ObjString* nKey = numberKeyString(numKey);
                        Value dummy;
                        // ... rest of logic for metamethods ...
                        if (tableGet(&t->table, nKey, &dummy)) {
                            tableSet(&t->table, nKey, value);
                        }
                        else if (t->metatable) {
                            int handled = handleNewIndexMetamethod(vm, t, table, key, value, &frame, &ip);
                            if (handled < 0) goto runtime_error;
                            if (handled == 0) tableSet(&t->table, nKey, value);
                        }
                        else {
                            tableSet(&t->table, nKey, value);
                        }
                    }
                }
                push(vm, value);
                maybeCollectGarbage(vm);
                break;
            }
            case OP_DELETE_TABLE: {
                Value key = pop(vm);
                Value table = pop(vm);

                if (!IS_TABLE(table)) {
                    vmRuntimeError(vm, "Attempt to index non-table.");
                    goto runtime_error;
                }
                ObjTable* t = AS_TABLE(table);

                if (IS_STRING(key)) {
                    if (!tableDelete(&t->table, AS_STRING(key))) {
                        vmRuntimeError(vm, "Key not found.");
                        goto runtime_error;
                    }
                } else if (IS_NUMBER(key)) {
                    double numKey = AS_NUMBER(key);
                    int idx = (int)numKey;
                    if (numKey == (double)idx) {
                        if (idx < 0) {
                            int len = 0;
                            for (int i = 1; ; i++) {
                                Value val;
                                if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                                    len = i - 1;
                                    break;
                                }
                            }
                            idx = len + idx + 1;
                        }
                        if (idx >= 1 && idx <= t->table.arrayCapacity) {
                            if (!IS_NIL(t->table.array[idx - 1])) {
                                tableSetArray(&t->table, idx, NIL_VAL);
                                break;
                            }
                        }
                    }

                    ObjString* nKey = numberKeyString(numKey);
                    if (!tableDelete(&t->table, nKey)) {
                        vmRuntimeError(vm, "Key not found.");
                        goto runtime_error;
                    }
                } else {
                    vmRuntimeError(vm, "Invalid key type for deletion.");
                    goto runtime_error;
                }
                break;
            }
            case OP_PRINT: {
                Value v = pop(vm);

                // For tables, check for __str metamethod and use tostring
                if (IS_TABLE(v)) {
                    ObjTable* table = AS_TABLE(v);
                    if (table->metatable != NULL) {
                        Value strMethod;
                        ObjString* strKey = vm->mm_str;
                        if (tableGet(&table->metatable->table, strKey, &strMethod) && (IS_CLOSURE(strMethod) || IS_NATIVE(strMethod))) {
                            // Call tostring which will invoke __str
                            int savedFrameCount = vm->currentThread->frameCount;

                            push(vm, strMethod);
                            push(vm, v);

                            frame->ip = ip;
                            if (callValue(vm, strMethod, 1, &frame, &ip)) {
                                InterpretResult result = vmRun(vm, savedFrameCount);
                                if (result == INTERPRET_OK) {
                                    // Result is on stack, print it
                                    Value strResult = pop(vm);
                                    printValue(strResult);
                                    printf("\n");
                                    break;
                                }
                                return result;
                            }
                            goto runtime_error;
                        }
                    }
                }

                // Default printing for non-tables or tables without __str
                printValue(v);
                printf("\n");
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                Value value = peek(vm, 0);
                if (isFalsey(value)) {
                    ip += offset;
                }
                break;
            }
            case OP_JUMP_IF_TRUE: {
                uint16_t offset = READ_SHORT();
                Value value = peek(vm, 0);
                if (!isFalsey(value)) {
                    ip += offset;
                }
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!invokeCallWithArgCount(vm, argCount, &frame, &ip)) {
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
                if (!invokeCallWithArgCount(vm, argCount, &frame, &ip)) {
                    goto runtime_error;
                }
                break;
            }
            case OP_ITER_PREP: {
                Value val = peek(vm, 0);
                if (IS_TABLE(val) || IS_STRING(val)) {
                    Value nextFn = NIL_VAL;
                    ObjString* name = copyString("next", 4);
                    if (!tableGet(&vm->globals, name, &nextFn)) {
                        vmRuntimeError(vm, "Global 'next' not found for implicit iteration.");
                        goto runtime_error;
                    }
                    if (!IS_NATIVE(nextFn) && !IS_CLOSURE(nextFn)) {
                        vmRuntimeError(vm, "Global 'next' is not a function.");
                        goto runtime_error;
                    }

                    pop(vm); // remove table
                    push(vm, nextFn);
                    push(vm, val); // table argument
                    push(vm, NIL_VAL);
                }
                break;
            }
            case OP_ITER_PREP_IPAIRS: {
                Value val = peek(vm, 0);
                if (IS_TABLE(val)) {
                    Value inextFn = NIL_VAL;
                    ObjString* name = copyString("inext", 5);
                    if (!tableGet(&vm->globals, name, &inextFn)) {
                        vmRuntimeError(vm, "Global 'inext' not found for implicit iteration.");
                        goto runtime_error;
                    }
                    if (!IS_NATIVE(inextFn) && !IS_CLOSURE(inextFn)) {
                        vmRuntimeError(vm, "Global 'inext' is not a function.");
                        goto runtime_error;
                    }

                    pop(vm); // remove table
                    push(vm, inextFn);
                    push(vm, val); // table argument
                    push(vm, NUMBER_VAL(0));
                }
                break;
            }
            case OP_RANGE: {
                Value end = pop(vm);
                Value start = pop(vm);
                Value rangeFn = NIL_VAL;
                ObjString* name = copyString("range", 5);
                if (!tableGet(&vm->globals, name, &rangeFn)) {
                    vmRuntimeError(vm, "range not found.");
                    goto runtime_error;
                }

                push(vm, rangeFn);
                push(vm, start);
                push(vm, end);

                int argCount = 2;
                if (IS_NATIVE(rangeFn) || IS_CLOSURE(rangeFn)) {
                    if (!callValue(vm, rangeFn, argCount, &frame, &ip)) goto runtime_error;
                } else {
                    vmRuntimeError(vm, "Can only call functions.");
                    goto runtime_error;
                }
                break;
            }
            case OP_FOR_PREP: {
                uint8_t varSlot = READ_BYTE();
                uint8_t endSlot = READ_BYTE();
                uint16_t offset = READ_SHORT();
                Value v = frame->slots[varSlot];
                Value end = frame->slots[endSlot];
                if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
                    vmRuntimeError(vm, "for range requires numeric bounds.");
                    goto runtime_error;
                }
                if (AS_NUMBER(v) > AS_NUMBER(end)) {
                    ip += offset;
                }
                break;
            }
            case OP_FOR_LOOP: {
                uint8_t varSlot = READ_BYTE();
                uint8_t endSlot = READ_BYTE();
                uint16_t offset = READ_SHORT();
                Value v = frame->slots[varSlot];
                Value end = frame->slots[endSlot];
                if (!IS_NUMBER(v) || !IS_NUMBER(end)) {
                    vmRuntimeError(vm, "for range requires numeric bounds.");
                    goto runtime_error;
                }
                double next = AS_NUMBER(v) + 1.0;
                frame->slots[varSlot] = NUMBER_VAL(next);
                if (next <= AS_NUMBER(end)) {
                    ip -= offset;
                }
                break;
            }
            case OP_SLICE: {
                Value step = pop(vm);
                Value end = pop(vm);
                Value start = pop(vm);
                Value obj = pop(vm);

                if (IS_NIL(step)) {
                    step = NUMBER_VAL(1);
                }
                if (!IS_NUMBER(step)) {
                    vmRuntimeError(vm, "slice step must be a number.");
                    goto runtime_error;
                }

                double stepNum = AS_NUMBER(step);
                if (stepNum == 0) {
                    vmRuntimeError(vm, "slice step cannot be 0.");
                    goto runtime_error;
                }

                if (IS_NIL(start) || IS_NIL(end)) {
                    int len = 0;
                    if (IS_TABLE(obj)) {
                        ObjTable* t = AS_TABLE(obj);
                        for (int i = 1; ; i++) {
                            Value val;
                            if (!tableGetArray(&t->table, i, &val) || IS_NIL(val)) {
                                len = i - 1;
                                break;
                            }
                        }
                    } else if (IS_STRING(obj)) {
                        len = AS_STRING(obj)->length;
                    } else {
                        vmRuntimeError(vm, "slice expects table or string.");
                        goto runtime_error;
                    }
                    if (IS_NIL(start)) {
                        start = NUMBER_VAL(stepNum < 0 ? (double)len : 1.0);
                    }
                    if (IS_NIL(end)) {
                        end = NUMBER_VAL(stepNum < 0 ? 1.0 : (double)len);
                    }
                }
                if (!IS_NUMBER(start) || !IS_NUMBER(end)) {
                    vmRuntimeError(vm, "slice start/end must be numbers.");
                    goto runtime_error;
                }

                Value sliceFn = NIL_VAL;
                ObjString* name = copyString("slice", 5);
                if (!tableGet(&vm->globals, name, &sliceFn)) {
                    vmRuntimeError(vm, "slice not found.");
                    goto runtime_error;
                }

                push(vm, sliceFn);
                push(vm, obj);
                push(vm, start);
                push(vm, end);
                push(vm, step);

                int argCount = 4;
                if (IS_NATIVE(sliceFn) || IS_CLOSURE(sliceFn)) {
                    if (!callValue(vm, sliceFn, argCount, &frame, &ip)) goto runtime_error;
                } else {
                    vmRuntimeError(vm, "Can only call functions.");
                    goto runtime_error;
                }
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
                Value a = peek(vm, 0);
                if (IS_STRING(a) && IS_STRING(b)) {
                    push(vm, b);
                    concatenate(vm);
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    pop(vm);
                    push(vm, NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
                } else if (IS_TABLE(a) && IS_TABLE(b)) {
                    ObjTable* tb = AS_TABLE(b);
                    ObjTable* ta = AS_TABLE(pop(vm));
                    ObjTable* result = newTable();
                    push(vm, OBJ_VAL(result)); // GC protection

                    int lenA = 0, lenB = 0;
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&ta->table, i, &val) || IS_NIL(val)) {
                            lenA = i - 1;
                            break;
                        }
                    }
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&tb->table, i, &val) || IS_NIL(val)) {
                            lenB = i - 1;
                            break;
                        }
                    }

                    for (int i = 1; i <= lenA; i++) {
                        Value val;
                        tableGetArray(&ta->table, i, &val);
                        tableSetArray(&result->table, i, val);
                    }

                    for (int i = 1; i <= lenB; i++) {
                        Value val;
                        tableGetArray(&tb->table, i, &val);
                        tableSetArray(&result->table, lenA + i, val);
                    }

                    tableAddAll(&ta->table, &result->table);
                    tableAddAll(&tb->table, &result->table);
                } else {
                    Value aPop = pop(vm);
                    Value method = getMetamethod(vm, aPop, "__add");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
                    if (IS_NIL(method)) {
                        vmRuntimeError(vm, "Operands must be two numbers or two strings.");
                        goto runtime_error;
                    }
                    push(vm, method);
                    push(vm, aPop);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_ADD: {
                if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
                    concatenate(vm);
                } else if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a + b));
                } else if (IS_TABLE(peek(vm, 0)) && IS_TABLE(peek(vm, 1))) {
                    // Table addition: combine arrays, merge keys (b overrides a)
                    ObjTable* tb = AS_TABLE(pop(vm));
                    ObjTable* ta = AS_TABLE(pop(vm));
                    ObjTable* result = newTable();
                    push(vm, OBJ_VAL(result)); // GC protection

                    // Find array lengths
                    int lenA = 0, lenB = 0;
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&ta->table, i, &val) || IS_NIL(val)) {
                            lenA = i - 1;
                            break;
                        }
                    }
                    for (int i = 1; ; i++) {
                        Value val;
                        if (!tableGetArray(&tb->table, i, &val) || IS_NIL(val)) {
                            lenB = i - 1;
                            break;
                        }
                    }

                    // Copy array part from a
                    for (int i = 1; i <= lenA; i++) {
                        Value val;
                        tableGetArray(&ta->table, i, &val);
                        tableSetArray(&result->table, i, val);
                    }

                    // Append array part from b
                    for (int i = 1; i <= lenB; i++) {
                        Value val;
                        tableGetArray(&tb->table, i, &val);
                        tableSetArray(&result->table, lenA + i, val);
                    }

                    // Copy key-value entries from a, then b (b overrides)
                    tableAddAll(&ta->table, &result->table);
                    tableAddAll(&tb->table, &result->table);
                } else {
                    Value b = pop(vm);
                    Value a = pop(vm);
                    Value method = getMetamethod(vm, a, "__add");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__add");
                    if (IS_NIL(method)) {
                        vmRuntimeError(vm, "Operands must be two numbers or two strings.");
                        goto runtime_error;
                    }
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_IADD: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a + b));
                break;
            }
            case OP_SUB_CONST: {
                Value b = READ_CONSTANT();
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b)));
                } else {
                    Value method = getMetamethod(vm, a, "__sub");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__sub");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_SUBTRACT: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a - b));
                } else {
                    Value b = pop(vm);
                    Value a = pop(vm);
                    Value method = getMetamethod(vm, a, "__sub");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__sub");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_ISUB: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a - b));
                break;
            }
            case OP_MUL_CONST: {
                Value b = READ_CONSTANT();
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b)));
                } else {
                    Value method = getMetamethod(vm, a, "__mul");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mul");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_MULTIPLY: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a * b));
                } else {
                    Value b = pop(vm);
                    Value a = pop(vm);
                    Value method = getMetamethod(vm, a, "__mul");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mul");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_IMUL: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a * b));
                break;
            }
            case OP_DIV_CONST: {
                Value b = READ_CONSTANT();
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, NUMBER_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
                } else {
                    Value method = getMetamethod(vm, a, "__div");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__div");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_DIVIDE: {
                if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm));
                    double a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a / b));
                } else {
                    Value b = pop(vm);
                    Value a = pop(vm);
                    Value method = getMetamethod(vm, a, "__div");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__div");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                }
                break;
            }
            case OP_IDIV: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a / b));
                break;
            }
            case OP_NEGATE:
                push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
                break;
            case OP_NOT: {
                Value v = pop(vm);
                push(vm, BOOL_VAL(isFalsey(v)));
                break;
            }
            case OP_LENGTH: {
                Value val = pop(vm);
                if (IS_STRING(val)) {
                    push(vm, NUMBER_VAL(AS_STRING(val)->length));
                }
                else if (IS_TABLE(val)) {
                    ObjTable* t = AS_TABLE(val);
                    // Total element count (array part + hash part).
                    int count = t->table.count;
                    for (int i = 0; i < t->table.arrayCapacity; i++) {
                        if (!IS_NIL(t->table.array[i])) count++;
                    }
                    push(vm, NUMBER_VAL(count));
                }
                else {
                    vmRuntimeError(vm, "Length operator (#) requires string or table.");
                    goto runtime_error;
                }
                break;
            }
            case OP_EQUAL: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (valuesEqualSimple(a, b)) {
                    push(vm, BOOL_VAL(1));
                } else if (IS_OBJ(a) && IS_OBJ(b)) {
                    if (AS_OBJ(a) == AS_OBJ(b)) {
                        push(vm, BOOL_VAL(1));
                    }
                    else if (IS_STRING(a) && IS_STRING(b)) {
                        // Compare strings by content, not just pointer
                        ObjString* sa = AS_STRING(a);
                        ObjString* sb = AS_STRING(b);
                        int equal = (sa->hash == sb->hash &&
                                     sa->length == sb->length &&
                                     memcmp(sa->chars, sb->chars, sa->length) == 0);
                        push(vm, BOOL_VAL(equal));
                    }
                    else {
                        Value method = getMetamethod(vm, a, "__eq");
                        if (IS_NIL(method)) method = getMetamethod(vm, b, "__eq");
                        if (!IS_NIL(method)) {
                            push(vm, method); push(vm, a); push(vm, b);
                            frame->ip = ip;
                            if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                            frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                        }
                        else {
                            push(vm, BOOL_VAL(0));
                        }
                    }
                }
                else if (IS_NIL(a) && IS_NIL(b)) {
                    push(vm, BOOL_VAL(1));
                }
                else {
                    push(vm, BOOL_VAL(0));
                }
                break;
            }
            case OP_GREATER: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b)));
                }
                else {
                    Value method = getMetamethod(vm, a, "__lt");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__lt");
                    if (!IS_NIL(method)) {
                        // a > b <=> b < a. Swap args.
                        push(vm, method); push(vm, b); push(vm, a);
                        frame->ip = ip;
                        if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                        frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                    }
                    else {
                        push(vm, BOOL_VAL(0)); 
                    }
                }
                break;
            }
            case OP_LESS: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b)));
                }
                else {
                    Value method = getMetamethod(vm, a, "__lt");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__lt");
                    if (!IS_NIL(method)) {
                        push(vm, method); push(vm, a); push(vm, b);
                        frame->ip = ip;
                        if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                        frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                    }
                    else {
                        push(vm, BOOL_VAL(0));
                    }
                }
                break;
            }
            case OP_HAS: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value method = getMetamethod(vm, a, "__has");
                if (IS_NIL(method)) method = getMetamethod(vm, b, "__has");
                if (!IS_NIL(method)) {
                    push(vm, method);
                    push(vm, a);
                    push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                    ip = frame->ip;
                    break;
                }
                if (IS_STRING(a)) {
                    if (!IS_STRING(b)) {
                        vmRuntimeError(vm, "Right operand of 'has' must be a string.");
                        goto runtime_error;
                    }
                    push(vm, BOOL_VAL(stringContains(AS_STRING(a), AS_STRING(b))));
                } else if (IS_TABLE(a)) {
                    ObjTable* t = AS_TABLE(a);
                    int found = 0;
                    int max = t->table.arrayMax;
                    if (max > t->table.arrayCapacity) max = t->table.arrayCapacity;
                    for (int i = 0; i < max; i++) {
                        Value v = t->table.array[i];
                        if (!IS_NIL(v) && valuesEqualSimple(v, b)) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        for (int i = 0; i < t->table.capacity; i++) {
                            Entry* entry = &t->table.entries[i];
                            if (entry->key != NULL && valuesEqualSimple(entry->value, b)) {
                                found = 1;
                                break;
                            }
                        }
                    }
                    push(vm, BOOL_VAL(found));
                } else {
                    vmRuntimeError(vm, "Left operand of 'has' must be a string or table.");
                    goto runtime_error;
                }
                break;
            }
            case OP_POWER: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, NUMBER_VAL(pow(AS_NUMBER(a), AS_NUMBER(b))));
                }
                else {
                    Value method = getMetamethod(vm, a, "__pow");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__pow");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method); push(vm, a); push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                }
                break;
            }
            case OP_INT_DIV: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    push(vm, NUMBER_VAL(floor(AS_NUMBER(a) / AS_NUMBER(b))));
                }
                else {
                    Value method = getMetamethod(vm, a, "__int_div");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__int_div");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method); push(vm, a); push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                }
                break;
            }
            case OP_MODULO: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    double ad = AS_NUMBER(a);
                    double bd = AS_NUMBER(b);
                    int64_t ia, ib;
                    if (to_int64(ad, &ia) && to_int64(bd, &ib) && ib != 0) {
                        push(vm, NUMBER_VAL((double)(ia % ib)));
                    } else {
                        push(vm, NUMBER_VAL(fmod(ad, bd)));
                    }
                }
                else {
                    Value method = getMetamethod(vm, a, "__mod");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mod");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method); push(vm, a); push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                }
                break;
            }
            case OP_IMOD: {
                double bd = AS_NUMBER(pop(vm));
                double ad = AS_NUMBER(pop(vm));
                int64_t ia, ib;
                if (to_int64(ad, &ia) && to_int64(bd, &ib) && ib != 0) {
                    push(vm, NUMBER_VAL((double)(ia % ib)));
                } else {
                    push(vm, NUMBER_VAL(fmod(ad, bd)));
                }
                break;
            }
            case OP_FADD: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a + b));
                break;
            }
            case OP_FSUB: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a - b));
                break;
            }
            case OP_FMUL: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a * b));
                break;
            }
            case OP_FDIV: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(a / b));
                break;
            }
            case OP_FMOD: {
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                push(vm, NUMBER_VAL(fmod(a, b)));
                break;
            }
            case OP_MOD_CONST: {
                Value b = READ_CONSTANT();
                Value a = pop(vm);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    double ad = AS_NUMBER(a);
                    double bd = AS_NUMBER(b);
                    int64_t ia, ib;
                    if (to_int64(ad, &ia) && to_int64(bd, &ib) && ib != 0) {
                        push(vm, NUMBER_VAL((double)(ia % ib)));
                    } else {
                        push(vm, NUMBER_VAL(fmod(ad, bd)));
                    }
                }
                else {
                    Value method = getMetamethod(vm, a, "__mod");
                    if (IS_NIL(method)) method = getMetamethod(vm, b, "__mod");
                    if (IS_NIL(method)) goto runtime_error;
                    push(vm, method); push(vm, a); push(vm, b);
                    frame->ip = ip;
                    if (!call(vm, AS_CLOSURE(method), 2)) goto runtime_error;
                    frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1]; ip = frame->ip;
                }
                break;
            }
            case OP_GC: {
                collectGarbage(vm);
                break;
            }
            case OP_IMPORT: {
               // Get module name from constant pool
               ObjString* moduleName = READ_STRING();

               // First, try to load as a native module
               if (loadNativeModule(vm, moduleName->chars)) {
                   // Native module loaded successfully, it's on the stack
                   break;
               }

               // Convert dots to slashes for directory paths
               // e.g., "my_module.sub_module" -> "my_module/sub_module"
               char modulePath[256];
               int j = 0;
               for (int i = 0; i < moduleName->length && j < 250; i++) {
                   if (moduleName->chars[i] == '.') {
                       modulePath[j++] = '/';
                   } else {
                       modulePath[j++] = moduleName->chars[i];
                   }
               }
               modulePath[j] = '\0';

               // Try script module file and package init variants:
               //   import record      -> record.pua, record/__.pua, lib/record.pua, lib/record/__.pua
               //   import foo.bar     -> foo/bar.pua, foo/bar/__.pua, lib/foo/bar.pua, lib/foo/bar/__.pua
               char filename[512];
               FILE* file = NULL;
               const char* candidates[4] = {
                   "%s.pua",
                   "%s/__.pua",
                   "lib/%s.pua",
                   "lib/%s/__.pua"
               };

               for (int ci = 0; ci < 4 && file == NULL; ci++) {
                   snprintf(filename, sizeof(filename), candidates[ci], modulePath);
                   file = fopen(filename, "rb");
               }

               if (file == NULL) {
                    printf("Could not open module '%s' (tried '%s.pua', '%s/__.pua', "
                           "'lib/%s.pua', and 'lib/%s/__.pua').\n",
                           moduleName->chars, modulePath, modulePath, modulePath, modulePath);
                    goto runtime_error;
               }

                fseek(file, 0L, SEEK_END);
                size_t fileSize = ftell(file);
                rewind(file);

                char* buffer = (char*)malloc(fileSize + 1);
                if (buffer == NULL) {
                    fclose(file);
                    printf("Not enough memory to read module '%s'.\n", moduleName->chars);
                    goto runtime_error;
                }

                size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
                buffer[bytesRead] = '\0';
                fclose(file);

                // Compile the module
                ObjFunction* moduleFunction = compile(buffer);
                free(buffer);

                if (moduleFunction == NULL) {
                    printf("Failed to compile module '%s'.\n", moduleName->chars);
                    return INTERPRET_COMPILE_ERROR;
                }

                // Execute the module - call it like a normal function
                ObjClosure* moduleClosure = newClosure(moduleFunction);
                push(vm, OBJ_VAL(moduleClosure));

                // Save current IP so we can resume after the module returns
                frame->ip = ip;

                if (!call(vm, moduleClosure, 0)) {
                    goto runtime_error;
                }

                // Update frame and ip to execute the module
                frame = &vm->currentThread->frames[vm->currentThread->frameCount - 1];
                ip = frame->ip;

                break;
            }
            case OP_IMPORT_STAR: {
                Value module = pop(vm);
                if (!IS_TABLE(module)) {
                    vmRuntimeError(vm, "from ... import * expects module table export.");
                    goto runtime_error;
                }

                ObjTable* t = AS_TABLE(module);
                for (int i = 0; i < t->table.capacity; i++) {
                    Entry* entry = &t->table.entries[i];
                    if (entry->key != NULL && !IS_NIL(entry->value)) {
                        tableSet(&vm->globals, entry->key, entry->value);
                    }
                }
                maybeCollectGarbage(vm);
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
