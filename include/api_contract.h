#ifndef API_CONTRACT_H
#define API_CONTRACT_H

/*
 * api_contract.h — API 서버 transport/message 계약
 *
 * 역할:
 *   - Role C 1차 소유
 *   - HTTP 요청/응답 메시지 규약 정의
 *   - wire format -> request struct, service result -> response meta 연결 기준 제공
 *
 * 이 파일에 두는 것:
 *   - route / method / application-level error code
 *   - API request option
 *   - transport metadata
 *
 * 이 파일에 두지 않는 것:
 *   - AST, Token, TableSchema, ResultSet 같은 엔진 도메인 구조
 *   - 소켓 fd, worker thread state, queue node, lock object
 *   - 실제 HTTP parser / serializer 구현
 *
 * 규칙:
 *   1. MVP 기준 한 요청은 하나의 SQL statement만 처리한다.
 *   2. multi-statement 지원은 명시적 합의 전까지 금지한다.
 *   3. HTTP status 매핑은 이 파일의 code를 기준으로 transport 계층이 수행한다.
 */

#define API_SQL_MAX_LEN      4096
#define API_ERROR_MAX_LEN     256
#define API_REQUEST_ID_MAX     64
#define API_PATH_MAX_LEN       32
#define API_METHOD_MAX_LEN      8

#define API_ROUTE_QUERY   "/query"
#define API_ROUTE_HEALTH  "/health"

typedef enum {
    API_METHOD_UNKNOWN = 0,
    API_METHOD_GET,
    API_METHOD_POST
} ApiMethod;

typedef enum {
    API_ROUTE_KIND_UNKNOWN = 0,
    API_ROUTE_KIND_HEALTH,
    API_ROUTE_KIND_QUERY
} ApiRouteKind;

typedef enum {
    API_CODE_OK = 0,
    API_CODE_BAD_REQUEST,
    API_CODE_UNSUPPORTED_ROUTE,
    API_CODE_UNSUPPORTED_METHOD,
    API_CODE_PARSE_ERROR,
    API_CODE_SCHEMA_ERROR,
    API_CODE_EXEC_ERROR,
    API_CODE_QUEUE_FULL,
    API_CODE_INTERNAL_ERROR
} ApiCode;

/*
 * 서버 디버그 옵션도 transport 계층에서 받되,
 * 실제 적용 가능 여부는 service 계층이 최종 판단한다.
 */
typedef struct {
    int force_linear;    /* 1이면 인덱스 대신 선형 경로를 강제 */
    int compare;         /* 비교 실행 요청, MVP에서는 미지원 처리 가능 */
    int include_profile; /* 응답에 실행 프로파일 포함 요청 */
} ApiQueryOptions;

/*
 * HTTP parser가 JSON/body를 해석한 뒤 넘기는 request-local 구조다.
 * sql은 NUL-terminated UTF-8 문자열을 가정한다.
 */
typedef struct {
    char            request_id[API_REQUEST_ID_MAX];
    ApiMethod       method;
    ApiRouteKind    route;
    char            path[API_PATH_MAX_LEN];
    ApiQueryOptions options;
    char            sql[API_SQL_MAX_LEN];
} ApiQueryRequest;

/*
 * 실제 row payload는 service result를 serializer가 변환해서 출력한다.
 * 이 구조는 transport metadata와 오류/상태 표현만 담당한다.
 */
typedef struct {
    int      http_status;
    ApiCode  code;
    int      ok;
    int      has_payload;
    int      include_profile;
    int      row_count;
    int      rows_affected;
    char     error[API_ERROR_MAX_LEN];
} ApiResponseMeta;

#endif /* API_CONTRACT_H */
