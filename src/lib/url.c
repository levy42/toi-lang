#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

static int is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int is_scheme_char(char c) {
    return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.';
}

static int is_unreserved(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static void table_set_string(VM* vm, ObjTable* table, const char* key, const char* s, int len) {
    ObjString* k = copy_string(key, (int)strlen(key));
    push(vm, OBJ_VAL(k));
    ObjString* v = copy_string(s, len);
    push(vm, OBJ_VAL(v));
    table_set(&table->table, k, OBJ_VAL(v));
    pop(vm);
    pop(vm);
}

static void table_set_number(VM* vm, ObjTable* table, const char* key, double n) {
    ObjString* k = copy_string(key, (int)strlen(key));
    push(vm, OBJ_VAL(k));
    table_set(&table->table, k, NUMBER_VAL(n));
    pop(vm);
}

// url.decode(str) -> string
static int url_decode(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    char* out = (char*)malloc((size_t)input->length + 1);
    if (out == NULL) RETURN_NIL;

    int j = 0;
    for (int i = 0; i < input->length; i++) {
        char c = input->chars[i];
        if (c == '%' && i + 2 < input->length &&
            is_hex(input->chars[i + 1]) && is_hex(input->chars[i + 2])) {
            int hi = hex_to_int(input->chars[i + 1]);
            int lo = hex_to_int(input->chars[i + 2]);
            out[j++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';

    ObjString* result = copy_string(out, j);
    free(out);
    RETURN_OBJ(result);
}

// url.encode(str) -> string
static int url_encode(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    size_t cap = (size_t)input->length * 3 + 1;
    char* out = (char*)malloc(cap);
    if (out == NULL) RETURN_NIL;

    static const char HEX[] = "0123456789ABCDEF";
    int j = 0;
    for (int i = 0; i < input->length; i++) {
        unsigned char c = (unsigned char)input->chars[i];
        if (is_unreserved((char)c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = HEX[(c >> 4) & 0x0F];
            out[j++] = HEX[c & 0x0F];
        }
    }
    out[j] = '\0';

    ObjString* result = copy_string(out, j);
    free(out);
    RETURN_OBJ(result);
}

// url.parse(url) -> table
static int url_parse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    const char* src = input->chars;
    const char* end = src + input->length;
    const char* p = src;

    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result));

    const char* scheme_start = p;
    const char* scheme_end = NULL;
    if (p < end && isalpha((unsigned char)*p)) {
        p++;
        while (p < end && is_scheme_char(*p)) p++;
        if (p < end && *p == ':') {
            scheme_end = p;
            p++;
        } else {
            p = src;
        }
    }

    if (scheme_end != NULL) {
        int len = (int)(scheme_end - scheme_start);
        char* lower = (char*)malloc((size_t)len + 1);
        if (lower == NULL) {
            pop(vm);
            RETURN_NIL;
        }
        for (int i = 0; i < len; i++) {
            lower[i] = (char)tolower((unsigned char)scheme_start[i]);
        }
        lower[len] = '\0';
        table_set_string(vm, result, "scheme", lower, len);
        free(lower);
    }

    int has_authority = 0;
    if (p + 1 < end && p[0] == '/' && p[1] == '/') {
        has_authority = 1;
        p += 2;
    }

    if (has_authority) {
        const char* auth_start = p;
        while (p < end && *p != '/' && *p != '?' && *p != '#') p++;
        const char* auth_end = p;

        if (auth_end > auth_start) {
            const char* at = NULL;
            for (const char* q = auth_start; q < auth_end; q++) {
                if (*q == '@') at = q;
            }

            const char* host_start = auth_start;
            if (at != NULL) {
                table_set_string(vm, result, "userinfo", auth_start, (int)(at - auth_start));
                host_start = at + 1;
            }

            const char* host_end = auth_end;
            int port = -1;

            if (host_start < auth_end && *host_start == '[') {
                const char* rb = host_start + 1;
                while (rb < auth_end && *rb != ']') rb++;
                if (rb < auth_end) {
                    table_set_string(vm, result, "host", host_start + 1, (int)(rb - host_start - 1));
                    host_end = rb + 1;
                    if (host_end < auth_end && *host_end == ':') {
                        const char* port_start = host_end + 1;
                        int ok = (port_start < auth_end);
                        int v = 0;
                        for (const char* q = port_start; q < auth_end; q++) {
                            if (!isdigit((unsigned char)*q)) {
                                ok = 0;
                                break;
                            }
                            v = v * 10 + (*q - '0');
                        }
                        if (ok) port = v;
                    }
                } else {
                    table_set_string(vm, result, "host", host_start, (int)(auth_end - host_start));
                }
            } else {
                const char* colon = NULL;
                for (const char* q = host_start; q < auth_end; q++) {
                    if (*q == ':') colon = q;
                }
                if (colon != NULL) {
                    table_set_string(vm, result, "host", host_start, (int)(colon - host_start));
                    int ok = (colon + 1 < auth_end);
                    int v = 0;
                    for (const char* q = colon + 1; q < auth_end; q++) {
                        if (!isdigit((unsigned char)*q)) {
                            ok = 0;
                            break;
                        }
                        v = v * 10 + (*q - '0');
                    }
                    if (ok) port = v;
                } else {
                    table_set_string(vm, result, "host", host_start, (int)(auth_end - host_start));
                }
            }

            if (port >= 0) {
                table_set_number(vm, result, "port", (double)port);
            } else {
                Value scheme_val;
                ObjString* scheme_key = copy_string("scheme", 6);
                if (table_get(&result->table, scheme_key, &scheme_val) && IS_STRING(scheme_val)) {
                    ObjString* scheme = AS_STRING(scheme_val);
                    if (scheme->length == 4 && memcmp(scheme->chars, "http", 4) == 0) {
                        table_set_number(vm, result, "port", 80);
                    } else if (scheme->length == 5 && memcmp(scheme->chars, "https", 5) == 0) {
                        table_set_number(vm, result, "port", 443);
                    }
                }
            }
        }
    }

    const char* path_start = p;
    const char* query_start = NULL;
    const char* frag_start = NULL;

    if (p < end && *p == '?') {
        query_start = p + 1;
        path_start = NULL;
    } else if (p < end && *p == '#') {
        frag_start = p + 1;
        path_start = NULL;
    }

    if (path_start != NULL) {
        while (p < end && *p != '?' && *p != '#') p++;
    }

    const char* path_end = p;
    if (p < end && *p == '?') {
        query_start = p + 1;
        p++;
        while (p < end && *p != '#') p++;
    }
    const char* query_end = p;
    if (p < end && *p == '#') {
        frag_start = p + 1;
        p++;
        while (p < end) p++;
    }
    const char* frag_end = p;

    if (path_start != NULL) {
        int path_len = (int)(path_end - path_start);
        if (path_len > 0) {
            table_set_string(vm, result, "path", path_start, path_len);
        } else {
            table_set_string(vm, result, "path", has_authority ? "/" : "", has_authority ? 1 : 0);
        }
    } else {
        table_set_string(vm, result, "path", has_authority ? "/" : "", has_authority ? 1 : 0);
    }

    if (query_start != NULL && query_end > query_start) {
        table_set_string(vm, result, "query", query_start, (int)(query_end - query_start));
    }
    if (frag_start != NULL && frag_end > frag_start) {
        table_set_string(vm, result, "fragment", frag_start, (int)(frag_end - frag_start));
    }

    return 1;
}

void register_url(VM* vm) {
    const NativeReg url_funcs[] = {
        {"parse", url_parse},
        {"encode", url_encode},
        {"decode", url_decode},
        {NULL, NULL}
    };

    register_module(vm, "url", url_funcs);
    pop(vm);
}
