#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

#define BTREE_MAGIC "PBT2"
#define BTREE_VERSION 1
#define BTREE_PAGE_SIZE 4096u

#define BTREE_PAGE_HEADER_SIZE 16u
#define BTREE_SLOT_SIZE 2u

#define BTREE_PAGE_TYPE_FREE 0u
#define BTREE_PAGE_TYPE_LEAF 1u
#define BTREE_PAGE_TYPE_INTERNAL 2u

#define BTREE_HEADER_ROOT_PAGE_OFFSET 8u
#define BTREE_HEADER_PAGE_COUNT_OFFSET 12u
#define BTREE_HEADER_FREE_HEAD_OFFSET 16u

#define BTREE_ATOM_NUMBER 1u
#define BTREE_ATOM_STRING 2u

typedef struct {
    uint8_t type;
    double number;
    char* string;
    uint32_t string_len;
} BTreeAtom;

typedef struct {
    BTreeAtom key;
    BTreeAtom value;
} LeafEntry;

typedef struct {
    BTreeAtom key;
    uint32_t child;
} InternalEntry;

typedef struct {
    uint32_t page_id;
    uint8_t type;
    uint16_t nkeys;
    uint32_t left_child;
    LeafEntry* leaf_entries;
    InternalEntry* internal_entries;
} NodeData;

typedef struct {
    BTreeAtom key;
    uint32_t right_page;
} Promote;

typedef struct {
    FILE* fp;
    char* path;
    uint8_t* mem_pages;
    uint32_t mem_capacity_pages;
    uint8_t in_memory;
    uint32_t root_page;
    uint32_t page_count;
    uint32_t free_head;
    uint8_t closed;
} BTreeDb;

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void atom_init(BTreeAtom* a) {
    a->type = 0;
    a->number = 0;
    a->string = NULL;
    a->string_len = 0;
}

static void atom_free(BTreeAtom* a) {
    if (a->type == BTREE_ATOM_STRING && a->string != NULL) {
        free(a->string);
    }
    atom_init(a);
}

static int atom_clone(BTreeAtom* dst, const BTreeAtom* src) {
    atom_init(dst);
    dst->type = src->type;
    dst->number = src->number;
    dst->string_len = src->string_len;
    if (src->type == BTREE_ATOM_STRING && src->string_len > 0) {
        dst->string = (char*)malloc(src->string_len);
        if (dst->string == NULL) {
            atom_init(dst);
            return 0;
        }
        memcpy(dst->string, src->string, src->string_len);
    }
    return 1;
}

static int atom_from_value(VM* vm, Value v, const char* what, BTreeAtom* out) {
    atom_init(out);
    if (IS_NUMBER(v)) {
        double n = AS_NUMBER(v);
        if (isnan(n)) {
            vm_runtime_error(vm, "%s cannot be NaN.", what);
            return 0;
        }
        out->type = BTREE_ATOM_NUMBER;
        out->number = n;
        return 1;
    }

    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        out->type = BTREE_ATOM_STRING;
        out->string_len = (uint32_t)s->length;
        if (out->string_len > 0) {
            out->string = (char*)malloc(out->string_len);
            if (out->string == NULL) {
                vm_runtime_error(vm, "Out of memory while copying %s.", what);
                atom_free(out);
                return 0;
            }
            memcpy(out->string, s->chars, out->string_len);
        }
        return 1;
    }

    vm_runtime_error(vm, "%s must be string or number.", what);
    return 0;
}

static int atom_compare(const BTreeAtom* a, const BTreeAtom* b) {
    if (a->type != b->type) return (a->type < b->type) ? -1 : 1;
    if (a->type == BTREE_ATOM_NUMBER) {
        if (a->number < b->number) return -1;
        if (a->number > b->number) return 1;
        return 0;
    }

    uint32_t min = a->string_len < b->string_len ? a->string_len : b->string_len;
    int c = 0;
    if (min > 0) c = memcmp(a->string, b->string, min);
    if (c < 0) return -1;
    if (c > 0) return 1;
    if (a->string_len < b->string_len) return -1;
    if (a->string_len > b->string_len) return 1;
    return 0;
}

static uint32_t atom_encoded_size(const BTreeAtom* atom) {
    if (atom->type == BTREE_ATOM_NUMBER) return 1u + 8u;
    return 1u + 4u + atom->string_len;
}

static int atom_encode(uint8_t* out, uint32_t out_size, uint32_t* used, const BTreeAtom* atom) {
    uint32_t need = atom_encoded_size(atom);
    if (out_size < need) return 0;

    out[0] = atom->type;
    if (atom->type == BTREE_ATOM_NUMBER) {
        union { double d; uint8_t b[8]; } u;
        u.d = atom->number;
        memcpy(out + 1, u.b, 8);
    } else if (atom->type == BTREE_ATOM_STRING) {
        wr_u32(out + 1, atom->string_len);
        if (atom->string_len > 0) memcpy(out + 5, atom->string, atom->string_len);
    } else {
        return 0;
    }
    *used = need;
    return 1;
}

static int atom_decode(const uint8_t* in, uint32_t in_size, uint32_t* used, BTreeAtom* atom) {
    atom_init(atom);
    if (in_size < 1) return 0;

    uint8_t type = in[0];
    atom->type = type;
    if (type == BTREE_ATOM_NUMBER) {
        if (in_size < 9) return 0;
        union { double d; uint8_t b[8]; } u;
        memcpy(u.b, in + 1, 8);
        atom->number = u.d;
        *used = 9;
        return 1;
    }

    if (type == BTREE_ATOM_STRING) {
        if (in_size < 5) return 0;
        uint32_t len = rd_u32(in + 1);
        if (in_size < 5u + len) return 0;
        atom->string_len = len;
        if (len > 0) {
            atom->string = (char*)malloc(len);
            if (atom->string == NULL) {
                atom_free(atom);
                return 0;
            }
            memcpy(atom->string, in + 5, len);
        }
        *used = 5u + len;
        return 1;
    }

    atom_free(atom);
    return 0;
}

static void leaf_entry_free(LeafEntry* e) {
    atom_free(&e->key);
    atom_free(&e->value);
}

static void internal_entry_free(InternalEntry* e) {
    atom_free(&e->key);
}

static void leaf_entries_free_array(LeafEntry* entries, uint16_t count) {
    if (entries == NULL) return;
    for (uint16_t i = 0; i < count; i++) leaf_entry_free(&entries[i]);
    free(entries);
}

