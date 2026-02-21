#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "libs.h"
#include "../object.h"
#include "../value.h"
#include "../vm.h"

// HTTP status codes and reasons
static const char* get_status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// Parse HTTP request: http.parse(data) -> {method, path, version, headers, body}
static int http_parse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* data = GET_STRING(0);
    const char* src = data->chars;
    int len = data->length;

    // Find end of request line
    const char* line_end = strstr(src, "\r\n");
    if (!line_end) {
        RETURN_NIL;
    }

    // Parse request line: METHOD PATH VERSION
    const char* p = src;

    // Method
    const char* method_start = p;
    while (p < line_end && *p != ' ') p++;
    if (p >= line_end) { RETURN_NIL; }
    int method_len = p - method_start;
    p++; // skip space

    // Path
    const char* path_start = p;
    while (p < line_end && *p != ' ' && *p != '?') p++;
    int path_len = p - path_start;

    // Skip query string if present
    const char* query_start = NULL;
    int query_len = 0;
    if (*p == '?') {
        p++; // skip '?'
        query_start = p;
        while (p < line_end && *p != ' ') p++;
        query_len = p - query_start;
    }

    if (p >= line_end) { RETURN_NIL; }
    p++; // skip space

    // Version
    const char* version_start = p;
    int version_len = line_end - p;

    // Create result table
    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result));

    // Set method
    ObjString* method_key = copy_string("method", 6);
    push(vm, OBJ_VAL(method_key));
    ObjString* method_val = copy_string(method_start, method_len);
    push(vm, OBJ_VAL(method_val));
    table_set(&result->table, method_key, OBJ_VAL(method_val));
    pop(vm); pop(vm);

    // Set path
    ObjString* path_key = copy_string("path", 4);
    push(vm, OBJ_VAL(path_key));
    ObjString* path_val = copy_string(path_start, path_len);
    push(vm, OBJ_VAL(path_val));
    table_set(&result->table, path_key, OBJ_VAL(path_val));
    pop(vm); pop(vm);

    // Set query (if present)
    if (query_start) {
        ObjString* query_key = copy_string("query", 5);
        push(vm, OBJ_VAL(query_key));
        ObjString* query_val = copy_string(query_start, query_len);
        push(vm, OBJ_VAL(query_val));
        table_set(&result->table, query_key, OBJ_VAL(query_val));
        pop(vm); pop(vm);
    }

    // Set version
    ObjString* version_key = copy_string("version", 7);
    push(vm, OBJ_VAL(version_key));
    ObjString* version_val = copy_string(version_start, version_len);
    push(vm, OBJ_VAL(version_val));
    table_set(&result->table, version_key, OBJ_VAL(version_val));
    pop(vm); pop(vm);

    // Parse headers
    ObjTable* headers = new_table();
    push(vm, OBJ_VAL(headers));

    p = line_end + 2; // Skip \r\n
    while (p < src + len) {
        line_end = strstr(p, "\r\n");
        if (!line_end) break;

        // Empty line = end of headers
        if (line_end == p) {
            p += 2;
            break;
        }

        // Find colon
        const char* colon = strchr(p, ':');
        if (!colon || colon > line_end) {
            p = line_end + 2;
            continue;
        }

        // Header name (lowercase for consistency)
        int name_len = colon - p;
        char name_buf[name_len + 1];
        for (int i = 0; i < name_len; i++) {
            name_buf[i] = (char)tolower((unsigned char)p[i]);
        }
        name_buf[name_len] = '\0';

        // Header value (skip leading whitespace)
        const char* val_start = colon + 1;
        while (val_start < line_end && isspace((unsigned char)*val_start)) val_start++;
        int val_len = line_end - val_start;

        ObjString* hdr_key = copy_string(name_buf, name_len);
        push(vm, OBJ_VAL(hdr_key));
        ObjString* hdr_val = copy_string(val_start, val_len);
        push(vm, OBJ_VAL(hdr_val));
        table_set(&headers->table, hdr_key, OBJ_VAL(hdr_val));
        pop(vm); pop(vm);

        p = line_end + 2;
    }

    ObjString* headers_key = copy_string("headers", 7);
    push(vm, OBJ_VAL(headers_key));
    table_set(&result->table, headers_key, OBJ_VAL(headers));
    pop(vm);
    pop(vm); // headers table

    // Body is everything after headers
    int body_len = (src + len) - p;
    if (body_len > 0) {
        ObjString* body_key = copy_string("body", 4);
        push(vm, OBJ_VAL(body_key));
        ObjString* body_val = copy_string(p, body_len);
        push(vm, OBJ_VAL(body_val));
        table_set(&result->table, body_key, OBJ_VAL(body_val));
        pop(vm); pop(vm);
    }

    // Result table is already on stack
    return 1;
}

