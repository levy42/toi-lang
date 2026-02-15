#ifndef OBJECT_H
#define OBJECT_H

#include <stdint.h>
#include "value.h"
#include "chunk.h"

struct VM;
typedef void (*UserdataFinalizer)(void*);

typedef enum {
    OBJ_STRING,
    OBJ_TABLE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_UPVALUE,
    OBJ_CLOSURE,
    OBJ_THREAD,
    OBJ_USERDATA,
    OBJ_BOUND_METHOD,
} ObjType;

// VM constants moved here because ObjThread needs them

#define STACK_MAX 256
#define FRAMES_MAX 64

// CallFrame moved here because ObjThread needs it
typedef struct {
    struct ObjClosure* closure;
    uint8_t* ip;
    Value* slots;
} CallFrame;

typedef struct ExceptionHandler {
    int frameCount;
    Value* stackTop;
    uint8_t* except_ip;
    uint8_t* finally_ip;
    uint8_t hasExcept;
    uint8_t hasFinally;
    uint8_t inExcept;
} ExceptionHandler;

struct Obj {
    ObjType type;
    int isMarked;
    struct Obj* next;
};

typedef struct ObjString {
    struct Obj obj;
    int length;
    char* chars;
    uint32_t hash;
} ObjString;

typedef struct ObjThread {
    struct Obj obj;
    struct VM* vm;
    CallFrame frames[FRAMES_MAX];
    int frameCount;
    Value stack[STACK_MAX];
    Value* stackTop;
    struct ObjUpvalue* openUpvalues;
    struct ObjThread* caller;
    struct ExceptionHandler* handlers;
    int handlerCount;
} ObjThread;

typedef int (*NativeFn)(struct VM* vm, int argCount, Value* args);

typedef struct {
    struct Obj obj;
    NativeFn function;
    ObjString* name;
    uint8_t isSelf;
} ObjNative;

typedef struct {
    struct Obj obj;
    int arity;
    int upvalueCount;
    Chunk chunk;
    ObjString* name;
    Value* defaults;
    int defaultsCount;
    int isVariadic;
    uint8_t* paramTypes;
    int paramTypesCount;
    ObjString** paramNames;
    int paramNamesCount;
    uint8_t isSelf;
} ObjFunction;

typedef struct ObjUpvalue {
    struct Obj obj;
    Value* location;      // Points to stack slot or closed value
    Value closed;         // Closed-over value when stack slot goes away
    struct ObjUpvalue* next;  // Linked list of open upvalues
} ObjUpvalue;

typedef struct ObjClosure {
    struct Obj obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;
    int upvalueCount;
} ObjClosure;

typedef struct ObjTable ObjTable; // Forward decl

typedef struct {
    struct Obj obj;
    void* data;
    UserdataFinalizer finalize;
    ObjTable* metatable;
} ObjUserdata;

typedef struct {
    struct Obj obj;
    Value receiver;
    struct Obj* method; // ObjClosure or ObjNative
} ObjBoundMethod;

#include "table.h"


struct ObjTable {
    struct Obj obj;
    Table table;
    struct ObjTable* metatable;
    uint8_t isModule;
};

#define OBJ_TYPE(value)   (AS_OBJ(value)->type)
#define IS_STRING(value)  isObjType(value, OBJ_STRING)
#define IS_TABLE(value)   isObjType(value, OBJ_TABLE)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define IS_NATIVE(value)   isObjType(value, OBJ_NATIVE)
#define IS_UPVALUE(value)  isObjType(value, OBJ_UPVALUE)
#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define IS_THREAD(value)   isObjType(value, OBJ_THREAD)
#define IS_USERDATA(value) isObjType(value, OBJ_USERDATA)
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)

#define AS_STRING(value)  ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString*)AS_OBJ(value))->chars)
#define AS_TABLE(value)   ((ObjTable*)AS_OBJ(value))
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE(value)   (((ObjNative*)AS_OBJ(value))->function)
#define AS_NATIVE_OBJ(value) ((ObjNative*)AS_OBJ(value))
#define AS_UPVALUE(value)  ((ObjUpvalue*)AS_OBJ(value))
#define AS_CLOSURE(value)  ((ObjClosure*)AS_OBJ(value))
#define AS_THREAD(value)   ((ObjThread*)AS_OBJ(value))
#define AS_USERDATA(value) ((ObjUserdata*)AS_OBJ(value))
#define AS_BOUND_METHOD(value) ((ObjBoundMethod*)AS_OBJ(value))

static inline int isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

ObjString* copyString(const char* chars, int length);
ObjString* takeString(char* chars, int length);
ObjTable* newTable();
ObjFunction* newFunction();
ObjNative* newNative(NativeFn function, ObjString* name);
ObjUpvalue* newUpvalue(Value* slot);
ObjClosure* newClosure(ObjFunction* function);
ObjThread* newThread();
ObjUserdata* newUserdata(void* data);
ObjUserdata* newUserdataWithFinalizer(void* data, UserdataFinalizer finalize);
ObjBoundMethod* newBoundMethod(Value receiver, struct Obj* method);
void printObject(Value value);


void markObject(struct Obj* object);
void markValue(Value value);
void sweepObjects();

#endif
