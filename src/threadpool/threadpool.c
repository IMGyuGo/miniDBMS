/* =========================================================
 * threadpool.c — 스레드 풀 구현
 *
 * 담당: Role D
 *
 * 설계:
 *   - 링버퍼 잡 큐: head/tail/count 방식
 *   - pthread_mutex_t   : 큐 접근 보호
 *   - pthread_cond_t    : 잡 있음(not_empty) 신호
 *   - stop 플래그로 안전 종료
 *
 * 스레드 안전성:
 *   - 모든 큐 접근은 mutex 로 보호한다.
 *   - 엔진 호출(db_service_execute_sql) 보호는 g_engine_rwlock 이 담당한다.
 *     SELECT → rdlock (동시 읽기 허용), INSERT → wrlock (단독 쓰기).
 *
 * Role C 연동:
 *   - JSON 직렬화/파싱은 src/http/http_message.h 의 함수를 사용한다.
 *   - 상태코드 매핑은 http_response_meta_from_service() 에 위임한다.
 * ========================================================= */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "threadpool.h"
#include "../../include/db_service.h"
#include "../http/http_message.h"

/* ── 전방 선언 ── */
static void *worker_loop(void *arg);
static void  send_http_response(int fd, int http_status,
                                const char *body, size_t body_len);

/* ── 엔진 보호용 rwlock ───────────────────────────────────
 * SELECT  → rdlock  (동시 읽기 허용)
 * INSERT  → wrlock  (단독 쓰기)
 * 판별은 sql 문자열 앞 6자 대소문자 비교로 처리한다.
 * --------------------------------------------------------- */
static pthread_rwlock_t g_engine_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static int worker_trace_enabled(void) {
    const char *value = getenv("MINIDBMS_TRACE_WORKERS");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int worker_delay_ms(void) {
    const char *value = getenv("MINIDBMS_WORKER_DELAY_MS");
    if (!value || value[0] == '\0') return 0;

    int delay = atoi(value);
    if (delay < 0) return 0;
    if (delay > 5000) return 5000;
    return delay;
}

/* ── Threadpool 구조체 ────────────────────────────────────── */
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

    int                stop;
};

/* ── 공개 API ─────────────────────────────────────────────── */

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

    if (tp->stop || tp->queue_count >= THREADPOOL_QUEUE_MAX) {
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

        ThreadpoolJob job = tp->queue[tp->queue_head];
        tp->queue_head  = (tp->queue_head + 1) % THREADPOOL_QUEUE_MAX;
        tp->queue_count--;

        pthread_cond_signal(&tp->cond_not_full);
        pthread_mutex_unlock(&tp->mutex);

        /* ── 잡 처리 ── */
        int trace = worker_trace_enabled();
        int delay_ms = worker_delay_ms();
        unsigned long worker_id = (unsigned long)pthread_self();

        if (trace) {
            fprintf(stderr,
                    "[worker %lu] start request_id=%s sql=\"%.80s\"\n",
                    worker_id,
                    job.request_id[0] ? job.request_id : "-",
                    job.sql);
        }

        if (delay_ms > 0) {
            struct timespec delay;
            delay.tv_sec = delay_ms / 1000;
            delay.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
            nanosleep(&delay, NULL);
        }

        DBServiceOptions opts;
        db_service_options_init(&opts);
        /* ApiQueryOptions → DBServiceOptions 매핑 */
        opts.force_linear    = job.options.force_linear;
        opts.compare         = job.options.compare;
        opts.include_profile = job.options.include_profile;
        opts.emit_log        = 0;  /* 서버 모드에서는 stderr 진단 로그 끔 */

        DBServiceResult result;
        db_service_result_init(&result);

        /* SELECT 는 rdlock(동시 읽기), 나머지(INSERT 등)는 wrlock(단독 쓰기) */
        int is_select = (strncasecmp(job.sql, "SELECT", 6) == 0);
        if (is_select)
            pthread_rwlock_rdlock(&g_engine_rwlock);
        else
            pthread_rwlock_wrlock(&g_engine_rwlock);

        db_service_execute_sql(job.sql, &opts, &result);

        pthread_rwlock_unlock(&g_engine_rwlock);

        /* ── Role C: DBServiceResult → ApiResponseMeta 변환 ── */
        ApiResponseMeta meta;
        http_response_meta_from_service(&result, &meta);
        strncpy(meta.request_id, job.request_id, sizeof(meta.request_id) - 1);

        /* ── Role C: JSON 직렬화 ── */
        char body[65536];
        body[0] = '\0';
        http_serialize_query_response(&meta, &result, body, sizeof(body));

        /* ── HTTP 응답 전송 ── */
        send_http_response(job.conn_fd, meta.http_status, body, strlen(body));

        if (trace) {
            fprintf(stderr,
                    "[worker %lu] done  request_id=%s status=%d rows=%d\n",
                    worker_id,
                    job.request_id[0] ? job.request_id : "-",
                    meta.http_status,
                    meta.row_count);
        }

        db_service_result_free(&result);
        close(job.conn_fd);
    }

    return NULL;
}

/* ── HTTP 응답 전송 (transport 계층: Role D 소유) ─────────── */

static void send_http_response(int fd, int http_status,
                               const char *body, size_t body_len) {
    const char *status_str =
        (http_status == 200) ? "OK" :
        (http_status == 400) ? "Bad Request" :
        (http_status == 404) ? "Not Found" :
        (http_status == 405) ? "Method Not Allowed" :
        (http_status == 503) ? "Service Unavailable" :
                               "Internal Server Error";

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        http_status, status_str, body_len);

    if (hlen > 0) {
        ssize_t sent = 0;
        while (sent < (ssize_t)hlen) {
            ssize_t n = write(fd, header + sent, (size_t)(hlen - sent));
            if (n <= 0) return;
            sent += n;
        }
    }

    ssize_t sent = 0;
    while (sent < (ssize_t)body_len) {
        ssize_t n = write(fd, body + sent, (size_t)(body_len - (size_t)sent));
        if (n <= 0) return;
        sent += n;
    }
}
