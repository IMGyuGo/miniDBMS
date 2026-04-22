/* =========================================================
 * schema.c — 스키마 로딩 + 검증
 *
 * 역할   : C
 * 금지   : 다른 팀원은 이 파일을 수정하지 않는다.
 *
 * 변경 이력:
 *   - WHERE_BETWEEN 조건 검증 추가
 *     (대상 컬럼이 INT 타입인지, val_from/val_to 가 정수인지 확인)
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../include/interface.h"

/* ── 내부 헬퍼 ──────────────────────────────────────────── */

/*
 * schema 검증은 문자열 기반으로 동작하므로
 * 숫자 여부 / BOOLEAN 허용값 여부 같은 기본 타입 검사 헬퍼를 먼저 둔다.
 */
static int is_integer_string(const char *s) {
    if (!s || *s == '\0') return 0;
    int i = 0;
    if (s[0] == '-') i = 1;
    if (s[i] == '\0') return 0;
    for (; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int is_boolean_string(const char *s) {
    return (s && (strcmp(s, "T") == 0 || strcmp(s, "F") == 0));
}

/* 컬럼 존재 여부만 빠르게 확인할 때 사용하는 단순 헬퍼 */
static int find_column(const TableSchema *schema, const char *col_name) {
    for (int i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, col_name) == 0) return 1;
    }
    return 0;
}

/* 컬럼명으로 ColType 을 반환한다. 없으면 COL_INT (기본값). */
static ColType column_type(const TableSchema *schema, const char *col_name) {
    for (int i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, col_name) == 0)
            return schema->columns[i].type;
    }
    return COL_INT;
}

/* =========================================================
 * schema_load
 * ========================================================= */
TableSchema *schema_load(const char *table_name) {
    if (!table_name) return NULL;

    /* 테이블명 users -> schema/users.schema */
    char path[256];
    snprintf(path, sizeof(path), "schema/%s.schema", table_name);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "schema: cannot open '%s'\n", path);
        return NULL;
    }

    TableSchema *s = (TableSchema *)calloc(1, sizeof(TableSchema));
    if (!s) { fclose(fp); return NULL; }

    char line[512];
    int  col_count = 0;

    /*
     * 스키마 파일 형식 예시:
     *   table=users
     *   columns=4
     *   col0=id,INT,0
     *   col1=name,VARCHAR,64
     *
     * 한 줄씩 읽으면서 table 이름, 컬럼 수, 개별 컬럼 정의를 차례로 채운다.
     */
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "table=", 6) == 0) {
            strncpy(s->table_name, line + 6, sizeof(s->table_name) - 1);

        } else if (strncmp(line, "columns=", 8) == 0) {
            col_count = atoi(line + 8);
            if (col_count <= 0) {
                fprintf(stderr,
                        "schema: invalid column count in '%s'\n", path);
                free(s); fclose(fp); return NULL;
            }
            s->column_count = col_count;
            s->columns = (ColDef *)calloc(col_count, sizeof(ColDef));
            if (!s->columns) { free(s); fclose(fp); return NULL; }

        } else if (strncmp(line, "col", 3) == 0) {
            char *eq = strchr(line, '=');
            if (!eq) continue;

            int idx = atoi(line + 3);
            if (idx < 0 || idx >= col_count || !s->columns) continue;

            char *val      = eq + 1;
            char  name[64]     = {0};
            char  type_str[32] = {0};
            int   max_len      = 0;

            /*
             * "id,INT,0" 같은 부분을 쉼표 기준으로 쪼갠다.
             * strtok 는 원본 버퍼를 직접 수정하므로 line 내부에서만 사용한다.
             */
            char *tok1 = strtok(val, ",");
            char *tok2 = strtok(NULL, ",");
            char *tok3 = strtok(NULL, ",");
            if (!tok1 || !tok2 || !tok3) continue;

            strncpy(name,     tok1, sizeof(name) - 1);
            strncpy(type_str, tok2, sizeof(type_str) - 1);
            max_len = atoi(tok3);

            strncpy(s->columns[idx].name, name,
                    sizeof(s->columns[idx].name) - 1);
            s->columns[idx].max_len = max_len;

            if      (strcmp(type_str, "INT")     == 0) s->columns[idx].type = COL_INT;
            else if (strcmp(type_str, "VARCHAR") == 0) s->columns[idx].type = COL_VARCHAR;
            else if (strcmp(type_str, "BOOLEAN") == 0) s->columns[idx].type = COL_BOOLEAN;
            else {
                fprintf(stderr,
                        "schema: unknown type '%s' for column '%s'\n",
                        type_str, name);
                schema_free(s); fclose(fp); return NULL;
            }
        }
    }

    fclose(fp);

    /* columns=... 없이 끝났거나 컬럼 배열 생성에 실패한 경우는 잘못된 스키마로 본다. */
    if (s->column_count == 0 || !s->columns) {
        fprintf(stderr,
                "schema: missing column definitions in '%s'\n", path);
        schema_free(s);
        return NULL;
    }
    return s;
}

/* =========================================================
 * schema_validate
 * ========================================================= */
