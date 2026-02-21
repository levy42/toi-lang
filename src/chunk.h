#ifndef CHUNK_H
#define CHUNK_H

#include "common.h"
#include "value.h"

struct ObjString;
struct ObjTable;

typedef enum {
    OP_CONSTANT,
    OP_APPEND,
    OP_ADD,
    OP_ADD_CONST,
    OP_SUBTRACT,
    OP_SUB_CONST,
    OP_MULTIPLY,
    OP_MUL_CONST,
    OP_DIVIDE,
    OP_DIV_CONST,
    OP_NOT,
    OP_NEGATE,
    OP_LENGTH,
    OP_PRINT,
    OP_POP,
    OP_GET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_SET_GLOBAL,
    OP_DELETE_GLOBAL,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_ADD_SET_LOCAL,
    OP_SUB_SET_LOCAL,
    OP_MUL_SET_LOCAL,
    OP_DIV_SET_LOCAL,
    OP_MOD_SET_LOCAL,
    OP_INC_LOCAL,
    OP_SUB_LOCAL_CONST,
    OP_MUL_LOCAL_CONST,
    OP_DIV_LOCAL_CONST,
    OP_MOD_LOCAL_CONST,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_CLOSE_UPVALUE,
    OP_GET_TABLE,
    OP_SET_TABLE,
    OP_DELETE_TABLE,
    OP_NEW_TABLE,
    OP_DUP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_JUMP_IF_TRUE,
    OP_LOOP,
    OP_CALL,
    OP_CALL0,
    OP_CALL1,
    OP_CALL2,
    OP_CALL_NAMED,
    OP_CALL_EXPAND,
    OP_CLOSURE,
    OP_RETURN,
    OP_TRUE,
    OP_FALSE,
    OP_NIL,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_HAS,
    OP_POWER,
    OP_INT_DIV,
    OP_MODULO,
    OP_IADD,
    OP_ISUB,
    OP_IMUL,
    OP_IDIV,
    OP_IMOD,
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FMOD,
    OP_MOD_CONST,
    OP_GC,
    OP_SET_METATABLE,
    OP_RETURN_N,
    OP_ADJUST_STACK,
    OP_TRY,
    OP_END_TRY,
    OP_END_FINALLY,
    OP_IMPORT,
    OP_IMPORT_STAR,
    OP_THROW,
    OP_BUILD_STRING,
    OP_ITER_PREP,
    OP_ITER_PREP_IPAIRS,
    OP_RANGE,
    OP_FOR_PREP,
    OP_FOR_LOOP,
    OP_SLICE
} OpCode;


typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    int* lines; // Line number for each byte for debugging
    uint32_t* global_ic_versions; // keyed by opcode byte offset
    struct ObjString** global_ic_names;
    Value* global_ic_values;
    uint32_t* get_table_ic_versions; // keyed by opcode byte offset
    struct ObjTable** get_table_ic_tables;
    struct ObjString** get_table_ic_keys;
    Value* get_table_ic_values;
    ValueArray constants;
} Chunk;

void init_chunk(Chunk* chunk);
void free_chunk(Chunk* chunk);
void write_chunk(Chunk* chunk, uint8_t byte, int line);
int add_constant(Chunk* chunk, Value value);

#endif
