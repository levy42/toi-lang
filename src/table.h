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
    int arrayCapacity;
    int arrayMax; // 1-based highest non-nil index in array
} Table;

void initTable(Table* table);
void freeTable(Table* table);
int tableGet(Table* table, struct ObjString* key, Value* value);
int tableSet(Table* table, struct ObjString* key, Value value);
int tableDelete(Table* table, struct ObjString* key);
void tableAddAll(Table* from, Table* to);
// ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash); // Needs full definition?
// No, returns pointer.
struct ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);

// Array optimization helpers
int tableGetArray(Table* table, int index, Value* value);
int tableSetArray(Table* table, int index, Value value);

#endif
