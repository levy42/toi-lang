#ifndef VM_OPS_META_H
#define VM_OPS_META_H

#include "../vm.h"

int vmHandleOpNewTable(VM* vm);
int vmHandleOpSetMetatable(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
