#include <stdio.h>

#include "../include/interface.h"

static int validate_sql(const char *sql, int expected_status) {
    TokenList *tokens = lexer_tokenize(sql);
    ASTNode *ast = NULL;
    TableSchema *schema = NULL;
    int actual = SQL_ERR;

    if (!tokens) return 0;
    ast = parser_parse(tokens);
    if (!ast) {
        lexer_free(tokens);
        return expected_status == SQL_ERR;
    }

    schema = schema_load("users");
    if (!schema) {
        parser_free(ast);
        lexer_free(tokens);
        return 0;
    }

    actual = schema_validate(ast, schema);

    schema_free(schema);
    parser_free(ast);
    lexer_free(tokens);
    return actual == expected_status;
}

int main(void) {
    if (!validate_sql("SELECT * FROM users WHERE age BETWEEN 10 AND 20;", SQL_OK)) {
        fprintf(stderr, "valid BETWEEN should pass schema validation\n");
        return 1;
    }

    if (!validate_sql("SELECT * FROM users WHERE name BETWEEN 'a' AND 'z';", SQL_ERR)) {
        fprintf(stderr, "VARCHAR BETWEEN should fail schema validation\n");
        return 1;
    }

    if (!validate_sql(
            "INSERT INTO users VALUES (1, 'alice', 20, 'alice@example.com');", SQL_OK)) {
        fprintf(stderr, "valid INSERT should pass schema validation\n");
        return 1;
    }

    if (!validate_sql(
            "INSERT INTO users VALUES (1, 'alice', 'oops', 'alice@example.com');", SQL_ERR)) {
        fprintf(stderr, "invalid INT INSERT should fail schema validation\n");
        return 1;
    }

    return 0;
}
