#include <stdio.h>
#include <string.h>

#include "../include/api_contract.h"
#include "../include/db_service.h"
#include "../src/http/http_message.h"

static int test_parse_query_request(void) {
    ApiQueryRequest request;
    ApiResponseMeta error_meta;
    const char *body =
        "{\"request_id\":\"req-1\",\"sql\":\"SELECT * FROM users\","
        "\"force_linear\":true,\"include_profile\":true}";

    if (!http_parse_query_request("POST", "/query", body, &request, &error_meta)) {
        return 0;
    }

    if (request.method != API_METHOD_POST) return 0;
    if (request.route != API_ROUTE_KIND_QUERY) return 0;
    if (strcmp(request.request_id, "req-1") != 0) return 0;
    if (strcmp(request.sql, "SELECT * FROM users") != 0) return 0;
    if (request.options.force_linear != 1) return 0;
    if (request.options.include_profile != 1) return 0;
    if (request.options.compare != 0) return 0;

    return 1;
}

static int test_parse_invalid_route(void) {
    ApiQueryRequest request;
    ApiResponseMeta error_meta;

    if (http_parse_query_request("POST", "/unknown", "{}", &request, &error_meta)) {
        return 0;
    }

    return error_meta.code == API_CODE_UNSUPPORTED_ROUTE &&
           error_meta.http_status == 404;
}

static int test_serialize_query_response(void) {
    ApiResponseMeta meta;
    DBServiceResult service_result;
    ResultSet result_set;
    Row row;
    char column_id[] = "id";
    char column_name[] = "name";
    char value_id[] = "1";
    char value_name[] = "alice";
    char *columns[2] = {column_id, column_name};
    char *values[2] = {value_id, value_name};
    char buffer[1024];

    memset(&meta, 0, sizeof(meta));
    memset(&service_result, 0, sizeof(service_result));
    memset(&result_set, 0, sizeof(result_set));
    memset(&row, 0, sizeof(row));

    row.values = values;
    row.count = 2;

    result_set.col_names = columns;
    result_set.col_count = 2;
    result_set.rows = &row;
    result_set.row_count = 1;

    service_result.status = DB_SERVICE_OK;
    service_result.has_result_set = 1;
    service_result.result_set = &result_set;
    service_result.rows_affected = 0;
    service_result.has_profile = 1;
    strcpy(service_result.profile.access_path, "index:id:eq");
    service_result.profile.elapsed_ms = 1.25;
    service_result.profile.tree_io = 3;
    service_result.profile.row_count = 1;

    http_response_meta_from_service(&service_result, &meta);

    if (!http_serialize_query_response(&meta, &service_result, buffer, sizeof(buffer))) {
        return 0;
    }

    if (!strstr(buffer, "\"ok\":true")) return 0;
    if (!strstr(buffer, "\"rows\"")) return 0;
    if (!strstr(buffer, "\"alice\"")) return 0;
    if (!strstr(buffer, "\"profile\"")) return 0;

    return 1;
}

int main(void) {
    if (!test_parse_query_request()) {
        fprintf(stderr, "test_parse_query_request failed\n");
        return 1;
    }

    if (!test_parse_invalid_route()) {
        fprintf(stderr, "test_parse_invalid_route failed\n");
        return 1;
    }

    if (!test_serialize_query_response()) {
        fprintf(stderr, "test_serialize_query_response failed\n");
        return 1;
    }

    return 0;
}
