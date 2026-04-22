#ifndef INTERFACE_H
#define INTERFACE_H

/* =========================================================
 * interface.h — SQL 엔진 공통 계약
 *
 * 역할:
 *   Role A (B+Tree 저장 코어)         : include/bptree.h, src/bptree/ 이하
 *   Role B (인덱스/파일 일관성)       : include/index_manager.h, src/index/ 이하
 *   Role C (SQL 입력/검증 + API 계약) : src/input/, src/parser/,
 *                                       src/schema/, src/http/,
 *                                       include/api_contract.h
 *   Role D (실행/CLI + 서버 런타임)   : src/executor/, src/main.c,
 *                                       src/service/, src/server/,
 *                                       src/threadpool/
 *
 * 이 파일의 역할:
 *   1. SQL 엔진 내부에서 Role C와 Role D가 공유하는 핵심 자료구조 계약.
 *   2. Role B는 index/executor 연동 시 이 계약을 간접적으로 소비한다.
 *   3. HTTP DTO/메시지 포맷 계약은 여기 넣지 않고 include/api_contract.h 로 분리한다.
 *   4. API 서버 추가 시에도 이 파일은 "엔진 도메인 계약"만 유지한다.
 *
 * 규칙:
 *   1. 선언(declaration)만 허용. 구현(definition) 금지.
 *   2. 수정 시 팀 합의 필수.
 *   3. 메모리 소유권은 주석으로 명시한다.
 *   4. HTTP 상태 코드, 헤더, 소켓 fd, connection context, 인증 정보,
 *      threadpool/runtime 제어 정보는 이 파일에 넣지 않는다.
 *   5. 이 파일의 구조체는 요청 단위(request-local) 데이터로 본다.
 *      여러 스레드가 공유/수정해야 하면 동기화는 service/server 런타임 계층이 맡는다.
 *   6. API 서버용 transport 규약은 include/api_contract.h,
 *      엔진 호출 경계는 include/db_service.h 에 둔다.
 *   7. parser/executor 양쪽에 동시에 영향이 가는 필드 추가는
 *      Role C와 Role D가 함께 검토한 뒤 반영한다.
 * ========================================================= */

/* =========================================================
 * 공통 에러 코드
 * - SQL_OK / SQL_ERR 는 엔진 내부 반환 코드다.
 * - HTTP status code 매핑은 transport/service 계층에서 처리한다.
 * ========================================================= */
#define SQL_OK   0
#define SQL_ERR -1

/* =========================================================
 * Role C -> Role D : 토큰 목록
 * SQL 문자열을 엔진이 이해할 수 있는 최소 단위로 나눈 결과다.
 * ========================================================= */
typedef enum {
    TOKEN_SELECT,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_VALUES,
    TOKEN_BETWEEN,  /* BETWEEN 키워드 */
    TOKEN_AND,      /* AND 키워드 (BETWEEN ~ AND ~ 에서 사용) */
    TOKEN_STAR,     /* * */
    TOKEN_COMMA,    /* , */
    TOKEN_LPAREN,   /* ( */
    TOKEN_RPAREN,   /* ) */
    TOKEN_EQ,       /* = */
    TOKEN_SEMICOLON,/* ; */
    TOKEN_IDENT,    /* 테이블명, 컬럼명 */
    TOKEN_STRING,   /* 'alice' */
    TOKEN_INTEGER,  /* 42 */
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    char      value[256];  /* 토큰 원문 */
    int       line;        /* 에러 리포팅용 줄 번호 */
} Token;

typedef struct {
    Token *tokens; /* heap array, lexer_free()가 해제 */
    int    count;
} TokenList;

/* Role C 구현 — 호출자가 free 책임 */
char      *input_read_file(const char *path);    /* heap 문자열, 호출자가 free() */
TokenList *lexer_tokenize(const char *sql);      /* heap TokenList, lexer_free() */
void       lexer_free(TokenList *list);

/* =========================================================
 * Role C -> Role D : AST
 * SQL 문장의 의미 구조다.
 * transport 전용 정보(HTTP 헤더, 소켓 fd 등)는 여기 넣지 않는다.
 * ========================================================= */
