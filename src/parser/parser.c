/* =========================================================
 * parser.c — SQL 파서 (AST 생성)
 *
 * 역할   : C
 * 금지   : 다른 팀원은 이 파일을 수정하지 않는다.
 *
 * 지원 문법:
 *   SELECT * FROM <table>
 *   SELECT <col1>, ... FROM <table>
 *   SELECT * FROM <table> WHERE <col> = <val>
 *   SELECT * FROM <table> WHERE <col> BETWEEN <from> AND <to>  ← 신규
 *   INSERT INTO <table> VALUES (<val1>, ...)
 *   INSERT INTO <table> (<col1>, ...) VALUES (<val1>, ...)
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/interface.h"

/* ── 내부 헬퍼 ──────────────────────────────────────────── */

/*
 * peek
 *
 * 현재 파싱 위치의 토큰을 읽기 전용으로 조회한다.
 * 위치가 범위를 벗어나면 마지막 EOF 토큰을 돌려줘서
 * 호출부가 별도 범위 검사 없이 "입력 끝"처럼 처리할 수 있게 한다.
 */
static Token *peek(TokenList *tokens, int pos) {
    if (pos >= tokens->count) return &tokens->tokens[tokens->count - 1];
    return &tokens->tokens[pos];
}

/*
 * expect
 *
 * "지금 이 위치에는 반드시 특정 토큰이 와야 한다"는 문법 규칙을 검사한다.
 * 일치하면 pos 를 다음 토큰으로 넘기고, 아니면 에러를 출력한다.
 */
static int expect(TokenList *tokens, int *pos, TokenType type) {
    Token *token = peek(tokens, *pos);
    if (token->type != type) {
        fprintf(stderr,
                "parse error: unexpected token '%s' at line %d\n",
                token->value, token->line);
        return SQL_ERR;
    }
    (*pos)++;
    return SQL_OK;
}

/*
 * dup_value
 *
 * AST 에 저장할 문자열은 토큰 배열과 생명주기가 달라질 수 있으므로
 * 별도 힙 메모리에 복사해 둔다.
 */
static char *dup_value(const char *value) {
    size_t len  = strlen(value) + 1;
    char  *copy = (char *)calloc(len, sizeof(char));
    if (!copy) { fprintf(stderr, "parser: out of memory\n"); return NULL; }
    memcpy(copy, value, len);
    return copy;
}

/*
 * copy_text
 *
 * 고정 길이 char 배열(AST 내부 필드 등)에 안전하게 문자열을 복사한다.
 * 길이를 넘는 입력은 뒤를 잘라내고 항상 '\0' 로 끝나게 만든다.
 */
static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/*
 * 현재 문법에서 "값" 자리로 허용하는 토큰 종류를 판별한다.
 * 예: WHERE id = 42 / WHERE name = 'alice' / WHERE id BETWEEN a AND b
 */
static int is_value_token(TokenType type) {
    return type == TOKEN_IDENT || type == TOKEN_STRING || type == TOKEN_INTEGER;
}

/*
 * append_string
 *
 * SELECT 컬럼 목록이나 INSERT 값 목록처럼
 * 길이가 가변적인 문자열 배열을 AST 쪽 힙 메모리에 축적할 때 사용한다.
 */
static int append_string(char ***items, int *count, int *capacity,
                          const char *value) {
    if (*count == *capacity) {
        int new_cap = (*capacity == 0) ? 4 : (*capacity * 2);
        char **resized = (char **)realloc(*items,
                                           (size_t)new_cap * sizeof(char *));
        if (!resized) { fprintf(stderr, "parser: out of memory\n"); return SQL_ERR; }
        *items    = resized;
        *capacity = new_cap;
    }
    (*items)[*count] = dup_value(value);
    if (!(*items)[*count]) return SQL_ERR;
    (*count)++;
    return SQL_OK;
}

/*
 * expect_ident / expect_value
 *
 * 각각 "식별자여야 하는 위치", "값이어야 하는 위치"를 검사하고
 * 통과하면 해당 문자열을 목적지 버퍼에 복사한다.
 */
static int expect_ident(TokenList *tokens, int *pos,
                         char *dst, size_t dst_size) {
    Token *token = peek(tokens, *pos);
    if (token->type != TOKEN_IDENT) {
        fprintf(stderr,
                "parse error: unexpected token '%s' at line %d\n",
                token->value, token->line);
        return SQL_ERR;
    }
    copy_text(dst, dst_size, token->value);
    (*pos)++;
    return SQL_OK;
}

static int expect_value(TokenList *tokens, int *pos,
                         char *dst, size_t dst_size) {
    Token *token = peek(tokens, *pos);
    if (!is_value_token(token->type)) {
        fprintf(stderr,
                "parse error: unexpected token '%s' at line %d\n",
                token->value, token->line);
        return SQL_ERR;
    }
    copy_text(dst, dst_size, token->value);
    (*pos)++;
    return SQL_OK;
}

