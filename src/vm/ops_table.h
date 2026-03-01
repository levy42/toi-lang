#ifndef VM_OPS_TABLE_H
#define VM_OPS_TABLE_H

#include "../vm.h"

int vm_handle_op_append(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_get_table_inner(
    VM* vm, CallFrame** frame, uint8_t** ip, Chunk* chunk, int opcode_offset, Value table, Value key);
int vm_handle_op_get_table(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_get_table_num(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_get_field_const(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_get_meta_table(VM* vm);
int vm_handle_op_set_table(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_set_table_keep(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_set_table_num(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_set_field_const(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_set_field_const_keep(VM* vm, CallFrame** frame, uint8_t** ip);
int vm_handle_op_delete_table(VM* vm, CallFrame** frame, uint8_t** ip);

#endif
