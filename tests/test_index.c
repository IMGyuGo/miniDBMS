#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(path) _mkdir(path)
#else
#  define MKDIR(path) mkdir(path, 0755)
#endif

#include "index_manager.h"

#define ARRAY_LEN(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

#define ASSERT_TRUE(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __func__, __LINE__, #expr); \
            failed = 1;                                                       \
            goto cleanup;                                                     \
        }                                                                     \
    } while (0)

#define ASSERT_INT_EQ(expected, actual)                                       \
    do {                                                                      \
        int expected_value__ = (expected);                                    \
        int actual_value__ = (actual);                                        \
        if (expected_value__ != actual_value__) {                             \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: expected %d, got %d (%s)\n",               \
                    __func__, __LINE__, expected_value__, actual_value__,      \
                    #actual);                                                 \
            failed = 1;                                                       \
            goto cleanup;                                                     \
        }                                                                     \
    } while (0)

#define ASSERT_LONG_EQ(expected, actual)                                      \
    do {                                                                      \
        long expected_value__ = (expected);                                   \
        long actual_value__ = (actual);                                       \
        if (expected_value__ != actual_value__) {                             \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: expected %ld, got %ld (%s)\n",             \
                    __func__, __LINE__, expected_value__, actual_value__,      \
                    #actual);                                                 \
            failed = 1;                                                       \
            goto cleanup;                                                     \
        }                                                                     \
    } while (0)

static int ensure_data_dir(void) {
    if (MKDIR("data") == 0) return 1;
    return errno == EEXIST;
}

static void build_data_path(char *path, size_t size, const char *table) {
    snprintf(path, size, "data/%s.dat", table);
}

static void remove_table_file(const char *table) {
    char path[256];
    build_data_path(path, sizeof(path), table);
    (void)remove(path);
}

static int write_rows_with_offsets(const char *table,
                                   const char *const *rows,
                                   int row_count,
                                   long *offsets) {
    char path[256];
    FILE *fp = NULL;

    if (!ensure_data_dir()) return 0;

    build_data_path(path, sizeof(path), table);
    fp = fopen(path, "wb");
    if (!fp) return 0;

    for (int i = 0; i < row_count; i++) {
        if (offsets) offsets[i] = ftell(fp);

        if (fwrite(rows[i], 1, strlen(rows[i]), fp) != strlen(rows[i])) {
            fclose(fp);
            return 0;
        }

        if (fputc('\n', fp) == EOF) {
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return 1;
}

static int assert_offsets_equal(const long *expected,
                                const long *actual,
                                int count,
                                const char *test_name,
                                int line) {
    for (int i = 0; i < count; i++) {
        if (expected[i] != actual[i]) {
            fprintf(stderr,
                    "[FAIL] %s:%d: expected offsets[%d]=%ld, got %ld\n",
                    test_name, line, i, expected[i], actual[i]);
            return 0;
        }
    }
    return 1;
}

static int test_init_without_file_is_idempotent(void) {
    const char *table = "roleb_missing_init";
    int failed = 0;

    index_cleanup();
    remove_table_file(table);

    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    ASSERT_LONG_EQ(-1L, index_search_id(table, 10));
    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_DEFAULT, IDX_ORDER_DEFAULT));
    ASSERT_INT_EQ(1, index_height_id(table));
    ASSERT_INT_EQ(1, index_height_age(table));

cleanup:
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_insert_and_search_id(void) {
    const char *table = "roleb_insert_search";
    int failed = 0;

    index_cleanup();
    remove_table_file(table);

    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    ASSERT_INT_EQ(0, index_insert_id(table, 7, 42L));
    ASSERT_LONG_EQ(42L, index_search_id(table, 7));
    ASSERT_LONG_EQ(-1L, index_search_id(table, 999));
    ASSERT_INT_EQ(-1, index_insert_id("never_initialized", 1, 5L));

cleanup:
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_range_queries_for_id_and_age(void) {
    const char *table = "roleb_ranges";
    long id_offsets[4] = {0};
    long expected_id_offsets[2] = {30L, 50L};
    long expected_age_offsets[2] = {10L, 30L};
    long age_offsets_inline[4] = {0};
    int failed = 0;
    int count = 0;
    long *age_offsets = NULL;

    index_cleanup();
    remove_table_file(table);

    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    ASSERT_INT_EQ(0, index_insert_id(table, 1, 10L));
    ASSERT_INT_EQ(0, index_insert_id(table, 3, 30L));
    ASSERT_INT_EQ(0, index_insert_id(table, 5, 50L));
    ASSERT_INT_EQ(0, index_insert_age(table, 20, 10L));
    ASSERT_INT_EQ(0, index_insert_age(table, 20, 30L));
    ASSERT_INT_EQ(0, index_insert_age(table, 35, 50L));

    ASSERT_INT_EQ(2, index_range_id(table, 2, 5, id_offsets, ARRAY_LEN(id_offsets)));
    ASSERT_TRUE(assert_offsets_equal(expected_id_offsets, id_offsets, 2,
                                     __func__, __LINE__));

    ASSERT_INT_EQ(0, index_range_id(table, 6, 8, id_offsets, ARRAY_LEN(id_offsets)));

    age_offsets = index_range_age_alloc(table, 20, 25, &count);
    ASSERT_TRUE(age_offsets != NULL);
    ASSERT_INT_EQ(2, count);
    ASSERT_TRUE(assert_offsets_equal(expected_age_offsets, age_offsets, 2,
                                     __func__, __LINE__));
    free(age_offsets);
    age_offsets = NULL;

    ASSERT_INT_EQ(1, index_range_age(table, 35, 35,
                                     age_offsets_inline,
                                     ARRAY_LEN(age_offsets_inline)));
    ASSERT_LONG_EQ(50L, age_offsets_inline[0]);

cleanup:
    free(age_offsets);
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_height_and_io_stats(void) {
    const char *table = "roleb_height_io";
    int failed = 0;
    int count = 0;
    long *age_offsets = NULL;

    index_cleanup();
    remove_table_file(table);

    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    for (int i = 0; i < 12; i++) {
        ASSERT_INT_EQ(0, index_insert_id(table, i + 1, (long)(i + 1) * 100L));
        ASSERT_INT_EQ(0, index_insert_age(table, 20 + i, (long)(i + 1) * 100L));
    }

    ASSERT_TRUE(index_height_id(table) >= 2);
    ASSERT_TRUE(index_height_age(table) >= 2);

    index_reset_io_stats(table);
    ASSERT_INT_EQ(0, index_last_io_id(table));
    ASSERT_INT_EQ(0, index_last_io_age(table));

    ASSERT_LONG_EQ(900L, index_search_id(table, 9));
    ASSERT_TRUE(index_last_io_id(table) > 0);

    age_offsets = index_range_age_alloc(table, 24, 27, &count);
    ASSERT_TRUE(age_offsets != NULL);
    ASSERT_INT_EQ(4, count);
    ASSERT_TRUE(index_last_io_age(table) > 0);

cleanup:
    free(age_offsets);
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_io_stats_are_scoped_per_table(void) {
    const char *table_a = "roleb_io_scope_a";
    const char *table_b = "roleb_io_scope_b";
    int failed = 0;

    index_cleanup();
    remove_table_file(table_a);
    remove_table_file(table_b);

    ASSERT_INT_EQ(0, index_init(table_a, IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    ASSERT_INT_EQ(0, index_init(table_b, IDX_ORDER_SMALL, IDX_ORDER_SMALL));

    ASSERT_INT_EQ(0, index_insert_id(table_a, 1, 10L));
    ASSERT_INT_EQ(0, index_insert_id(table_b, 2, 20L));

    index_reset_io_stats(table_a);
    index_reset_io_stats(table_b);

    ASSERT_LONG_EQ(10L, index_search_id(table_a, 1));
    ASSERT_TRUE(index_last_io_id(table_a) > 0);
    ASSERT_INT_EQ(0, index_last_io_id(table_b));

    ASSERT_LONG_EQ(20L, index_search_id(table_b, 2));
    ASSERT_TRUE(index_last_io_id(table_b) > 0);
    ASSERT_INT_EQ(0, index_last_io_id(table_a));

cleanup:
    index_cleanup();
    remove_table_file(table_a);
    remove_table_file(table_b);
    return failed ? 1 : 0;
}

static int test_init_rebuilds_offsets_from_dat(void) {
    const char *table = "roleb_rebuild_offsets";
    const char *rows[] = {
        "1 | alice | 30 | alice@example.com",
        "2 | bob | 25 | bob@example.com",
        "3 | chris | 25 | chris@example.com"
    };
    long row_offsets[ARRAY_LEN(rows)] = {0};
    long id_offsets[4] = {0};
    long expected_id_offsets[2] = {0};
    int failed = 0;
    int age_count = 0;
    long *age_offsets = NULL;

    index_cleanup();
    remove_table_file(table);

    ASSERT_TRUE(write_rows_with_offsets(table, rows, ARRAY_LEN(rows), row_offsets));
    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));

    ASSERT_LONG_EQ(row_offsets[1], index_search_id(table, 2));
    ASSERT_INT_EQ(2, index_range_id(table, 1, 2, id_offsets, ARRAY_LEN(id_offsets)));
    expected_id_offsets[0] = row_offsets[0];
    expected_id_offsets[1] = row_offsets[1];
    ASSERT_TRUE(assert_offsets_equal(expected_id_offsets, id_offsets, 2,
                                     __func__, __LINE__));

    age_offsets = index_range_age_alloc(table, 25, 25, &age_count);
    ASSERT_TRUE(age_offsets != NULL);
    ASSERT_INT_EQ(2, age_count);
    expected_id_offsets[0] = row_offsets[1];
    expected_id_offsets[1] = row_offsets[2];
    ASSERT_TRUE(assert_offsets_equal(expected_id_offsets, age_offsets, 2,
                                     __func__, __LINE__));

cleanup:
    free(age_offsets);
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_init_skips_malformed_numeric_rows(void) {
    const char *table = "roleb_skip_bad_rows";
    const char *rows[] = {
        "1 | alice | 30 | alice@example.com",
        "bad | broken | 44 | wrong-id@example.com",
        "2 | bob | age? | wrong-age@example.com",
        "",
        "3 | chris | 31 | chris@example.com"
    };
    long row_offsets[ARRAY_LEN(rows)] = {0};
    long expected_age_offsets[1] = {0};
    int failed = 0;
    int age_count = 0;
    long *age_offsets = NULL;

    index_cleanup();
    remove_table_file(table);

    ASSERT_TRUE(write_rows_with_offsets(table, rows, ARRAY_LEN(rows), row_offsets));
    ASSERT_INT_EQ(0, index_init(table, IDX_ORDER_SMALL, IDX_ORDER_SMALL));

    ASSERT_LONG_EQ(row_offsets[0], index_search_id(table, 1));
    ASSERT_LONG_EQ(row_offsets[4], index_search_id(table, 3));
    ASSERT_LONG_EQ(-1L, index_search_id(table, 0));
    ASSERT_LONG_EQ(-1L, index_search_id(table, 2));

    age_offsets = index_range_age_alloc(table, 44, 44, &age_count);
    ASSERT_TRUE(age_offsets == NULL);
    ASSERT_INT_EQ(0, age_count);

    age_offsets = index_range_age_alloc(table, 31, 31, &age_count);
    ASSERT_TRUE(age_offsets != NULL);
    ASSERT_INT_EQ(1, age_count);
    expected_age_offsets[0] = row_offsets[4];
    ASSERT_TRUE(assert_offsets_equal(expected_age_offsets, age_offsets, 1,
                                     __func__, __LINE__));

cleanup:
    free(age_offsets);
    index_cleanup();
    remove_table_file(table);
    return failed ? 1 : 0;
}

static int test_uninitialized_table_errors_and_empty_ranges(void) {
    long offsets[2] = {0};
    int failed = 0;
    int count = -1;
    long *alloc_offsets = NULL;

    index_cleanup();

    ASSERT_LONG_EQ(-1L, index_search_id("roleb_never_init", 1));
    ASSERT_INT_EQ(-1, index_insert_id("roleb_never_init", 1, 10L));
    ASSERT_INT_EQ(0, index_range_id("roleb_never_init", 1, 10,
                                    offsets, ARRAY_LEN(offsets)));

    alloc_offsets = index_range_id_alloc("roleb_never_init", 1, 10, &count);
    ASSERT_TRUE(alloc_offsets == NULL);
    ASSERT_INT_EQ(0, count);

cleanup:
    free(alloc_offsets);
    index_cleanup();
    return failed ? 1 : 0;
}

static int test_max_tables_limit_keeps_existing_tables_idempotent(void) {
    char table_names[IDX_MAX_TABLES + 1][32] = {{0}};
    int failed = 0;

    index_cleanup();

    for (int i = 0; i < IDX_MAX_TABLES; i++) {
        snprintf(table_names[i], sizeof(table_names[i]), "roleb_limit_%d", i);
        remove_table_file(table_names[i]);
        ASSERT_INT_EQ(0, index_init(table_names[i],
                                    IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    }

    snprintf(table_names[IDX_MAX_TABLES], sizeof(table_names[IDX_MAX_TABLES]),
             "roleb_limit_extra");
    remove_table_file(table_names[IDX_MAX_TABLES]);

    ASSERT_INT_EQ(-1, index_init(table_names[IDX_MAX_TABLES],
                                 IDX_ORDER_SMALL, IDX_ORDER_SMALL));
    ASSERT_INT_EQ(0, index_init(table_names[0],
                                IDX_ORDER_DEFAULT, IDX_ORDER_DEFAULT));

cleanup:
    index_cleanup();
    for (int i = 0; i <= IDX_MAX_TABLES; i++) {
        if (table_names[i][0] != '\0')
            remove_table_file(table_names[i]);
    }
    return failed ? 1 : 0;
}

int main(void) {
    int failed = 0;

    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        {"init_without_file_is_idempotent", test_init_without_file_is_idempotent},
        {"insert_and_search_id", test_insert_and_search_id},
        {"range_queries_for_id_and_age", test_range_queries_for_id_and_age},
        {"height_and_io_stats", test_height_and_io_stats},
        {"io_stats_are_scoped_per_table", test_io_stats_are_scoped_per_table},
        {"init_rebuilds_offsets_from_dat", test_init_rebuilds_offsets_from_dat},
        {"init_skips_malformed_numeric_rows", test_init_skips_malformed_numeric_rows},
        {"uninitialized_table_errors_and_empty_ranges", test_uninitialized_table_errors_and_empty_ranges},
        {"max_tables_limit_keeps_existing_tables_idempotent", test_max_tables_limit_keeps_existing_tables_idempotent}
    };

    for (int i = 0; i < ARRAY_LEN(tests); i++) {
        if (tests[i].fn() == 0) {
            fprintf(stderr, "[PASS] %s\n", tests[i].name);
        } else {
            failed = 1;
        }
    }

    return failed ? 1 : 0;
}
