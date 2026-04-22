/* =========================================================
 * test_executor.c — executor 단위 테스트
 *
 * 담당: Role D
 *
 * 커버리지:
 *   1. INSERT 기본 동작
 *   2. SELECT * (전체)
 *   3. SELECT WHERE id = N (인덱스 point 조회)
 *   4. SELECT WHERE id BETWEEN a AND b (인덱스 range 조회)
 *   5. SELECT WHERE age BETWEEN a AND b (age 인덱스 range 조회)
 *   6. SELECT WHERE name = '...' (linear scan)
 *   7. --force-linear 경로 vs 인덱스 경로 결과 일치 검증
 *   8. ResultSet 메모리 해제 (valgrind 용)
 *   9. db_service_execute_sql 통합 경로 검증
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#ifdef _WIN32
#  include <direct.h>
#  define TEST_MKDIR(path) _mkdir(path)
#else
#  include <sys/stat.h>
#  define TEST_MKDIR(path) mkdir(path, 0755)
#endif

#include "../include/interface.h"
#include "../include/index_manager.h"
#include "../include/db_service.h"
#include "../src/executor/executor_internal.h"

/* ── 테스트 유틸 ─────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); \
        g_fail++; \
    } \
} while (0)

/* 테스트용 임시 테이블 이름 */
#define TEST_TABLE "test_exec_tmp"

static int ensure_dir(const char *path) {
    if (TEST_MKDIR(path) == 0) return 1;
    return errno == EEXIST;
}

/* 테스트 전 임시 파일 및 스키마 생성 */
static int setup_test_env(void) {
    /* data 디렉터리 생성 */
    if (!ensure_dir("data")) {
        fprintf(stderr, "setup: cannot create data directory\n");
        return -1;
    }
    /* 기존 임시 파일 초기화 (remove 실패 대비 truncate로 처리) */
    remove("data/" TEST_TABLE ".dat");
    {
        FILE *f = fopen("data/" TEST_TABLE ".dat", "wb");
        if (f) fclose(f);
    }

    /* 임시 스키마 파일 생성 */
    if (!ensure_dir("schema")) {
        fprintf(stderr, "setup: cannot create schema directory\n");
        return -1;
    }
    FILE *sf = fopen("schema/" TEST_TABLE ".schema", "w");
    if (!sf) {
        fprintf(stderr, "setup: cannot create schema file\n");
        return -1;
    }
    fprintf(sf,
        "table=" TEST_TABLE "\n"
        "columns=3\n"
        "col0=id,INT,0\n"
        "col1=name,VARCHAR,64\n"
        "col2=age,INT,0\n");
    fclose(sf);

    /* 인덱스 초기화 */
    if (index_init(TEST_TABLE, 4, 4) != 0) {
        fprintf(stderr, "setup: index_init failed\n");
        return -1;
    }
    return 0;
}

static void teardown_test_env(void) {
    index_cleanup();
    remove("data/" TEST_TABLE ".dat");
    remove("schema/" TEST_TABLE ".schema");
}

/* ── INSERT 헬퍼 ─────────────────────────────────────────── */
static int do_insert(int id, const char *name, int age) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT INTO " TEST_TABLE " VALUES (%d, '%s', %d);",
        id, name, age);

    TokenList *tl = lexer_tokenize(sql);
    if (!tl) return -1;

    /* 토큰 배열에서 첫 statement 분리 */
    int semi = -1;
    for (int i = 0; i < tl->count; i++)
        if (tl->tokens[i].type == TOKEN_SEMICOLON) { semi = i; break; }

    int stmt_len = (semi >= 0) ? semi : tl->count;
    TokenList sub;
    sub.count  = stmt_len + 1;
    sub.tokens = (Token *)calloc((size_t)sub.count, sizeof(Token));
    for (int i = 0; i < stmt_len; i++) sub.tokens[i] = tl->tokens[i];
    sub.tokens[stmt_len].type     = TOKEN_EOF;
    sub.tokens[stmt_len].value[0] = '\0';
    lexer_free(tl);

    ASTNode *ast = parser_parse(&sub);
    free(sub.tokens);
    if (!ast) return -1;

    TableSchema *schema = schema_load(TEST_TABLE);
    if (!schema) { parser_free(ast); return -1; }

    int rc = db_insert(&ast->insert, schema);
    schema_free(schema);
    parser_free(ast);
    return rc;
}

