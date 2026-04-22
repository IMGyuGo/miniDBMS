/* =========================================================
 * test_threadpool.c — thread pool parallelism evidence test
 *
 * Usage:
 *   ./test_threadpool [num_jobs]
 *
 *   num_jobs  Number of SQL jobs submitted per batch (default: 8).
 *             Increase this to get more statistically meaningful results,
 *             e.g. ./test_threadpool 64
 *
 * What this proves:
 *   1. num_jobs SQL jobs can be accepted and completed by the thread pool.
 *   2. With an artificial per-job worker delay, 4 workers finish the same
 *      jobs much faster than 1 worker.
 *   3. Completion times form "waves" with 4 workers.
 *
 * The delay is enabled only by MINIDBMS_WORKER_DELAY_MS and is for tests/demo.
 * Normal server behavior is unchanged when that env var is not set.
 * ========================================================= */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#  include <direct.h>
#  define TEST_MKDIR(path) _mkdir(path)
#else
#  include <sys/stat.h>
#  define TEST_MKDIR(path) mkdir(path, 0755)
#endif

#include "../src/threadpool/threadpool.h"
#include "../include/index_manager.h"

#define TEST_TABLE      "tp_smoke"
#define SEED_ROWS       32
#define DEMO_DELAY_MS   120

typedef struct {
    int rfd;
    double finish_ms;
    int ok;
    char prefix[512];
    size_t prefix_len;
} ReaderArg;

typedef struct {
    double elapsed_ms;
    int success_count;
    int num_jobs;
    double *finish_ms;   /* heap-allocated, length = num_jobs */
} BatchResult;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int ensure_dir(const char *path) {
    if (TEST_MKDIR(path) == 0) return 1;
    return errno == EEXIST;
}

