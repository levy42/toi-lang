#ifndef VM_OPS_META_H
#define VM_OPS_META_H

#include "../vm.h"

int vm_handle_op_new_table(VM* vm);
int vm_handle_op_set_metatable(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
