/* =========================================================
 * threadpool.c — 스레드 풀 구현
 *
 * 담당: Role D
 *
 * 설계:
 *   - 링버퍼 잡 큐: head/tail/count 방식
 *   - pthread_mutex_t   : 큐 접근 보호
 *   - pthread_cond_t    : 잡 있음(not_empty) / 잡 공간 있음(not_full) 신호
 *   - stop 플래그로 안전 종료
 *
 * 스레드 안전성:
 *   - 모든 큐 접근은 mutex 로 보호한다.
 *   - 엔진 호출(db_service_execute_sql) 보호는 server.c 의 engine_lock 이 담당한다.
 * ========================================================= */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>

#include "threadpool.h"
#include "../../include/db_service.h"

/* ── 전방 선언 ── */
static void *worker_loop(void *arg);
static void  serialize_response(char *buf, size_t buf_size,
                                const DBServiceResult *res);
static void  send_http_response(int fd, int http_status,
                                const char *body, size_t body_len);

/* ── 엔진 보호용 전역 rwlock ──────────────────────────────
 * INSERT는 write lock, SELECT는 read lock을 잡는다.
 * MVP 기준: 안전성 최우선이므로 단순 mutex 사용.
 * TODO(RoleD): 성능 개선 시 pthread_rwlock_t 로 교체 검토.
 * --------------------------------------------------------- */
static pthread_mutex_t g_engine_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Threadpool 구조체 (불투명 타입 구현) ──────────────────── */
struct Threadpool {
    pthread_t         *threads;
    int                num_threads;

    ThreadpoolJob      queue[THREADPOOL_QUEUE_MAX];
    int                queue_head;
    int                queue_tail;
    int                queue_count;

    pthread_mutex_t    mutex;
    pthread_cond_t     cond_not_empty;
    pthread_cond_t     cond_not_full;

    int                stop;  /* 1이면 worker가 큐 소진 후 종료 */
};

/* ── 공개 API 구현 ──────────────────────────────────────── */

Threadpool *threadpool_create(int num_threads) {
    if (num_threads <= 0) return NULL;

    Threadpool *tp = (Threadpool *)calloc(1, sizeof(Threadpool));
    if (!tp) return NULL;

    tp->threads = (pthread_t *)calloc((size_t)num_threads, sizeof(pthread_t));
    if (!tp->threads) { free(tp); return NULL; }

    tp->num_threads = num_threads;
    tp->stop        = 0;

    if (pthread_mutex_init(&tp->mutex, NULL) != 0) {
        free(tp->threads); free(tp); return NULL;
    }
    if (pthread_cond_init(&tp->cond_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&tp->mutex);
        free(tp->threads); free(tp); return NULL;
    }
    if (pthread_cond_init(&tp->cond_not_full, NULL) != 0) {
        pthread_cond_destroy(&tp->cond_not_empty);
        pthread_mutex_destroy(&tp->mutex);
        free(tp->threads); free(tp); return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker_loop, tp) != 0) {
            /* 이미 생성된 스레드들도 정리 */
            tp->stop = 1;
            pthread_cond_broadcast(&tp->cond_not_empty);
            for (int j = 0; j < i; j++)
                pthread_join(tp->threads[j], NULL);
            pthread_cond_destroy(&tp->cond_not_full);
            pthread_cond_destroy(&tp->cond_not_empty);
            pthread_mutex_destroy(&tp->mutex);
            free(tp->threads); free(tp); return NULL;
        }
    }

    return tp;
}

int threadpool_submit(Threadpool *tp, const ThreadpoolJob *job) {
    if (!tp || !job) return 0;

    pthread_mutex_lock(&tp->mutex);

    if (tp->stop) {
        pthread_mutex_unlock(&tp->mutex);
        return 0;
    }

    if (tp->queue_count >= THREADPOOL_QUEUE_MAX) {
        /* 큐 가득 참: 드롭 (API_CODE_QUEUE_FULL 에 해당) */
        pthread_mutex_unlock(&tp->mutex);
        return 0;
    }

    tp->queue[tp->queue_tail] = *job;
    tp->queue_tail  = (tp->queue_tail + 1) % THREADPOOL_QUEUE_MAX;
    tp->queue_count++;

    pthread_cond_signal(&tp->cond_not_empty);
    pthread_mutex_unlock(&tp->mutex);
    return 1;
}

void threadpool_destroy(Threadpool *tp) {
    if (!tp) return;

    pthread_mutex_lock(&tp->mutex);
    tp->stop = 1;
    pthread_cond_broadcast(&tp->cond_not_empty);
    pthread_mutex_unlock(&tp->mutex);

    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->threads[i], NULL);

    /* 남은 잡의 conn_fd 닫기 */
    while (tp->queue_count > 0) {
        ThreadpoolJob *j = &tp->queue[tp->queue_head];
        if (j->conn_fd >= 0) close(j->conn_fd);
        tp->queue_head  = (tp->queue_head + 1) % THREADPOOL_QUEUE_MAX;
        tp->queue_count--;
    }

    pthread_cond_destroy(&tp->cond_not_full);
    pthread_cond_destroy(&tp->cond_not_empty);
    pthread_mutex_destroy(&tp->mutex);
    free(tp->threads);
    free(tp);
}

/* ── worker 루프 ──────────────────────────────────────────── */

