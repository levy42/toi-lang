#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "object.h"
#include "value.h"

struct Obj* objects = NULL;
size_t bytesAllocated = 0;
size_t nextGC = 1024 * 1024; // 1MB initial threshold

// Simple FNV-1a hash function
static uint32_t hashString(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static struct Obj* allocateObject(size_t size, ObjType type) {
    struct Obj* object = (struct Obj*)malloc(size);
    object->type = type;
    object->isMarked = 0;
    object->next = objects;
    objects = object;
    
    bytesAllocated += size;
    return object;
}

static ObjString* allocateString(char* chars, int length, uint32_t hash) {
    bytesAllocated += length + 1; // Track string content too
    ObjString* string = (ObjString*)allocateObject(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;
    return string;
}

ObjString* copyString(const char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    char* heapChars = (char*)malloc(length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocateString(heapChars, length, hash);
}

ObjString* takeString(char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    return allocateString(chars, length, hash);
}

ObjTable* newTable() {
    ObjTable* table = (ObjTable*)allocateObject(sizeof(ObjTable), OBJ_TABLE);
    initTable(&table->table);
    table->metatable = NULL;
    table->isModule = 0;
    return table;
}

ObjFunction* newFunction() {
    ObjFunction* function = (ObjFunction*)allocateObject(sizeof(ObjFunction), OBJ_FUNCTION);
    function->arity = 0;
    function->upvalueCount = 0;
    function->name = NULL;
    function->defaults = NULL;
    function->defaultsCount = 0;
    function->isVariadic = 0;
    function->paramTypes = NULL;
    function->paramTypesCount = 0;
    function->isSelf = 0;
    initChunk(&function->chunk);
    return function;
}

ObjNative* newNative(NativeFn function, ObjString* name) {
    ObjNative* native = (ObjNative*)allocateObject(sizeof(ObjNative), OBJ_NATIVE);
    native->function = function;
    native->name = name;
    native->isSelf = 0;
    return native;
}

ObjUpvalue* newUpvalue(Value* slot) {
    ObjUpvalue* upvalue = (ObjUpvalue*)allocateObject(sizeof(ObjUpvalue), OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

ObjClosure* newClosure(ObjFunction* function) {
    // Allocate upvalues array - will be filled in by VM/compiler
    ObjUpvalue** upvalues = (ObjUpvalue**)malloc(sizeof(ObjUpvalue*) * 256);
    for (int i = 0; i < 256; i++) {
        upvalues[i] = NULL;
    }

    ObjClosure* closure = (ObjClosure*)allocateObject(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalueCount = 0;  // Will be set by compiler
    bytesAllocated += sizeof(ObjUpvalue*) * 256;
    return closure;
}

ObjThread* newThread() {
    ObjThread* thread = (ObjThread*)allocateObject(sizeof(ObjThread), OBJ_THREAD);
    thread->stackTop = thread->stack;
    thread->frameCount = 0;
    thread->openUpvalues = NULL;
    thread->caller = NULL;
    thread->handlers = (ExceptionHandler*)malloc(sizeof(ExceptionHandler) * 64);
    thread->handlerCount = 0;
    bytesAllocated += sizeof(ExceptionHandler) * 64;
    return thread;
}

ObjUserdata* newUserdata(void* data) {

    ObjUserdata* userdata = (ObjUserdata*)allocateObject(sizeof(ObjUserdata), OBJ_USERDATA);

    userdata->data = data;

    userdata->metatable = NULL;

    return userdata;

}



ObjBoundMethod* newBoundMethod(Value receiver, struct Obj* method) {

    ObjBoundMethod* bound = (ObjBoundMethod*)allocateObject(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);

    bound->receiver = receiver;

    bound->method = method;

    return bound;

}

static void printValueRec(Value value, int depth);

static void printTable(ObjTable* table, int depth) {
    if (depth > 5) {
        printf("...");
        return;
    }

    printf("{");
    int count = 0;
    
    // Array part
    if (table->table.array != NULL) {
        int maxIndex = -1;
        for (int i = 0; i < table->table.arrayCapacity; i++) {
            if (!IS_NIL(table->table.array[i])) {
                maxIndex = i;
            }
        }
        for (int i = 0; i <= maxIndex; i++) {
            if (count > 0) printf(", ");
            printValueRec(table->table.array[i], depth + 1);
            count++;
        }
    }

    // Hash part
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            if (count > 0) printf(", ");
            printf("%s: ", entry->key->chars);
            printValueRec(entry->value, depth + 1);
            count++;
        }
    }
    printf("}");
}

static void printValueRec(Value value, int depth) {
    if (IS_TABLE(value)) {
        printTable(AS_TABLE(value), depth);
    } else {
        printValue(value);
    }
}

void printObject(Value value) {

    switch (OBJ_TYPE(value)) {

        case OBJ_STRING:

            printf("\"%s\"", AS_CSTRING(value));

            break;

        case OBJ_TABLE:

            printTable(AS_TABLE(value), 0);

            break;

        case OBJ_FUNCTION:

            if (AS_FUNCTION(value)->name != NULL) {

                printf("<fn %s>", AS_FUNCTION(value)->name->chars);

            } else {

                printf("<script>");

            }

            break;

        case OBJ_NATIVE: {
            ObjNative* native = (ObjNative*)AS_OBJ(value);
            if (native->name != NULL) {
                printf("<native fn %s>", native->name->chars);
            } else {
                printf("<native fn>");
            }
            break;
        }

        case OBJ_CLOSURE:

            if (AS_CLOSURE(value)->function->name != NULL) {

                printf("<fn %s>", AS_CLOSURE(value)->function->name->chars);

            } else {

                printf("<script>");

            }

            break;

        case OBJ_UPVALUE:

            printf("upvalue");

            break;

        case OBJ_THREAD:

            printf("thread");

            break;

        case OBJ_USERDATA:

            printf("userdata");

            break;

        case OBJ_BOUND_METHOD:

            printf("<bound method>");

            break;

    }

}



static void markTable(Table* table);

void markObject(struct Obj* object) {
    if (object == NULL) return;
    if (object->isMarked) return;

    object->isMarked = 1;

    if (object->type == OBJ_TABLE) {
        ObjTable* table = (ObjTable*)object;
        markTable(&table->table);
        if (table->metatable) {
            markObject((struct Obj*)table->metatable);
        }
    } else if (object->type == OBJ_FUNCTION) {
        ObjFunction* function = (ObjFunction*)object;
        markObject((struct Obj*)function->name);
        // Mark constants in chunk
        for (int i = 0; i < function->chunk.constants.count; i++) {
            markValue(function->chunk.constants.values[i]);
        }
        // Mark default parameter values
        for (int i = 0; i < function->defaultsCount; i++) {
            markValue(function->defaults[i]);
        }
    } else if (object->type == OBJ_NATIVE) {
        ObjNative* native = (ObjNative*)object;
        if (native->name != NULL) {
            markObject((struct Obj*)native->name);
        }
    } else if (object->type == OBJ_UPVALUE) {
        ObjUpvalue* upvalue = (ObjUpvalue*)object;
        markValue(upvalue->closed);
    } else if (object->type == OBJ_CLOSURE) {
        ObjClosure* closure = (ObjClosure*)object;
        markObject((struct Obj*)closure->function);
        for (int i = 0; i < closure->upvalueCount; i++) {
            markObject((struct Obj*)closure->upvalues[i]);
        }
    } else if (object->type == OBJ_THREAD) {
        ObjThread* thread = (ObjThread*)object;
        for (Value* slot = thread->stack; slot < thread->stackTop; slot++) {
            markValue(*slot);
        }
        for (int i = 0; i < thread->frameCount; i++) {
            markObject((struct Obj*)thread->frames[i].closure);
        }
        for (ObjUpvalue* upvalue = thread->openUpvalues; upvalue != NULL; upvalue = upvalue->next) {
            markObject((struct Obj*)upvalue);
        }
        if (thread->caller != NULL) {
            markObject((struct Obj*)thread->caller);
        }
    } else if (object->type == OBJ_USERDATA) {
        ObjUserdata* userdata = (ObjUserdata*)object;
        if (userdata->metatable) {
            markObject((struct Obj*)userdata->metatable);
        }
    } else if (object->type == OBJ_BOUND_METHOD) {
        ObjBoundMethod* bound = (ObjBoundMethod*)object;
        markValue(bound->receiver);
        markObject(bound->method);
    }
}

void markValue(Value value) {
    if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

static void markTable(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            markObject((struct Obj*)entry->key);
            markValue(entry->value);
        }
    }
    // Mark array part
    for (int i = 0; i < table->arrayCapacity; i++) {
        if (!IS_NIL(table->array[i])) {
            markValue(table->array[i]);
        }
    }
}

void freeObject(struct Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            bytesAllocated -= string->length + 1;
            bytesAllocated -= sizeof(ObjString);
            free(string->chars);
            free(string);
            break;
        }
        case OBJ_TABLE: {
            ObjTable* table = (ObjTable*)object;
            // Note: Table entries size is complex to track perfectly here, 
            // but we can estimate or just track the table struct.
            bytesAllocated -= sizeof(ObjTable);
            freeTable(&table->table);
            free(table);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            freeChunk(&function->chunk);
            if (function->defaults != NULL) {
                bytesAllocated -= sizeof(Value) * function->defaultsCount;
                free(function->defaults);
            }
            if (function->paramTypes != NULL) {
                bytesAllocated -= sizeof(uint8_t) * function->paramTypesCount;
                free(function->paramTypes);
            }
            bytesAllocated -= sizeof(ObjFunction);
            free(function);
            break;
        }
        case OBJ_NATIVE: {
            bytesAllocated -= sizeof(ObjNative);
            free(object);
            break;
        }
        case OBJ_UPVALUE: {
            bytesAllocated -= sizeof(ObjUpvalue);
            free(object);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            bytesAllocated -= sizeof(ObjUpvalue*) * 256;
            bytesAllocated -= sizeof(ObjClosure);
            free(closure->upvalues);
            free(closure);
            break;
        }
        case OBJ_THREAD: {
            bytesAllocated -= sizeof(ObjThread);
            bytesAllocated -= sizeof(ExceptionHandler) * 64;
            ObjThread* thread = (ObjThread*)object;
            free(thread->handlers);
            free(object);
            break;
        }
        case OBJ_USERDATA: {
            bytesAllocated -= sizeof(ObjUserdata);
            free(object);
            break;
        }
        case OBJ_BOUND_METHOD: {
            bytesAllocated -= sizeof(ObjBoundMethod);
            free(object);
            break;
        }
    }
}


void sweepObjects() {
    struct Obj* previous = NULL;
    struct Obj* object = objects;
    
    while (object != NULL) {
        if (object->isMarked) {
            object->isMarked = 0; // Unmark for next cycle
            previous = object;
            object = object->next;
        } else {
            struct Obj* unreached = object;
            object = object->next;
            if (previous != NULL) {
                previous->next = object;
            } else {
                objects = object;
            }
            freeObject(unreached);
        }
    }
    
    // Adjust threshold: target 2x live memory
    nextGC = bytesAllocated * 2;
    if (nextGC < 1024 * 1024) nextGC = 1024 * 1024; 
}