// Format HTTP response: http.response(status, headers, body) -> string
static int http_response(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1) {
        RETURN_NIL;
    }
    ASSERT_NUMBER(0);

    int status = (int)GET_NUMBER(0);
    const char* reason = get_status_reason(status);

    // Build response
    char status_line[64];
    int status_line_len = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n", status, reason);
    if (status_line_len < 0) {
        RETURN_NIL;
    }

    // Calculate total size
    size_t total_len = (size_t)status_line_len;

    // Headers
    ObjTable* headers = NULL;
    if (arg_count >= 2 && IS_TABLE(args[1])) {
        headers = GET_TABLE(1);
    }

    // Body
    const char* body = "";
    int body_len = 0;
    if (arg_count >= 3 && IS_STRING(args[2])) {
        body = AS_CSTRING(args[2]);
        body_len = AS_STRING(args[2])->length;
    }

    // Compute headers length.
    size_t headers_len = 0;
    if (headers) {
        for (int i = 0; i < headers->table.capacity; i++) {
            Entry* entry = &headers->table.entries[i];
            if (entry->key != NULL) {
                if (IS_STRING(entry->value)) {
                    ObjString* val = AS_STRING(entry->value);
                    headers_len += (size_t)entry->key->length + 2 + (size_t)val->length + 2;
                }
            }
        }
    }

    // Add Content-Length if body present
    char content_length[64] = "";
    int content_length_len = 0;
    if (body_len > 0) {
        content_length_len = snprintf(content_length, sizeof(content_length), "Content-Length: %d\r\n", body_len);
        if (content_length_len < 0) {
            RETURN_NIL;
        }
    }

    total_len += headers_len + (size_t)content_length_len + 2 + (size_t)body_len; // +2 for final \r\n
    char* response = (char*)malloc(total_len + 1);
    if (response == NULL) {
        RETURN_NIL;
    }

    char* p = response;
    memcpy(p, status_line, (size_t)status_line_len);
    p += status_line_len;

    if (headers) {
        for (int i = 0; i < headers->table.capacity; i++) {
            Entry* entry = &headers->table.entries[i];
            if (entry->key != NULL && IS_STRING(entry->value)) {
                ObjString* key = entry->key;
                ObjString* val = AS_STRING(entry->value);
                memcpy(p, key->chars, (size_t)key->length);
                p += key->length;
                memcpy(p, ": ", 2);
                p += 2;
                memcpy(p, val->chars, (size_t)val->length);
                p += val->length;
                memcpy(p, "\r\n", 2);
                p += 2;
            }
        }
    }

    if (content_length_len > 0) {
        memcpy(p, content_length, (size_t)content_length_len);
        p += content_length_len;
    }

    memcpy(p, "\r\n", 2);
    p += 2;

    if (body_len > 0) {
        memcpy(p, body, body_len);
        p += body_len;
    }

    *p = '\0';

    ObjString* result = copy_string(response, (int)(p - response));
    free(response);

    RETURN_OBJ(result);
}

// URL decode: http.urldecode(str) -> str
static int http_urldecode(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    char* output = malloc(input->length + 1);
    char* out = output;

    for (int i = 0; i < input->length; i++) {
        char c = input->chars[i];
        if (c == '%' && i + 2 < input->length) {
            char hex[3] = {input->chars[i+1], input->chars[i+2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (c == '+') {
            *out++ = ' ';
        } else {
            *out++ = c;
        }
    }
    *out = '\0';

    ObjString* result = copy_string(output, out - output);
    free(output);
    RETURN_OBJ(result);
}

// Parse query string: http.parsequery(str) -> table
static int http_parsequery(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* input = GET_STRING(0);
    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result));

    const char* p = input->chars;
    const char* end = p + input->length;

    while (p < end) {
        // Find key
        const char* key_start = p;
        while (p < end && *p != '=' && *p != '&') p++;
        int key_len = p - key_start;

        const char* val_start = p;
        int val_len = 0;

        if (p < end && *p == '=') {
            p++; // skip '='
            val_start = p;
            while (p < end && *p != '&') p++;
            val_len = p - val_start;
        }

        if (p < end && *p == '&') p++;

        if (key_len > 0) {
            ObjString* key = copy_string(key_start, key_len);
            push(vm, OBJ_VAL(key));
            ObjString* val = copy_string(val_start, val_len);
            push(vm, OBJ_VAL(val));
            table_set(&result->table, key, OBJ_VAL(val));
            pop(vm); pop(vm);
        }
    }

    return 1;
}

void register_http(VM* vm) {
    const NativeReg http_funcs[] = {
        {"parse", http_parse},
        {"response", http_response},
        {"urldecode", http_urldecode},
        {"parsequery", http_parsequery},
        {NULL, NULL}
    };

    register_module(vm, "http", http_funcs);
    pop(vm);
}
