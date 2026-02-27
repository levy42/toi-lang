#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef TOI_WASM
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#endif

#ifdef TOI_HAVE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

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

static const char* find_crlf(const char* start, const char* end) {
    const char* p = start;
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
        p++;
    }
    return NULL;
}

static int parse_content_length(const char* s, int len, int* out) {
    int i = 0;
    int value = 0;

    while (i < len && isspace((unsigned char)s[i])) i++;
    if (i >= len) return 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        int digit = s[i] - '0';
        if (value > 214748364 || (value == 214748364 && digit > 7)) {
            return 0;
        }
        value = value * 10 + digit;
        i++;
    }

    while (i < len && isspace((unsigned char)s[i])) i++;
    if (i != len) return 0;

    *out = value;
    return 1;
}

static int has_csv_token_ci(const char* s, int len, const char* token) {
    int token_len = (int)strlen(token);
    int i = 0;

    while (i < len) {
        while (i < len && (s[i] == ',' || isspace((unsigned char)s[i]))) i++;
        if (i >= len) break;

        int start = i;
        while (i < len && s[i] != ',' && s[i] != ';') i++;
        int end = i;
        while (end > start && isspace((unsigned char)s[end - 1])) end--;

        if ((end - start) == token_len) {
            int ok = 1;
            for (int j = 0; j < token_len; j++) {
                char a = (char)tolower((unsigned char)s[start + j]);
                char b = (char)tolower((unsigned char)token[j]);
                if (a != b) {
                    ok = 0;
                    break;
                }
            }
            if (ok) return 1;
        }

        while (i < len && s[i] != ',') i++;
    }

    return 0;
}

// Returns: 1 success, 0 incomplete input, -1 invalid chunked framing.
static int decode_chunked_body(const char* body_start, const char* end,
                               char** out_body, int* out_len, int* out_consumed) {
    const char* cursor = body_start;
    int cap = (int)(end - body_start);
    if (cap < 0) cap = 0;
    char* decoded = (char*)malloc((size_t)cap + 1);
    if (decoded == NULL) return -1;
    int decoded_len = 0;

    while (1) {
        const char* line_end = find_crlf(cursor, end);
        if (line_end == NULL) {
            free(decoded);
            return 0;
        }

        const char* size_end = line_end;
        const char* semi = cursor;
        while (semi < line_end && *semi != ';') semi++;
        if (semi < line_end) size_end = semi;

        while (cursor < size_end && isspace((unsigned char)*cursor)) cursor++;
        while (size_end > cursor && isspace((unsigned char)size_end[-1])) size_end--;
        if (size_end == cursor) {
            free(decoded);
            return -1;
        }

        int chunk_size = 0;
        const char* p = cursor;
        while (p < size_end) {
            char c = *p;
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
            else {
                free(decoded);
                return -1;
            }

            if (chunk_size > 0x0FFFFFFF) {
                free(decoded);
                return -1;
            }
            chunk_size = (chunk_size << 4) | digit;
            p++;
        }

        cursor = line_end + 2;

        if (chunk_size == 0) {
            while (1) {
                const char* trailer_end = find_crlf(cursor, end);
                if (trailer_end == NULL) {
                    free(decoded);
                    return 0;
                }
                if (trailer_end == cursor) {
                    cursor += 2;
                    decoded[decoded_len] = '\0';
                    *out_body = decoded;
                    *out_len = decoded_len;
                    *out_consumed = (int)(cursor - body_start);
                    return 1;
                }
                cursor = trailer_end + 2;
            }
        }

        if ((int)(end - cursor) < chunk_size + 2) {
            free(decoded);
            return 0;
        }

        memcpy(decoded + decoded_len, cursor, (size_t)chunk_size);
        decoded_len += chunk_size;
        cursor += chunk_size;

        if (cursor[0] != '\r' || cursor[1] != '\n') {
            free(decoded);
            return -1;
        }
        cursor += 2;
    }
}

