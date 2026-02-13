#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
    table->array = NULL;
    table->arrayCapacity = 0;
    table->arrayMax = 0;
}

void freeTable(Table* table) {
    free(table->entries);
    free(table->array);
    initTable(table);
}

static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
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

static void adjustCapacity(Table* table, int capacity) {
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

        Entry* dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
}

int tableGet(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return 0;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    *value = entry->value;
    return 1;
}

int tableSet(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjustCapacity(table, capacity);
    }

    Entry* entry = findEntry(table->entries, table->capacity, key);

    int isNewKey = entry->key == NULL;
    if (isNewKey && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

int tableDelete(Table* table, ObjString* key) {
    if (table->count == 0) return 0;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;

    // Place a tombstone.
    entry->key = NULL;
    entry->value = BOOL_VAL(1);
    return 1;
}

int tableGetArray(Table* table, int index, Value* value) {
    // 1-based indexing for Lua compatibility
    int rawIndex = index - 1;
    if (rawIndex >= 0 && rawIndex < table->arrayCapacity) {
        if (!IS_NIL(table->array[rawIndex])) {
            *value = table->array[rawIndex];
            return 1;
        }
    }
    return 0;
}

int tableSetArray(Table* table, int index, Value value) {
    // 1-based indexing
    int rawIndex = index - 1;
    
    if (rawIndex < 0) return 0; // Negative indices use hash map

    // Keep array dense-ish: only allow append or update within current capacity.
    int maxIndex = table->arrayMax;
    if (index > maxIndex + 1 && index > table->arrayCapacity) {
        return 0; // Too sparse, use hash map
    }

    if (rawIndex >= table->arrayCapacity) {
        int newCapacity = table->arrayCapacity == 0 ? 8 : table->arrayCapacity * 2;
        while (rawIndex >= newCapacity) newCapacity *= 2;
        table->array = (Value*)realloc(table->array, sizeof(Value) * newCapacity);
        for (int i = table->arrayCapacity; i < newCapacity; i++) {
            table->array[i] = NIL_VAL;
        }
        table->arrayCapacity = newCapacity;
    }

    table->array[rawIndex] = value;
    if (!IS_NIL(value)) {
        if (index > table->arrayMax) table->arrayMax = index;
    } else if (index == table->arrayMax) {
        int newMax = table->arrayMax - 1;
        while (newMax >= 1 && newMax <= table->arrayCapacity &&
               IS_NIL(table->array[newMax - 1])) {
            newMax--;
        }
        table->arrayMax = newMax;
    }
    return 1;
}

void tableAddAll(Table* from, Table* to) {
    for (int i = 0; i < from->capacity; i++) {
        Entry* entry = &from->entries[i];
        if (entry->key != NULL) {
            tableSet(to, entry->key, entry->value);
        }
    }
}

ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash) {
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
