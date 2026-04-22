/* =========================================================
 * db_service.c — SQL 엔진 공용 실행 경계
 *
 * 담당: Role D
 * 역할:
 *   - HTTP/server 런타임이 parser/executor 내부를 직접 우회하지 않도록
 *     db_service_execute_sql() 하나로 엔진 호출을 고정한다.
 *   - SQL 문자열을 받아 파싱/검증/실행 결과를 DBServiceResult 로 반환한다.
 *
 * 규칙:
 *   - transport 정보(소켓 fd, HTTP 헤더)를 직접 받지 않는다.
 *   - 동기화(lock) 책임은 server/threadpool 런타임 계층에 있다.
 *   - MVP 기준 한 호출은 하나의 logical statement만 처리한다.
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/interface.h"
#include "../../include/index_manager.h"
#include "../../include/db_service.h"
#include "../executor/executor_internal.h"

/* ── 내부 헬퍼: statement 분리 ──────────────────────────────
 * main.c 의 split_tokens 와 동일한 로직.
 * service 계층이 직접 수행해야 하므로 여기에 복사한다.
 * --------------------------------------------------------- */
static TokenList *extract_first_statement(const TokenList *all, int *out_semi_count) {
    int semi_count = 0;
    for (int i = 0; i < all->count; i++) {
        if (all->tokens[i].type == TOKEN_SEMICOLON)
            semi_count++;
    }
    *out_semi_count = semi_count;

    /* 첫 번째 statement 끝 위치 탐색 */
    int end = 0;
    while (end < all->count &&
           all->tokens[end].type != TOKEN_SEMICOLON &&
           all->tokens[end].type != TOKEN_EOF)
        end++;

    int token_count = end;
    if (token_count == 0) return NULL;

    TokenList *sub = (TokenList *)malloc(sizeof(TokenList));
    if (!sub) return NULL;

    sub->count  = token_count + 1;
    sub->tokens = (Token *)calloc((size_t)sub->count, sizeof(Token));
    if (!sub->tokens) { free(sub); return NULL; }

    for (int i = 0; i < token_count; i++)
        sub->tokens[i] = all->tokens[i];

    /* parser 는 statement 끝에 EOF 토큰을 기대한다 */
    sub->tokens[token_count].type        = TOKEN_EOF;
    sub->tokens[token_count].value[0]    = '\0';
    sub->tokens[token_count].line        =
        all->tokens[end > 0 ? end - 1 : 0].line;

    return sub;
}

/* ── 공개 API 구현 ────────────────────────────────────────── */

void db_service_options_init(DBServiceOptions *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
}

void db_service_result_init(DBServiceResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = DB_SERVICE_OK;
}

void db_service_result_reset(DBServiceResult *result) {
    if (!result) return;
    /* result_set 이 있으면 먼저 해제한다 */
    if (result->result_set) {
        result_free(result->result_set);
        result->result_set = NULL;
    }
    memset(result, 0, sizeof(*result));
    result->status = DB_SERVICE_OK;
}

void db_service_result_free(DBServiceResult *result) {
    if (!result) return;
    if (result->result_set) {
        result_free(result->result_set);
        result->result_set = NULL;
    }
    /* result 구조체 자체는 caller 소유이므로 free 하지 않는다 */
}

/*
 * db_service_execute_sql
 *
 * 전제:
 *   - sql은 NUL-terminated UTF-8 문자열이어야 한다.
 *   - out은 caller가 할당하고 db_service_result_init/reset 된 상태여야 한다.
 *   - opts가 NULL 이면 기본 옵션(모든 필드 0)을 적용한다.
 *   - 동기화는 이 함수를 호출하는 런타임 계층(server/threadpool)이 책임진다.
 *
 * 반환: out->status 와 동일한 DBServiceStatus
 */
