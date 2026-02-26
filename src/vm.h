#ifndef VM_H
#define VM_H

#include "object.h"
#include "value.h"
#include "table.h"

typedef struct VM {
   ObjThread* current_thread;
   ObjThread* gc_parked_threads;
   Table globals;
   Table modules;  // Cache of loaded native modules
   int use_thread_tls;
   int cli_argc;
   char** cli_argv;
    int disable_gc;
    int is_repl;
    ObjString* mm_index;
    ObjString* mm_newindex;
    ObjString* mm_str;
    ObjString* mm_call;
    ObjString* mm_new;
    ObjString* mm_append;
    ObjString* mm_next;
    ObjString* mm_slice;
    ObjString* str_module_name;
    ObjString* str_upper_name;
    ObjString* str_lower_name;
    ObjString* slice_name;
    Value str_upper_fn;
    Value str_lower_fn;
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
InterpretResult vm_run_until_thread(VM* vm, int min_frame_count, ObjThread* stop_thread);
Value get_metamethod(VM* vm, Value val, const char* name);
void vm_request_interrupt(void);
ObjThread* vm_current_thread(VM* vm);
void vm_set_current_thread(VM* vm, ObjThread* thread);
void vm_enable_thread_tls(VM* vm);

#endif