// Parse HTTP request: http.parse(data) -> {method, path, version, headers, body}
static int http_parse(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_EQ(1);
    ASSERT_STRING(0);

    ObjString* data = GET_STRING(0);
    const char* src = data->chars;
    int len = data->length;

    const char* src_end = src + len;

    // Find end of request line
    const char* line_end = find_crlf(src, src_end);
    if (!line_end) {
        RETURN_NIL;
    }

    // Parse request line: METHOD PATH VERSION
    const char* p = src;

    // Method
    const char* method_start = p;
    while (p < line_end && *p != ' ') p++;
    if (p >= line_end) { RETURN_FALSE; }
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

    if (p >= line_end) { RETURN_FALSE; }
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
    int content_length = -1;
    int transfer_chunked = 0;

    p = line_end + 2; // Skip \r\n
    while (p < src_end) {
        line_end = find_crlf(p, src_end);
        if (!line_end) {
            pop(vm); // headers table
            pop(vm); // result table
            RETURN_NIL;
        }

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

        if (name_len == 14 && memcmp(name_buf, "content-length", 14) == 0) {
            int parsed_len = 0;
            if (!parse_content_length(val_start, val_len, &parsed_len)) {
                pop(vm); // headers table
                pop(vm); // result table
                RETURN_FALSE;
            }
            content_length = parsed_len;
        } else if (name_len == 17 && memcmp(name_buf, "transfer-encoding", 17) == 0) {
            if (has_csv_token_ci(val_start, val_len, "chunked")) {
                transfer_chunked = 1;
            }
        }

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

    int body_offset = (int)(p - src);
    int body_len = 0;
    const char* body_ptr = p;
    char* chunked_body = NULL;
    int chunked_consumed = 0;
    int consumed = body_offset;

    if (transfer_chunked) {
        int chunk_status = decode_chunked_body(p, src_end, &chunked_body, &body_len, &chunked_consumed);
        if (chunk_status == 0) {
            pop(vm); // result table
            RETURN_NIL;
        } else if (chunk_status < 0) {
            pop(vm); // result table
            RETURN_FALSE;
        }
        body_ptr = chunked_body;
        consumed = body_offset + chunked_consumed;
    } else if (content_length >= 0) {
        int available = (int)(src_end - p);
        if (available < content_length) {
            pop(vm); // result table
            RETURN_NIL;
        }
        body_len = content_length;
        consumed = body_offset + content_length;
    } else {
        body_len = 0;
        consumed = body_offset;
    }

    if (body_len > 0) {
        ObjString* body_key = copy_string("body", 4);
        push(vm, OBJ_VAL(body_key));
        ObjString* body_val = copy_string(body_ptr, body_len);
        push(vm, OBJ_VAL(body_val));
        table_set(&result->table, body_key, OBJ_VAL(body_val));
        pop(vm); pop(vm);
    }

    if (chunked_body != NULL) {
        free(chunked_body);
    }

    ObjString* consumed_key = copy_string("consumed", 8);
    push(vm, OBJ_VAL(consumed_key));
    table_set(&result->table, consumed_key, NUMBER_VAL((double)consumed));
    pop(vm);

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

#ifndef TOI_WASM
typedef struct {
    char* host;
    int port;
    int use_tls;
    char* target;
} FetchUrl;

static int fetch_table_get(ObjTable* table, const char* key, Value* out) {
    ObjString* k = copy_string(key, (int)strlen(key));
    return table_get(&table->table, k, out);
}

static int fetch_header_has(ObjTable* headers, const char* key) {
    int key_len = (int)strlen(key);
    for (int i = 0; i < headers->table.capacity; i++) {
        Entry* entry = &headers->table.entries[i];
        if (entry->key == NULL || !IS_STRING(entry->value)) continue;
        if (entry->key->length != key_len) continue;
        int match = 1;
        for (int j = 0; j < key_len; j++) {
            char a = (char)tolower((unsigned char)entry->key->chars[j]);
            char b = (char)tolower((unsigned char)key[j]);
            if (a != b) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

static int parse_fetch_url(const ObjString* input, FetchUrl* out, const char** err) {
    const char* s = input->chars;
    const char* end = s + input->length;
    const char* scheme_end = strstr(s, "://");
    if (scheme_end == NULL) {
        *err = "URL must include scheme (http:// or https://)";
        return 0;
    }

    int scheme_len = (int)(scheme_end - s);
    if (scheme_len == 4 && memcmp(s, "http", 4) == 0) {
        out->use_tls = 0;
        out->port = 80;
    } else if (scheme_len == 5 && memcmp(s, "https", 5) == 0) {
        out->use_tls = 1;
        out->port = 443;
    } else {
        *err = "unsupported URL scheme";
        return 0;
    }

    const char* p = scheme_end + 3;
    if (p >= end) {
        *err = "URL host is empty";
        return 0;
    }

    const char* authority_end = p;
    while (authority_end < end && *authority_end != '/' && *authority_end != '?' && *authority_end != '#') {
        authority_end++;
    }
    if (authority_end == p) {
        *err = "URL host is empty";
        return 0;
    }

    const char* host_start = p;
    const char* host_end = authority_end;
    if (*host_start == '[') {
        const char* rb = host_start + 1;
        while (rb < authority_end && *rb != ']') rb++;
        if (rb >= authority_end) {
            *err = "invalid IPv6 host syntax";
            return 0;
        }
        host_start++;
        host_end = rb;
        if (rb + 1 < authority_end && rb[1] == ':') {
            const char* port_start = rb + 2;
            int port = 0;
            if (port_start >= authority_end) {
                *err = "invalid URL port";
                return 0;
            }
            for (const char* q = port_start; q < authority_end; q++) {
                if (!isdigit((unsigned char)*q)) {
                    *err = "invalid URL port";
                    return 0;
                }
                port = port * 10 + (*q - '0');
            }
            if (port <= 0 || port > 65535) {
                *err = "URL port out of range";
                return 0;
            }
            out->port = port;
        }
    } else {
        const char* colon = NULL;
        for (const char* q = host_start; q < authority_end; q++) {
            if (*q == ':') colon = q;
        }
        if (colon != NULL) {
            host_end = colon;
            const char* port_start = colon + 1;
            int port = 0;
            if (port_start >= authority_end) {
                *err = "invalid URL port";
                return 0;
            }
            for (const char* q = port_start; q < authority_end; q++) {
                if (!isdigit((unsigned char)*q)) {
                    *err = "invalid URL port";
                    return 0;
                }
                port = port * 10 + (*q - '0');
            }
            if (port <= 0 || port > 65535) {
                *err = "URL port out of range";
                return 0;
            }
            out->port = port;
        }
    }

    int host_len = (int)(host_end - host_start);
    if (host_len <= 0) {
        *err = "URL host is empty";
        return 0;
    }
    out->host = (char*)malloc((size_t)host_len + 1);
    if (out->host == NULL) {
        *err = "out of memory";
        return 0;
    }
    memcpy(out->host, host_start, (size_t)host_len);
    out->host[host_len] = '\0';

    const char* target_start = authority_end;
    if (target_start < end && *target_start == '#') {
        target_start = end;
    }
    const char* frag = target_start;
    while (frag < end && *frag != '#') frag++;
    int target_len = (int)(frag - target_start);
    if (target_len <= 0) {
        out->target = (char*)malloc(2);
        if (out->target == NULL) {
            free(out->host);
            out->host = NULL;
            *err = "out of memory";
            return 0;
        }
        out->target[0] = '/';
        out->target[1] = '\0';
    } else {
        if (*target_start != '/') {
            out->target = (char*)malloc((size_t)target_len + 2);
            if (out->target == NULL) {
                free(out->host);
                out->host = NULL;
                *err = "out of memory";
                return 0;
            }
            out->target[0] = '/';
            memcpy(out->target + 1, target_start, (size_t)target_len);
            out->target[target_len + 1] = '\0';
        } else {
            out->target = (char*)malloc((size_t)target_len + 1);
            if (out->target == NULL) {
                free(out->host);
                out->host = NULL;
                *err = "out of memory";
                return 0;
            }
            memcpy(out->target, target_start, (size_t)target_len);
            out->target[target_len] = '\0';
        }
    }

    return 1;
}

static void free_fetch_url(FetchUrl* u) {
    if (u->host != NULL) free(u->host);
    if (u->target != NULL) free(u->target);
    u->host = NULL;
    u->target = NULL;
}

static int parse_http_response_table(VM* vm, const char* src, int len, const char** err) {
    const char* src_end = src + len;
    const char* line_end = find_crlf(src, src_end);
    if (!line_end) {
        *err = "invalid HTTP response";
        return 0;
    }

    const char* p = src;
    const char* version_start = p;
    while (p < line_end && *p != ' ') p++;
    if (p >= line_end) {
        *err = "invalid HTTP status line";
        return 0;
    }
    int version_len = (int)(p - version_start);
    p++;

    const char* status_start = p;
    while (p < line_end && *p != ' ') p++;
    int status_len = (int)(p - status_start);
    if (status_len <= 0) {
        *err = "invalid HTTP status line";
        return 0;
    }
    int status = 0;
    for (int i = 0; i < status_len; i++) {
        if (!isdigit((unsigned char)status_start[i])) {
            *err = "invalid HTTP status code";
            return 0;
        }
        status = status * 10 + (status_start[i] - '0');
    }
    if (p < line_end && *p == ' ') p++;
    const char* reason_start = p;
    int reason_len = (int)(line_end - reason_start);

    ObjTable* result = new_table();
    push(vm, OBJ_VAL(result));

    ObjString* status_key = copy_string("status", 6);
    push(vm, OBJ_VAL(status_key));
    table_set(&result->table, status_key, NUMBER_VAL((double)status));
    pop(vm);

    ObjString* version_key = copy_string("version", 7);
    push(vm, OBJ_VAL(version_key));
    ObjString* version_val = copy_string(version_start, version_len);
    push(vm, OBJ_VAL(version_val));
    table_set(&result->table, version_key, OBJ_VAL(version_val));
    pop(vm);
    pop(vm);

    ObjString* reason_key = copy_string("reason", 6);
    push(vm, OBJ_VAL(reason_key));
    ObjString* reason_val = copy_string(reason_start, reason_len);
    push(vm, OBJ_VAL(reason_val));
    table_set(&result->table, reason_key, OBJ_VAL(reason_val));
    pop(vm);
    pop(vm);

    ObjTable* headers = new_table();
    push(vm, OBJ_VAL(headers));

    int content_length = -1;
    int transfer_chunked = 0;
    p = line_end + 2;
    while (p < src_end) {
        line_end = find_crlf(p, src_end);
        if (!line_end) {
            pop(vm);
            pop(vm);
            *err = "invalid HTTP headers";
            return 0;
        }
        if (line_end == p) {
            p += 2;
            break;
        }

        const char* colon = strchr(p, ':');
        if (!colon || colon > line_end) {
            p = line_end + 2;
            continue;
        }

        int name_len = (int)(colon - p);
        char name_buf[name_len + 1];
        for (int i = 0; i < name_len; i++) {
            name_buf[i] = (char)tolower((unsigned char)p[i]);
        }
        name_buf[name_len] = '\0';

        const char* val_start = colon + 1;
        while (val_start < line_end && isspace((unsigned char)*val_start)) val_start++;
        int val_len = (int)(line_end - val_start);

        if (name_len == 14 && memcmp(name_buf, "content-length", 14) == 0) {
            int parsed_len = 0;
            if (!parse_content_length(val_start, val_len, &parsed_len)) {
                pop(vm);
                pop(vm);
                *err = "invalid content-length";
                return 0;
            }
            content_length = parsed_len;
        } else if (name_len == 17 && memcmp(name_buf, "transfer-encoding", 17) == 0) {
            if (has_csv_token_ci(val_start, val_len, "chunked")) {
                transfer_chunked = 1;
            }
        }

        ObjString* hdr_key = copy_string(name_buf, name_len);
        push(vm, OBJ_VAL(hdr_key));
        ObjString* hdr_val = copy_string(val_start, val_len);
        push(vm, OBJ_VAL(hdr_val));
        table_set(&headers->table, hdr_key, OBJ_VAL(hdr_val));
        pop(vm);
        pop(vm);

        p = line_end + 2;
    }

    ObjString* headers_key = copy_string("headers", 7);
    push(vm, OBJ_VAL(headers_key));
    table_set(&result->table, headers_key, OBJ_VAL(headers));
    pop(vm);
    pop(vm);

    int body_offset = (int)(p - src);
    int body_len = 0;
    const char* body_ptr = p;
    char* chunked_body = NULL;
    int chunked_consumed = 0;

    if (transfer_chunked) {
        int chunk_status = decode_chunked_body(p, src_end, &chunked_body, &body_len, &chunked_consumed);
        if (chunk_status <= 0) {
            pop(vm);
            if (chunked_body != NULL) free(chunked_body);
            *err = "invalid chunked response body";
            return 0;
        }
        body_ptr = chunked_body;
        (void)chunked_consumed;
    } else {
        int available = (int)(src_end - p);
        if (content_length >= 0) {
            if (available < content_length) {
                pop(vm);
                *err = "truncated response body";
                return 0;
            }
            body_len = content_length;
        } else {
            body_len = available;
        }
    }

    ObjString* body_key = copy_string("body", 4);
    push(vm, OBJ_VAL(body_key));
    ObjString* body_val = copy_string(body_ptr, body_len);
    push(vm, OBJ_VAL(body_val));
    table_set(&result->table, body_key, OBJ_VAL(body_val));
    pop(vm);
    pop(vm);

    ObjString* consumed_key = copy_string("consumed", 8);
    push(vm, OBJ_VAL(consumed_key));
    table_set(&result->table, consumed_key, NUMBER_VAL((double)(body_offset + body_len)));
    pop(vm);

    if (chunked_body != NULL) free(chunked_body);
    return 1;
}

static int socket_connect_host(const char* host, int port, int timeout_ms, int* out_fd, const char** err) {
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int gai = getaddrinfo(host, port_buf, &hints, &res);
    if (gai != 0) {
        *err = "host lookup failed";
        return 0;
    }

    int fd = -1;
    for (struct addrinfo* it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;

        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        *err = "connect failed";
        return 0;
    }

    *out_fd = fd;
    return 1;
}

static int http_fetch(VM* vm, int arg_count, Value* args) {
    ASSERT_ARGC_GE(1);
    ASSERT_STRING(0);

    ObjString* raw_url = GET_STRING(0);
    ObjTable* options = NULL;
    if (arg_count >= 2 && IS_TABLE(args[1])) {
        options = GET_TABLE(1);
    }

    const char* method = "GET";
    int method_len = 3;
    ObjTable* headers = NULL;
    const char* body = NULL;
    int body_len = 0;
    int timeout_ms = 5000;
    int verify_tls = 0;

    if (options != NULL) {
        Value v;
        if (fetch_table_get(options, "method", &v) && IS_STRING(v)) {
            ObjString* m = AS_STRING(v);
            method = m->chars;
            method_len = m->length;
        }
        if (fetch_table_get(options, "headers", &v) && IS_TABLE(v)) {
            headers = AS_TABLE(v);
        }
        if (fetch_table_get(options, "body", &v) && IS_STRING(v)) {
            ObjString* b = AS_STRING(v);
            body = b->chars;
            body_len = b->length;
        }
        if (fetch_table_get(options, "timeout_ms", &v) && IS_NUMBER(v)) {
            timeout_ms = (int)AS_NUMBER(v);
            if (timeout_ms < 1) timeout_ms = 1;
        }
        if (fetch_table_get(options, "verify_tls", &v) && IS_BOOL(v)) {
            verify_tls = AS_BOOL(v) ? 1 : 0;
        }
    }

    FetchUrl url = {0};
    const char* err = NULL;
    if (!parse_fetch_url(raw_url, &url, &err)) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(err, (int)strlen(err))));
        return 2;
    }

    int fd = -1;
    if (!socket_connect_host(url.host, url.port, timeout_ms, &fd, &err)) {
        free_fetch_url(&url);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(err, (int)strlen(err))));
        return 2;
    }

#ifdef TOI_HAVE_TLS
    SSL_CTX* tls_ctx = NULL;
    SSL* tls = NULL;
#endif

    if (url.use_tls) {
#ifdef TOI_HAVE_TLS
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();

        tls_ctx = SSL_CTX_new(TLS_client_method());
        if (tls_ctx == NULL) {
            close(fd);
            free_fetch_url(&url);
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("failed to create TLS context", 28)));
            return 2;
        }
        if (verify_tls) {
            SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_PEER, NULL);
            if (SSL_CTX_set_default_verify_paths(tls_ctx) != 1) {
                SSL_CTX_free(tls_ctx);
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("failed to load system CA certs", 30)));
                return 2;
            }
        } else {
            SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_NONE, NULL);
        }

        tls = SSL_new(tls_ctx);
        if (tls == NULL) {
            SSL_CTX_free(tls_ctx);
            close(fd);
            free_fetch_url(&url);
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("failed to create TLS handle", 27)));
            return 2;
        }
        SSL_set_fd(tls, fd);
        SSL_set_tlsext_host_name(tls, url.host);
        if (SSL_connect(tls) != 1) {
            SSL_free(tls);
            SSL_CTX_free(tls_ctx);
            close(fd);
            free_fetch_url(&url);
            push(vm, NIL_VAL);
            push(vm, OBJ_VAL(copy_string("TLS handshake failed", 20)));
            return 2;
        }
