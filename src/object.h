#ifndef OBJECT_H
#define OBJECT_H

#include <stdint.h>
#include "value.h"
#include "chunk.h"

struct VM;
typedef void (*UserdataFinalizer)(void*);
typedef void (*UserdataMarker)(void*);

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
#define HANDLERS_MAX 64
#define GEN_STACK_MAX 96
#define GEN_FRAMES_MAX 24
#define GEN_HANDLERS_MAX 16

// CallFrame moved here because ObjThread needs it
typedef struct {
    struct ObjClosure* closure;
    uint8_t* ip;
    Value* slots;
} CallFrame;

typedef struct ExceptionHandler {
    int frame_count;
    Value* stack_top;
    uint8_t* except_ip;
    uint8_t* finally_ip;
    uint8_t has_except;
    uint8_t has_finally;
    uint8_t in_except;
} ExceptionHandler;

struct Obj {
    ObjType type;
    int is_marked;
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
    CallFrame* frames;
    int frame_capacity;
    int frame_count;
    Value* stack;
    int stack_capacity;
    Value* stack_top;
    struct ObjUpvalue* open_upvalues;
    struct ObjThread* caller;
    uint8_t is_generator;
    uint8_t generator_mode;
    uint32_t generator_index;
    struct ExceptionHandler* handlers;
    int handler_capacity;
    int handler_count;
    struct ObjThread* gc_park_next;
    int gc_park_count;
    int has_exception;
    Value exception;
    Value last_error;
    int pending_set_local_count;
    int pending_set_local_slots[8];
    int pending_set_local_frames[8];
} ObjThread;

typedef int (*NativeFn)(struct VM* vm, int arg_count, Value* args);

typedef enum {
    NATIVE_FAST_NONE = 0,
    NATIVE_FAST_MATH_SIN,
    NATIVE_FAST_MATH_COS,
    NATIVE_FAST_MATH_TAN,
    NATIVE_FAST_MATH_ASIN,
    NATIVE_FAST_MATH_ACOS,
    NATIVE_FAST_MATH_ATAN,
    NATIVE_FAST_MATH_SQRT,
    NATIVE_FAST_MATH_FLOOR,
    NATIVE_FAST_MATH_CEIL,
    NATIVE_FAST_MATH_ABS,
    NATIVE_FAST_MATH_EXP,
    NATIVE_FAST_MATH_LOG,
    NATIVE_FAST_MATH_POW,
    NATIVE_FAST_MATH_FMOD,
    NATIVE_FAST_MATH_DEG,
    NATIVE_FAST_MATH_RAD
} NativeFastKind;

typedef struct {
    struct Obj obj;
    NativeFn function;
    ObjString* name;
    uint8_t is_self;
    uint8_t fast_kind;
} ObjNative;

typedef struct {
    struct Obj obj;
    int arity;
    int upvalue_count;
    Chunk chunk;
    ObjString* name;
    ObjString* doc;
    Value* defaults;
    int defaults_count;
    int is_variadic;
    uint8_t* param_types;
    int param_types_count;
    ObjString** param_names;
    int param_names_count;
    uint8_t is_self;
    uint8_t is_generator;
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
    int upvalue_count;
} ObjClosure;

typedef struct ObjTable ObjTable; // Forward decl

typedef struct {
    struct Obj obj;
    void* data;
    UserdataFinalizer finalize;
    UserdataMarker mark;
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
    uint8_t is_module;
};

#define OBJ_TYPE(value)   (AS_OBJ(value)->type)
#define IS_STRING(value)  is_obj_type(value, OBJ_STRING)
#define IS_TABLE(value)   is_obj_type(value, OBJ_TABLE)
#define IS_FUNCTION(value) is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value)   is_obj_type(value, OBJ_NATIVE)
#define IS_UPVALUE(value)  is_obj_type(value, OBJ_UPVALUE)
#define IS_CLOSURE(value)  is_obj_type(value, OBJ_CLOSURE)
#define IS_THREAD(value)   is_obj_type(value, OBJ_THREAD)
#define IS_USERDATA(value) is_obj_type(value, OBJ_USERDATA)
#define IS_BOUND_METHOD(value) is_obj_type(value, OBJ_BOUND_METHOD)

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

static inline int is_obj_type(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

ObjString* copy_string(const char* chars, int length);
ObjString* take_string(char* chars, int length);
ObjTable* new_table();
ObjFunction* new_function();
ObjNative* new_native(NativeFn function, ObjString* name);
ObjUpvalue* new_upvalue(Value* slot);
ObjClosure* new_closure(ObjFunction* function);
ObjThread* new_thread();
ObjThread* new_thread_with_caps(int stack_cap, int frame_cap, int handler_cap);
ObjUserdata* new_userdata(void* data);
ObjUserdata* new_userdata_with_finalizer(void* data, UserdataFinalizer finalize);
ObjUserdata* new_userdata_with_hooks(void* data, UserdataFinalizer finalize, UserdataMarker mark);
ObjBoundMethod* new_bound_method(Value receiver, struct Obj* method);
void print_object(Value value);


void mark_object(struct Obj* object);
void mark_value(Value value);
void sweep_objects();

#endif
