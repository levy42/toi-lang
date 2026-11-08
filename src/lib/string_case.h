#ifndef TOI_STRING_CASE_H
#define TOI_STRING_CASE_H

#include "../object.h"

/* Shared by library calls and JIT primitives; returns zero on allocation failure. */
int string_case_transform(ObjString* source, int to_upper, Value* out);

#endif