DBServiceStatus db_service_execute_sql(const char *sql,
                                       const DBServiceOptions *opts,
                                       DBServiceResult *out) {
    /* opts 기본값 처리 */
    DBServiceOptions default_opts;
    if (!opts) {
        db_service_options_init(&default_opts);
        opts = &default_opts;
    }

    if (!sql || *sql == '\0') {
        out->status = DB_SERVICE_BAD_REQUEST;
        snprintf(out->message, sizeof(out->message), "empty SQL string");
        return out->status;
    }

    /* ── 1. 토큰화 ── */
    TokenList *all_tokens = lexer_tokenize(sql);
    if (!all_tokens) {
        out->status = DB_SERVICE_PARSE_ERROR;
        snprintf(out->message, sizeof(out->message), "tokenization failed");
        return out->status;
    }

    /* ── 2. multi-statement 검사 (MVP: 1개만 허용) ── */
    int semi_count = 0;
    TokenList *stmt_tokens = extract_first_statement(all_tokens, &semi_count);

    if (semi_count > 1) {
        lexer_free(all_tokens);
        out->status = DB_SERVICE_BAD_REQUEST;
        snprintf(out->message, sizeof(out->message),
                 "multi-statement not supported (got %d statements)", semi_count);
        return out->status;
    }

    if (!stmt_tokens) {
        lexer_free(all_tokens);
        out->status = DB_SERVICE_PARSE_ERROR;
        snprintf(out->message, sizeof(out->message), "empty or unparseable statement");
        return out->status;
    }

    /* ── 3. 파싱 ── */
    ASTNode *ast = parser_parse(stmt_tokens);
    lexer_free(stmt_tokens);
    lexer_free(all_tokens);

    if (!ast) {
        out->status = DB_SERVICE_PARSE_ERROR;
        snprintf(out->message, sizeof(out->message), "parse failed");
        return out->status;
    }

    /* ── 4. 스키마 로드 ── */
    const char *table = (ast->type == STMT_SELECT)
                        ? ast->select.table
                        : ast->insert.table;

    TableSchema *schema = schema_load(table);
    if (!schema) {
        parser_free(ast);
        out->status = DB_SERVICE_SCHEMA_ERROR;
        snprintf(out->message, sizeof(out->message),
                 "schema not found for table '%s'", table);
        return out->status;
    }

    /* ── 5. 스키마 검증 ── */
    if (schema_validate(ast, schema) != SQL_OK) {
        schema_free(schema);
        parser_free(ast);
        out->status = DB_SERVICE_SCHEMA_ERROR;
        snprintf(out->message, sizeof(out->message),
                 "schema validation failed for table '%s'", table);
        return out->status;
    }

    /* ── 6. 인덱스 초기화 (멱등적으로 동작) ── */
    if (index_init(table, IDX_ORDER_DEFAULT, IDX_ORDER_DEFAULT) != 0) {
        schema_free(schema);
        parser_free(ast);
        out->status = DB_SERVICE_INTERNAL_ERROR;
        snprintf(out->message, sizeof(out->message),
                 "index_init failed for table '%s'", table);
        return out->status;
    }

    /* ── 7. 실행 ── */
    out->stmt_type       = ast->type;
    out->statement_count = 1;

    if (ast->type == STMT_SELECT) {
        SelectExecInfo exec_info = {0};
        SelectExecInfo *info_ptr = opts->include_profile ? &exec_info : NULL;

        ResultSet *rs = db_select_mode(
            &ast->select, schema,
            opts->force_linear,
            opts->emit_log,
            info_ptr
        );

        if (!rs) {
            schema_free(schema);
            parser_free(ast);
            out->status = DB_SERVICE_EXEC_ERROR;
            snprintf(out->message, sizeof(out->message), "SELECT execution failed");
            return out->status;
        }

        out->has_result_set = 1;
        out->result_set     = rs;

        if (opts->include_profile && info_ptr) {
            out->has_profile = 1;
            strncpy(out->profile.access_path, exec_info.path,
                    sizeof(out->profile.access_path) - 1);
            out->profile.elapsed_ms = exec_info.elapsed_ms;
            out->profile.tree_io    = exec_info.tree_io;
            out->profile.row_count  = exec_info.row_count;
        }

    } else {
        /* INSERT */
        if (db_insert(&ast->insert, schema) != SQL_OK) {
            schema_free(schema);
            parser_free(ast);
            out->status = DB_SERVICE_EXEC_ERROR;
            snprintf(out->message, sizeof(out->message), "INSERT execution failed");
            return out->status;
        }
        out->rows_affected = 1;
    }

    schema_free(schema);
    parser_free(ast);
    out->status = DB_SERVICE_OK;
    return DB_SERVICE_OK;
}
