#ifndef TABLE_H
#define TABLE_H

#include "value.h"
// #include "object.h" // Removed to avoid circular dependency

struct ObjString;

typedef struct {
    struct ObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
    
    // Array optimization
    Value* array;
    int array_capacity;
    int array_max; // 1-based highest non-nil index in array
} Table;

void init_table(Table* table);
void free_table(Table* table);
int table_get(Table* table, struct ObjString* key, Value* value);
int table_set(Table* table, struct ObjString* key, Value value);
int table_delete(Table* table, struct ObjString* key);
void table_add_all(Table* from, Table* to);
// ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash); // Needs full definition?
// No, returns pointer.
struct ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash);

// Array optimization helpers
int table_get_array(Table* table, int index, Value* value);
int table_set_array(Table* table, int index, Value value);

#endif