/*
 * parse_ident_list
 *
 * INSERT INTO users (id, name, age) 처럼
 * 괄호로 감싼 식별자 목록을 읽어 문자열 배열로 만든다.
 */
static int parse_ident_list(TokenList *tokens, int *pos,
                             char ***items, int *count, int *capacity) {
    if (expect(tokens, pos, TOKEN_LPAREN) != SQL_OK) return SQL_ERR;
    while (1) {
        Token *token = peek(tokens, *pos);
        if (token->type != TOKEN_IDENT) {
            fprintf(stderr,
                    "parse error: unexpected token '%s' at line %d\n",
                    token->value, token->line);
            return SQL_ERR;
        }
        if (append_string(items, count, capacity, token->value) != SQL_OK)
            return SQL_ERR;
        (*pos)++;
        if (peek(tokens, *pos)->type != TOKEN_COMMA) break;
        (*pos)++;
    }
    return expect(tokens, pos, TOKEN_RPAREN);
}

/*
 * parse_value_list
 *
 * VALUES (1, 'alice', 25) 같은 값을 읽는다.
 * 현재 parser 는 값 자리에 IDENT / STRING / INTEGER 를 허용한다.
 */
static int parse_value_list(TokenList *tokens, int *pos,
                             char ***items, int *count, int *capacity) {
    if (expect(tokens, pos, TOKEN_LPAREN) != SQL_OK) return SQL_ERR;
    while (1) {
        Token *token = peek(tokens, *pos);
        if (!is_value_token(token->type)) {
            fprintf(stderr,
                    "parse error: unexpected token '%s' at line %d\n",
                    token->value, token->line);
            return SQL_ERR;
        }
        if (append_string(items, count, capacity, token->value) != SQL_OK)
            return SQL_ERR;
        (*pos)++;
        if (peek(tokens, *pos)->type != TOKEN_COMMA) break;
        (*pos)++;
    }
    return expect(tokens, pos, TOKEN_RPAREN);
}

/* =========================================================
 * parse_select
 * ========================================================= */
static ASTNode *parse_select(TokenList *tokens) {
    int      pos      = 0;
    int      capacity = 0;
    ASTNode *node     = (ASTNode *)calloc(1, sizeof(ASTNode));
    if (!node) { fprintf(stderr, "parser: out of memory\n"); return NULL; }

    /*
     * calloc 으로 AST 를 만들기 때문에
     * has_where, column_count, where.val* 같은 필드는 기본적으로 0 / 빈 문자열이다.
     * 이 성질 덕분에 WHERE_EQ 와 WHERE_BETWEEN 필드를 같은 구조체에서 공존시킬 수 있다.
     */
    node->type = STMT_SELECT;

    if (expect(tokens, &pos, TOKEN_SELECT) != SQL_OK) goto fail;

    /* SELECT * 또는 SELECT col1, col2, ... */
    if (peek(tokens, pos)->type == TOKEN_STAR) {
        node->select.select_all = 1;
        pos++;
    } else {
        /*
         * SELECT id, name, age ...
         * 콤마가 끊길 때까지 컬럼명을 차례대로 축적한다.
         */
        while (1) {
            Token *column = peek(tokens, pos);
            if (column->type != TOKEN_IDENT) {
                fprintf(stderr,
                        "parse error: unexpected token '%s' at line %d\n",
                        column->value, column->line);
                goto fail;
            }
            if (append_string(&node->select.columns,
                               &node->select.column_count,
                               &capacity, column->value) != SQL_OK)
                goto fail;
            pos++;
            if (peek(tokens, pos)->type != TOKEN_COMMA) break;
            pos++;
        }
    }

    if (expect(tokens, &pos, TOKEN_FROM) != SQL_OK) goto fail;
    if (expect_ident(tokens, &pos,
                     node->select.table,
                     sizeof(node->select.table)) != SQL_OK)
        goto fail;

    /* WHERE 절 파싱 */
    if (peek(tokens, pos)->type == TOKEN_WHERE) {
        pos++;
        node->select.has_where = 1;

        /* WHERE 컬럼명 */
        if (expect_ident(tokens, &pos,
                          node->select.where.col,
                          sizeof(node->select.where.col)) != SQL_OK)
            goto fail;

        /* BETWEEN 또는 = 분기 */
        if (peek(tokens, pos)->type == TOKEN_BETWEEN) {
            /* ── WHERE col BETWEEN from AND to ── */
            pos++;
            node->select.where.type = WHERE_BETWEEN;

            /*
             * WHERE_BETWEEN 은 val 대신
             * val_from / val_to 두 필드에 경계값을 저장한다.
             */
            if (expect_value(tokens, &pos,
                              node->select.where.val_from,
                              sizeof(node->select.where.val_from)) != SQL_OK)
                goto fail;

            if (expect(tokens, &pos, TOKEN_AND) != SQL_OK) goto fail;

            if (expect_value(tokens, &pos,
                              node->select.where.val_to,
                              sizeof(node->select.where.val_to)) != SQL_OK)
                goto fail;

        } else {
            /* ── WHERE col = val ── */
            if (expect(tokens, &pos, TOKEN_EQ) != SQL_OK) goto fail;
            node->select.where.type = WHERE_EQ;

            /* 단순 비교는 기존 val 필드만 사용한다. */
            if (expect_value(tokens, &pos,
                              node->select.where.val,
                              sizeof(node->select.where.val)) != SQL_OK)
                goto fail;
        }
    }

    /* 세미콜론이 있으면 소비하고 EOF 를 기대한다.
     * main.c 처럼 세미콜론을 미리 제거한 경우에도, 테스트처럼 그대로 넘긴 경우에도 동작. */
    if (peek(tokens, pos)->type == TOKEN_SEMICOLON) pos++;
    if (expect(tokens, &pos, TOKEN_EOF) != SQL_OK) goto fail;
    return node;

fail:
    /* 부분적으로만 채워졌더라도 parser_free 가 안전하게 정리한다. */
    parser_free(node);
    return NULL;
}

