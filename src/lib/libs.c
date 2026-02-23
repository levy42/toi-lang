#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// Forward declarations for individual library registration functions
void register_core(VM* vm);
void register_math(VM* vm);
#ifndef TOI_WASM
void register_time(VM* vm);
#endif
void register_io(VM* vm);
#ifndef TOI_WASM
void register_os(VM* vm);
void register_stat(VM* vm);
void register_dir(VM* vm);
void register_signal(VM* vm);
void register_mmap(VM* vm);
void register_poll(VM* vm);
#endif
void register_coroutine(VM* vm);
void register_string(VM* vm);
void register_table(VM* vm);
#ifndef TOI_WASM
void register_socket(VM* vm);
void register_thread(VM* vm);
#endif
void register_json(VM* vm);
void register_template(VM* vm);
void register_http(VM* vm);
void register_inspect(VM* vm);
#ifndef TOI_WASM
void register_regex(VM* vm);
void register_fnmatch(VM* vm);
void register_glob(VM* vm);
#endif
void register_binary(VM* vm);
void register_struct(VM* vm);
void register_btree(VM* vm);
#ifndef TOI_WASM
void register_uuid(VM* vm);
#endif
#ifndef TOI_WASM
void register_gzip(VM* vm);
#endif

static int load_registered_module(VM* vm, const char* name, void (*register_fn)(VM*)) {
    register_fn(vm);

    ObjString* module_name = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(module_name));

    Value module = NIL_VAL;
    int found = table_get(&vm->globals, module_name, &module);
    pop(vm);
    if (!found || !IS_TABLE(module)) {
        return 0;
    }

    push(vm, module);
    return 1;
}

// Module loader wrappers - each loads its module and pushes it onto the stack
static int load_math(VM* vm) { return load_registered_module(vm, "math", register_math); }
#ifndef TOI_WASM
static int load_time(VM* vm) { return load_registered_module(vm, "time", register_time); }
#endif
static int load_io(VM* vm) { return load_registered_module(vm, "io", register_io); }
#ifndef TOI_WASM
static int load_os(VM* vm) { return load_registered_module(vm, "os", register_os); }
static int load_stat(VM* vm) { return load_registered_module(vm, "stat", register_stat); }
static int load_dir(VM* vm) { return load_registered_module(vm, "dir", register_dir); }
static int load_signal(VM* vm) { return load_registered_module(vm, "signal", register_signal); }
static int load_mmap(VM* vm) { return load_registered_module(vm, "mmap", register_mmap); }
static int load_poll(VM* vm) { return load_registered_module(vm, "poll", register_poll); }
#endif
static int load_coroutine(VM* vm) { return load_registered_module(vm, "coroutine", register_coroutine); }
static int load_string(VM* vm) { return load_registered_module(vm, "string", register_string); }
static int load_table(VM* vm) { return load_registered_module(vm, "table", register_table); }
#ifndef TOI_WASM
static int load_socket(VM* vm) { return load_registered_module(vm, "socket", register_socket); }
static int load_thread(VM* vm) { return load_registered_module(vm, "thread", register_thread); }
#endif
static int load_json(VM* vm) { return load_registered_module(vm, "json", register_json); }
static int load_template(VM* vm) { return load_registered_module(vm, "template", register_template); }
static int load_http(VM* vm) { return load_registered_module(vm, "http", register_http); }
static int load_inspect(VM* vm) { return load_registered_module(vm, "inspect", register_inspect); }
#ifndef TOI_WASM
static int load_regex(VM* vm) { return load_registered_module(vm, "regex", register_regex); }
static int load_fnmatch(VM* vm) { return load_registered_module(vm, "fnmatch", register_fnmatch); }
static int load_glob(VM* vm) { return load_registered_module(vm, "glob", register_glob); }
#endif
static int load_binary(VM* vm) { return load_registered_module(vm, "binary", register_binary); }
static int load_struct(VM* vm) { return load_registered_module(vm, "struct", register_struct); }
static int load_btree(VM* vm) { return load_registered_module(vm, "btree", register_btree); }
#ifndef TOI_WASM
static int load_uuid(VM* vm) { return load_registered_module(vm, "uuid", register_uuid); }
#endif
#ifndef TOI_WASM
static int load_gzip(VM* vm) { return load_registered_module(vm, "gzip", register_gzip); }
#endif

