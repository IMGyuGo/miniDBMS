#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_message.h"

#define HTTP_RESPONSE_BUFFER_MIN 64

static void copy_text(char *dst, size_t dst_size, const char *src) {
    size_t len = 0;

    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void init_error_meta(ApiResponseMeta *meta) {
    if (!meta) return;
    memset(meta, 0, sizeof(*meta));
    meta->http_status = 500;
    meta->code = API_CODE_INTERNAL_ERROR;
}

static void init_success_meta(ApiResponseMeta *meta) {
    if (!meta) return;
    memset(meta, 0, sizeof(*meta));
    meta->http_status = 200;
    meta->code = API_CODE_OK;
    meta->ok = 1;
}

static void set_error_meta(ApiResponseMeta *meta,
                           int http_status,
                           ApiCode code,
                           const char *message) {
    char request_id[API_REQUEST_ID_MAX];

    if (!meta) return;

    copy_text(request_id, sizeof(request_id), meta->request_id);
    memset(meta, 0, sizeof(*meta));
    meta->http_status = http_status;
    meta->code = code;
    meta->ok = 0;
    meta->has_payload = 0;
    copy_text(meta->request_id, sizeof(meta->request_id), request_id);
    copy_text(meta->error, sizeof(meta->error), message);
}

static void set_meta_request_id(ApiResponseMeta *meta, const char *request_id) {
    if (!meta) return;
    copy_text(meta->request_id, sizeof(meta->request_id), request_id);
}

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static int is_json_delimiter(char ch) {
    return ch == '\0' || ch == ',' || ch == '}' || isspace((unsigned char)ch);
}

static const char *find_json_key(const char *body, const char *key) {
    char pattern[128];
    const char *found = NULL;
    const char *colon = NULL;

    if (!body || !key) return NULL;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    found = strstr(body, pattern);
    if (!found) return NULL;

    colon = strchr(found + strlen(pattern), ':');
    if (!colon) return NULL;

    return skip_ws(colon + 1);
}

static int json_key_exists(const char *body, const char *key) {
    return find_json_key(body, key) != NULL;
}

static int parse_json_string_field(const char *body,
                                   const char *key,
                                   char *out,
                                   size_t out_size) {
    const char *p = find_json_key(body, key);
    size_t written = 0;

    if (!p || *p != '"') return 0;
    p++;

    while (*p && *p != '"') {
        char ch = *p++;

        if (ch == '\\') {
            if (!*p) return 0;
            ch = *p++;
            switch (ch) {
                case '"': break;
                case '\\': break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default: return 0;
            }
        }

        if (written + 1 >= out_size) return 0;
        out[written++] = ch;
    }

    if (*p != '"') return 0;
    out[written] = '\0';
    return 1;
}

static int parse_json_bool_field(const char *body, const char *key, int *out) {
    const char *p = find_json_key(body, key);

    if (!p || !out) return 0;

    if (strncmp(p, "true", 4) == 0 && is_json_delimiter(p[4])) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0 && is_json_delimiter(p[5])) {
        *out = 0;
        return 1;
    }
    if (*p == '1' && is_json_delimiter(p[1])) {
        *out = 1;
        return 1;
    }
    if (*p == '0' && is_json_delimiter(p[1])) {
        *out = 0;
        return 1;
    }

    return 0;
}

static int is_blank_text(const char *text) {
    const unsigned char *p = (const unsigned char *)text;

    if (!p) return 1;

    while (*p) {
        if (!isspace(*p)) return 0;
        p++;
    }

    return 1;
}

static ApiMethod parse_method(const char *method) {
    if (!method) return API_METHOD_UNKNOWN;
    if (strcmp(method, "GET") == 0) return API_METHOD_GET;
    if (strcmp(method, "POST") == 0) return API_METHOD_POST;
    return API_METHOD_UNKNOWN;
}

