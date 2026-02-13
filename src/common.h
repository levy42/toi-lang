#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// #define DEBUG_TRACE_EXECUTION
// #define DEBUG_PRINT_CODE
// #define DEBUG_MULTI_RETURN
// #define DEBUG_VARIADIC

// ANSI color codes
#define COLOR_RED     "\033[91m"
#define COLOR_RESET   "\033[0m"

typedef enum {
    TYPEHINT_ANY,
    TYPEHINT_INT,
    TYPEHINT_FLOAT,
    TYPEHINT_BOOL,
    TYPEHINT_STR,
    TYPEHINT_TABLE
} TypeHint;

#endif