/* ── SELECT 헬퍼 ─────────────────────────────────────────── */
static ResultSet *do_select(const char *where_clause, int force_linear) {
    char sql[512];
    if (where_clause)
        snprintf(sql, sizeof(sql),
            "SELECT * FROM " TEST_TABLE " WHERE %s;", where_clause);
    else
        snprintf(sql, sizeof(sql),
            "SELECT * FROM " TEST_TABLE ";");

    TokenList *tl = lexer_tokenize(sql);
    if (!tl) return NULL;

    int semi = -1;
    for (int i = 0; i < tl->count; i++)
        if (tl->tokens[i].type == TOKEN_SEMICOLON) { semi = i; break; }

    int stmt_len = (semi >= 0) ? semi : tl->count;
    TokenList sub;
    sub.count  = stmt_len + 1;
    sub.tokens = (Token *)calloc((size_t)sub.count, sizeof(Token));
    for (int i = 0; i < stmt_len; i++) sub.tokens[i] = tl->tokens[i];
    sub.tokens[stmt_len].type     = TOKEN_EOF;
    sub.tokens[stmt_len].value[0] = '\0';
    lexer_free(tl);

    ASTNode *ast = parser_parse(&sub);
    free(sub.tokens);
    if (!ast) return NULL;

    TableSchema *schema = schema_load(TEST_TABLE);
    if (!schema) { parser_free(ast); return NULL; }

    ResultSet *rs = db_select_mode(&ast->select, schema, force_linear, 0, NULL);
    schema_free(schema);
    parser_free(ast);
    return rs;
}

/* ── 테스트 케이스 ──────────────────────────────────────── */

static void test_insert_basic(void) {
    printf("\n[T1] INSERT 기본 동작\n");
    CHECK(do_insert(1, "alice", 25) == SQL_OK, "INSERT id=1 OK");
    CHECK(do_insert(2, "bob",   30) == SQL_OK, "INSERT id=2 OK");
    CHECK(do_insert(3, "carol", 22) == SQL_OK, "INSERT id=3 OK");
    CHECK(do_insert(4, "dave",  35) == SQL_OK, "INSERT id=4 OK");
    CHECK(do_insert(5, "eve",   28) == SQL_OK, "INSERT id=5 OK");
}

static void test_select_all(void) {
    printf("\n[T2] SELECT * (전체 스캔)\n");
    ResultSet *rs = do_select(NULL, 1);
    CHECK(rs != NULL, "ResultSet not NULL");
    if (rs) {
        CHECK(rs->row_count == 5, "row_count == 5");
        CHECK(rs->col_count == 3, "col_count == 3");
        CHECK(strcmp(rs->col_names[0], "id")   == 0, "col[0] == 'id'");
        CHECK(strcmp(rs->col_names[1], "name") == 0, "col[1] == 'name'");
        CHECK(strcmp(rs->col_names[2], "age")  == 0, "col[2] == 'age'");
        result_free(rs);
    }
}

static void test_select_id_eq(void) {
    printf("\n[T3] SELECT WHERE id = N (인덱스 point 조회)\n");
    ResultSet *rs = do_select("id = 3", 0);
    CHECK(rs != NULL, "ResultSet not NULL");
    if (rs) {
        CHECK(rs->row_count == 1, "row_count == 1");
        if (rs->row_count == 1) {
            CHECK(strcmp(rs->rows[0].values[0], "3")     == 0, "id == '3'");
            CHECK(strcmp(rs->rows[0].values[1], "carol") == 0, "name == 'carol'");
            CHECK(strcmp(rs->rows[0].values[2], "22")    == 0, "age == '22'");
        }
        result_free(rs);
    }
}

static void test_select_id_range(void) {
    printf("\n[T4] SELECT WHERE id BETWEEN a AND b (id 인덱스 range)\n");
    ResultSet *rs = do_select("id BETWEEN 2 AND 4", 0);
    CHECK(rs != NULL, "ResultSet not NULL");
    if (rs) {
        CHECK(rs->row_count == 3, "row_count == 3 (id 2,3,4)");
        result_free(rs);
    }
}

static void test_select_age_range(void) {
    printf("\n[T5] SELECT WHERE age BETWEEN a AND b (age 인덱스 range)\n");
    /* alice(25), bob(30), carol(22), dave(35), eve(28) */
    /* 25 <= age <= 30 → alice, bob, eve */
    ResultSet *rs = do_select("age BETWEEN 25 AND 30", 0);
    CHECK(rs != NULL, "ResultSet not NULL");
    if (rs) {
        CHECK(rs->row_count == 3, "row_count == 3 (age 25,28,30)");
        result_free(rs);
    }
}

