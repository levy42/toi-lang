#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "object.h"
#include "value.h"

struct Obj* objects = NULL;
size_t bytes_allocated = 0;
size_t next_gc = 1024 * 1024; // 1MB initial threshold

// Simple FNV-1a hash function
static uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static struct Obj* allocate_object(size_t size, ObjType type) {
    struct Obj* object = (struct Obj*)malloc(size);
    object->type = type;
    object->is_marked = 0;
    object->next = objects;
    objects = object;
    
    bytes_allocated += size;
    return object;
}

static ObjString* allocate_string(char* chars, int length, uint32_t hash) {
    bytes_allocated += length + 1; // Track string content too
    ObjString* string = (ObjString*)allocate_object(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;
    return string;
}

ObjString* copy_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    char* heap_chars = (char*)malloc(length + 1);
    memcpy(heap_chars, chars, length);
    heap_chars[length] = '\0';
    return allocate_string(heap_chars, length, hash);
}

ObjString* take_string(char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    return allocate_string(chars, length, hash);
}

ObjTable* new_table() {
    ObjTable* table = (ObjTable*)allocate_object(sizeof(ObjTable), OBJ_TABLE);
    init_table(&table->table);
    table->metatable = NULL;
    table->is_module = 0;
    return table;
}

ObjFunction* new_function() {
    ObjFunction* function = (ObjFunction*)allocate_object(sizeof(ObjFunction), OBJ_FUNCTION);
    function->arity = 0;
    function->upvalue_count = 0;
    function->name = NULL;
    function->defaults = NULL;
    function->defaults_count = 0;
    function->is_variadic = 0;
    function->param_types = NULL;
    function->param_types_count = 0;
    function->param_names = NULL;
    function->param_names_count = 0;
    function->is_self = 0;
    function->is_generator = 0;
    init_chunk(&function->chunk);
    return function;
}

ObjNative* new_native(NativeFn function, ObjString* name) {
    ObjNative* native = (ObjNative*)allocate_object(sizeof(ObjNative), OBJ_NATIVE);
    native->function = function;
    native->name = name;
    native->is_self = 0;
    return native;
}

