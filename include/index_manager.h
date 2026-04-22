#ifndef INDEX_MANAGER_H
#define INDEX_MANAGER_H

#define IDX_ORDER_DEFAULT  128
#define IDX_ORDER_SMALL      4
#define IDX_MAX_RANGE    65536
#define IDX_MAX_TABLES       8

/*
 * External synchronization contract for a rwlock-based runtime:
 *
 * 1. `src/index/` below this layer does not create or own runtime locks by itself.
 *    The caller must serialize access.
 *
 * 2. Table registry mutations are global write sections.
 *    - first registration via `index_init()` on a new table
 *    - `index_cleanup()`
 *
 * 3. Per-table data path can use a read/write lock after the table is registered.
 *    Write lock:
 *      - `.dat` append + `index_insert_*()`
 *      - rebuild on an already-registered table
 *    Read lock:
 *      - `index_search_id()`
 *      - `index_range_*()`
 *      - `index_height_*()`
 *
 * 4. `index_reset_io_stats()` and `index_last_io_*()` are per-thread query
 *    metadata helpers. They do not require a table write lock and are meant to
 *    bracket a single worker's read path.
 */

int  index_init(const char *table, int order_id, int order_age);
void index_cleanup(void);

int  index_insert_id(const char *table, int id, long offset);
long index_search_id(const char *table, int id);
int  index_range_id(const char *table, int from, int to,
                    long *offsets, int max);
long *index_range_id_alloc(const char *table, int from, int to,
                           int *out_count);

int  index_insert_age(const char *table, int age, long offset);
int  index_range_age(const char *table, int from, int to,
                     long *offsets, int max);
long *index_range_age_alloc(const char *table, int from, int to,
                            int *out_count);

int  index_height_id(const char *table);
int  index_height_age(const char *table);

/* Per-thread helper metadata for the calling worker's most recent read path. */
void index_reset_io_stats(const char *table);
int  index_last_io_id(const char *table);
int  index_last_io_age(const char *table);

#endif /* INDEX_MANAGER_H */
