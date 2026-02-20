#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void init_table(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
    table->array = NULL;
    table->array_capacity = 0;
    table->array_max = 0;
}

void free_table(Table* table) {
    free(table->entries);
    free(table->array);
    init_table(table);
}

static Entry* find_entry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % capacity;
    Entry* tombstone = NULL;
    for (;;) {
        Entry* entry = &entries[index];
        // In open addressing, we find either the matching key or an empty slot.
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                return tombstone != NULL ? tombstone : entry;
            }
            if (tombstone == NULL) tombstone = entry;
        } else if (entry->key == key ||
                   (entry->key->length == key->length &&
                    entry->key->hash == key->hash &&
                    memcmp(entry->key->chars, key->chars, key->length) == 0)) {
            return entry;
        }

        index = (index + 1) % capacity;
    }
}

static void adjust_capacity(Table* table, int capacity) {
    Entry* entries = (Entry*)malloc(sizeof(Entry) * capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    // Re-hash existing entries into the new array
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;

        Entry* dest = find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
}

int table_get(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return 0;

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    *value = entry->value;
    return 1;
}

int table_set(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjust_capacity(table, capacity);
    }

    Entry* entry = find_entry(table->entries, table->capacity, key);

    int is_new_key = entry->key == NULL;
    if (is_new_key && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    return is_new_key;
}

int table_delete(Table* table, ObjString* key) {
    if (table->count == 0) return 0;

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    // Place a tombstone.
    entry->key = NULL;
    entry->value = BOOL_VAL(1);
    return 1;
}

int table_get_array(Table* table, int index, Value* value) {
    // 1-based indexing for Lua compatibility
    int raw_index = index - 1;
    if (raw_index >= 0 && raw_index < table->array_capacity) {
        if (!IS_NIL(table->array[raw_index])) {
            *value = table->array[raw_index];
            return 1;
        }
    }
    return 0;
}

int table_set_array(Table* table, int index, Value value) {
    // 1-based indexing
    int raw_index = index - 1;
    
    if (raw_index < 0) return 0; // Negative indices use hash map

    // Keep array dense-ish: only allow append or update within current capacity.
    int max_index = table->array_max;
    if (index > max_index + 1 && index > table->array_capacity) {
        return 0; // Too sparse, use hash map
    }

    if (raw_index >= table->array_capacity) {
        int new_capacity = table->array_capacity == 0 ? 8 : table->array_capacity * 2;
        while (raw_index >= new_capacity) new_capacity *= 2;
        table->array = (Value*)realloc(table->array, sizeof(Value) * new_capacity);
        for (int i = table->array_capacity; i < new_capacity; i++) {
            table->array[i] = NIL_VAL;
        }
        table->array_capacity = new_capacity;
    }

    table->array[raw_index] = value;
    if (!IS_NIL(value)) {
        if (index > table->array_max) table->array_max = index;
    } else if (index == table->array_max) {
        int new_max = table->array_max - 1;
        while (new_max >= 1 && new_max <= table->array_capacity &&
               IS_NIL(table->array[new_max - 1])) {
            new_max--;
        }
        table->array_max = new_max;
    }
    return 1;
}

void table_add_all(Table* from, Table* to) {
    for (int i = 0; i < from->capacity; i++) {
        Entry* entry = &from->entries[i];
        if (entry->key != NULL) {
            table_set(to, entry->key, entry->value);
        }
    }
}

ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;

    uint32_t index = hash % table->capacity;
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            // Stop if we find an empty non-tombstone slot.
            if (IS_NIL(entry->value)) return NULL; 
        } else if (entry->key->length == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            // Found it.
            return entry->key;
        }

        index = (index + 1) % table->capacity;
    }
}
