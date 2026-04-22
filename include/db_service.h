#ifndef DB_SERVICE_H
#define DB_SERVICE_H

#include "interface.h"

/*
 * db_service.h — API 서버 -> SQL 엔진 공용 실행 경계
 *
 * 역할:
 *   - 공통 합의 파일
 *   - Role D가 주 구현을 맡되 Role C와 함께 계약을 검토한다.
 *
 * 목적:
 *   - HTTP/server 런타임이 parser/executor 내부 구현을 직접 우회하지 않게 한다.
 *   - request 단위 SQL 실행을 하나의 service API로 고정한다.
 *   - 실행 결과, 오류, 프로파일 메타데이터를 한 구조로 전달한다.
 *
 * 규칙:
 *   1. service 계층은 transport 비의존적이어야 한다.
 *      즉 method/path/header/socket fd를 직접 받지 않는다.
 *   2. 동기화(lock) 책임은 server/threadpool 런타임 계층에 있다.
 *      service 함수는 "이미 호출 가능한 상태"라는 전제에서 실행한다.
 *   3. MVP 기준 한 호출은 하나의 logical statement만 처리한다.
 *      여러 statement가 들어오면 service 계층에서 거절한다.
 */

#define DB_SERVICE_MESSAGE_MAX 256
#define DB_SERVICE_PATH_MAX     32

typedef enum {
    DB_SERVICE_OK = 0,
    DB_SERVICE_BAD_REQUEST,
    DB_SERVICE_PARSE_ERROR,
    DB_SERVICE_SCHEMA_ERROR,
    DB_SERVICE_EXEC_ERROR,
    DB_SERVICE_INTERNAL_ERROR,
    DB_SERVICE_UNSUPPORTED
} DBServiceStatus;

/*
 * transport 계층이 service에 넘기는 실행 옵션이다.
 * transport-specific field는 제거하고, 엔진 실행에 필요한 옵션만 유지한다.
 */
typedef struct {
    int force_linear;   /* 1이면 인덱스 대신 선형 경로를 강제 */
    int compare;        /* compare 요청, 미구현 시 DB_SERVICE_UNSUPPORTED 가능 */
    int emit_log;       /* stderr 진단 로그 출력 여부 */
    int include_profile;/* 결과에 profile 메타데이터 포함 요청 */
} DBServiceOptions;

/*
 * SELECT 경로의 실행 프로파일.
 * INSERT 에서는 row_count/path/tree_io가 0 또는 빈 문자열일 수 있다.
 */
typedef struct {
    char   access_path[DB_SERVICE_PATH_MAX];
    double elapsed_ms;
    int    tree_io;
    int    row_count;
} DBServiceProfile;

/*
 * service 계층의 통합 실행 결과.
 *
 * 메모리 소유권:
 *   - result_set 은 heap object이며 db_service_result_free()가 정리한다.
 *   - message 는 고정 버퍼이며 별도 해제 불필요.
 */
typedef struct {
    DBServiceStatus status;
    StmtType        stmt_type;
    int             statement_count; /* MVP에서는 성공 시 1, 실패 시 0 또는 감지 개수 */
    int             rows_affected;   /* INSERT 성공 시 보통 1 */
    int             has_result_set;
    int             has_profile;
    ResultSet      *result_set;      /* SELECT 결과, result_free() 또는 helper가 해제 */
    DBServiceProfile profile;
    char            message[DB_SERVICE_MESSAGE_MAX];
} DBServiceResult;

/*
 * helper 규약:
 *   - init/reset/free는 result 구조의 메모리와 상태를 안전하게 관리한다.
 *   - execute_sql은 out이 유효한 상태라고 가정한다.
 */
void db_service_options_init(DBServiceOptions *opts);
void db_service_result_init(DBServiceResult *result);
void db_service_result_reset(DBServiceResult *result);
void db_service_result_free(DBServiceResult *result);

/*
 * raw SQL 문자열 하나를 파싱/검증/실행한다.
 *
 * 계약:
 *   - sql은 NUL-terminated 문자열이어야 한다.
 *   - opts가 NULL이면 기본 옵션으로 간주한다.
 *   - out은 caller가 할당하고, 실행 전 init/reset 된 상태여야 한다.
 *   - 반환값과 out->status는 같은 상태 코드를 표현한다.
 */
DBServiceStatus db_service_execute_sql(const char *sql,
                                       const DBServiceOptions *opts,
                                       DBServiceResult *out);

#endif /* DB_SERVICE_H */
