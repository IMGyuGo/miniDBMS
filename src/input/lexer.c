/* =========================================================
 * lexer.c — SQL 토크나이저
 *
 * 담당자 : 김규민 (역할 C)
 * 금지   : 다른 팀원은 이 파일을 수정하지 않는다.
 *
 * 변경 이력:
 *   - BETWEEN, AND 키워드 추가 (TOKEN_BETWEEN, TOKEN_AND)
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../include/interface.h"

/* ── 키워드 테이블 ──────────────────────────────────────── */
static struct
{
    const char *word;
    TokenType type;
} keywords[] = {
    {"SELECT", TOKEN_SELECT},
    {"INSERT", TOKEN_INSERT},
    {"INTO", TOKEN_INTO},
    {"FROM", TOKEN_FROM},
    {"WHERE", TOKEN_WHERE},
    {"VALUES", TOKEN_VALUES},
    {"BETWEEN", TOKEN_BETWEEN}, /* 김규민 추가 */
    {"AND", TOKEN_AND},         /* 김규민 추가 — BETWEEN ~ AND ~ 에서 사용 */
    {NULL, TOKEN_EOF}};

/*
 * keyword_lookup
 *
 * lexer 는 SQL 키워드를 대소문자 구분 없이 처리한다.
 * 따라서 입력 단어를 먼저 대문자로 정규화한 뒤 키워드 테이블과 비교한다.
 *
 * 주의:
 *   - 반환 타입만 키워드/식별자를 결정한다.
 *   - 실제 토큰 원문(value)은 원래 입력 문자열을 그대로 보존한다.
 */
static TokenType keyword_lookup(const char *word)
{
    char upper[64];
    int i;
    for (i = 0; word[i] && i < 63; i++)
        upper[i] = (char)toupper((unsigned char)word[i]);
    upper[i] = '\0';

    for (int k = 0; keywords[k].word; k++)
    {
        if (strcmp(upper, keywords[k].word) == 0)
            return keywords[k].type;
    }
    return TOKEN_IDENT;
}

/*
 * append_token
 *
 * TokenList 에 새 토큰 1개를 뒤에 붙인다.
 * 현재 구현은 별도의 capacity 필드를 두지 않고
 * "8개 단위로 시작 -> 가득 차면 2배 확장" 규칙으로 메모리를 늘린다.
 */
static int append_token(TokenList *list, TokenType type,
                        const char *value, int line)
{
    if (!list)
        return SQL_ERR;

    if (list->count == 0)
    {
        list->tokens = (Token *)calloc(8, sizeof(Token));
    }
    else if (list->count % 8 == 0)
    {
        // 새로운 용량을 확장하기 위함
        size_t new_cap = (size_t)list->count * 2;
        Token *next = (Token *)realloc(list->tokens,
                                       new_cap * sizeof(Token));
        if (!next)
            return SQL_ERR;
        list->tokens = next;
    }

    if (!list->tokens)
        return SQL_ERR;

    Token *tok = &list->tokens[list->count++];
    tok->type = type;
    tok->line = line;
    if (value)
    {
        strncpy(tok->value, value, sizeof(tok->value) - 1);
        tok->value[sizeof(tok->value) - 1] = '\0';
    }
    else
    {
        tok->value[0] = '\0';
    }
    return SQL_OK;
}

/*
 * lexer_tokenize
 *
 * 문자열 SQL 입력을 한 글자씩 훑으면서 TokenList 로 바꾼다.
 *
 * 이 함수가 하는 일:
 *   1. 공백과 줄바꿈을 건너뛴다.
 *   2. 식별자/키워드, 숫자, 문자열, 기호를 각각 다른 규칙으로 토큰화한다.
 *   3. 마지막에는 parser 가 "입력 끝"을 알 수 있도록 TOKEN_EOF 를 붙인다.
 *
 * 참고:
 *   - ';' 는 여기서 TOKEN_SEMICOLON 으로 토큰화만 한다.
 *   - 실제 문장 분리는 main.c 의 split_tokens 가 담당한다.
 */
