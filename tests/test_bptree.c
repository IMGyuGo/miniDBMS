#include <stdio.h>
#include <stdlib.h>

#include "../include/bptree.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_fail++; \
    } \
} while (0)

static BPTree *must_create_tree(int order) {
    BPTree *tree = bptree_create(order);
    CHECK(tree != NULL, "bptree_create returns non-NULL");
    return tree;
}

static void test_create_and_empty_search(void) {
    printf("\n[T1] create / empty search\n");

    BPTree *tree = must_create_tree(2);
    if (!tree) return;

    CHECK(bptree_height(tree) == 1, "minimum order tree starts at height 1");
    CHECK(bptree_search(tree, 42) == -1, "search on empty tree returns -1");
    CHECK(bptree_last_io(tree) >= 1, "empty search still records node visit");

    bptree_destroy(tree);
}

static void test_basic_insert_and_search(void) {
    printf("\n[T2] basic insert / search\n");

    BPTree *tree = must_create_tree(4);
    if (!tree) return;

    CHECK(bptree_insert(tree, 10, 100L) == 0, "insert key 10");
    CHECK(bptree_insert(tree, 20, 200L) == 0, "insert key 20");
    CHECK(bptree_insert(tree, 30, 300L) == 0, "insert key 30");

    CHECK(bptree_search(tree, 10) == 100L, "search key 10");
    CHECK(bptree_search(tree, 20) == 200L, "search key 20");
    CHECK(bptree_search(tree, 30) == 300L, "search key 30");
    CHECK(bptree_search(tree, 99) == -1, "search missing key returns -1");

    bptree_destroy(tree);
}

static void test_duplicate_key_offsets_are_sorted(void) {
    printf("\n[T3] duplicate key ordering\n");

    BPTree *tree = must_create_tree(4);
    long *offsets = NULL;
    int count = 0;

    if (!tree) return;

    CHECK(bptree_insert(tree, 7, 300L) == 0, "insert duplicate offset 300");
    CHECK(bptree_insert(tree, 7, 100L) == 0, "insert duplicate offset 100");
    CHECK(bptree_insert(tree, 7, 200L) == 0, "insert duplicate offset 200");

    CHECK(bptree_search(tree, 7) == 100L,
          "point search returns smallest offset for duplicate key");

    offsets = bptree_range_alloc(tree, 7, 7, &count);
    CHECK(offsets != NULL, "range_alloc for duplicate key returns data");
    CHECK(count == 3, "duplicate key expands to 3 offsets");
    if (offsets && count == 3) {
        CHECK(offsets[0] == 100L, "duplicate offset[0] sorted");
        CHECK(offsets[1] == 200L, "duplicate offset[1] sorted");
        CHECK(offsets[2] == 300L, "duplicate offset[2] sorted");
    }

    free(offsets);
    bptree_destroy(tree);
}

static void test_identical_offset_is_not_duplicated(void) {
    printf("\n[T4] identical key+offset dedupe\n");

    BPTree *tree = must_create_tree(4);
    long *offsets = NULL;
    int count = 0;

    if (!tree) return;

    CHECK(bptree_insert(tree, 11, 500L) == 0, "insert first identical offset");
    CHECK(bptree_insert(tree, 11, 500L) == 0, "insert same key+offset again");

    offsets = bptree_range_alloc(tree, 11, 11, &count);
    CHECK(offsets != NULL, "range_alloc returns deduped offset");
    CHECK(count == 1, "identical key+offset is stored once");
    if (offsets && count == 1)
        CHECK(offsets[0] == 500L, "deduped offset value is preserved");

    free(offsets);
    bptree_destroy(tree);
}

static void test_leaf_and_root_split(void) {
    printf("\n[T5] leaf split / root promotion\n");

    BPTree *tree = must_create_tree(3);
    if (!tree) return;

    CHECK(bptree_insert(tree, 10, 10L) == 0, "insert 10");
    CHECK(bptree_insert(tree, 20, 20L) == 0, "insert 20");
    CHECK(bptree_insert(tree, 30, 30L) == 0, "insert 30 causes split");

    CHECK(bptree_height(tree) == 2, "root split increases height to 2");
    CHECK(bptree_search(tree, 10) == 10L, "search survives split for 10");
    CHECK(bptree_search(tree, 20) == 20L, "search survives split for 20");
    CHECK(bptree_search(tree, 30) == 30L, "search survives split for 30");

    bptree_destroy(tree);
}