/* =========================================================
 * parse_insert
 * ========================================================= */
static ASTNode *parse_insert(TokenList *tokens) {
    int      pos              = 0;
    int      column_capacity  = 0;
    int      value_capacity   = 0;
    ASTNode *node             = (ASTNode *)calloc(1, sizeof(ASTNode));
    if (!node) { fprintf(stderr, "parser: out of memory\n"); return NULL; }

    node->type = STMT_INSERT;

    if (expect(tokens, &pos, TOKEN_INSERT) != SQL_OK) goto fail;
    if (expect(tokens, &pos, TOKEN_INTO)   != SQL_OK) goto fail;
    if (expect_ident(tokens, &pos,
                     node->insert.table,
                     sizeof(node->insert.table)) != SQL_OK)
        goto fail;

    /*
     * INSERT INTO users VALUES (...)
     * INSERT INTO users (id, name, ...) VALUES (...)
     * 두 문법을 모두 지원하므로 컬럼 목록은 선택적이다.
     */
    if (peek(tokens, pos)->type == TOKEN_LPAREN) {
        if (parse_ident_list(tokens, &pos,
                             &node->insert.columns,
                             &node->insert.column_count,
                             &column_capacity) != SQL_OK)
            goto fail;
    }

    if (expect(tokens, &pos, TOKEN_VALUES) != SQL_OK) goto fail;
    if (parse_value_list(tokens, &pos,
                         &node->insert.values,
                         &node->insert.value_count,
                         &value_capacity) != SQL_OK)
        goto fail;

    if (peek(tokens, pos)->type == TOKEN_SEMICOLON) pos++;
    if (expect(tokens, &pos, TOKEN_EOF) != SQL_OK) goto fail;
    return node;

fail:
    /* INSERT 도 동일하게 부분 AST 를 정리한 뒤 실패를 알린다. */
    parser_free(node);
    return NULL;
}

/* =========================================================
 * 공개 API
 * ========================================================= */
ASTNode *parser_parse(TokenList *tokens) {
    if (!tokens || tokens->count == 0) return NULL;

    /*
     * 첫 토큰만 보면 문장 종류를 결정할 수 있으므로
     * 여기서는 dispatcher 역할만 맡고 실제 문법 처리는 각 parse_* 로 넘긴다.
     */
    switch (peek(tokens, 0)->type) {
        case TOKEN_SELECT: return parse_select(tokens);
        case TOKEN_INSERT: return parse_insert(tokens);
        default: {
            Token *token = peek(tokens, 0);
            fprintf(stderr,
                    "parse error: unexpected token '%s' at line %d\n",
                    token->value, token->line);
            return NULL;
        }
    }
}

/*
 * parser_free
 *
 * AST 안에서 힙 할당이 들어가는 곳은
 * SELECT 의 columns[], INSERT 의 columns[] / values[] 배열뿐이다.
 * 고정 길이 배열(table, where.val 등)은 AST 본체 안에 들어 있으므로 별도 free 가 필요 없다.
 */
void parser_free(ASTNode *node) {
    if (!node) return;

    if (node->type == STMT_SELECT) {
        if (node->select.columns) {
            for (int i = 0; i < node->select.column_count; i++)
                free(node->select.columns[i]);
            free(node->select.columns);
        }
    } else if (node->type == STMT_INSERT) {
        if (node->insert.columns) {
            for (int i = 0; i < node->insert.column_count; i++)
                free(node->insert.columns[i]);
            free(node->insert.columns);
        }
        if (node->insert.values) {
            for (int i = 0; i < node->insert.value_count; i++)
                free(node->insert.values[i]);
            free(node->insert.values);
        }
    }
    free(node);
}