static void *worker_loop(void *arg) {
    Threadpool *tp = (Threadpool *)arg;

    for (;;) {
        pthread_mutex_lock(&tp->mutex);

        while (tp->queue_count == 0 && !tp->stop)
            pthread_cond_wait(&tp->cond_not_empty, &tp->mutex);

        if (tp->stop && tp->queue_count == 0) {
            pthread_mutex_unlock(&tp->mutex);
            break;
        }

        /* 잡 하나 꺼내기 */
        ThreadpoolJob job = tp->queue[tp->queue_head];
        tp->queue_head  = (tp->queue_head + 1) % THREADPOOL_QUEUE_MAX;
        tp->queue_count--;

        pthread_cond_signal(&tp->cond_not_full);
        pthread_mutex_unlock(&tp->mutex);

        /* ── 잡 처리 ── */
        DBServiceOptions opts;
        db_service_options_init(&opts);
        opts.force_linear    = job.force_linear;
        opts.include_profile = job.include_profile;
        opts.emit_log        = job.emit_log;

        DBServiceResult result;
        db_service_result_init(&result);

        /* 엔진은 전역 mutex 로 보호한다 (MVP: 단순 직렬화) */
        pthread_mutex_lock(&g_engine_lock);
        db_service_execute_sql(job.sql, &opts, &result);
        pthread_mutex_unlock(&g_engine_lock);

        /* 응답 직렬화 및 전송 */
        char body[65536];
        serialize_response(body, sizeof(body), &result);
        size_t body_len = strlen(body);

        int http_status = 200;
        if (result.status != DB_SERVICE_OK) {
            switch (result.status) {
                case DB_SERVICE_BAD_REQUEST:  http_status = 400; break;
                case DB_SERVICE_PARSE_ERROR:  http_status = 400; break;
                case DB_SERVICE_SCHEMA_ERROR: http_status = 400; break;
                case DB_SERVICE_EXEC_ERROR:   http_status = 500; break;
                default:                      http_status = 500; break;
            }
        }

        send_http_response(job.conn_fd, http_status, body, body_len);
        db_service_result_free(&result);

        close(job.conn_fd);
    }

    return NULL;
}

/* ── JSON 직렬화 ─────────────────────────────────────────── */

/* 문자열 안의 특수문자를 JSON 이스케이프해서 buf에 쓴다. */
static int json_escape(char *buf, size_t buf_size, const char *src) {
    size_t pos = 0;
    for (; *src && pos + 2 < buf_size; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"') {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = '\\'; buf[pos++] = '"';
        } else if (c == '\\') {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = '\\'; buf[pos++] = '\\';
        } else if (c == '\n') {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = '\\'; buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = '\\'; buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 2 >= buf_size) break;
            buf[pos++] = '\\'; buf[pos++] = 't';
        } else {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

static void serialize_response(char *buf, size_t buf_size,
                               const DBServiceResult *res) {
    size_t pos = 0;

#define APPEND(...) do { \
    int _n = snprintf(buf + pos, buf_size - pos, __VA_ARGS__); \
    if (_n > 0) pos += (size_t)_n; \
    if (pos >= buf_size - 1) { pos = buf_size - 1; goto done; } \
} while (0)

    if (res->status != DB_SERVICE_OK) {
        char esc[512];
        json_escape(esc, sizeof(esc), res->message);
        APPEND("{\"ok\":false,\"code\":%d,\"error\":\"%s\"}",
               (int)res->status, esc);
        goto done;
    }

    if (res->stmt_type == STMT_INSERT) {
        APPEND("{\"ok\":true,\"stmt_type\":\"INSERT\",\"rows_affected\":%d}",
               res->rows_affected);
        goto done;
    }

    /* SELECT */
    APPEND("{\"ok\":true,\"stmt_type\":\"SELECT\"");
    APPEND(",\"row_count\":%d", res->result_set ? res->result_set->row_count : 0);
    APPEND(",\"rows\":[");

    if (res->has_result_set && res->result_set) {
        ResultSet *rs = res->result_set;
        for (int r = 0; r < rs->row_count; r++) {
            if (r > 0) APPEND(",");
            APPEND("{");
            for (int c = 0; c < rs->col_count; c++) {
                if (c > 0) APPEND(",");
                char key[128], val[512];
                json_escape(key, sizeof(key), rs->col_names[c]);
                json_escape(val, sizeof(val),
                            (rs->rows[r].values && rs->rows[r].values[c])
                            ? rs->rows[r].values[c] : "");
                APPEND("\"%s\":\"%s\"", key, val);
            }
            APPEND("}");
        }
    }
    APPEND("]");

    if (res->has_profile) {
        char path[64];
        json_escape(path, sizeof(path), res->profile.access_path);
        APPEND(",\"profile\":{\"access_path\":\"%s\","
               "\"elapsed_ms\":%.3f,\"tree_io\":%d,\"row_count\":%d}",
               path,
               res->profile.elapsed_ms,
               res->profile.tree_io,
               res->profile.row_count);
    }

    APPEND("}");

done:
    buf[buf_size - 1] = '\0';
#undef APPEND
}

/* ── HTTP 응답 전송 ──────────────────────────────────────── */

static void send_http_response(int fd, int http_status,
                               const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        http_status,
        (http_status == 200) ? "OK" :
        (http_status == 400) ? "Bad Request" : "Internal Server Error",
        body_len);

    /* 헤더 전송 */
    if (hlen > 0) {
        ssize_t sent = 0, total = (ssize_t)hlen;
        while (sent < total) {
            ssize_t n = write(fd, header + sent, (size_t)(total - sent));
            if (n <= 0) return;
            sent += n;
        }
    }

    /* 바디 전송 */
    ssize_t sent = 0, total = (ssize_t)body_len;
    while (sent < total) {
        ssize_t n = write(fd, body + sent, (size_t)(total - sent));
        if (n <= 0) return;
        sent += n;
    }
}