static ApiRouteKind parse_route(const char *path) {
    if (!path) return API_ROUTE_KIND_UNKNOWN;
    if (strcmp(path, API_ROUTE_QUERY) == 0) return API_ROUTE_KIND_QUERY;
    if (strcmp(path, API_ROUTE_HEALTH) == 0) return API_ROUTE_KIND_HEALTH;
    return API_ROUTE_KIND_UNKNOWN;
}

static const char *api_code_name(ApiCode code) {
    switch (code) {
        case API_CODE_OK: return "OK";
        case API_CODE_BAD_REQUEST: return "BAD_REQUEST";
        case API_CODE_UNSUPPORTED_ROUTE: return "UNSUPPORTED_ROUTE";
        case API_CODE_UNSUPPORTED_METHOD: return "UNSUPPORTED_METHOD";
        case API_CODE_PARSE_ERROR: return "PARSE_ERROR";
        case API_CODE_SCHEMA_ERROR: return "SCHEMA_ERROR";
        case API_CODE_EXEC_ERROR: return "EXEC_ERROR";
        case API_CODE_QUEUE_FULL: return "QUEUE_FULL";
        case API_CODE_INTERNAL_ERROR: return "INTERNAL_ERROR";
        default: return "UNKNOWN";
    }
}

static ApiCode api_code_from_service(DBServiceStatus status) {
    switch (status) {
        case DB_SERVICE_OK: return API_CODE_OK;
        case DB_SERVICE_BAD_REQUEST: return API_CODE_BAD_REQUEST;
        case DB_SERVICE_PARSE_ERROR: return API_CODE_PARSE_ERROR;
        case DB_SERVICE_SCHEMA_ERROR: return API_CODE_SCHEMA_ERROR;
        case DB_SERVICE_EXEC_ERROR: return API_CODE_EXEC_ERROR;
        case DB_SERVICE_UNSUPPORTED: return API_CODE_BAD_REQUEST;
        case DB_SERVICE_INTERNAL_ERROR:
        default:
            return API_CODE_INTERNAL_ERROR;
    }
}

static int http_status_from_service(DBServiceStatus status) {
    switch (status) {
        case DB_SERVICE_OK: return 200;
        case DB_SERVICE_BAD_REQUEST: return 400;
        case DB_SERVICE_PARSE_ERROR: return 400;
        case DB_SERVICE_SCHEMA_ERROR: return 400;
        case DB_SERVICE_EXEC_ERROR: return 500;
        case DB_SERVICE_UNSUPPORTED: return 400;
        case DB_SERVICE_INTERNAL_ERROR:
        default:
            return 500;
    }
}

static int append_format(char *buffer,
                         size_t buffer_size,
                         size_t *offset,
                         const char *fmt,
                         ...) {
    va_list args;
    int written = 0;

    if (!buffer || !offset || *offset >= buffer_size) return 0;

    va_start(args, fmt);
    written = vsnprintf(buffer + *offset, buffer_size - *offset, fmt, args);
    va_end(args);

    if (written < 0) return 0;
    if ((size_t)written >= buffer_size - *offset) return 0;

    *offset += (size_t)written;
    return 1;
}

static int append_json_escaped(char *buffer,
                               size_t buffer_size,
                               size_t *offset,
                               const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    if (!append_format(buffer, buffer_size, offset, "\"")) return 0;

    while (*p) {
        switch (*p) {
            case '"':
                if (!append_format(buffer, buffer_size, offset, "\\\"")) return 0;
                break;
            case '\\':
                if (!append_format(buffer, buffer_size, offset, "\\\\")) return 0;
                break;
            case '\n':
                if (!append_format(buffer, buffer_size, offset, "\\n")) return 0;
                break;
            case '\r':
                if (!append_format(buffer, buffer_size, offset, "\\r")) return 0;
                break;
            case '\t':
                if (!append_format(buffer, buffer_size, offset, "\\t")) return 0;
                break;
            default:
                if (!append_format(buffer, buffer_size, offset, "%c", *p)) return 0;
                break;
        }
        p++;
    }

    return append_format(buffer, buffer_size, offset, "\"");
}