static void internal_entries_free_array(InternalEntry* entries, uint16_t count) {
    if (entries == NULL) return;
    for (uint16_t i = 0; i < count; i++) internal_entry_free(&entries[i]);
    free(entries);
}

static void node_data_init(NodeData* n) {
    n->page_id = 0;
    n->type = BTREE_PAGE_TYPE_FREE;
    n->nkeys = 0;
    n->left_child = 0;
    n->leaf_entries = NULL;
    n->internal_entries = NULL;
}

static void node_data_free(NodeData* n) {
    if (n->type == BTREE_PAGE_TYPE_LEAF && n->leaf_entries != NULL) leaf_entries_free_array(n->leaf_entries, n->nkeys);
    if (n->type == BTREE_PAGE_TYPE_INTERNAL && n->internal_entries != NULL) internal_entries_free_array(n->internal_entries, n->nkeys);
    node_data_init(n);
}

static int db_seek_page(FILE* fp, uint32_t page_id) {
    long off = (long)page_id * (long)BTREE_PAGE_SIZE;
    return fseek(fp, off, SEEK_SET) == 0;
}

static int db_mem_ensure_pages(BTreeDb* db, uint32_t needed_pages) {
    if (db->mem_capacity_pages >= needed_pages) return 1;
    uint32_t new_cap = db->mem_capacity_pages == 0 ? 4u : db->mem_capacity_pages;
    while (new_cap < needed_pages) new_cap *= 2u;

    size_t bytes = (size_t)new_cap * (size_t)BTREE_PAGE_SIZE;
    uint8_t* grown = (uint8_t*)realloc(db->mem_pages, bytes);
    if (grown == NULL) return 0;
    size_t old_bytes = (size_t)db->mem_capacity_pages * (size_t)BTREE_PAGE_SIZE;
    memset(grown + old_bytes, 0, bytes - old_bytes);
    db->mem_pages = grown;
    db->mem_capacity_pages = new_cap;
    return 1;
}

static int db_read_page(BTreeDb* db, uint32_t page_id, uint8_t* out) {
    if (db->in_memory) {
        if (page_id >= db->page_count || page_id >= db->mem_capacity_pages) return 0;
        memcpy(out, db->mem_pages + (size_t)page_id * BTREE_PAGE_SIZE, BTREE_PAGE_SIZE);
        return 1;
    }
    if (!db_seek_page(db->fp, page_id)) return 0;
    return fread(out, 1, BTREE_PAGE_SIZE, db->fp) == BTREE_PAGE_SIZE;
}

static int db_write_page(BTreeDb* db, uint32_t page_id, const uint8_t* in) {
    if (db->in_memory) {
        if (!db_mem_ensure_pages(db, page_id + 1u)) return 0;
        memcpy(db->mem_pages + (size_t)page_id * BTREE_PAGE_SIZE, in, BTREE_PAGE_SIZE);
        return 1;
    }
    if (!db_seek_page(db->fp, page_id)) return 0;
    if (fwrite(in, 1, BTREE_PAGE_SIZE, db->fp) != BTREE_PAGE_SIZE) return 0;
    return fflush(db->fp) == 0;
}

