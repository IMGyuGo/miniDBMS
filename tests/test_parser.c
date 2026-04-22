#include <stdio.h>
#include <string.h>

#include "../include/interface.h"

static int test_select_between(void) {
    TokenList *tokens = lexer_tokenize("SELECT * FROM users WHERE age BETWEEN 10 AND 20;");
    ASTNode *ast = NULL;

    if (!tokens) return 0;
    ast = parser_parse(tokens);

    if (!ast) {
        lexer_free(tokens);
        return 0;
    }

    if (ast->type != STMT_SELECT) return 0;
    if (!ast->select.select_all) return 0;
    if (!ast->select.has_where) return 0;
    if (ast->select.where.type != WHERE_BETWEEN) return 0;
    if (strcmp(ast->select.where.col, "age") != 0) return 0;
    if (strcmp(ast->select.where.val_from, "10") != 0) return 0;
    if (strcmp(ast->select.where.val_to, "20") != 0) return 0;

    parser_free(ast);
    lexer_free(tokens);
    return 1;
}

static int test_insert_with_columns(void) {
    TokenList *tokens = lexer_tokenize(
        "INSERT INTO users (id, name, age, email) VALUES (1, 'alice', 20, 'a@b.com');");
    ASTNode *ast = NULL;

    if (!tokens) return 0;
    ast = parser_parse(tokens);

    if (!ast) {
        lexer_free(tokens);
        return 0;
    }

    if (ast->type != STMT_INSERT) return 0;
    if (strcmp(ast->insert.table, "users") != 0) return 0;
    if (ast->insert.column_count != 4) return 0;
    if (ast->insert.value_count != 4) return 0;
    if (strcmp(ast->insert.columns[1], "name") != 0) return 0;
    if (strcmp(ast->insert.values[1], "alice") != 0) return 0;

    parser_free(ast);
    lexer_free(tokens);
    return 1;
}

int main(void) {
    if (!test_select_between()) {
        fprintf(stderr, "test_select_between failed\n");
        return 1;
    }

    if (!test_insert_with_columns()) {
        fprintf(stderr, "test_insert_with_columns failed\n");
        return 1;
    }

    return 0;
}
