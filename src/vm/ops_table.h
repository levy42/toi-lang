#ifndef VM_OPS_TABLE_H
#define VM_OPS_TABLE_H

#include "../vm.h"

int vmHandleOpAppend(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpGetTable(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpSetTable(VM* vm, CallFrame** frame, uint8_t** ip);
int vmHandleOpDeleteTable(VM* vm);

#endif