static int db_write_header(BTreeDb* db) {
    uint8_t page[BTREE_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    memcpy(page, BTREE_MAGIC, 4);
    page[4] = BTREE_VERSION;
    wr_u32(page + BTREE_HEADER_ROOT_PAGE_OFFSET, db->root_page);
    wr_u32(page + BTREE_HEADER_PAGE_COUNT_OFFSET, db->page_count);
    wr_u32(page + BTREE_HEADER_FREE_HEAD_OFFSET, db->free_head);
    return db_write_page(db, 0, page);
}

static uint16_t page_get_nkeys(const uint8_t* page) {
    return rd_u16(page + 2);
}

static uint16_t page_get_free_start(const uint8_t* page) {
    return rd_u16(page + 4);
}

static uint16_t page_get_free_end(const uint8_t* page) {
    return rd_u16(page + 6);
}

static uint32_t page_get_left_child(const uint8_t* page) {
    return rd_u32(page + 8);
}

static uint16_t page_slot(const uint8_t* page, uint16_t idx) {
    return rd_u16(page + BTREE_PAGE_HEADER_SIZE + (uint32_t)idx * BTREE_SLOT_SIZE);
}

static void page_set_slot(uint8_t* page, uint16_t idx, uint16_t off) {
    wr_u16(page + BTREE_PAGE_HEADER_SIZE + (uint32_t)idx * BTREE_SLOT_SIZE, off);
}

static void page_init(uint8_t* page, uint8_t type, uint32_t left_child) {
    memset(page, 0, BTREE_PAGE_SIZE);
    page[0] = type;
    wr_u16(page + 2, 0);
    wr_u16(page + 4, (uint16_t)BTREE_PAGE_HEADER_SIZE);
    wr_u16(page + 6, (uint16_t)BTREE_PAGE_SIZE);
    wr_u32(page + 8, left_child);
}

static int page_add_record(uint8_t* page, uint16_t index, const uint8_t* rec, uint16_t rec_size) {
    uint16_t n = page_get_nkeys(page);
    uint16_t fs = page_get_free_start(page);
    uint16_t fe = page_get_free_end(page);
    if (index > n) return 0;
    if ((uint32_t)rec_size + BTREE_SLOT_SIZE > (uint32_t)(fe - fs)) return 0;

    uint16_t new_off = (uint16_t)(fe - rec_size);
    memcpy(page + new_off, rec, rec_size);

    for (uint16_t i = n; i > index; i--) {
        page_set_slot(page, i, page_slot(page, i - 1));
    }
    page_set_slot(page, index, new_off);

    wr_u16(page + 2, (uint16_t)(n + 1));
    wr_u16(page + 4, (uint16_t)(fs + BTREE_SLOT_SIZE));
    wr_u16(page + 6, new_off);
    return 1;
}

static int node_load(BTreeDb* db, uint32_t page_id, NodeData* out) {
    node_data_init(out);
    uint8_t page[BTREE_PAGE_SIZE];
    if (!db_read_page(db, page_id, page)) return 0;

    uint8_t type = page[0];
    if (type != BTREE_PAGE_TYPE_LEAF && type != BTREE_PAGE_TYPE_INTERNAL) return 0;

    uint16_t nkeys = page_get_nkeys(page);
    uint16_t fs = page_get_free_start(page);
    uint16_t fe = page_get_free_end(page);
    if (fs < BTREE_PAGE_HEADER_SIZE || fs > BTREE_PAGE_SIZE) return 0;
    if (fe > BTREE_PAGE_SIZE || fe < BTREE_PAGE_HEADER_SIZE) return 0;

    out->page_id = page_id;
    out->type = type;
    out->nkeys = nkeys;
    out->left_child = page_get_left_child(page);

    if (type == BTREE_PAGE_TYPE_LEAF) {
        out->leaf_entries = (LeafEntry*)calloc(nkeys == 0 ? 1 : nkeys, sizeof(LeafEntry));
        if (out->leaf_entries == NULL) return 0;

        for (uint16_t i = 0; i < nkeys; i++) {
            uint16_t off = page_slot(page, i);
            if (off >= BTREE_PAGE_SIZE) {
                node_data_free(out);
                return 0;
            }
            uint32_t used = 0;
            if (!atom_decode(page + off, BTREE_PAGE_SIZE - off, &used, &out->leaf_entries[i].key)) {
                node_data_free(out);
                return 0;
            }
            if (!atom_decode(page + off + used, BTREE_PAGE_SIZE - off - used, &used, &out->leaf_entries[i].value)) {
                node_data_free(out);
                return 0;
            }
        }
    } else {
        out->internal_entries = (InternalEntry*)calloc(nkeys == 0 ? 1 : nkeys, sizeof(InternalEntry));
        if (out->internal_entries == NULL) return 0;

        for (uint16_t i = 0; i < nkeys; i++) {
            uint16_t off = page_slot(page, i);
            if (off >= BTREE_PAGE_SIZE) {
                node_data_free(out);
                return 0;
            }
            uint32_t used = 0;
            if (!atom_decode(page + off, BTREE_PAGE_SIZE - off, &used, &out->internal_entries[i].key)) {
                node_data_free(out);
                return 0;
            }
            uint32_t rem = BTREE_PAGE_SIZE - off - used;
            if (rem < 4) {
                node_data_free(out);
                return 0;
            }
            out->internal_entries[i].child = rd_u32(page + off + used);
        }
    }

    return 1;
}

static int encode_leaf_record(const LeafEntry* entry, uint8_t** out_rec, uint16_t* out_size) {
    uint32_t ks = atom_encoded_size(&entry->key);
    uint32_t vs = atom_encoded_size(&entry->value);
    uint32_t rs = ks + vs;
    if (rs > 65535u) return 0;

    uint8_t* rec = (uint8_t*)malloc(rs);
    if (rec == NULL) return 0;
    uint32_t used = 0;
    if (!atom_encode(rec, rs, &used, &entry->key)) {
        free(rec);
        return 0;
    }
    uint32_t used2 = 0;
    if (!atom_encode(rec + used, rs - used, &used2, &entry->value)) {
        free(rec);
        return 0;
    }
    *out_rec = rec;
    *out_size = (uint16_t)rs;
    return 1;
}

static int encode_internal_record(const InternalEntry* entry, uint8_t** out_rec, uint16_t* out_size) {
    uint32_t ks = atom_encoded_size(&entry->key);
    uint32_t rs = ks + 4u;
    if (rs > 65535u) return 0;

    uint8_t* rec = (uint8_t*)malloc(rs);
    if (rec == NULL) return 0;
    uint32_t used = 0;
    if (!atom_encode(rec, rs, &used, &entry->key)) {
        free(rec);
        return 0;
    }
    wr_u32(rec + used, entry->child);
    *out_rec = rec;
    *out_size = (uint16_t)rs;
    return 1;
}

static int node_write(BTreeDb* db, uint32_t page_id, uint8_t type, uint32_t left_child,
                      LeafEntry* leaf_entries, InternalEntry* internal_entries, uint16_t nkeys) {
    uint8_t page[BTREE_PAGE_SIZE];
    page_init(page, type, left_child);

    for (uint16_t i = 0; i < nkeys; i++) {
        uint8_t* rec = NULL;
        uint16_t rec_size = 0;
        int ok = 0;
        if (type == BTREE_PAGE_TYPE_LEAF) {
            if (!encode_leaf_record(&leaf_entries[i], &rec, &rec_size)) return 0;
            ok = page_add_record(page, i, rec, rec_size);
        } else {
            if (!encode_internal_record(&internal_entries[i], &rec, &rec_size)) return 0;
            ok = page_add_record(page, i, rec, rec_size);
        }
        free(rec);
        if (!ok) return 0;
    }

    return db_write_page(db, page_id, page);
}

static int node_write_leaf(BTreeDb* db, uint32_t page_id, LeafEntry* entries, uint16_t nkeys) {
    return node_write(db, page_id, BTREE_PAGE_TYPE_LEAF, 0, entries, NULL, nkeys);
}

static int node_write_internal(BTreeDb* db, uint32_t page_id, uint32_t left_child,
                               InternalEntry* entries, uint16_t nkeys) {
    return node_write(db, page_id, BTREE_PAGE_TYPE_INTERNAL, left_child, NULL, entries, nkeys);
}

static int node_fits_leaf(LeafEntry* entries, uint16_t nkeys) {
    uint32_t used = BTREE_PAGE_HEADER_SIZE + (uint32_t)nkeys * BTREE_SLOT_SIZE;
    for (uint16_t i = 0; i < nkeys; i++) {
        used += atom_encoded_size(&entries[i].key) + atom_encoded_size(&entries[i].value);
        if (used > BTREE_PAGE_SIZE) return 0;
    }
    return 1;
}

static int node_fits_internal(InternalEntry* entries, uint16_t nkeys) {
    uint32_t used = BTREE_PAGE_HEADER_SIZE + (uint32_t)nkeys * BTREE_SLOT_SIZE;
    for (uint16_t i = 0; i < nkeys; i++) {
        used += atom_encoded_size(&entries[i].key) + 4u;
        if (used > BTREE_PAGE_SIZE) return 0;
    }
    return 1;
}

static int db_alloc_page(BTreeDb* db, uint32_t* out_page_id) {
    if (db->free_head != 0) {
        uint32_t page_id = db->free_head;
        uint8_t page[BTREE_PAGE_SIZE];
        if (!db_read_page(db, page_id, page)) return 0;
        if (page[0] != BTREE_PAGE_TYPE_FREE) return 0;

        db->free_head = rd_u32(page + 8);
        if (!db_write_header(db)) return 0;
        *out_page_id = page_id;
        return 1;
    }

    uint32_t page_id = db->page_count;
    db->page_count++;

    uint8_t zero[BTREE_PAGE_SIZE];
    memset(zero, 0, sizeof(zero));
    if (!db_write_page(db, page_id, zero)) {
        db->page_count--;
        return 0;
    }
    if (!db_write_header(db)) {
        db->page_count--;
        return 0;
    }

    *out_page_id = page_id;
    return 1;
}

static int db_free_page(BTreeDb* db, uint32_t page_id) {
    if (page_id == 0 || page_id >= db->page_count) return 0;
    if (page_id == db->root_page) return 0;

    uint8_t page[BTREE_PAGE_SIZE];
    page_init(page, BTREE_PAGE_TYPE_FREE, 0);
    wr_u32(page + 8, db->free_head);

    if (!db_write_page(db, page_id, page)) return 0;
    db->free_head = page_id;
    return db_write_header(db);
}

static int leaf_find_slot(LeafEntry* entries, uint16_t nkeys, const BTreeAtom* key, int* found) {
    int lo = 0;
    int hi = (int)nkeys;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = atom_compare(key, &entries[mid].key);
        if (cmp == 0) {
            *found = 1;
            return mid;
        }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    *found = 0;
    return lo;
}

static int internal_find_route(NodeData* node, const BTreeAtom* key, int* out_slot, uint32_t* out_child) {
    uint32_t child = node->left_child;
    int slot = 0;
    while (slot < node->nkeys) {
        int cmp = atom_compare(key, &node->internal_entries[slot].key);
        if (cmp < 0) break;
        child = node->internal_entries[slot].child;
        slot++;
    }
    *out_child = child;
    *out_slot = slot;
    return 1;
}

static int insert_recursive(BTreeDb* db, uint32_t page_id, const BTreeAtom* key, const BTreeAtom* value,
                            int* out_split, Promote* out_promote);
static int leaf_build_with_insert(NodeData* node, int pos, const BTreeAtom* key, const BTreeAtom* value,
                                  LeafEntry** out_entries, uint16_t* out_count);
static int internal_build_with_insert(NodeData* node, int insert_pos, const Promote* promote,
                                      InternalEntry** out_entries, uint16_t* out_count);

static int leaf_insert(BTreeDb* db, NodeData* node, const BTreeAtom* key, const BTreeAtom* value,
                       int* out_split, Promote* out_promote) {
    int found = 0;
    int pos = leaf_find_slot(node->leaf_entries, node->nkeys, key, &found);

    if (found) {
        BTreeAtom new_value;
        if (!atom_clone(&new_value, value)) return 0;
        atom_free(&node->leaf_entries[pos].value);
        node->leaf_entries[pos].value = new_value;

        if (!node_write_leaf(db, node->page_id, node->leaf_entries, node->nkeys)) return 0;
        *out_split = 0;
        return 1;
    }

    uint16_t new_count = 0;
    LeafEntry* all = NULL;
    if (!leaf_build_with_insert(node, pos, key, value, &all, &new_count)) return 0;

    if (node_fits_leaf(all, new_count)) {
        int ok = node_write_leaf(db, node->page_id, all, new_count);
        leaf_entries_free_array(all, new_count);
        if (!ok) return 0;
        *out_split = 0;
        return 1;
    }

    uint16_t mid = (uint16_t)(new_count / 2);
    if (mid == 0 || mid >= new_count) {
        leaf_entries_free_array(all, new_count);
        return 0;
    }

    uint16_t left_count = mid;
    uint16_t right_count = (uint16_t)(new_count - mid);

    uint32_t right_page = 0;
    if (!db_alloc_page(db, &right_page)) {
        leaf_entries_free_array(all, new_count);
        return 0;
    }

    int ok_left = node_write_leaf(db, node->page_id, all, left_count);
    int ok_right = node_write_leaf(db, right_page, all + mid, right_count);
    if (!ok_left || !ok_right) {
        leaf_entries_free_array(all, new_count);
        return 0;
    }

    atom_init(&out_promote->key);
    if (!atom_clone(&out_promote->key, &all[mid].key)) {
        leaf_entries_free_array(all, new_count);
        return 0;
    }
    out_promote->right_page = right_page;

    leaf_entries_free_array(all, new_count);

    *out_split = 1;
    return 1;
}

static int leaf_build_with_insert(NodeData* node, int pos, const BTreeAtom* key, const BTreeAtom* value,
                                  LeafEntry** out_entries, uint16_t* out_count) {
    uint16_t new_count = (uint16_t)(node->nkeys + 1);
    LeafEntry* all = (LeafEntry*)calloc(new_count, sizeof(LeafEntry));
    if (all == NULL) return 0;

    uint16_t ai = 0;
    for (uint16_t i = 0; i < node->nkeys; i++) {
        if ((int)i == pos) ai++;
        if (!atom_clone(&all[ai].key, &node->leaf_entries[i].key) ||
            !atom_clone(&all[ai].value, &node->leaf_entries[i].value)) {
            leaf_entries_free_array(all, new_count);
            return 0;
        }
        ai++;
    }

    if (!atom_clone(&all[pos].key, key) || !atom_clone(&all[pos].value, value)) {
        leaf_entries_free_array(all, new_count);
        return 0;
    }

    *out_entries = all;
    *out_count = new_count;
    return 1;
}

static int internal_insert(BTreeDb* db, NodeData* node, const BTreeAtom* key, const BTreeAtom* value,
                           int* out_split, Promote* out_promote) {
    uint32_t child_page = 0;
    int insert_pos = 0;
    if (!internal_find_route(node, key, &insert_pos, &child_page)) return 0;

    Promote child_promote;
    atom_init(&child_promote.key);
    int child_split = 0;
    if (!insert_recursive(db, child_page, key, value, &child_split, &child_promote)) {
        atom_free(&child_promote.key);
        return 0;
    }

    if (!child_split) {
        *out_split = 0;
        atom_free(&child_promote.key);
        return 1;
    }

    uint16_t new_count = 0;
    InternalEntry* all = NULL;
    if (!internal_build_with_insert(node, insert_pos, &child_promote, &all, &new_count)) {
        atom_free(&child_promote.key);
        return 0;
    }

    atom_free(&child_promote.key);

    if (node_fits_internal(all, new_count)) {
        int ok = node_write_internal(db, node->page_id, node->left_child, all, new_count);
        internal_entries_free_array(all, new_count);
        if (!ok) return 0;
        *out_split = 0;
        return 1;
    }

    uint16_t mid = (uint16_t)(new_count / 2);
    if (mid == 0 || mid >= new_count) {
        internal_entries_free_array(all, new_count);
        return 0;
    }

    uint32_t right_page = 0;
    if (!db_alloc_page(db, &right_page)) {
        internal_entries_free_array(all, new_count);
        return 0;
    }

    uint32_t right_left_child = all[mid].child;
    uint16_t left_count = mid;
    uint16_t right_count = (uint16_t)(new_count - mid - 1);

    int ok_left = node_write_internal(db, node->page_id, node->left_child, all, left_count);
    int ok_right = node_write_internal(db, right_page, right_left_child, all + mid + 1, right_count);
    if (!ok_left || !ok_right) {
        internal_entries_free_array(all, new_count);
        return 0;
    }

    atom_init(&out_promote->key);
    if (!atom_clone(&out_promote->key, &all[mid].key)) {
        internal_entries_free_array(all, new_count);
        return 0;
    }
    out_promote->right_page = right_page;

    internal_entries_free_array(all, new_count);

    *out_split = 1;
    return 1;
}

static int internal_build_with_insert(NodeData* node, int insert_pos, const Promote* promote,
                                      InternalEntry** out_entries, uint16_t* out_count) {
    uint16_t new_count = (uint16_t)(node->nkeys + 1);
    InternalEntry* all = (InternalEntry*)calloc(new_count, sizeof(InternalEntry));
    if (all == NULL) return 0;

    int inserted = 0;
    uint16_t dst = 0;
    for (uint16_t i = 0; i < node->nkeys; i++) {
        if (!inserted && insert_pos == (int)i) {
            if (!atom_clone(&all[dst].key, &promote->key)) {
                internal_entries_free_array(all, new_count);
                return 0;
            }
            all[dst].child = promote->right_page;
            dst++;
            inserted = 1;
        }
        if (!atom_clone(&all[dst].key, &node->internal_entries[i].key)) {
            internal_entries_free_array(all, new_count);
            return 0;
        }
        all[dst].child = node->internal_entries[i].child;
        dst++;
    }

    if (!inserted) {
        if (!atom_clone(&all[dst].key, &promote->key)) {
            internal_entries_free_array(all, new_count);
            return 0;
        }
        all[dst].child = promote->right_page;
    }

    *out_entries = all;
    *out_count = new_count;
    return 1;
}

static int insert_recursive(BTreeDb* db, uint32_t page_id, const BTreeAtom* key, const BTreeAtom* value,
                            int* out_split, Promote* out_promote) {
    NodeData node;
    if (!node_load(db, page_id, &node)) return 0;

    int ok = 0;
    if (node.type == BTREE_PAGE_TYPE_LEAF) {
        ok = leaf_insert(db, &node, key, value, out_split, out_promote);
    } else {
        ok = internal_insert(db, &node, key, value, out_split, out_promote);
    }

    node_data_free(&node);
    return ok;
}

static int btree_get_value(BTreeDb* db, const BTreeAtom* key, BTreeAtom* out) {
    atom_init(out);

    uint32_t page_id = db->root_page;
    for (;;) {
        NodeData node;
        if (!node_load(db, page_id, &node)) return 0;

        if (node.type == BTREE_PAGE_TYPE_LEAF) {
            int found = 0;
            int pos = leaf_find_slot(node.leaf_entries, node.nkeys, key, &found);
            if (!found) {
                node_data_free(&node);
                return 2;
            }
            int ok = atom_clone(out, &node.leaf_entries[pos].value);
            node_data_free(&node);
            return ok ? 1 : 0;
        }

        uint32_t child = node.left_child;
        for (uint16_t i = 0; i < node.nkeys; i++) {
            if (atom_compare(key, &node.internal_entries[i].key) < 0) break;
            child = node.internal_entries[i].child;
        }
        page_id = child;
        node_data_free(&node);
    }
}

static int btree_put(BTreeDb* db, const BTreeAtom* key, const BTreeAtom* value) {
    Promote promote;
    atom_init(&promote.key);
    int split = 0;

    if (!insert_recursive(db, db->root_page, key, value, &split, &promote)) {
        atom_free(&promote.key);
        return 0;
    }

    if (split) {
        uint32_t new_root_page = 0;
        if (!db_alloc_page(db, &new_root_page)) {
            atom_free(&promote.key);
            return 0;
        }

        InternalEntry e;
        atom_init(&e.key);
        if (!atom_clone(&e.key, &promote.key)) {
            atom_free(&promote.key);
            return 0;
        }
        e.child = promote.right_page;

        if (!node_write_internal(db, new_root_page, db->root_page, &e, 1)) {
            internal_entry_free(&e);
            atom_free(&promote.key);
            return 0;
        }
        internal_entry_free(&e);

        db->root_page = new_root_page;
        if (!db_write_header(db)) {
            atom_free(&promote.key);
            return 0;
        }
    }

    atom_free(&promote.key);
    return 1;
}

typedef struct {
    int deleted;
    int remove_child;
    uint32_t replacement_child;
    int min_changed;
    BTreeAtom new_min;
} DeleteResult;

static void delete_result_init(DeleteResult* r) {
    r->deleted = 0;
    r->remove_child = 0;
    r->replacement_child = 0;
    r->min_changed = 0;
    atom_init(&r->new_min);
}

static void delete_result_free(DeleteResult* r) {
    atom_free(&r->new_min);
}

static int subtree_min_key(BTreeDb* db, uint32_t page_id, BTreeAtom* out) {
    atom_init(out);
    for (;;) {
        NodeData node;
        if (!node_load(db, page_id, &node)) return 0;
        if (node.type == BTREE_PAGE_TYPE_LEAF) {
            if (node.nkeys == 0) {
                node_data_free(&node);
                return 0;
            }
            int ok = atom_clone(out, &node.leaf_entries[0].key);
            node_data_free(&node);
            return ok;
        }
        page_id = node.left_child;
        node_data_free(&node);
    }
}

static void leaf_remove_at(NodeData* node, uint16_t idx) {
    leaf_entry_free(&node->leaf_entries[idx]);
    for (uint16_t i = idx; i + 1 < node->nkeys; i++) {
        node->leaf_entries[i] = node->leaf_entries[i + 1];
    }
    atom_init(&node->leaf_entries[node->nkeys - 1].key);
    atom_init(&node->leaf_entries[node->nkeys - 1].value);
    node->nkeys--;
}

static void internal_remove_slot(NodeData* node, int slot) {
    if (slot == 0) {
        uint32_t new_left = node->internal_entries[0].child;
        internal_entry_free(&node->internal_entries[0]);
        for (uint16_t i = 0; i + 1 < node->nkeys; i++) {
            node->internal_entries[i] = node->internal_entries[i + 1];
        }
        atom_init(&node->internal_entries[node->nkeys - 1].key);
        node->internal_entries[node->nkeys - 1].child = 0;
        node->left_child = new_left;
        node->nkeys--;
        return;
    }

    uint16_t eidx = (uint16_t)(slot - 1);
    internal_entry_free(&node->internal_entries[eidx]);
    for (uint16_t i = eidx; i + 1 < node->nkeys; i++) {
        node->internal_entries[i] = node->internal_entries[i + 1];
    }
    atom_init(&node->internal_entries[node->nkeys - 1].key);
    node->internal_entries[node->nkeys - 1].child = 0;
    node->nkeys--;
}

static int btree_delete_recursive(BTreeDb* db, uint32_t page_id, const BTreeAtom* key,
                                  int is_root, DeleteResult* out);

static int btree_delete_leaf(BTreeDb* db, NodeData* node, const BTreeAtom* key,
                             int is_root, DeleteResult* out) {
    int found = 0;
    int pos = leaf_find_slot(node->leaf_entries, node->nkeys, key, &found);
    if (!found) {
        out->deleted = 0;
        return 1;
    }

    int removed_first = (pos == 0);
    leaf_remove_at(node, (uint16_t)pos);

    if (is_root) {
        if (!node_write_leaf(db, node->page_id, node->leaf_entries, node->nkeys)) return 0;
        out->deleted = 1;
        if (removed_first && node->nkeys > 0) {
            out->min_changed = atom_clone(&out->new_min, &node->leaf_entries[0].key);
            if (!out->min_changed) return 0;
        }
        return 1;
    }

    if (node->nkeys == 0) {
        if (!db_free_page(db, node->page_id)) return 0;
        out->deleted = 1;
        out->remove_child = 1;
        out->replacement_child = 0;
        return 1;
    }

    if (!node_write_leaf(db, node->page_id, node->leaf_entries, node->nkeys)) return 0;
    out->deleted = 1;
    if (removed_first) {
        out->min_changed = atom_clone(&out->new_min, &node->leaf_entries[0].key);
        if (!out->min_changed) return 0;
    }
    return 1;
}

static int btree_delete_internal(BTreeDb* db, NodeData* node, const BTreeAtom* key,
                                 int is_root, DeleteResult* out) {
    int slot = 0;
    uint32_t child_page = 0;
    internal_find_route(node, key, &slot, &child_page);

    DeleteResult child;
    delete_result_init(&child);
    if (!btree_delete_recursive(db, child_page, key, 0, &child)) {
        delete_result_free(&child);
        return 0;
    }

    if (!child.deleted) {
        out->deleted = 0;
        delete_result_free(&child);
        return 1;
    }

    if (child.remove_child) {
        if (child.replacement_child != 0) {
            if (slot == 0) node->left_child = child.replacement_child;
            else node->internal_entries[slot - 1].child = child.replacement_child;

            if (slot > 0) {
                atom_free(&node->internal_entries[slot - 1].key);
                if (!subtree_min_key(db, child.replacement_child, &node->internal_entries[slot - 1].key)) {
                    delete_result_free(&child);
                    return 0;
                }
            }
        } else {
            internal_remove_slot(node, slot);
        }
    } else if (child.min_changed && slot > 0) {
        atom_free(&node->internal_entries[slot - 1].key);
        if (!atom_clone(&node->internal_entries[slot - 1].key, &child.new_min)) {
            delete_result_free(&child);
            return 0;
        }
    }

    if (is_root) {
        if (node->nkeys == 0 && node->left_child != 0) {
            uint32_t old_root = node->page_id;
            db->root_page = node->left_child;
            if (!db_write_header(db)) {
                delete_result_free(&child);
                return 0;
            }
            if (old_root != db->root_page) {
                if (!db_free_page(db, old_root)) {
                    delete_result_free(&child);
                    return 0;
                }
            }
        } else {
            if (!node_write_internal(db, node->page_id, node->left_child, node->internal_entries, node->nkeys)) {
                delete_result_free(&child);
                return 0;
            }
        }

        out->deleted = 1;
        delete_result_free(&child);
        return 1;
    }

    if (node->nkeys == 0) {
        uint32_t promote_child = node->left_child;
        if (!db_free_page(db, node->page_id)) {
            delete_result_free(&child);
            return 0;
        }
        out->deleted = 1;
        out->remove_child = 1;
        out->replacement_child = promote_child;
        delete_result_free(&child);
        return 1;
    }

    if (!node_write_internal(db, node->page_id, node->left_child, node->internal_entries, node->nkeys)) {
        delete_result_free(&child);
        return 0;
    }

    out->deleted = 1;
    if (slot == 0) {
        out->min_changed = subtree_min_key(db, node->page_id, &out->new_min);
        if (!out->min_changed) {
            delete_result_free(&child);
            return 0;
        }
    }
    delete_result_free(&child);
    return 1;
}

static int btree_delete_recursive(BTreeDb* db, uint32_t page_id, const BTreeAtom* key,
                                  int is_root, DeleteResult* out) {
    NodeData node;
    if (!node_load(db, page_id, &node)) return 0;

    int ok = 0;
    if (node.type == BTREE_PAGE_TYPE_LEAF) {
        ok = btree_delete_leaf(db, &node, key, is_root, out);
    } else {
        ok = btree_delete_internal(db, &node, key, is_root, out);
    }

    node_data_free(&node);
    return ok;
}

static int btree_delete(BTreeDb* db, const BTreeAtom* key, int* deleted) {
    DeleteResult r;
    delete_result_init(&r);
    int ok = btree_delete_recursive(db, db->root_page, key, 1, &r);
    if (!ok) {
        delete_result_free(&r);
        return 0;
    }
    *deleted = r.deleted;
    delete_result_free(&r);
    return 1;
}

static int btree_open_file(const char* path, BTreeDb** out_db) {
    *out_db = NULL;
    BTreeDb* db = (BTreeDb*)calloc(1, sizeof(BTreeDb));
    if (db == NULL) return 0;

    db->path = (char*)malloc(strlen(path) + 1);
    if (db->path == NULL) {
        free(db);
        return 0;
    }
    strcpy(db->path, path);

    db->fp = fopen(path, "r+b");
    if (db->fp == NULL && errno == ENOENT) {
        db->fp = fopen(path, "w+b");
    }
    if (db->fp == NULL) {
        free(db->path);
        free(db);
        return 0;
    }

    if (fseek(db->fp, 0, SEEK_END) != 0) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    long size = ftell(db->fp);
    if (size < 0) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    if (size == 0) {
        db->root_page = 1;
        db->page_count = 2;
        db->free_head = 0;

        uint8_t root[BTREE_PAGE_SIZE];
        page_init(root, BTREE_PAGE_TYPE_LEAF, 0);

        if (!db_write_header(db) || !db_write_page(db, db->root_page, root)) {
            fclose(db->fp);
            free(db->path);
            free(db);
            return 0;
        }

        *out_db = db;
        return 1;
    }

    if ((uint32_t)size < BTREE_PAGE_SIZE) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    uint8_t header[BTREE_PAGE_SIZE];
    if (!db_read_page(db, 0, header)) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    if (memcmp(header, BTREE_MAGIC, 4) != 0 || header[4] != BTREE_VERSION) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    db->root_page = rd_u32(header + BTREE_HEADER_ROOT_PAGE_OFFSET);
    db->page_count = rd_u32(header + BTREE_HEADER_PAGE_COUNT_OFFSET);
    db->free_head = rd_u32(header + BTREE_HEADER_FREE_HEAD_OFFSET);
    if (db->root_page == 0 || db->page_count < 2 || db->root_page >= db->page_count) {
        fclose(db->fp);
        free(db->path);
        free(db);
        return 0;
    }

    *out_db = db;
    return 1;
}

static int btree_open_memory(BTreeDb** out_db) {
    *out_db = NULL;
    BTreeDb* db = (BTreeDb*)calloc(1, sizeof(BTreeDb));
    if (db == NULL) return 0;

    db->in_memory = 1;
    db->root_page = 1;
    db->page_count = 2;
    db->free_head = 0;
    if (!db_mem_ensure_pages(db, db->page_count)) {
        free(db);
        return 0;
    }

    uint8_t root[BTREE_PAGE_SIZE];
    page_init(root, BTREE_PAGE_TYPE_LEAF, 0);
    if (!db_write_header(db) || !db_write_page(db, db->root_page, root)) {
        free(db->mem_pages);
        free(db);
        return 0;
    }

    *out_db = db;
    return 1;
}

static void btree_close_db(BTreeDb* db) {
    if (db == NULL) return;
    if (db->fp != NULL) fclose(db->fp);
    free(db->mem_pages);
    free(db->path);
    free(db);
}

static void btree_userdata_finalizer(void* ptr) {
    BTreeDb* db = (BTreeDb*)ptr;
    btree_close_db(db);
}

static BTreeDb* get_db(ObjUserdata* udata) {
    if (udata == NULL || udata->data == NULL) return NULL;
    return (BTreeDb*)udata->data;
}

static BTreeDb* get_open_db_or_nil(Value* args) {
    BTreeDb* db = get_db(GET_USERDATA(0));
    if (db == NULL || db->closed) return NULL;
    return db;
}

static int parse_key_arg(VM* vm, Value* args, int index, BTreeAtom* out_key) {
    return atom_from_value(vm, args[index], "btree key", out_key);
}

static int return_atom_value(VM* vm, BTreeAtom* value) {
    if (value->type == BTREE_ATOM_NUMBER) {
        double n = value->number;
        atom_free(value);
        push(vm, NUMBER_VAL(n));
        return 1;
    }
    if (value->type == BTREE_ATOM_STRING) {
        ObjString* s = copy_string(value->string != NULL ? value->string : "", (int)value->string_len);
        atom_free(value);
        push(vm, OBJ_VAL(s));
        return 1;
    }
    atom_free(value);
    push(vm, NIL_VAL);
    return 1;
}

static int atom_to_value(const BTreeAtom* atom, Value* out) {
    if (atom->type == BTREE_ATOM_NUMBER) {
        *out = NUMBER_VAL(atom->number);
        return 1;
    }

    if (atom->type == BTREE_ATOM_STRING) {
        ObjString* s = copy_string(atom->string != NULL ? atom->string : "", (int)atom->string_len);
        *out = OBJ_VAL(s);
        return 1;
    }

    return 0;
}

static int btree_collect_range(VM* vm, BTreeDb* db, uint32_t page_id,
                               const BTreeAtom* min, const BTreeAtom* max,
                               ObjTable* out, int* index, int limit) {
    if (limit >= 0 && (*index - 1) >= limit) return 1;

    NodeData node;
    if (!node_load(db, page_id, &node)) return 0;

    if (node.type == BTREE_PAGE_TYPE_LEAF) {
        for (uint16_t i = 0; i < node.nkeys; i++) {
            if (limit >= 0 && (*index - 1) >= limit) break;
            LeafEntry* entry = &node.leaf_entries[i];

            if (min != NULL && atom_compare(&entry->key, min) < 0) continue;
            if (max != NULL && atom_compare(&entry->key, max) > 0) break;

            ObjTable* row = new_table();
            push(vm, OBJ_VAL(row));

            Value key_val;
            if (!atom_to_value(&entry->key, &key_val)) {
                pop(vm);
                node_data_free(&node);
                return 0;
            }

            Value value_val;
            if (!atom_to_value(&entry->value, &value_val)) {
                pop(vm);
                node_data_free(&node);
                return 0;
            }

            table_set(&row->table, copy_string("key", 3), key_val);
            table_set(&row->table, copy_string("value", 5), value_val);
            table_set_array(&out->table, *index, OBJ_VAL(row));
            (*index)++;

            pop(vm);
        }

        node_data_free(&node);
        return 1;
    }

    if (!btree_collect_range(vm, db, node.left_child, min, max, out, index, limit)) {
        node_data_free(&node);
        return 0;
    }

    for (uint16_t i = 0; i < node.nkeys; i++) {
        if (limit >= 0 && (*index - 1) >= limit) break;
        if (!btree_collect_range(vm, db, node.internal_entries[i].child, min, max, out, index, limit)) {
            node_data_free(&node);
            return 0;
        }
    }

    node_data_free(&node);
    return 1;
}

static int btree_open_native(VM* vm, int arg_count, Value* args) {
    BTreeDb* db = NULL;
    if (arg_count == 0) {
        if (!btree_open_memory(&db)) {
            vm_runtime_error(vm, "cannot open in-memory btree");
            return 0;
        }
    } else if (arg_count == 1) {
        ASSERT_STRING(0);
        if (!btree_open_file(GET_CSTRING(0), &db)) {
            vm_runtime_error(vm, "cannot open btree");
            return 0;
        }
    } else {
        vm_runtime_error(vm, "btree.open() expects 0 or 1 argument.");
        return 0;
    }

    ObjUserdata* udata = new_userdata_with_finalizer(db, btree_userdata_finalizer);

    Value mod = NIL_VAL;
    ObjString* mod_name = copy_string("btree", 5);
    if (table_get(&vm->globals, mod_name, &mod) && IS_TABLE(mod)) {
        Value mt = NIL_VAL;
        ObjString* mt_name = copy_string("_db_mt", 6);
        if (table_get(&AS_TABLE(mod)->table, mt_name, &mt) && IS_TABLE(mt)) {
            udata->metatable = AS_TABLE(mt);
        }
    }

    RETURN_OBJ(udata);
}

static int btree_put_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(3);
    ASSERT_USERDATA(0);

    BTreeDb* db = get_open_db_or_nil(args);
    if (db == NULL) RETURN_NIL;

    BTreeAtom key, value;
    if (!parse_key_arg(vm, args, 1, &key)) return 0;
    if (!atom_from_value(vm, args[2], "btree value", &value)) {
        atom_free(&key);
        return 0;
    }

    int ok = btree_put(db, &key, &value);
    atom_free(&key);
    atom_free(&value);

    if (!ok) {
        vm_runtime_error(vm, "btree.put failed.");
        return 0;
    }

    RETURN_VAL(args[0]);
}

