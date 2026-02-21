#include <stdlib.h>

#include "chunk.h"

void init_chunk(Chunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->global_ic_versions = NULL;
    chunk->global_ic_names = NULL;
    chunk->global_ic_values = NULL;
    chunk->get_table_ic_versions = NULL;
    chunk->get_table_ic_tables = NULL;
    chunk->get_table_ic_keys = NULL;
    chunk->get_table_ic_values = NULL;
    init_value_array(&chunk->constants);
}

void free_chunk(Chunk* chunk) {
    free(chunk->code);
    free(chunk->lines);
    free(chunk->global_ic_versions);
    free(chunk->global_ic_names);
    free(chunk->global_ic_values);
    free(chunk->get_table_ic_versions);
    free(chunk->get_table_ic_tables);
    free(chunk->get_table_ic_keys);
    free(chunk->get_table_ic_values);
    free_value_array(&chunk->constants);
    init_chunk(chunk);
}

void write_chunk(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int old_capacity = chunk->capacity;
        chunk->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        chunk->code = (uint8_t*)realloc(chunk->code, sizeof(uint8_t) * chunk->capacity);
        chunk->lines = (int*)realloc(chunk->lines, sizeof(int) * chunk->capacity);
        chunk->global_ic_versions = (uint32_t*)realloc(chunk->global_ic_versions, sizeof(uint32_t) * chunk->capacity);
        chunk->global_ic_names = (struct ObjString**)realloc(chunk->global_ic_names, sizeof(struct ObjString*) * chunk->capacity);
        chunk->global_ic_values = (Value*)realloc(chunk->global_ic_values, sizeof(Value) * chunk->capacity);
        chunk->get_table_ic_versions = (uint32_t*)realloc(chunk->get_table_ic_versions, sizeof(uint32_t) * chunk->capacity);
        chunk->get_table_ic_tables = (struct ObjTable**)realloc(chunk->get_table_ic_tables, sizeof(struct ObjTable*) * chunk->capacity);
        chunk->get_table_ic_keys = (struct ObjString**)realloc(chunk->get_table_ic_keys, sizeof(struct ObjString*) * chunk->capacity);
        chunk->get_table_ic_values = (Value*)realloc(chunk->get_table_ic_values, sizeof(Value) * chunk->capacity);
        for (int i = old_capacity; i < chunk->capacity; i++) {
            chunk->global_ic_versions[i] = 0;
            chunk->global_ic_names[i] = NULL;
            chunk->global_ic_values[i] = NIL_VAL;
            chunk->get_table_ic_versions[i] = 0;
            chunk->get_table_ic_tables[i] = NULL;
            chunk->get_table_ic_keys[i] = NULL;
            chunk->get_table_ic_values[i] = NIL_VAL;
        }
    }
    
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int add_constant(Chunk* chunk, Value value) {
    write_value_array(&chunk->constants, value);
    return chunk->constants.count - 1;
}
