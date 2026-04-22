# Index Rebuild Flow

## Source Of Truth

- `data/{table}.dat` is the persistent source of truth.
- The in-memory id/age trees are rebuildable acceleration structures.
- If the process exits after appending a row but before both index inserts
  finish, the next `index_init()` rebuild repairs the in-memory index from
  the table file.

## Rebuild Steps

1. Create empty id and age trees for the target table.
2. Open `data/{table}.dat` in binary mode (`rb`).
3. For each line in the file:
   - capture the row start offset with `ftell()`
   - trim trailing `CR` / `LF`
   - skip blank lines
   - read `col0` as `id`
   - read `col2` as `age`
   - skip the row if either numeric field is malformed
   - insert `(id, offset)` into the id tree
   - insert `(age, offset)` into the age tree
4. Mark the table as initialized.

## Current Recovery Rules

- Missing `.dat` file is not an error. It means "initialized empty index".
- Malformed numeric rows are skipped during rebuild so they do not pollute the
  trees with `0` keys from lossy parsing.
- Duplicate `age` values are valid and are returned as multiple offsets during
  range search.
- `id` uniqueness is not enforced in `src/index/**`; callers must keep that
  contract if they need it.

## Protection Windows For Role D

The rwlock contract should be split into two lock domains.

### 1. Global Registry Write Section

This protects the in-memory table registry itself (`g_tables[]`, `g_count`).

- first `index_init()` on a table that is not registered yet
- `index_cleanup()`

If the runtime keeps lazy table initialization, the first request that sees a
new table must enter this global exclusive section before any per-table rwlock
discipline matters.

### 2. Per-Table Read/Write Lock

After a table entry exists, the data path can use a per-table rwlock.

Write lock:

- `ftell()` before append
- row append to `data/{table}.dat`
- file close after append
- `index_insert_id()`
- `index_insert_age()`
- rebuild work on an already-registered table

Read lock:

- `index_search_id()`
- `index_range_id()` / `index_range_id_alloc()`
- `index_range_age()` / `index_range_age_alloc()`
- `index_height_id()` / `index_height_age()`

### 3. Query I/O Metadata

`index_reset_io_stats()` and `index_last_io_*()` are per-thread helpers for the
calling worker's most recent read path. They should not force the runtime to
escalate a reader into a write lock.

That contract matters because a shared-lock runtime cannot safely rely on
table-global mutable `last_io` counters. The index layer therefore keeps this
metadata per worker thread instead of per table entry.

## Non-Negotiables

- Keep `.dat` access in binary mode.
- Keep row output on `stdout` and diagnostics on `stderr`.
- Do not bypass `include/index_manager.h` from runtime code.