static int append_rows(char *buffer,
                       size_t buffer_size,
                       size_t *offset,
                       const ResultSet *rs) {
    int r = 0;
    int c = 0;

    if (!append_format(buffer, buffer_size, offset, "[")) return 0;

    if (rs) {
        for (r = 0; r < rs->row_count; r++) {
            if (r > 0 && !append_format(buffer, buffer_size, offset, ",")) return 0;
            if (!append_format(buffer, buffer_size, offset, "{")) return 0;

            for (c = 0; c < rs->col_count; c++) {
                if (c > 0 && !append_format(buffer, buffer_size, offset, ",")) return 0;
                if (!append_json_escaped(buffer, buffer_size, offset, rs->col_names[c])) return 0;
                if (!append_format(buffer, buffer_size, offset, ":")) return 0;
                if (!append_json_escaped(buffer, buffer_size, offset, rs->rows[r].values[c])) return 0;
            }

            if (!append_format(buffer, buffer_size, offset, "}")) return 0;
        }
    }

    return append_format(buffer, buffer_size, offset, "]");
}

int http_parse_query_request(const char *method,
                             const char *path,
                             const char *body,
                             ApiQueryRequest *out,
                             ApiResponseMeta *error_meta) {
    int parsed_bool = 0;

    if (!out) return 0;

    memset(out, 0, sizeof(*out));
    init_error_meta(error_meta);

    out->method = parse_method(method);
    out->route = parse_route(path);
    copy_text(out->path, sizeof(out->path), path);

    if (out->method == API_METHOD_UNKNOWN) {
        set_error_meta(error_meta, 405, API_CODE_UNSUPPORTED_METHOD,
                       "unsupported HTTP method");
        return 0;
    }

    if (out->route == API_ROUTE_KIND_UNKNOWN) {
        set_error_meta(error_meta, 404, API_CODE_UNSUPPORTED_ROUTE,
                       "unsupported route");
        return 0;
    }

    if (out->route == API_ROUTE_KIND_HEALTH) {
        if (out->method != API_METHOD_GET) {
            set_error_meta(error_meta, 405, API_CODE_UNSUPPORTED_METHOD,
                           "health route only supports GET");
            return 0;
        }
        init_success_meta(error_meta);
        return 1;
    }

    if (out->method != API_METHOD_POST) {
        set_error_meta(error_meta, 405, API_CODE_UNSUPPORTED_METHOD,
                       "query route only supports POST");
        return 0;
    }

    if (!body || *body == '\0') {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST, "empty request body");
        return 0;
    }

    if (!parse_json_string_field(body, "sql", out->sql, sizeof(out->sql))) {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST,
                       "missing or invalid sql field");
        return 0;
    }

    if (is_blank_text(out->sql)) {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST, "empty sql field");
        return 0;
    }

    if (json_key_exists(body, "request_id")) {
        if (!parse_json_string_field(body, "request_id",
                                     out->request_id, sizeof(out->request_id))) {
            set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST,
                           "invalid request_id field");
            return 0;
        }
        set_meta_request_id(error_meta, out->request_id);
    } else {
        out->request_id[0] = '\0';
    }

    parsed_bool = parse_json_bool_field(body, "force_linear", &out->options.force_linear);
    if (!parsed_bool && json_key_exists(body, "force_linear")) {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST,
                       "invalid force_linear field");
        return 0;
    }
    if (!parsed_bool) out->options.force_linear = 0;

    parsed_bool = parse_json_bool_field(body, "compare", &out->options.compare);
    if (!parsed_bool && json_key_exists(body, "compare")) {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST,
                       "invalid compare field");
        return 0;
    }
    if (!parsed_bool) out->options.compare = 0;

    parsed_bool = parse_json_bool_field(body, "include_profile", &out->options.include_profile);
    if (!parsed_bool && json_key_exists(body, "include_profile")) {
        set_error_meta(error_meta, 400, API_CODE_BAD_REQUEST,
                       "invalid include_profile field");
        return 0;
    }
    if (!parsed_bool) out->options.include_profile = 0;

    init_success_meta(error_meta);
    return 1;
}