static int btree_get_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);

    BTreeDb* db = get_open_db_or_nil(args);
    if (db == NULL) RETURN_NIL;

    BTreeAtom key;
    if (!parse_key_arg(vm, args, 1, &key)) return 0;

    BTreeAtom value;
    int res = btree_get_value(db, &key, &value);
    atom_free(&key);

    if (res == 2) RETURN_NIL;
    if (res == 0) {
        vm_runtime_error(vm, "btree.get failed.");
        return 0;
    }
    return return_atom_value(vm, &value);
}

static int btree_delete_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(2);
    ASSERT_USERDATA(0);

    BTreeDb* db = get_open_db_or_nil(args);
    if (db == NULL) RETURN_NIL;

    BTreeAtom key;
    if (!parse_key_arg(vm, args, 1, &key)) return 0;

    int deleted = 0;
    int ok = btree_delete(db, &key, &deleted);
    atom_free(&key);
    if (!ok) {
        vm_runtime_error(vm, "btree.delete failed.");
        return 0;
    }
    RETURN_BOOL(deleted);
}

static int btree_close_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_USERDATA(0);

    ObjUserdata* u = GET_USERDATA(0);
    BTreeDb* db = get_db(u);
    if (db == NULL) RETURN_TRUE;

    db->closed = 1;
    RETURN_TRUE;
}