#else
        close(fd);
        free_fetch_url(&url);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("https unsupported (built without TLS)", 36)));
        return 2;
#endif
    }

    int has_host = headers != NULL && fetch_header_has(headers, "host");
    int has_conn = headers != NULL && fetch_header_has(headers, "connection");
    int has_clen = headers != NULL && fetch_header_has(headers, "content-length");

    size_t req_len = (size_t)method_len + 1 + strlen(url.target) + 11;
    req_len += 2;
    req_len += has_host ? 0 : strlen("Host: \r\n") + strlen(url.host) + 8;
    req_len += has_conn ? 0 : strlen("Connection: close\r\n");
    req_len += body_len > 0 && !has_clen ? strlen("Content-Length: \r\n") + 20 : 0;

    if (headers != NULL) {
        for (int i = 0; i < headers->table.capacity; i++) {
            Entry* entry = &headers->table.entries[i];
            if (entry->key == NULL || !IS_STRING(entry->value)) continue;
            ObjString* v = AS_STRING(entry->value);
            req_len += (size_t)entry->key->length + 2 + (size_t)v->length + 2;
        }
    }
    req_len += (size_t)body_len;

    char* req = (char*)malloc(req_len + 1);
    if (req == NULL) {
#ifdef TOI_HAVE_TLS
        if (tls != NULL) SSL_free(tls);
        if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
        close(fd);
        free_fetch_url(&url);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("out of memory", 13)));
        return 2;
    }

    int n = snprintf(req, req_len + 1, "%.*s %s HTTP/1.1\r\n", method_len, method, url.target);
    size_t off = (size_t)n;
    if (!has_host) {
        if ((url.use_tls && url.port != 443) || (!url.use_tls && url.port != 80)) {
            n = snprintf(req + off, req_len + 1 - off, "Host: %s:%d\r\n", url.host, url.port);
        } else {
            n = snprintf(req + off, req_len + 1 - off, "Host: %s\r\n", url.host);
        }
        off += (size_t)n;
    }
    if (!has_conn) {
        n = snprintf(req + off, req_len + 1 - off, "Connection: close\r\n");
        off += (size_t)n;
    }
    if (body_len > 0 && !has_clen) {
        n = snprintf(req + off, req_len + 1 - off, "Content-Length: %d\r\n", body_len);
        off += (size_t)n;
    }
    if (headers != NULL) {
        for (int i = 0; i < headers->table.capacity; i++) {
            Entry* entry = &headers->table.entries[i];
            if (entry->key == NULL || !IS_STRING(entry->value)) continue;
            ObjString* v = AS_STRING(entry->value);
            memcpy(req + off, entry->key->chars, (size_t)entry->key->length);
            off += (size_t)entry->key->length;
            memcpy(req + off, ": ", 2);
            off += 2;
            memcpy(req + off, v->chars, (size_t)v->length);
            off += (size_t)v->length;
            memcpy(req + off, "\r\n", 2);
            off += 2;
        }
    }
    memcpy(req + off, "\r\n", 2);
    off += 2;
    if (body_len > 0) {
        memcpy(req + off, body, (size_t)body_len);
        off += (size_t)body_len;
    }
    req[off] = '\0';

    size_t sent_total = 0;
    while (sent_total < off) {
        int sent = 0;
#ifdef TOI_HAVE_TLS
        if (tls != NULL) {
            sent = SSL_write(tls, req + sent_total, (int)(off - sent_total));
            if (sent <= 0) {
                int ssl_err = SSL_get_error(tls, sent);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) continue;
                free(req);
                SSL_free(tls);
                SSL_CTX_free(tls_ctx);
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("send failed", 11)));
                return 2;
            }
        } else
