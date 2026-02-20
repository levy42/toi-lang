#ifndef VM_OPS_IMPORT_H
#define VM_OPS_IMPORT_H

#include "../vm.h"

InterpretResult vm_handle_op_import(VM* vm, ObjString* module_name, CallFrame** frame, uint8_t** ip);

#endif
