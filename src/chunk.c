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
    chunk->get_table_ic_slots = NULL;
    chunk->get_table_ic_values = NULL;
    chunk->set_table_ic_versions = NULL;
    chunk->set_table_ic_tables = NULL;
    chunk->set_table_ic_keys = NULL;
    chunk->set_table_ic_slots = NULL;
    chunk->delete_table_ic_versions = NULL;
    chunk->delete_table_ic_tables = NULL;
    chunk->delete_table_ic_keys = NULL;
    chunk->delete_table_ic_slots = NULL;
    chunk->call_ic_closures = NULL;
    chunk->call_ic_argc = NULL;
    chunk->call2_ic_closures = NULL;
    chunk->call2_ic_kind = NULL;
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
    free(chunk->get_table_ic_slots);
    free(chunk->get_table_ic_values);
    free(chunk->set_table_ic_versions);
    free(chunk->set_table_ic_tables);
    free(chunk->set_table_ic_keys);
    free(chunk->set_table_ic_slots);
    free(chunk->delete_table_ic_versions);
    free(chunk->delete_table_ic_tables);
    free(chunk->delete_table_ic_keys);
    free(chunk->delete_table_ic_slots);
    free(chunk->call_ic_closures);
    free(chunk->call_ic_argc);
    free(chunk->call2_ic_closures);
    free(chunk->call2_ic_kind);
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
        chunk->get_table_ic_slots = (int*)realloc(chunk->get_table_ic_slots, sizeof(int) * chunk->capacity);
        chunk->get_table_ic_values = (Value*)realloc(chunk->get_table_ic_values, sizeof(Value) * chunk->capacity);
        chunk->set_table_ic_versions = (uint32_t*)realloc(chunk->set_table_ic_versions, sizeof(uint32_t) * chunk->capacity);
        chunk->set_table_ic_tables = (struct ObjTable**)realloc(chunk->set_table_ic_tables, sizeof(struct ObjTable*) * chunk->capacity);
        chunk->set_table_ic_keys = (struct ObjString**)realloc(chunk->set_table_ic_keys, sizeof(struct ObjString*) * chunk->capacity);
        chunk->set_table_ic_slots = (int*)realloc(chunk->set_table_ic_slots, sizeof(int) * chunk->capacity);
        chunk->delete_table_ic_versions = (uint32_t*)realloc(chunk->delete_table_ic_versions, sizeof(uint32_t) * chunk->capacity);
        chunk->delete_table_ic_tables = (struct ObjTable**)realloc(chunk->delete_table_ic_tables, sizeof(struct ObjTable*) * chunk->capacity);
        chunk->delete_table_ic_keys = (struct ObjString**)realloc(chunk->delete_table_ic_keys, sizeof(struct ObjString*) * chunk->capacity);
        chunk->delete_table_ic_slots = (int*)realloc(chunk->delete_table_ic_slots, sizeof(int) * chunk->capacity);
        chunk->call_ic_closures = (struct ObjClosure**)realloc(chunk->call_ic_closures, sizeof(struct ObjClosure*) * chunk->capacity);
        chunk->call_ic_argc = (uint8_t*)realloc(chunk->call_ic_argc, sizeof(uint8_t) * chunk->capacity);
        chunk->call2_ic_closures = (struct ObjClosure**)realloc(chunk->call2_ic_closures, sizeof(struct ObjClosure*) * chunk->capacity);
        chunk->call2_ic_kind = (uint8_t*)realloc(chunk->call2_ic_kind, sizeof(uint8_t) * chunk->capacity);
        for (int i = old_capacity; i < chunk->capacity; i++) {
            chunk->global_ic_versions[i] = 0;
            chunk->global_ic_names[i] = NULL;
            chunk->global_ic_values[i] = NIL_VAL;
            chunk->get_table_ic_versions[i] = 0;
            chunk->get_table_ic_tables[i] = NULL;
            chunk->get_table_ic_keys[i] = NULL;
            chunk->get_table_ic_slots[i] = -1;
            chunk->get_table_ic_values[i] = NIL_VAL;
            chunk->set_table_ic_versions[i] = 0;
            chunk->set_table_ic_tables[i] = NULL;
            chunk->set_table_ic_keys[i] = NULL;
            chunk->set_table_ic_slots[i] = -1;
            chunk->delete_table_ic_versions[i] = 0;
            chunk->delete_table_ic_tables[i] = NULL;
            chunk->delete_table_ic_keys[i] = NULL;
            chunk->delete_table_ic_slots[i] = -1;
            chunk->call_ic_closures[i] = NULL;
            chunk->call_ic_argc[i] = 0;
            chunk->call2_ic_closures[i] = NULL;
            chunk->call2_ic_kind[i] = 0;
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