#endif
        {
            sent = (int)send(fd, req + sent_total, off - sent_total, 0);
            if (sent < 0) {
                if (errno == EINTR) continue;
                free(req);
#ifdef TOI_HAVE_TLS
                if (tls != NULL) SSL_free(tls);
                if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("send failed", 11)));
                return 2;
            }
        }
        sent_total += (size_t)sent;
    }
    free(req);

    size_t cap = 8192;
    size_t len = 0;
    char* resp = (char*)malloc(cap);
    if (resp == NULL) {
#ifdef TOI_HAVE_TLS
        if (tls != NULL) SSL_free(tls);
        if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
        close(fd);
        free_fetch_url(&url);
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string("out of memory", 13)));
        return 2;
    }

    while (1) {
        if (len + 4096 + 1 > cap) {
            size_t next = cap * 2;
            char* grown = (char*)realloc(resp, next);
            if (grown == NULL) {
                free(resp);
#ifdef TOI_HAVE_TLS
                if (tls != NULL) SSL_free(tls);
                if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("out of memory", 13)));
                return 2;
            }
            resp = grown;
            cap = next;
        }

        int received = 0;
#ifdef TOI_HAVE_TLS
        if (tls != NULL) {
            received = SSL_read(tls, resp + len, 4096);
            if (received <= 0) {
                int ssl_err = SSL_get_error(tls, received);
                if (ssl_err == SSL_ERROR_ZERO_RETURN) break;
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) continue;
                free(resp);
                SSL_free(tls);
                SSL_CTX_free(tls_ctx);
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("recv failed", 11)));
                return 2;
            }
        } else