static int btree_range_native(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    if (arg_count > 4) {
        vm_runtime_error(vm, "btree.range() expects at most 3 arguments.");
        return 0;
    }
    ASSERT_USERDATA(0);

    BTreeDb* db = get_open_db_or_nil(args);
    if (db == NULL) RETURN_NIL;

    BTreeAtom min_key, max_key;
    BTreeAtom* min = NULL;
    BTreeAtom* max = NULL;
    int limit = -1;
    atom_init(&min_key);
    atom_init(&max_key);

    if (arg_count >= 2 && !IS_NIL(args[1])) {
        if (!parse_key_arg(vm, args, 1, &min_key)) {
            atom_free(&min_key);
            atom_free(&max_key);
            return 0;
        }
        min = &min_key;
    }

    if (arg_count >= 3 && !IS_NIL(args[2])) {
        if (!parse_key_arg(vm, args, 2, &max_key)) {
            atom_free(&min_key);
            atom_free(&max_key);
            return 0;
        }
        max = &max_key;
    }

    if (arg_count >= 4 && !IS_NIL(args[3])) {
        if (!IS_NUMBER(args[3])) {
            atom_free(&min_key);
            atom_free(&max_key);
            vm_runtime_error(vm, "btree.range limit must be a non-negative integer.");
            return 0;
        }
        double n = AS_NUMBER(args[3]);
        if (n < 0 || floor(n) != n) {
            atom_free(&min_key);
            atom_free(&max_key);
            vm_runtime_error(vm, "btree.range limit must be a non-negative integer.");
            return 0;
        }
        limit = (int)n;
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));

    if (limit == 0) {
        atom_free(&min_key);
        atom_free(&max_key);
        RETURN_VAL(pop(vm));
    }

    if (min != NULL && max != NULL && atom_compare(min, max) > 0) {
        atom_free(&min_key);
        atom_free(&max_key);
        RETURN_VAL(pop(vm));
    }

    int index = 1;
    int ok = btree_collect_range(vm, db, db->root_page, min, max, out, &index, limit);
    atom_free(&min_key);
    atom_free(&max_key);
    if (!ok) {
        pop(vm);
        vm_runtime_error(vm, "btree.range failed.");
        return 0;
    }

    RETURN_VAL(pop(vm));
}