typedef enum {
    STMT_SELECT,
    STMT_INSERT
} StmtType;

/* WHERE 절 조건 타입 */
typedef enum {
    WHERE_EQ,      /* col = val                  */
    WHERE_BETWEEN  /* col BETWEEN val_from AND val_to */
} WhereType;

typedef struct {
    char      col[64];        /* WHERE 컬럼명 */
    WhereType type;           /* 조건 종류 */
    char      val[256];       /* WHERE_EQ: 비교값 */
    char      val_from[256];  /* WHERE_BETWEEN: 시작값 */
    char      val_to[256];    /* WHERE_BETWEEN: 끝값   */
} WhereClause;

typedef struct {
    int          select_all;    /* SELECT * 이면 1 */
    char       **columns;       /* heap array, parser_free()가 원소와 함께 해제 */
    int          column_count;
    char         table[64];
    int          has_where;
    WhereClause  where;
} SelectStmt;

typedef struct {
    char   table[64];
    char **columns;      /* heap array, parser_free()가 원소와 함께 해제 */
    int    column_count; /* 지정된 컬럼 수.   0   = 미지정 (기존 방식) */
    char **values;       /* heap array, parser_free()가 원소와 함께 해제 */
    int    value_count;
} InsertStmt;

typedef struct {
    StmtType type;
    union {
        SelectStmt select;
        InsertStmt insert;
    };
} ASTNode;

/* Role C 구현 — 호출자가 free 책임 */
ASTNode *parser_parse(TokenList *tokens);  /* heap AST, parser_free()가 중첩 해제 */
void     parser_free(ASTNode *node);

/* =========================================================
 * Role C -> Role D : 스키마 계층
 * SQL 엔진 도메인 계약. transport 정보는 넣지 않는다.
 * ========================================================= */
typedef enum {
    COL_INT = 0,
    COL_VARCHAR,
    COL_BOOLEAN
} ColType;

typedef struct {
    char    name[64];
    ColType type;
    int     max_len;   /* VARCHAR 최대 길이, 다른 타입은 0 */
} ColDef;

/*
 * TableSchema: heap object.
 * columns 는 heap 배열이며 schema_free()가 해제한다.
 */
typedef struct {
    char    table_name[64];
    ColDef *columns;       /* heap array, schema_free()가 해제 */
    int     column_count;
} TableSchema;

/* Role C 구현 — 호출자가 free 책임 */
TableSchema *schema_load(const char *table_name);
int          schema_validate(const ASTNode *node, const TableSchema *schema);
void         schema_free(TableSchema *schema);

/* =========================================================
 * Role D -> Role C : SELECT/INSERT 실행 결과
 * result_free()가 ResultSet 전체를 해제한다.
 * ========================================================= */

/* Row: ResultSet 안에서 관리하는 한 행. */
typedef struct {
    char **values;  /* heap array, count 개의 NUL-terminated 문자열 */
    int    count;   /* ResultSet.col_count 와 동일 */
} Row;

/*
 * ResultSet: SELECT 결과 전체를 담는 heap object.
 * db_select 계열 함수가 반환하며, result_free()로 해제한다.
 */
typedef struct {
    char **col_names;  /* heap array, col_count 개의 컬럼명 문자열 */
    int    col_count;
    Row   *rows;       /* heap array, row_count 개의 Row */
    int    row_count;
} ResultSet;

/* =========================================================
 * Role D 구현 — executor 공개 API
 * ========================================================= */

/* 일반 SELECT 진입점 (emit_log=1) */
ResultSet *db_select(const SelectStmt *stmt, const TableSchema *schema);

/* 벤치마크용 조용한 SELECT (emit_log=0) */
ResultSet *db_select_bench(const SelectStmt *stmt, const TableSchema *schema,
                           int force_linear);

/* INSERT 실행: 성공 SQL_OK, 실패 SQL_ERR */
int db_insert(const InsertStmt *stmt, const TableSchema *schema);

/* AST 타입 기반 디스패처 (기존 인터페이스 호환) */
int executor_run(const ASTNode *node, const TableSchema *schema);

/* ResultSet 전체 해제 */
void result_free(ResultSet *rs);

#endif /* INTERFACE_H */

