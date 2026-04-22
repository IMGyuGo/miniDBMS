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
 * Role C -> Role D : 스키마
 * schema/{table}.schema 를 메모리 계약으로 올린 결과다.
 * ========================================================= */
typedef enum {
    COL_INT,
    COL_VARCHAR,
    COL_BOOLEAN  /* 허용값: "T" / "F" */
} ColType;

typedef struct {
    char    name[64];
    ColType type;
    int     max_len;    /* VARCHAR 전용, INT 는 0 */
} ColDef;

typedef struct {
    char    table_name[64];
    ColDef *columns;   /* heap array, schema_free()가 해제 */
    int     column_count;
} TableSchema;

/* Role C 구현 — 호출자가 free 책임 */
TableSchema *schema_load(const char *table_name); /* heap schema, schema_free() */
int          schema_validate(const ASTNode *node,
                             const TableSchema *schema);
void         schema_free(TableSchema *schema);

/* =========================================================
 * Role D : 실행 결과
 * SQL 엔진의 표 형태 결과다.
 * HTTP 응답 JSON 포맷은 include/api_contract.h 에서 별도 정의한다.
 * 이 구조체 자체는 transport 메타데이터를 들고 있지 않는다.
 * ========================================================= */
typedef struct {
    char **values; /* heap array, result_free()가 원소와 함께 해제 */
    int    count;
} Row;

typedef struct {
    char **col_names; /* heap array, result_free()가 원소와 함께 해제 */
    int    col_count;
    Row   *rows;      /* heap array, result_free()가 해제 */
    int    row_count;
} ResultSet;

/* Role D 구현 — 호출자가 free 책임 */
int        executor_run(const ASTNode *node, const TableSchema *schema);
ResultSet *db_select(const SelectStmt *stmt, const TableSchema *schema); /* result_free() */
int        db_insert(const InsertStmt *stmt, const TableSchema *schema);
void       result_free(ResultSet *rs);

/* =========================================================
 * API 서버 확장 시 추가 규약
 *
 * 1. 새 요청/응답 직렬화 포맷이 필요하면 include/api_contract.h 에 추가한다.
 * 2. 엔진 호출 진입점이 필요하면 include/db_service.h 에 추가한다.
 * 3. 이 파일에 새 필드를 넣을 때는 아래 질문을 먼저 확인한다.
 *    - 이 값이 SQL 엔진 의미 자체에 필요한가?
 *    - transport/runtime 이 아니라 parser/schema/executor 공통 의미인가?
 *    - free 책임과 스레드 공유 방식이 명확한가?
 * 4. 위 질문에 하나라도 "아니오"면 이 파일이 아니라 다른 계층 헤더로 분리한다.
 * ========================================================= */

#endif /* INTERFACE_H */
