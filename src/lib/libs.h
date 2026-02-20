#ifndef LIBS_H
#define LIBS_H

#include "../vm.h"

// Structure for native function registration
typedef struct {
    const char* name;
    NativeFn function;
} NativeReg;

// Registers all standard libraries into the VM
 
 // Module loader function type - returns 1 on success, 0 on failure
 // The module table is pushed onto the stack
 typedef int (*ModuleLoader)(VM* vm);
 
 // Structure for native module registration
 typedef struct {
     const char* name;       // Module name (e.g., "math", "io")
     ModuleLoader loader;    // Function to load the module
 } ModuleReg;
 
 // Load a native module by name. Returns 1 if found and loaded, 0 otherwise.
 // The module table is pushed onto the stack if successful.
 int load_native_module(VM* vm, const char* name);
 
 // Check if a module name is a known native module
 int is_native_module(const char* name);
 
void register_libs(VM* vm);

// Helper to register a module with a list of native functions
void register_module(VM* vm, const char* name, const NativeReg* funcs);

// Exposed Core Function
int core_tostring(VM* vm, int arg_count, Value* args);

// --- Macros for Native Functions ---

// Return helpers (convention: return 1)
#define RETURN_NIL \
    do { push(vm, NIL_VAL); return 1; } while(0)

#define RETURN_TRUE \
    do { push(vm, BOOL_VAL(1)); return 1; } while(0)

#define RETURN_FALSE \
    do { push(vm, BOOL_VAL(0)); return 1; } while(0)

#define RETURN_BOOL(val) \
    do { push(vm, BOOL_VAL(val)); return 1; } while(0)

#define RETURN_NUMBER(val) \
    do { push(vm, NUMBER_VAL(val)); return 1; } while(0)

#define RETURN_OBJ(val) \
    do { push(vm, OBJ_VAL(val)); return 1; } while(0)

#define RETURN_VAL(val) \
    do { push(vm, val); return 1; } while(0)

#define RETURN_STRING(s, len) \
    do { push(vm, OBJ_VAL(copy_string(s, len))); return 1; } while(0)

// Argument checking helpers
#define ASSERT_ARGC_GE(n) \
    if (arg_count < (n)) { vm_runtime_error(vm, "Expected at least %d arguments but got %d.", (n), arg_count); return 0; }

#define ASSERT_ARGC_EQ(n) \
    if (arg_count != (n)) { vm_runtime_error(vm, "Expected %d arguments but got %d.", (n), arg_count); return 0; }

#define ASSERT_TYPE(index, check_macro, type_name) \
    if ((index) >= arg_count || !check_macro(args[index])) { vm_runtime_error(vm, "Argument %d must be a %s.", (index) + 1, type_name); return 0; }

#define ASSERT_NUMBER(index) ASSERT_TYPE(index, IS_NUMBER, "number")
#define ASSERT_STRING(index) ASSERT_TYPE(index, IS_STRING, "string")
#define ASSERT_TABLE(index)  ASSERT_TYPE(index, IS_TABLE, "table")
#define ASSERT_USERDATA(index) ASSERT_TYPE(index, IS_USERDATA, "userdata")
#define ASSERT_THREAD(index) ASSERT_TYPE(index, IS_THREAD, "thread")

// Accessors (unsafe, assumes ASSERT passed)
#define GET_NUMBER(index) AS_NUMBER(args[index])
#define GET_STRING(index) AS_STRING(args[index])
#define GET_CSTRING(index) AS_CSTRING(args[index])
#define GET_TABLE(index) AS_TABLE(args[index])
#define GET_USERDATA(index) AS_USERDATA(args[index])
#define GET_THREAD(index) AS_THREAD(args[index])

#endif
