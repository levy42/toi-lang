#include <string.h>
#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// Forward declarations for individual library registration functions
void registerCore(VM* vm);
void registerMath(VM* vm);
void registerTime(VM* vm);
void registerIO(VM* vm);
void registerOS(VM* vm);
void registerCoroutine(VM* vm);
void registerString(VM* vm);
void registerTable(VM* vm);
void registerSocket(VM* vm);
void registerThread(VM* vm);
void registerJSON(VM* vm);
void registerTemplate(VM* vm);
void registerHTTP(VM* vm);
void registerInspect(VM* vm);
void registerBinary(VM* vm);

static int loadRegisteredModule(VM* vm, const char* name, void (*registerFn)(VM*)) {
    registerFn(vm);

    ObjString* moduleName = copyString(name, (int)strlen(name));
    push(vm, OBJ_VAL(moduleName));

    Value module = NIL_VAL;
    int found = tableGet(&vm->globals, moduleName, &module);
    pop(vm);
    if (!found || !IS_TABLE(module)) {
        return 0;
    }

    push(vm, module);
    return 1;
}

// Module loader wrappers - each loads its module and pushes it onto the stack
static int loadMath(VM* vm) { return loadRegisteredModule(vm, "math", registerMath); }
static int loadTime(VM* vm) { return loadRegisteredModule(vm, "time", registerTime); }
static int loadIO(VM* vm) { return loadRegisteredModule(vm, "io", registerIO); }
static int loadOS(VM* vm) { return loadRegisteredModule(vm, "os", registerOS); }
static int loadCoroutine(VM* vm) { return loadRegisteredModule(vm, "coroutine", registerCoroutine); }
static int loadString(VM* vm) { return loadRegisteredModule(vm, "string", registerString); }
static int loadTable(VM* vm) { return loadRegisteredModule(vm, "table", registerTable); }
static int loadSocket(VM* vm) { return loadRegisteredModule(vm, "socket", registerSocket); }
static int loadThread(VM* vm) { return loadRegisteredModule(vm, "thread", registerThread); }
static int loadJSON(VM* vm) { return loadRegisteredModule(vm, "json", registerJSON); }
static int loadTemplate(VM* vm) { return loadRegisteredModule(vm, "template", registerTemplate); }
static int loadHTTP(VM* vm) { return loadRegisteredModule(vm, "http", registerHTTP); }
static int loadInspect(VM* vm) { return loadRegisteredModule(vm, "inspect", registerInspect); }
static int loadBinary(VM* vm) { return loadRegisteredModule(vm, "binary", registerBinary); }

// Native module registry - modules that can be imported on demand
static const ModuleReg nativeModules[] = {
    {"math", loadMath},
    {"time", loadTime},
    {"io", loadIO},
    {"os", loadOS},
    {"coroutine", loadCoroutine},
    {"string", loadString},
    {"table", loadTable},
    {"socket", loadSocket},
    {"thread", loadThread},
    {"json", loadJSON},
    {"template", loadTemplate},
    {"http", loadHTTP},
    {"inspect", loadInspect},
    {"binary", loadBinary},
    {NULL, NULL}  // Sentinel
};

int isNativeModule(const char* name) {
    for (int i = 0; nativeModules[i].name != NULL; i++) {
        if (strcmp(name, nativeModules[i].name) == 0) {
            return 1;
        }
    }
    return 0;
}

int loadNativeModule(VM* vm, const char* name) {
    // Check if already loaded (in modules cache)
    ObjString* moduleName = copyString(name, (int)strlen(name));
    push(vm, OBJ_VAL(moduleName));
    
    Value cached;
    if (tableGet(&vm->modules, moduleName, &cached)) {
        // Already loaded - push cached module
        pop(vm); // moduleName
        push(vm, cached);
        return 1;
    }
    
    // Find and load the module
    for (int i = 0; nativeModules[i].name != NULL; i++) {
        if (strcmp(name, nativeModules[i].name) == 0) {
            // Call the loader - it will push the module onto the stack
            if (!nativeModules[i].loader(vm)) {
                pop(vm); // moduleName
                return 0;
            }
            
            // Stack now: [..., moduleName, module]
            // Cache the module in vm->modules
            Value module = peek(vm, 0);
            tableSet(&vm->modules, moduleName, module);
            
            // Pop module and moduleName, then push module back
            pop(vm); // Pop module, stack: [..., moduleName]
            pop(vm); // Pop moduleName, stack: [...]
            push(vm, module); // Push module back for caller
            return 1;
        }
    }
    
    pop(vm); // moduleName
    return 0;  // Not found
}

void registerLibs(VM* vm) {
    // Core functions are registered directly to globals (always available)
    registerCore(vm);
}

void registerModule(VM* vm, const char* name, const NativeReg* funcs) {
    if (name == NULL) {
        // Register directly to globals
        for (int i = 0; funcs[i].name != NULL; i++) {
            ObjString* nameStr = copyString(funcs[i].name, (int)strlen(funcs[i].name));
            push(vm, OBJ_VAL(nameStr));
            push(vm, OBJ_VAL(newNative(funcs[i].function, nameStr)));
            tableSet(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
            pop(vm); // Pop native function
            pop(vm); // Pop name string
        }
    } else {
        ObjTable* module = newTable();
        module->isModule = 1;
        push(vm, OBJ_VAL(module)); // Push module onto stack to protect from GC
        
        for (int i = 0; funcs[i].name != NULL; i++) {
            ObjString* nameStr = copyString(funcs[i].name, (int)strlen(funcs[i].name));
            push(vm, OBJ_VAL(nameStr));
            push(vm, OBJ_VAL(newNative(funcs[i].function, nameStr)));
            tableSet(&module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
            pop(vm); // Pop native function
            pop(vm); // Pop name string
        }
        
        push(vm, OBJ_VAL(copyString(name, (int)strlen(name)))); // Push module name
        push(vm, OBJ_VAL(module)); // Push module object (already on stack)
        tableSet(&vm->globals, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm); // Pop module object (from tableSet)
        pop(vm); // Pop module name
        
        // Module object is still on stack (from line 24)
    }
}