ObjUpvalue* new_upvalue(Value* slot) {
    ObjUpvalue* upvalue = (ObjUpvalue*)allocate_object(sizeof(ObjUpvalue), OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

ObjClosure* new_closure(ObjFunction* function) {
    // Allocate upvalues array - will be filled in by VM/compiler
    ObjUpvalue** upvalues = (ObjUpvalue**)malloc(sizeof(ObjUpvalue*) * 256);
    for (int i = 0; i < 256; i++) {
        upvalues[i] = NULL;
    }

    ObjClosure* closure = (ObjClosure*)allocate_object(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalue_count = 0;  // Will be set by compiler
    bytes_allocated += sizeof(ObjUpvalue*) * 256;
    return closure;
}

ObjThread* new_thread_with_caps(int stack_cap, int frame_cap, int handler_cap) {
    if (stack_cap < 8) stack_cap = 8;
    if (frame_cap < 4) frame_cap = 4;
    if (handler_cap < 4) handler_cap = 4;

    ObjThread* thread = (ObjThread*)allocate_object(sizeof(ObjThread), OBJ_THREAD);
    thread->stack = (Value*)malloc(sizeof(Value) * (size_t)stack_cap);
    thread->frames = (CallFrame*)malloc(sizeof(CallFrame) * (size_t)frame_cap);
    thread->handlers = (ExceptionHandler*)malloc(sizeof(ExceptionHandler) * (size_t)handler_cap);
    if (thread->stack == NULL || thread->frames == NULL || thread->handlers == NULL) {
        fprintf(stderr, "Out of memory creating thread.\n");
        exit(1);
    }
    thread->stack_capacity = stack_cap;
    thread->frame_capacity = frame_cap;
    thread->handler_capacity = handler_cap;
    thread->stack_top = thread->stack;
    thread->frame_count = 0;
    thread->open_upvalues = NULL;
    thread->caller = NULL;
    thread->is_generator = 0;
    thread->generator_mode = 0;
    thread->generator_index = 0;
    thread->handler_count = 0;
    bytes_allocated += sizeof(Value) * (size_t)stack_cap;
    bytes_allocated += sizeof(CallFrame) * (size_t)frame_cap;
    bytes_allocated += sizeof(ExceptionHandler) * (size_t)handler_cap;
    return thread;
}

ObjThread* new_thread() {
    return new_thread_with_caps(STACK_MAX, FRAMES_MAX, HANDLERS_MAX);
}

ObjUserdata* new_userdata(void* data) {
    return new_userdata_with_finalizer(data, NULL);
}

ObjUserdata* new_userdata_with_finalizer(void* data, UserdataFinalizer finalize) {
    ObjUserdata* userdata = (ObjUserdata*)allocate_object(sizeof(ObjUserdata), OBJ_USERDATA);
    userdata->data = data;
    userdata->finalize = finalize;
    userdata->metatable = NULL;
    return userdata;
}



ObjBoundMethod* new_bound_method(Value receiver, struct Obj* method) {

    ObjBoundMethod* bound = (ObjBoundMethod*)allocate_object(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);

    bound->receiver = receiver;

    bound->method = method;

    return bound;

}

static void print_value_rec(Value value, int depth);

static ObjString* metatable_name(ObjTable* metatable) {
    if (metatable == NULL) return NULL;
    for (int i = 0; i < metatable->table.capacity; i++) {
        Entry* entry = &metatable->table.entries[i];
        if (entry->key == NULL || IS_NIL(entry->value)) continue;
        if (entry->key->length == 6 &&
            memcmp(entry->key->chars, "__name", 6) == 0 &&
            IS_STRING(entry->value)) {
            return AS_STRING(entry->value);
        }
    }
    return NULL;
}

static void print_table(ObjTable* table, int depth) {
    if (depth > 5) {
        printf("...");
        return;
    }

    printf("{");
    int count = 0;
    
    // Array part
    if (table->table.array != NULL) {
        int max_index = -1;
        for (int i = 0; i < table->table.array_capacity; i++) {
            if (!IS_NIL(table->table.array[i])) {
                max_index = i;
            }
        }
        for (int i = 0; i <= max_index; i++) {
            if (count > 0) printf(", ");
            print_value_rec(table->table.array[i], depth + 1);
            count++;
        }
    }

    // Hash part
    for (int i = 0; i < table->table.capacity; i++) {
        Entry* entry = &table->table.entries[i];
        if (entry->key != NULL && !IS_NIL(entry->value)) {
            if (count > 0) printf(", ");
            printf("%s: ", entry->key->chars);
            print_value_rec(entry->value, depth + 1);
            count++;
        }
    }
    printf("}");
}

static void print_value_rec(Value value, int depth) {
    if (IS_TABLE(value)) {
        print_table(AS_TABLE(value), depth);
    } else {
        print_value(value);
    }
}

void print_object(Value value) {

    switch (OBJ_TYPE(value)) {

        case OBJ_STRING:

            printf("\"%s\"", AS_CSTRING(value));

            break;

        case OBJ_TABLE:

            print_table(AS_TABLE(value), 0);

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
            {
                ObjUserdata* userdata = AS_USERDATA(value);
                ObjString* type_name = metatable_name(userdata->metatable);
                if (type_name != NULL) {
                    if (userdata->data != NULL) {
                        printf("<%s %p>", type_name->chars, userdata->data);
                    } else {
                        printf("<%s closed>", type_name->chars);
                    }
                } else if (userdata->data != NULL) {
                    printf("<userdata %p>", userdata->data);
                } else {
                    printf("<userdata closed>");
                }
            }

            break;

        case OBJ_BOUND_METHOD:

            printf("<bound method>");

            break;

    }

}



static void mark_table(Table* table);

void mark_object(struct Obj* object) {
    if (object == NULL) return;
    if (object->is_marked) return;

    object->is_marked = 1;

    if (object->type == OBJ_TABLE) {
        ObjTable* table = (ObjTable*)object;
        mark_table(&table->table);
        if (table->metatable) {
            mark_object((struct Obj*)table->metatable);
        }
    } else if (object->type == OBJ_FUNCTION) {
        ObjFunction* function = (ObjFunction*)object;
        mark_object((struct Obj*)function->name);
        // Mark constants in chunk
        for (int i = 0; i < function->chunk.constants.count; i++) {
            mark_value(function->chunk.constants.values[i]);
        }
        // Mark default parameter values
        for (int i = 0; i < function->defaults_count; i++) {
            mark_value(function->defaults[i]);
        }
        for (int i = 0; i < function->param_names_count; i++) {
            mark_object((struct Obj*)function->param_names[i]);
        }
    } else if (object->type == OBJ_NATIVE) {
        ObjNative* native = (ObjNative*)object;
        if (native->name != NULL) {
            mark_object((struct Obj*)native->name);
        }
    } else if (object->type == OBJ_UPVALUE) {
        ObjUpvalue* upvalue = (ObjUpvalue*)object;
        mark_value(upvalue->closed);
    } else if (object->type == OBJ_CLOSURE) {
        ObjClosure* closure = (ObjClosure*)object;
        mark_object((struct Obj*)closure->function);
        for (int i = 0; i < closure->upvalue_count; i++) {
            mark_object((struct Obj*)closure->upvalues[i]);
        }
    } else if (object->type == OBJ_THREAD) {
        ObjThread* thread = (ObjThread*)object;
        for (Value* slot = thread->stack; slot < thread->stack_top; slot++) {
            mark_value(*slot);
        }
        for (int i = 0; i < thread->frame_count; i++) {
            mark_object((struct Obj*)thread->frames[i].closure);
        }
        for (ObjUpvalue* upvalue = thread->open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
            mark_object((struct Obj*)upvalue);
        }
        if (thread->caller != NULL) {
            mark_object((struct Obj*)thread->caller);
        }
    } else if (object->type == OBJ_USERDATA) {
        ObjUserdata* userdata = (ObjUserdata*)object;
        if (userdata->metatable) {
            mark_object((struct Obj*)userdata->metatable);
        }
    } else if (object->type == OBJ_BOUND_METHOD) {
        ObjBoundMethod* bound = (ObjBoundMethod*)object;
        mark_value(bound->receiver);
        mark_object(bound->method);
    }
}

void mark_value(Value value) {
    if (IS_OBJ(value)) mark_object(AS_OBJ(value));
}

static void mark_table(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            mark_object((struct Obj*)entry->key);
            mark_value(entry->value);
        }
    }
    // Mark array part
    for (int i = 0; i < table->array_capacity; i++) {
        if (!IS_NIL(table->array[i])) {
            mark_value(table->array[i]);
        }
    }
}

void free_object(struct Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            bytes_allocated -= string->length + 1;
            bytes_allocated -= sizeof(ObjString);
            free(string->chars);
            free(string);
            break;
        }
        case OBJ_TABLE: {
            ObjTable* table = (ObjTable*)object;
            // Note: Table entries size is complex to track perfectly here, 
            // but we can estimate or just track the table struct.
            bytes_allocated -= sizeof(ObjTable);
            free_table(&table->table);
            free(table);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            free_chunk(&function->chunk);
            if (function->defaults != NULL) {
                bytes_allocated -= sizeof(Value) * function->defaults_count;
                free(function->defaults);
            }
            if (function->param_types != NULL) {
                bytes_allocated -= sizeof(uint8_t) * function->param_types_count;
                free(function->param_types);
            }
            if (function->param_names != NULL) {
                free(function->param_names);
            }
            bytes_allocated -= sizeof(ObjFunction);
            free(function);
            break;
        }
        case OBJ_NATIVE: {
            bytes_allocated -= sizeof(ObjNative);
            free(object);
            break;
        }
        case OBJ_UPVALUE: {
            bytes_allocated -= sizeof(ObjUpvalue);
            free(object);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            bytes_allocated -= sizeof(ObjUpvalue*) * 256;
            bytes_allocated -= sizeof(ObjClosure);
            free(closure->upvalues);
            free(closure);
            break;
        }
        case OBJ_THREAD: {
            ObjThread* thread = (ObjThread*)object;
            bytes_allocated -= sizeof(ObjThread);
            bytes_allocated -= sizeof(Value) * (size_t)thread->stack_capacity;
            bytes_allocated -= sizeof(CallFrame) * (size_t)thread->frame_capacity;
            bytes_allocated -= sizeof(ExceptionHandler) * (size_t)thread->handler_capacity;
            free(thread->stack);
            free(thread->frames);
            free(thread->handlers);
            free(object);
            break;
        }
        case OBJ_USERDATA: {
            ObjUserdata* userdata = (ObjUserdata*)object;
            if (userdata->data != NULL && userdata->finalize != NULL) {
                userdata->finalize(userdata->data);
                userdata->data = NULL;
            }
            bytes_allocated -= sizeof(ObjUserdata);
            free(object);
            break;
        }
        case OBJ_BOUND_METHOD: {
            bytes_allocated -= sizeof(ObjBoundMethod);
            free(object);
            break;
        }
    }
}


void sweep_objects() {
    struct Obj* previous = NULL;
    struct Obj* object = objects;
    
    while (object != NULL) {
        if (object->is_marked) {
            object->is_marked = 0; // Unmark for next cycle
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
            free_object(unreached);
        }
    }
    
    // Adjust threshold: target 2x live memory
    next_gc = bytes_allocated * 2;
    if (next_gc < 1024 * 1024) next_gc = 1024 * 1024; 
}