// Native module registry - modules that can be imported on demand
static const ModuleReg native_modules[] = {
    {"math", load_math},
#ifndef TOI_WASM
    {"time", load_time},
#endif
    {"io", load_io},
#ifndef TOI_WASM
    {"os", load_os},
    {"stat", load_stat},
    {"dir", load_dir},
    {"signal", load_signal},
    {"mmap", load_mmap},
    {"poll", load_poll},
#endif
    {"coroutine", load_coroutine},
    {"string", load_string},
    {"table", load_table},
#ifndef TOI_WASM
    {"socket", load_socket},
    {"thread", load_thread},
#endif
    {"json", load_json},
    {"template", load_template},
    {"http", load_http},
    {"inspect", load_inspect},
#ifndef TOI_WASM
    {"regex", load_regex},
    {"fnmatch", load_fnmatch},
    {"glob", load_glob},
#endif
    {"binary", load_binary},
    {"struct", load_struct},
    {"btree", load_btree},
#ifndef TOI_WASM
    {"uuid", load_uuid},
#endif
#ifndef TOI_WASM
    {"gzip", load_gzip},
#endif
    {NULL, NULL}  // Sentinel
};

int is_native_module(const char* name) {
    for (int i = 0; native_modules[i].name != NULL; i++) {
        if (strcmp(name, native_modules[i].name) == 0) {
            return 1;
        }
    }
    return 0;
}

int load_native_module(VM* vm, const char* name) {
    // Check if already loaded (in modules cache)
    ObjString* module_name = copy_string(name, (int)strlen(name));
    push(vm, OBJ_VAL(module_name));

    Value cached;
    if (table_get(&vm->modules, module_name, &cached)) {
        // Already loaded - push cached module
        pop(vm); // module_name
        push(vm, cached);
        return 1;
    }

    // Find and load the module
    for (int i = 0; native_modules[i].name != NULL; i++) {
        if (strcmp(name, native_modules[i].name) == 0) {
            // Call the loader - it will push the module onto the stack
            if (!native_modules[i].loader(vm)) {
                pop(vm); // module_name
                return 0;
            }

            // Stack now: [..., module_name, module]
            // Cache the module in vm->modules
            Value module = peek(vm, 0);
            table_set(&vm->modules, module_name, module);

            // Pop module and module_name, then push module back
            pop(vm); // Pop module, stack: [..., module_name]
            pop(vm); // Pop module_name, stack: [...]
            push(vm, module); // Push module back for caller
            return 1;
        }
    }

    pop(vm); // module_name
    return 0;  // Not found
}

void register_libs(VM* vm) {
    // Core functions are registered directly to globals (always available)
    register_core(vm);
}

void register_module(VM* vm, const char* name, const NativeReg* funcs) {
    if (name == NULL) {
        // Register directly to globals
        for (int i = 0; funcs[i].name != NULL; i++) {
            ObjString* name_str = copy_string(funcs[i].name, (int)strlen(funcs[i].name));
            push(vm, OBJ_VAL(name_str));
            push(vm, OBJ_VAL(new_native(funcs[i].function, name_str)));
            table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
            pop(vm); // Pop native function
            pop(vm); // Pop name string
        }
    } else {
        ObjTable* module = new_table();
        module->is_module = 1;
        push(vm, OBJ_VAL(module)); // Push module onto stack to protect from GC

        for (int i = 0; funcs[i].name != NULL; i++) {
            ObjString* name_str = copy_string(funcs[i].name, (int)strlen(funcs[i].name));
            push(vm, OBJ_VAL(name_str));
            push(vm, OBJ_VAL(new_native(funcs[i].function, name_str)));
            table_set(&module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
            pop(vm); // Pop native function
            pop(vm); // Pop name string
        }

        push(vm, OBJ_VAL(copy_string(name, (int)strlen(name)))); // Push module name
        push(vm, OBJ_VAL(module)); // Push module object (already on stack)
        table_set(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm); // Pop module object (from table_set)
        pop(vm); // Pop module name

        // Module object is still on stack (from line 24)
    }
}
