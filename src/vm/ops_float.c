#include <math.h>

#include "ops_float.h"

void vm_handle_op_f_add(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a + b));
}

void vm_handle_op_f_sub(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a - b));
}

void vm_handle_op_f_mul(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a * b));
}

void vm_handle_op_f_div(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(a / b));
}

void vm_handle_op_f_mod(VM* vm) {
    double b = AS_NUMBER(pop(vm));
    double a = AS_NUMBER(pop(vm));
    push(vm, NUMBER_VAL(fmod(a, b)));
}