#endif
        {
            received = (int)recv(fd, resp + len, 4096, 0);
            if (received == 0) break;
            if (received < 0) {
                if (errno == EINTR) continue;
                free(resp);
#ifdef TOI_HAVE_TLS
                if (tls != NULL) SSL_free(tls);
                if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
                close(fd);
                free_fetch_url(&url);
                push(vm, NIL_VAL);
                push(vm, OBJ_VAL(copy_string("recv failed", 11)));
                return 2;
            }
        }

        len += (size_t)received;
    }

#ifdef TOI_HAVE_TLS
    if (tls != NULL) SSL_free(tls);
    if (tls_ctx != NULL) SSL_CTX_free(tls_ctx);
#endif
    close(fd);
    free_fetch_url(&url);

    const char* parse_err = NULL;
    int ok = parse_http_response_table(vm, resp, (int)len, &parse_err);
    free(resp);
    if (!ok) {
        push(vm, NIL_VAL);
        push(vm, OBJ_VAL(copy_string(parse_err, (int)strlen(parse_err))));
        return 2;
    }
    return 1;
}
#endif

void register_http(VM* vm) {
    const NativeReg http_funcs[] = {
        {"parse", http_parse},
        {"response", http_response},
        {"urldecode", http_urldecode},
        {"parsequery", http_parsequery},
#ifndef TOI_WASM
        {"fetch", http_fetch},
#endif
        {NULL, NULL}
    };

    register_module(vm, "http", http_funcs);
    pop(vm);
}