static void test_select_linear_name(void) {
    printf("\n[T6] SELECT WHERE name = '...' (linear scan)\n");
    ResultSet *rs = do_select("name = 'bob'", 0);
    CHECK(rs != NULL, "ResultSet not NULL");
    if (rs) {
        CHECK(rs->row_count == 1, "row_count == 1");
        if (rs->row_count == 1)
            CHECK(strcmp(rs->rows[0].values[0], "2") == 0, "id == '2'");
        result_free(rs);
    }
}

static void test_index_vs_linear_consistency(void) {
    printf("\n[T7] 인덱스 경로 vs linear 경로 결과 일치\n");
    ResultSet *idx    = do_select("id BETWEEN 1 AND 5", 0);
    ResultSet *linear = do_select("id BETWEEN 1 AND 5", 1);
    CHECK(idx    != NULL, "인덱스 ResultSet not NULL");
    CHECK(linear != NULL, "linear ResultSet not NULL");
    if (idx && linear) {
        CHECK(idx->row_count == linear->row_count,
              "인덱스 row_count == linear row_count");
    }
    if (idx)    result_free(idx);
    if (linear) result_free(linear);
}

static void test_service_execute(void) {
    printf("\n[T8] db_service_execute_sql 통합 경로\n");

    DBServiceOptions opts;
    db_service_options_init(&opts);
    opts.include_profile = 1;

    DBServiceResult res;
    db_service_result_init(&res);

    /* SELECT */
    DBServiceStatus st = db_service_execute_sql(
        "SELECT * FROM " TEST_TABLE " WHERE id = 1;",
        &opts, &res);

    CHECK(st == DB_SERVICE_OK, "service status == OK");
    CHECK(res.has_result_set,  "has_result_set == 1");
    if (res.has_result_set && res.result_set)
        CHECK(res.result_set->row_count == 1, "service SELECT row_count == 1");
    CHECK(res.has_profile, "has_profile == 1");

    db_service_result_free(&res);

    /* INSERT */
    db_service_result_init(&res);
    st = db_service_execute_sql(
        "INSERT INTO " TEST_TABLE " VALUES (99, 'zara', 20);",
        &opts, &res);
    CHECK(st == DB_SERVICE_OK,      "service INSERT status == OK");
    CHECK(res.rows_affected == 1,   "service INSERT rows_affected == 1");
    db_service_result_free(&res);

    /* 잘못된 SQL */
    db_service_result_init(&res);
    st = db_service_execute_sql("INVALID GARBAGE;", &opts, &res);
    CHECK(st != DB_SERVICE_OK, "invalid SQL → not OK");
    db_service_result_free(&res);

    /* 빈 SQL */
    db_service_result_init(&res);
    st = db_service_execute_sql("", &opts, &res);
    CHECK(st == DB_SERVICE_BAD_REQUEST, "empty SQL → BAD_REQUEST");
    db_service_result_free(&res);

    /* compare 옵션은 현재 명시적으로 미지원 */
    db_service_result_init(&res);
    db_service_options_init(&opts);
    opts.compare = 1;
    st = db_service_execute_sql(
        "SELECT * FROM " TEST_TABLE " WHERE id = 1;",
        &opts, &res);
    CHECK(st == DB_SERVICE_UNSUPPORTED, "compare ??UNSUPPORTED");
    CHECK(strcmp(res.message, "compare option not supported yet") == 0,
          "compare 미지원 메시지 확인");
    db_service_result_free(&res);
}

static void test_null_safety(void) {
    printf("\n[T9] NULL 인자 안전성\n");
    /* result_free(NULL) 은 안전해야 한다 */
    result_free(NULL);
    CHECK(1, "result_free(NULL) 안전");

    DBServiceResult res;
    db_service_result_init(&res);
    db_service_result_free(&res);
    CHECK(1, "db_service_result_free(초기화된 빈 result) 안전");
}

/* ── main ─────────────────────────────────────────────────── */

int main(void) {
    printf("========================================\n");
    printf("  test_executor — Role D 단위 테스트\n");
    printf("========================================\n");

    if (setup_test_env() < 0) {
        fprintf(stderr, "FATAL: test environment setup failed\n");
        return 1;
    }

    test_insert_basic();
    test_select_all();
    test_select_id_eq();
    test_select_id_range();
    test_select_age_range();
    test_select_linear_name();
    test_index_vs_linear_consistency();
    test_service_execute();
    test_null_safety();

    teardown_test_env();

    printf("\n========================================\n");
    printf("  결과: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");

    return g_fail > 0 ? 1 : 0;
}