void register_btree(VM* vm) {
    const NativeReg funcs[] = {
        {"open", btree_open_native},
        {NULL, NULL}
    };
    register_module(vm, "btree", funcs);

    ObjTable* module = AS_TABLE(peek(vm, 0));
    ObjTable* mt = new_table();
    push(vm, OBJ_VAL(mt));

    const NativeReg methods[] = {
        {"put", btree_put_native},
        {"get", btree_get_native},
        {"delete", btree_delete_native},
        {"range", btree_range_native},
        {"close", btree_close_native},
        {NULL, NULL}
    };

    for (int i = 0; methods[i].name != NULL; i++) {
        ObjString* name = copy_string(methods[i].name, (int)strlen(methods[i].name));
        push(vm, OBJ_VAL(name));
        ObjNative* fn = new_native(methods[i].function, name);
        fn->is_self = 1;
        push(vm, OBJ_VAL(fn));
        table_set(&mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
        pop(vm);
        pop(vm);
    }

    ObjString* idx = copy_string("__index", 7);
    push(vm, OBJ_VAL(idx));
    push(vm, OBJ_VAL(mt));
    table_set(&mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("__name", 6)));
    push(vm, OBJ_VAL(copy_string("btree.db", 8)));
    table_set(&mt->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);

    push(vm, OBJ_VAL(copy_string("_db_mt", 6)));
    push(vm, OBJ_VAL(mt));
    table_set(&module->table, AS_STRING(peek(vm, 1)), peek(vm, 0));
    pop(vm);
    pop(vm);
    pop(vm); // mt

    pop(vm); // module
}