TokenList *lexer_tokenize(const char *sql)
{
    if (!sql)
        return NULL;

    TokenList *list = (TokenList *)malloc(sizeof(TokenList));
    if (!list)
        return NULL;
    list->tokens = NULL;
    list->count = 0;

    int line = 1;
    const char *p = sql;

    while (*p)
    {
        /* 공백 / 줄바꿈 건너뛰기 */
        while (isspace((unsigned char)*p))
        {
            if (*p == '\n')
                line++;
            p++;
        }
        if (!*p)
            break;

        /* 식별자 / 키워드 */
        if (isalpha((unsigned char)*p) || *p == '_')
        {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            size_t len = (size_t)(p - start);

            char word[64];
            if (len >= sizeof(word))
                len = sizeof(word) - 1;
            memcpy(word, start, len);
            word[len] = '\0';

            /*
             * 여기서는 "단어 모양"만 잘라낸 뒤,
             * keyword_lookup 으로 이 단어가 키워드인지 일반 식별자인지 결정한다.
             */
            TokenType type = keyword_lookup(word);
            if (append_token(list, type, word, line) == SQL_ERR)
                goto fail;
            continue;
        }

        /* 정수 리터럴 */
        if (isdigit((unsigned char)*p))
        {
            const char *start = p;
            while (isdigit((unsigned char)*p))
                p++;
            size_t len = (size_t)(p - start);

            char num[256];
            if (len >= sizeof(num))
                len = sizeof(num) - 1;
            memcpy(num, start, len);
            num[len] = '\0';

            if (append_token(list, TOKEN_INTEGER, num, line) == SQL_ERR)
                goto fail;
            continue;
        }

        /* 문자열 리터럴 '...' */
        if (*p == '\'')
        {
            p++;
            const char *start = p;
            while (*p && *p != '\'')
                p++;
            if (!*p)
            {
                fprintf(stderr,
                        "lexer: unterminated string literal at line %d\n",
                        line);
                goto fail;
            }
            size_t len = (size_t)(p - start);
            char value[256];
            if (len >= sizeof(value))
                len = sizeof(value) - 1;
            memcpy(value, start, len);
            value[len] = '\0';

            /*
             * 토큰 값에는 작은따옴표 자체를 넣지 않고,
             * 따옴표 안의 실제 문자열 내용만 저장한다.
             */
            if (append_token(list, TOKEN_STRING, value, line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }

        /*
         * 단일 문자 기호:
         * parser 가 문법을 판별할 수 있게 각각 독립 토큰으로 만든다.
         */
        if (*p == '*')
        {
            if (append_token(list, TOKEN_STAR, "*", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }
        if (*p == ',')
        {
            if (append_token(list, TOKEN_COMMA, ",", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }
        if (*p == '(')
        {
            if (append_token(list, TOKEN_LPAREN, "(", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }
        if (*p == ')')
        {
            if (append_token(list, TOKEN_RPAREN, ")", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }
        if (*p == '=')
        {
            if (append_token(list, TOKEN_EQ, "=", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }
        if (*p == ';')
        {
            if (append_token(list, TOKEN_SEMICOLON, ";", line) == SQL_ERR)
                goto fail;
            p++;
            continue;
        }

        fprintf(stderr,
                "lexer: unknown character '%c' at line %d\n", *p, line);
        goto fail;
    }

    if (append_token(list, TOKEN_EOF, "", line) == SQL_ERR)
        goto fail;
    return list;

fail:
    lexer_free(list);
    return NULL;
}

/*
 * lexer_free
 *
 * TokenList 는 "토큰 배열 1개 + TokenList 본체 1개" 구조라서
 * 해제도 그 두 덩어리만 처리하면 된다.
 */
void lexer_free(TokenList *list)
{
    if (!list)
        return;
    free(list->tokens);
    free(list);
}