void http_response_meta_from_service(const DBServiceResult *service_result,
                                     ApiResponseMeta *meta) {
    if (!meta) return;

    memset(meta, 0, sizeof(*meta));

    if (!service_result) {
        meta->http_status = 500;
        meta->code = API_CODE_INTERNAL_ERROR;
        copy_text(meta->error, sizeof(meta->error), "service result is null");
        return;
    }

    meta->http_status = http_status_from_service(service_result->status);
    meta->code = api_code_from_service(service_result->status);
    meta->ok = (service_result->status == DB_SERVICE_OK);
    meta->has_payload = service_result->has_result_set;
    meta->include_profile = service_result->has_profile;
    meta->row_count = service_result->has_result_set && service_result->result_set
                    ? service_result->result_set->row_count : 0;
    meta->rows_affected = service_result->rows_affected;

    if (!meta->ok) {
        copy_text(meta->error, sizeof(meta->error), service_result->message);
    }
}

int http_serialize_query_response(const ApiResponseMeta *meta,
                                  const DBServiceResult *service_result,
                                  char *buffer,
                                  size_t buffer_size) {
    size_t offset = 0;

    if (!meta || !buffer || buffer_size < HTTP_RESPONSE_BUFFER_MIN) return 0;

    buffer[0] = '\0';

    if (!append_format(buffer, buffer_size, &offset, "{")) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"ok\":%s,", meta->ok ? "true" : "false")) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"http_status\":%d,", meta->http_status)) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"code\":")) return 0;
    if (!append_json_escaped(buffer, buffer_size, &offset, api_code_name(meta->code))) return 0;
    if (meta->request_id[0] != '\0') {
        if (!append_format(buffer, buffer_size, &offset, ",\"request_id\":")) return 0;
        if (!append_json_escaped(buffer, buffer_size, &offset, meta->request_id)) return 0;
    }
    if (!append_format(buffer, buffer_size, &offset, ",\"row_count\":%d,", meta->row_count)) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"rows_affected\":%d,", meta->rows_affected)) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"has_payload\":%s,", meta->has_payload ? "true" : "false")) return 0;
    if (!append_format(buffer, buffer_size, &offset, "\"error\":")) return 0;
    if (!append_json_escaped(buffer, buffer_size, &offset, meta->error)) return 0;

    if (meta->has_payload && service_result && service_result->result_set) {
        if (!append_format(buffer, buffer_size, &offset, ",\"rows\":")) return 0;
        if (!append_rows(buffer, buffer_size, &offset, service_result->result_set)) return 0;
    }

    if (meta->include_profile && service_result && service_result->has_profile) {
        if (!append_format(buffer, buffer_size, &offset,
                ",\"profile\":{\"elapsed_ms\":%.3f,\"tree_io\":%d,\"row_count\":%d,\"access_path\":",
                service_result->profile.elapsed_ms,
                service_result->profile.tree_io,
                service_result->profile.row_count)) return 0;
        if (!append_json_escaped(buffer, buffer_size, &offset,
                service_result->profile.access_path)) return 0;
        if (!append_format(buffer, buffer_size, &offset, "}")) return 0;
    }

    return append_format(buffer, buffer_size, &offset, "}");
}

int http_serialize_health_response(char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size < HTTP_RESPONSE_BUFFER_MIN) return 0;
    int n = snprintf(buffer, buffer_size,
        "{\"ok\":true,\"http_status\":200,\"code\":\"OK\",\"message\":\"healthy\"}");
    return (n > 0 && (size_t)n < buffer_size) ? 1 : 0;
}