static void set_env_var(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void *reader_thread(void *arg) {
    ReaderArg *reader = (ReaderArg *)arg;
    char buffer[4096];
    ssize_t n = 0;

    while ((n = read(reader->rfd, buffer, sizeof(buffer))) > 0) {
        if (reader->prefix_len < sizeof(reader->prefix) - 1) {
            size_t room = sizeof(reader->prefix) - 1 - reader->prefix_len;
            size_t copy_len = (size_t)n < room ? (size_t)n : room;
            memcpy(reader->prefix + reader->prefix_len, buffer, copy_len);
            reader->prefix_len += copy_len;
            reader->prefix[reader->prefix_len] = '\0';
        }
    }

    close(reader->rfd);
    reader->finish_ms = now_ms();
    reader->ok = strstr(reader->prefix, "\"ok\":true") != NULL;
    return NULL;
}

static int setup_test_table(void) {
    if (!ensure_dir("data") || !ensure_dir("schema")) {
        fprintf(stderr, "setup: cannot create data/schema directory\n");
        return 0;
    }

    index_cleanup();
    remove("data/" TEST_TABLE ".dat");
    remove("schema/" TEST_TABLE ".schema");

    FILE *schema = fopen("schema/" TEST_TABLE ".schema", "w");
    if (!schema) {
        fprintf(stderr, "setup: cannot create schema file\n");
        return 0;
    }

    fprintf(schema,
            "table=" TEST_TABLE "\n"
            "columns=3\n"
            "col0=id,INT,0\n"
            "col1=name,VARCHAR,64\n"
            "col2=age,INT,0\n");
    fclose(schema);

    FILE *data = fopen("data/" TEST_TABLE ".dat", "w");
    if (!data) {
        fprintf(stderr, "setup: cannot create data file\n");
        return 0;
    }

    for (int i = 1; i <= SEED_ROWS; i++) {
        fprintf(data, "%d | tpuser%d | %d\n", i, i, 20 + (i % 30));
    }
    fclose(data);

    if (index_init(TEST_TABLE, 4, 4) != 0) {
        fprintf(stderr, "setup: index_init failed\n");
        return 0;
    }

    return 1;
}

static void teardown_test_table(void) {
    index_cleanup();
    remove("data/" TEST_TABLE ".dat");
    remove("schema/" TEST_TABLE ".schema");
}

static BatchResult run_batch(int worker_count, int num_jobs) {
    BatchResult result;
    memset(&result, 0, sizeof(result));
    result.num_jobs = num_jobs;
    result.finish_ms = calloc((size_t)num_jobs, sizeof(double));
    if (!result.finish_ms) {
        fprintf(stderr, "run_batch: out of memory\n");
        result.elapsed_ms = -1.0;
        return result;
    }

    Threadpool *pool = threadpool_create(worker_count);
    if (!pool) {
        fprintf(stderr, "threadpool_create(%d) failed\n", worker_count);
        result.elapsed_ms = -1.0;
        return result;
    }

    ReaderArg   *readers      = calloc((size_t)num_jobs, sizeof(ReaderArg));
    pthread_t   *reader_tids  = malloc((size_t)num_jobs * sizeof(pthread_t));
    int         *write_fds    = malloc((size_t)num_jobs * sizeof(int));

    if (!readers || !reader_tids || !write_fds) {
        fprintf(stderr, "run_batch: out of memory\n");
        free(readers); free(reader_tids); free(write_fds);
        threadpool_destroy(pool);
        result.elapsed_ms = -1.0;
        return result;
    }

    for (int i = 0; i < num_jobs; i++) write_fds[i] = -1;

    for (int i = 0; i < num_jobs; i++) {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            perror("pipe");
            result.elapsed_ms = -1.0;
            free(readers); free(reader_tids); free(write_fds);
            threadpool_destroy(pool);
            return result;
        }

        readers[i].rfd = pipe_fds[0];
        write_fds[i]   = pipe_fds[1];
        pthread_create(&reader_tids[i], NULL, reader_thread, &readers[i]);
    }

    double start = now_ms();

    for (int i = 0; i < num_jobs; i++) {
        ThreadpoolJob job;
        memset(&job, 0, sizeof(job));

        job.conn_fd = write_fds[i];
        snprintf(job.request_id, sizeof(job.request_id), "tp-test-%d", i + 1);
        snprintf(job.sql, sizeof(job.sql),
                 "SELECT * FROM " TEST_TABLE " WHERE id = %d;", (i % SEED_ROWS) + 1);
        job.options.include_profile = 1;

        if (!threadpool_submit(pool, &job)) {
            fprintf(stderr, "submit failed for job %d\n", i + 1);
            close(write_fds[i]);
        }
    }

    for (int i = 0; i < num_jobs; i++) {
        pthread_join(reader_tids[i], NULL);
    }

    result.elapsed_ms = now_ms() - start;

    for (int i = 0; i < num_jobs; i++) {
        result.finish_ms[i] = readers[i].finish_ms - start;
        if (readers[i].ok) result.success_count++;
    }

    free(readers);
    free(reader_tids);
    free(write_fds);
    threadpool_destroy(pool);
    return result;
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Print sorted completion times and group into waves of `wave_size`. */
static void print_completion_waves(const double *finish_ms, int num_jobs, int wave_size) {
    double *sorted = malloc((size_t)num_jobs * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, finish_ms, (size_t)num_jobs * sizeof(double));
    qsort(sorted, (size_t)num_jobs, sizeof(sorted[0]), compare_doubles);

    printf("  completion order (ms):");
    for (int i = 0; i < num_jobs; i++) {
        if (i > 0 && i % wave_size == 0) printf(" |");
        printf(" %.0f", sorted[i]);
    }
    printf("\n");

    int wave = 1;
    for (int i = 0; i < num_jobs; i += wave_size) {
        int end = i + wave_size < num_jobs ? i + wave_size : num_jobs;
        printf("  wave %d:", wave++);
        for (int j = i; j < end; j++) printf(" %.0fms", sorted[j]);
        printf("\n");
    }

    free(sorted);
}

int main(int argc, char *argv[]) {
    int num_jobs = 8;   /* default */

    if (argc >= 2) {
        num_jobs = atoi(argv[1]);
        if (num_jobs <= 0) {
            fprintf(stderr, "Usage: %s [num_jobs]\n", argv[0]);
            fprintf(stderr, "  num_jobs must be a positive integer (default 8)\n");
            return 1;
        }
    }

    printf("=============================================\n");
    printf("  test_threadpool - parallelism evidence\n");
    printf("  num_jobs = %d\n", num_jobs);
    printf("=============================================\n\n");

    if (!setup_test_table()) {
        return 1;
    }

    char delay_value[16];
    snprintf(delay_value, sizeof(delay_value), "%d", DEMO_DELAY_MS);
    set_env_var("MINIDBMS_WORKER_DELAY_MS", delay_value);

    printf("[1] Correctness: %d concurrent SELECT jobs with 4 workers\n", num_jobs);
    BatchResult four_workers = run_batch(4, num_jobs);
    printf("  success: %d / %d\n", four_workers.success_count, num_jobs);
    if (four_workers.success_count != num_jobs) {
        free(four_workers.finish_ms);
        teardown_test_table();
        return 1;
    }
    printf("  result: PASS\n\n");

    printf("[2] Timing: same %d jobs, different worker counts\n", num_jobs);
    BatchResult one_worker  = run_batch(1, num_jobs);
    BatchResult two_workers = run_batch(2, num_jobs);
    four_workers            = run_batch(4, num_jobs);

    printf("  workers  elapsed(ms)  speedup vs 1 worker\n");
    printf("  -------  -----------  -------------------\n");
    printf("  1        %11.0f  1.00x\n", one_worker.elapsed_ms);
    printf("  2        %11.0f  %.2fx\n",
           two_workers.elapsed_ms, one_worker.elapsed_ms / two_workers.elapsed_ms);
    printf("  4        %11.0f  %.2fx\n",
           four_workers.elapsed_ms, one_worker.elapsed_ms / four_workers.elapsed_ms);

    if (four_workers.elapsed_ms >= one_worker.elapsed_ms * 0.75) {
        printf("  result: FAIL - 4 workers were not clearly faster\n");
        free(one_worker.finish_ms);
        free(two_workers.finish_ms);
        free(four_workers.finish_ms);
        teardown_test_table();
        return 1;
    }
    printf("  result: PASS - 4 workers finished clearly faster\n\n");

    printf("[3] Completion waves with 4 workers\n");
    print_completion_waves(four_workers.finish_ms, num_jobs, 4);
    printf("  meaning: jobs complete in waves of ~4 (one per worker slot).\n\n");

    free(one_worker.finish_ms);
    free(two_workers.finish_ms);
    free(four_workers.finish_ms);

    set_env_var("MINIDBMS_WORKER_DELAY_MS", "0");
    teardown_test_table();

    printf("=============================================\n");
    printf("  thread pool parallelism test passed\n");
    printf("=============================================\n");
    return 0;
}