static void test_multi_level_growth_and_range_scan(void) {
    printf("\n[T6] multi-level growth / range scan\n");

    BPTree *tree = must_create_tree(3);
    long *offsets = NULL;
    int count = 0;

    if (!tree) return;

    for (int key = 1; key <= 12; key++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "insert key %d", key);
        CHECK(bptree_insert(tree, key, (long)key * 10L) == 0, msg);
    }

    CHECK(bptree_height(tree) >= 3, "many inserts grow tree beyond one internal level");

    offsets = bptree_range_alloc(tree, 3, 9, &count);
    CHECK(offsets != NULL, "range_alloc across multiple leaves returns data");
    CHECK(count == 7, "range_alloc count matches inclusive key span");
    if (offsets && count == 7) {
        for (int i = 0; i < count; i++) {
            long expected = (long)(i + 3) * 10L;
            char msg[64];
            snprintf(msg, sizeof(msg), "range result[%d] keeps key order", i);
            CHECK(offsets[i] == expected, msg);
        }
    }
    CHECK(bptree_last_io(tree) > 1, "multi-leaf range records more than one node visit");

    free(offsets);
    bptree_destroy(tree);
}

static void test_empty_range_result(void) {
    printf("\n[T7] empty range result\n");

    BPTree *tree = must_create_tree(4);
    long *offsets = NULL;
    int count = -1;

    if (!tree) return;

    CHECK(bptree_insert(tree, 1, 10L) == 0, "insert key 1");
    CHECK(bptree_insert(tree, 3, 30L) == 0, "insert key 3");
    CHECK(bptree_insert(tree, 5, 50L) == 0, "insert key 5");

    offsets = bptree_range_alloc(tree, 6, 8, &count);
    CHECK(offsets == NULL, "range with no matching keys returns NULL");
    CHECK(count == 0, "range with no matching keys sets out_count to 0");

    bptree_destroy(tree);
}

static void test_limited_range_copy(void) {
    printf("\n[T8] limited range copy\n");

    BPTree *tree = must_create_tree(4);
    long out[3] = {0, 0, 0};
    int copied = 0;

    if (!tree) return;

    for (int key = 1; key <= 5; key++)
        CHECK(bptree_insert(tree, key, (long)key * 100L) == 0, "insert for limited range");

    copied = bptree_range(tree, 1, 5, out, 3);
    CHECK(copied == 3, "bptree_range respects max_count");
    CHECK(out[0] == 100L, "limited range out[0]");
    CHECK(out[1] == 200L, "limited range out[1]");
    CHECK(out[2] == 300L, "limited range out[2]");

    bptree_destroy(tree);
}

static void test_invalid_arguments(void) {
    printf("\n[T9] invalid argument handling\n");

    BPTree *tree = must_create_tree(4);
    long out[2] = {0, 0};
    int count = -1;

    if (!tree) return;

    CHECK(bptree_insert(NULL, 1, 10L) == -1, "insert NULL tree returns -1");
    CHECK(bptree_search(NULL, 1) == -1, "search NULL tree returns -1");
    CHECK(bptree_range(NULL, 1, 2, out, 2) == 0, "range NULL tree returns 0");
    CHECK(bptree_range(tree, 5, 1, out, 2) == 0, "range with reversed bounds returns 0");
    CHECK(bptree_range_alloc(tree, 5, 1, &count) == NULL,
          "range_alloc with reversed bounds returns NULL");
    CHECK(bptree_range_alloc(tree, 1, 5, NULL) == NULL,
          "range_alloc without out_count returns NULL");

    bptree_destroy(tree);
}

int main(void) {
    printf("========================================\n");
    printf("  test_bptree — Role A unit test\n");
    printf("========================================\n");

    test_create_and_empty_search();
    test_basic_insert_and_search();
    test_duplicate_key_offsets_are_sorted();
    test_identical_offset_is_not_duplicated();
    test_leaf_and_root_split();
    test_multi_level_growth_and_range_scan();
    test_empty_range_result();
    test_limited_range_copy();
    test_invalid_arguments();

    printf("\n========================================\n");
    printf("  result: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");

    return g_fail > 0 ? 1 : 0;
}
