#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void init_table(Table* table) {
    table->count = 0;
    table->capacity = 8;
    table->entries = table->inline_entries;
    for (int i = 0; i < table->capacity; i++) {
        table->entries[i].key = NULL;
        table->entries[i].value = NIL_VAL;
    }
    table->version = 0;
    table->shape_version = 0;
    table->array = NULL;
    table->array_capacity = 0;
    table->array_max = 0;
}

void free_table(Table* table) {
    if (table->entries != NULL && table->entries != table->inline_entries) {
        free(table->entries);
    }
    free(table->array);
    init_table(table);
}

static Entry* find_entry(Entry* entries, int capacity, ObjString* key) {
    uint32_t key_hash = objstring_hash(key);
    uint32_t index = key_hash % capacity;
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
                    objstring_hash(entry->key) == key_hash &&
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

    if (table->entries != NULL && table->entries != table->inline_entries) {
        free(table->entries);
    }
    table->entries = entries;
    table->capacity = capacity;
    table->shape_version++;
}

int table_get(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return 0;

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    *value = entry->value;
    return 1;
}

int table_get_with_slot(Table* table, ObjString* key, Value* value, int* slot_index) {
    if (table->count == 0) return 0;

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    if (slot_index != NULL) {
        *slot_index = (int)(entry - table->entries);
    }
    *value = entry->value;
    return 1;
}

int table_set_with_slot(Table* table, ObjString* key, Value value, int* slot_index) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity * 2;
        adjust_capacity(table, capacity);
    }

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (slot_index != NULL) {
        *slot_index = (int)(entry - table->entries);
    }

    int is_new_key = entry->key == NULL;
    if (is_new_key && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    table->version++;
    if (is_new_key) {
        table->shape_version++;
    }
    return is_new_key;
}

int table_set_at_slot(Table* table, int slot_index, ObjString* key, Value value) {
    if (slot_index < 0 || slot_index >= table->capacity) return 0;
    Entry* entry = &table->entries[slot_index];
    if (entry->key != key) return 0;
    entry->value = value;
    table->version++;
    return 1;
}

int table_set_at_slot_or_empty(Table* table, int slot_index, ObjString* key, Value value) {
    if (slot_index < 0 || slot_index >= table->capacity) return 0;
    Entry* entry = &table->entries[slot_index];
    if (entry->key == key) {
        entry->value = value;
        table->version++;
        return 1;
    }
    // Fast insertion path for cached literal/object layouts.
    if (entry->key == NULL && IS_NIL(entry->value)) {
        if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
            return 0;
        }
        entry->key = key;
        entry->value = value;
        table->count++;
        table->version++;
        table->shape_version++;
        return 1;
    }
    return 0;
}

int table_set(Table* table, ObjString* key, Value value) {
    return table_set_with_slot(table, key, value, NULL);
}

int table_delete_at_slot(Table* table, int slot_index, ObjString* key) {
    if (slot_index < 0 || slot_index >= table->capacity) return 0;
    Entry* entry = &table->entries[slot_index];
    if (entry->key != key) return 0;

    entry->key = NULL;
    entry->value = BOOL_VAL(1);
    table->version++;
    table->shape_version++;
    return 1;
}

int table_delete(Table* table, ObjString* key) {
    if (table->count == 0) return 0;

    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    // Place a tombstone.
    entry->key = NULL;
    entry->value = BOOL_VAL(1);
    table->version++;
    table->shape_version++;
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
        table->shape_version++;
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
    table->version++;
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
                   objstring_hash(entry->key) == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            // Found it.
            return entry->key;
        }

        index = (index + 1) % table->capacity;
    }
}
