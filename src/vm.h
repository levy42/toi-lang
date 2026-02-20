#ifndef VM_H
#define VM_H

#include "object.h"
#include "value.h"
#include "table.h"

typedef struct VM {
   ObjThread* current_thread;
   Table globals;
   Table modules;  // Cache of loaded native modules
   int cli_argc;
   char** cli_argv;
   int disable_gc;
    int is_repl;
    int pending_set_local_count;
    int pending_set_local_slots[8];
    int pending_set_local_frames[8];
    int has_exception;
    Value exception;
    ObjString* mm_index;
    ObjString* mm_newindex;
    ObjString* mm_str;
    ObjString* mm_call;
    ObjString* mm_new;
    ObjString* mm_append;
    ObjString* mm_next;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

// Exposed VM Core Functions
void init_vm(VM* vm);
void free_vm(VM* vm);
InterpretResult interpret(VM* vm, ObjFunction* function);
void push(VM* vm, Value value);
Value pop(VM* vm);
Value peek(VM* vm, int distance);
void define_native(VM* vm, const char* name, NativeFn function);
void vm_runtime_error(VM* vm, const char* format, ...);
ObjString* number_key_string(double num);
int call(VM* vm, ObjClosure* closure, int arg_count);
int call_value(VM* vm, Value callee, int arg_count, CallFrame** frame, uint8_t** ip);
void maybe_collect_garbage(VM* vm);
InterpretResult vm_run(VM* vm, int min_frame_count);
Value get_metamethod(VM* vm, Value val, const char* name);
void vm_request_interrupt(void);

#endif