int schema_validate(const ASTNode *node, const TableSchema *schema) {
    if (!node || !schema) return SQL_ERR;

    /* ── INSERT 검증 ── */
    if (node->type == STMT_INSERT) {
        const InsertStmt *ins = &node->insert;

        if (ins->column_count > 0) {
            /*
             * 컬럼 지정 방식:
             *   INSERT INTO users (id, name) VALUES (1, 'alice')
             * 컬럼 이름으로 schema 쪽 실제 컬럼을 찾아 타입을 검증한다.
             */
            if (ins->column_count != ins->value_count) {
                fprintf(stderr,
                        "schema: INSERT column count %d != value count %d\n",
                        ins->column_count, ins->value_count);
                return SQL_ERR;
            }
            for (int i = 0; i < ins->column_count; i++) {
                const char *col_name = ins->columns[i];
                const char *val      = ins->values[i];

                /* 사용자가 지정한 컬럼명이 schema 에 실제로 존재하는지 찾는다. */
                int found = -1;
                for (int j = 0; j < schema->column_count; j++) {
                    if (strcmp(schema->columns[j].name, col_name) == 0) {
                        found = j; break;
                    }
                }
                if (found == -1) {
                    fprintf(stderr,
                            "schema: unknown column '%s' in INSERT\n",
                            col_name);
                    return SQL_ERR;
                }

                /*
                 * 컬럼 타입마다 검증 규칙이 다르다.
                 *   - INT     : 정수 문자열이어야 함
                 *   - VARCHAR : max_len 을 넘으면 안 됨
                 *   - BOOLEAN : T/F 만 허용
                 */
                ColType expected = schema->columns[found].type;
                if (expected == COL_INT && !is_integer_string(val)) {
                    fprintf(stderr,
                            "schema: column '%s' expects INT, got '%s'\n",
                            col_name, val);
                    return SQL_ERR;
                }
                if (expected == COL_VARCHAR) {
                    int max_len = schema->columns[found].max_len;
                    if (max_len > 0 && (int)strlen(val) > max_len) {
                        fprintf(stderr,
                                "schema: column '%s' max length %d, got %d\n",
                                col_name, max_len, (int)strlen(val));
                        return SQL_ERR;
                    }
                }
                if (expected == COL_BOOLEAN && !is_boolean_string(val)) {
                    fprintf(stderr,
                            "schema: column '%s' expects BOOLEAN (T/F), "
                            "got '%s'\n", col_name, val);
                    return SQL_ERR;
                }
            }
        } else {
            /*
             * 컬럼 미지정 방식:
             *   INSERT INTO users VALUES (...)
             * 값의 순서가 schema 의 컬럼 순서와 정확히 맞아야 한다.
             */
            if (ins->value_count != schema->column_count) {
                fprintf(stderr,
                        "schema: INSERT expects %d values, got %d\n",
                        schema->column_count, ins->value_count);
                return SQL_ERR;
            }
            for (int i = 0; i < ins->value_count; i++) {
                const char *val      = ins->values[i];
                ColType     expected = schema->columns[i].type;
                if (expected == COL_INT && !is_integer_string(val)) {
                    fprintf(stderr,
                            "schema: column '%s' expects INT, got '%s'\n",
                            schema->columns[i].name, val);
                    return SQL_ERR;
                }
                if (expected == COL_VARCHAR) {
                    int max_len = schema->columns[i].max_len;
                    if (max_len > 0 && (int)strlen(val) > max_len) {
                        fprintf(stderr,
                                "schema: column '%s' max length %d, got %d\n",
                                schema->columns[i].name, max_len,
                                (int)strlen(val));
                        return SQL_ERR;
                    }
                }
                if (expected == COL_BOOLEAN && !is_boolean_string(val)) {
                    fprintf(stderr,
                            "schema: column '%s' expects BOOLEAN (T/F), "
                            "got '%s'\n", schema->columns[i].name, val);
                    return SQL_ERR;
                }
            }
        }
        return SQL_OK;
    }

    /* ── SELECT 검증 ── */
    if (node->type == STMT_SELECT) {
        const SelectStmt *sel = &node->select;

        /* SELECT * 가 아니면 요청한 컬럼명이 실제 스키마에 존재해야 한다. */
        if (!sel->select_all) {
            for (int i = 0; i < sel->column_count; i++) {
                if (!find_column(schema, sel->columns[i])) {
                    fprintf(stderr,
                            "schema: unknown column '%s' in SELECT\n",
                            sel->columns[i]);
                    return SQL_ERR;
                }
            }
        }

        if (sel->has_where) {
            /* WHERE 대상 컬럼 자체가 존재하는지 먼저 확인한다. */
            if (!find_column(schema, sel->where.col)) {
                fprintf(stderr,
                        "schema: unknown column '%s' in WHERE\n",
                        sel->where.col);
                return SQL_ERR;
            }

            /* WHERE_BETWEEN additional validation */
            if (sel->where.type == WHERE_BETWEEN) {
                /*
                 * 현재 프로젝트 규칙:
                 *   BETWEEN 은 INT 컬럼에만 허용한다.
                 * 그래서 컬럼 타입과 양쪽 경계값을 함께 확인한다.
                 */
                ColType ctype = column_type(schema, sel->where.col);
                if (ctype != COL_INT) {
                    fprintf(stderr,
                            "schema: BETWEEN is only supported on INT "
                            "columns, '%s' is not INT\n",
                            sel->where.col);
                    return SQL_ERR;
                }
                if (!is_integer_string(sel->where.val_from) ||
                    !is_integer_string(sel->where.val_to)) {
                    fprintf(stderr,
                            "schema: BETWEEN values must be integers\n");
                    return SQL_ERR;
                }
            }
        }
        return SQL_OK;
    }

    fprintf(stderr, "schema: unknown statement type\n");
    return SQL_ERR;
}

/* =========================================================
 * schema_free
 * ========================================================= */
void schema_free(TableSchema *schema) {
    if (!schema) return;
    /* columns 배열만 별도 힙 메모리이고, 각 ColDef 는 배열 내부 값 타입이다. */
    free(schema->columns);
    free(schema);
}
