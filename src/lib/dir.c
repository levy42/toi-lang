#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int push_error_pair(VM* vm) {
    push(vm, NIL_VAL);
    push(vm, OBJ_VAL(copy_string(strerror(errno), (int)strlen(strerror(errno)))));
    return 2;
}

static const char* dtype_name(unsigned char t) {
    switch (t) {
#ifdef DT_REG
        case DT_REG: return "file";
#endif
#ifdef DT_DIR
        case DT_DIR: return "dir";
#endif
#ifdef DT_LNK
        case DT_LNK: return "link";
#endif
#ifdef DT_BLK
        case DT_BLK: return "block";
#endif
#ifdef DT_CHR
        case DT_CHR: return "char";
#endif
#ifdef DT_FIFO
        case DT_FIFO: return "fifo";
#endif
#ifdef DT_SOCK
        case DT_SOCK: return "sock";
#endif
        default: return "unknown";
    }
}

static void set_bool_field(ObjTable* t, const char* key, int v) {
    table_set(&t->table, copy_string(key, (int)strlen(key)), BOOL_VAL(v ? 1 : 0));
}

static int dir_scandir(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* root = GET_CSTRING(0);
    DIR* dir = opendir(root);
    if (dir == NULL) {
        return push_error_pair(vm);
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));
    int idx = 1;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        ObjTable* row = new_table();
        push(vm, OBJ_VAL(row));

        table_set(&row->table, copy_string("name", 4),
                  OBJ_VAL(copy_string(ent->d_name, (int)strlen(ent->d_name))));

        size_t root_len = strlen(root);
        size_t name_len = strlen(ent->d_name);
        int has_trailing_slash = (root_len > 0 && root[root_len - 1] == '/');
        size_t full_len = root_len + (has_trailing_slash ? 0 : 1) + name_len;
        char* full = (char*)malloc(full_len + 1);
        if (full == NULL) {
            closedir(dir);
            vm_runtime_error(vm, "dir.scandir out of memory.");
            return 0;
        }
        memcpy(full, root, root_len);
        size_t off = root_len;
        if (!has_trailing_slash) {
            full[off++] = '/';
        }
        memcpy(full + off, ent->d_name, name_len);
        full[full_len] = '\0';
        table_set(&row->table, copy_string("path", 4), OBJ_VAL(copy_string(full, (int)full_len)));

        const char* tname = dtype_name(ent->d_type);
        table_set(&row->table, copy_string("type", 4), OBJ_VAL(copy_string(tname, (int)strlen(tname))));

        int is_dir = 0, is_file = 0, is_link = 0;
#ifdef DT_DIR
        is_dir = (ent->d_type == DT_DIR);
#endif
#ifdef DT_REG
        is_file = (ent->d_type == DT_REG);
#endif
#ifdef DT_LNK
        is_link = (ent->d_type == DT_LNK);
#endif

#ifdef DT_UNKNOWN
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_file = S_ISREG(st.st_mode);
#ifdef S_ISLNK
                is_link = S_ISLNK(st.st_mode);
#endif
            }
        }
#endif

        free(full);

        set_bool_field(row, "is_dir", is_dir);
        set_bool_field(row, "is_file", is_file);
        set_bool_field(row, "is_link", is_link);

        table_set_array(&out->table, idx++, OBJ_VAL(row));
        pop(vm); // row
    }

    closedir(dir);
    RETURN_OBJ(out);
}

static int dir_list(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    const char* root = GET_CSTRING(0);
    DIR* dir = opendir(root);
    if (dir == NULL) {
        return push_error_pair(vm);
    }

    ObjTable* out = new_table();
    push(vm, OBJ_VAL(out));
    int idx = 1;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        table_set_array(&out->table, idx++,
                        OBJ_VAL(copy_string(ent->d_name, (int)strlen(ent->d_name))));
    }
    closedir(dir);
    RETURN_OBJ(out);
}

void register_dir(VM* vm) {
    const NativeReg funcs[] = {
        {"scandir", dir_scandir},
        {"list", dir_list},
        {NULL, NULL}
    };
    register_module(vm, "dir", funcs);
    pop(vm);
}
