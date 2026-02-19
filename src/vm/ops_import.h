#ifndef VM_OPS_IMPORT_H
#define VM_OPS_IMPORT_H

#include "../vm.h"

InterpretResult vmHandleOpImport(VM* vm, ObjString* moduleName, CallFrame** frame, uint8_t** ip);

#endif
