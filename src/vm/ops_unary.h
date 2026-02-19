#ifndef VM_OPS_UNARY_H
#define VM_OPS_UNARY_H

#include "../vm.h"

void vmHandleOpNegate(VM* vm);
void vmHandleOpNot(VM* vm);
int vmHandleOpLength(VM* vm);

#endif
