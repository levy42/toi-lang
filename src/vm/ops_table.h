#ifndef VM_OPS_TABLE_H
#define VM_OPS_TABLE_H

#include "../vm.h"

int vm_handle_op_append(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_get_table(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_set_table(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_delete_table(VM* vm);

#endif
