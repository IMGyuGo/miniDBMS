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
    /* 발표/디버깅용 옵션: worker별 start/done 로그를 stderr에 찍는다. */
    const char *value = getenv("MINIDBMS_TRACE_WORKERS");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int worker_delay_ms(void) {
    /*
     * 병렬 처리 효과를 눈으로 보기 위한 테스트용 지연 시간이다.
     * 실제 서버 동작에서는 환경변수를 주지 않으면 0ms라서 지연이 없다.
     */
    const char *value = getenv("MINIDBMS_WORKER_DELAY_MS");
    if (!value || value[0] == '\0') return 0;

    int delay = atoi(value);
    if (delay < 0) return 0;
    if (delay > 5000) return 5000;
    return delay;
}

/* ── Threadpool 구조체 ────────────────────────────────────── */
struct Threadpool {
    /*
     * threads:
     *   threadpool_create() 때 만들어지는 worker thread 목록.
     *   각 worker는 같은 Threadpool 구조체를 공유하면서 queue에서 일을 꺼낸다.
     */
    pthread_t         *threads;
    int                num_threads;

    /*
     * queue:
     *   서버 accept loop가 만든 ThreadpoolJob이 들어가는 고정 크기 링버퍼.
     *   head는 다음에 꺼낼 위치, tail은 다음에 넣을 위치, count는 현재 job 수다.
     */
    ThreadpoolJob      queue[THREADPOOL_QUEUE_MAX];
    int                queue_head;
    int                queue_tail;
    int                queue_count;

    /*
     * mutex/cond:
     *   queue_head/tail/count/stop은 여러 스레드가 동시에 보므로 mutex로 보호한다.
     *   cond_not_empty는 "queue에 job이 들어왔다"는 신호로 worker를 깨운다.
     *   cond_not_full은 현재 코드에서는 signal만 하고 대기에는 쓰지 않는 여유 확장 포인트다.
     */
    pthread_mutex_t    mutex;
    pthread_cond_t     cond_not_empty;
    pthread_cond_t     cond_not_full;

    /*
     * stop:
     *   destroy 시 1로 바뀐다.
     *   worker는 stop=1이고 queue가 비었으면 루프를 빠져나와 종료한다.
     */
    int                stop;
};

/* ── 공개 API ─────────────────────────────────────────────── */

Threadpool *threadpool_create(int num_threads) {
    if (num_threads <= 0) return NULL;

    /* calloc으로 0 초기화해서 head/tail/count/stop의 기본값을 안전하게 시작한다. */
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

    /*
     * worker들은 생성 직후 worker_loop()에 들어간다.
     * 아직 queue가 비어 있으면 cond_not_empty에서 잠들어 있다가 submit 때 깨어난다.
     */
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker_loop, tp) != 0) {
            /*
             * 일부 worker 생성 후 실패하면 이미 만든 worker들을 깨워 종료시킨다.
             * 여기서 stop을 켜지 않으면 worker들이 빈 queue에서 계속 잠들 수 있다.
             */
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

    /* queue 상태 변경은 submitter와 worker가 동시에 접근하므로 반드시 mutex 안에서 한다. */
    pthread_mutex_lock(&tp->mutex);

    if (tp->stop || tp->queue_count >= THREADPOOL_QUEUE_MAX) {
        /*
         * stop 중이면 새 일을 받지 않는다.
         * queue가 꽉 차면 서버 계층은 이 실패를 API_CODE_QUEUE_FULL 응답으로 바꾼다.
         */
        pthread_mutex_unlock(&tp->mutex);
        return 0;
    }

    /*
     * job 안에는 server.c 가 만든 요청 실행 정보가 들어 있다.
     *   job.sql        <- JSON body 에서 파싱된 sql
     *   job.options    <- include_profile 같은 실행 옵션
     *   job.request_id <- 요청 추적용 id
     *   job.conn_fd    <- 응답을 다시 써 줄 TCP 연결
     */

    /* tail 위치에 복사한 뒤 tail을 한 칸 전진한다. 끝에 닿으면 0으로 돌아가는 링버퍼다. */
    tp->queue[tp->queue_tail] = *job;
    tp->queue_tail  = (tp->queue_tail + 1) % THREADPOOL_QUEUE_MAX;
    tp->queue_count++;

    /* 잠들어 있는 worker 하나에게 "일이 들어왔다"고 알려준다. */
    pthread_cond_signal(&tp->cond_not_empty);
    pthread_mutex_unlock(&tp->mutex);
    return 1;
}

void threadpool_destroy(Threadpool *tp) {
    if (!tp) return;

    /*
     * 종료 시작:
     *   stop을 켜고 모든 worker를 깨운다.
     *   queue에 남은 job이 있으면 worker가 처리하고, queue까지 비면 worker가 종료한다.
     */
    pthread_mutex_lock(&tp->mutex);
    tp->stop = 1;
    pthread_cond_broadcast(&tp->cond_not_empty);
    pthread_mutex_unlock(&tp->mutex);

    /* 모든 worker가 worker_loop를 빠져나올 때까지 기다린다. */
    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->threads[i], NULL);

    /*
     * 정상 종료라면 queue_count는 보통 0이다.
     * 그래도 남아 있는 job이 있으면 열린 fd를 닫아서 리소스 누수를 막는다.
     */
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
        /*
         * 1. queue에서 job 하나를 꺼내는 구간.
         *    queue 구조는 모든 worker가 공유하므로 mutex를 잡고 확인한다.
         */
        pthread_mutex_lock(&tp->mutex);

        /*
         * queue가 비어 있고 종료 요청도 없으면 worker는 잠든다.
         * pthread_cond_wait는 잠드는 동안 mutex를 풀고, 깨어나면 다시 mutex를 잡는다.
         */
        while (tp->queue_count == 0 && !tp->stop)
            pthread_cond_wait(&tp->cond_not_empty, &tp->mutex);

        /*
         * destroy가 호출되었고 더 처리할 job이 없으면 worker를 종료한다.
         * stop이어도 queue에 남은 job이 있으면 아래에서 계속 처리한다.
         */
        if (tp->stop && tp->queue_count == 0) {
            pthread_mutex_unlock(&tp->mutex);
            break;
        }

        /*
         * head 위치 job을 지역 변수로 복사한 뒤 queue에서 제거한다.
         * 여기서 worker는 "클라이언트 요청 하나"를 자기 손으로 가져왔다고 보면 된다.
         */
        ThreadpoolJob job = tp->queue[tp->queue_head];
        tp->queue_head  = (tp->queue_head + 1) % THREADPOOL_QUEUE_MAX;
        tp->queue_count--;

        pthread_cond_signal(&tp->cond_not_full);
        pthread_mutex_unlock(&tp->mutex);

        /*
         * 2. 실제 job 처리 구간.
         *    여기서는 queue mutex를 잡지 않는다.
         *    그래야 다른 worker가 동시에 queue에서 다음 job을 꺼낼 수 있다.
         */
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

        /*
         * 3. DB 엔진 호출 구간 보호.
         *
         * SELECT:
         *   읽기 작업으로 보고 rdlock을 건다. 여러 SELECT는 동시에 들어갈 수 있다.
         *
         * INSERT 또는 그 외 SQL:
         *   데이터 파일/인덱스를 바꿀 수 있으므로 wrlock을 건다.
         *   wrlock은 다른 reader/writer가 모두 빠질 때까지 단독으로 실행된다.
         *
         * 주의:
         *   SQL 종류 판별은 현재 MVP 기준으로 문자열 앞 6자 비교만 한다.
         */
        int is_select = (strncasecmp(job.sql, "SELECT", 6) == 0);
        if (is_select)
            pthread_rwlock_rdlock(&g_engine_rwlock);
        else
            pthread_rwlock_wrlock(&g_engine_rwlock);

        /*
         * job.sql 은 원래 클라이언트가 JSON body 로 보낸 sql 문자열이다.
         * worker 는 JSON 을 다시 읽지 않고, server.c 가 정리해 준 job 값을 실행만 한다.
         *
         * 흐름:
         *   client JSON -> server.c -> http_parse_query_request()
         *   -> ThreadpoolJob -> worker -> db_service_execute_sql()
         */
        db_service_execute_sql(job.sql, &opts, &result);

        /* 엔진 실행이 끝났으므로 read/write lock을 풀어 다음 요청이 들어오게 한다. */
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
        /* 이 connection은 job 하나를 처리하고 닫는 short-lived connection이다. */
        close(job.conn_fd);
    }

    return NULL;
}

/* ── HTTP 응답 전송 (transport 계층: Role D 소유) ─────────── */

static void send_http_response(int fd, int http_status,
                               const char *body, size_t body_len) {
    /* ApiResponseMeta의 http_status를 HTTP status line에 들어갈 짧은 문구로 바꾼다. */
    const char *status_str =
        (http_status == 200) ? "OK" :
        (http_status == 400) ? "Bad Request" :
        (http_status == 404) ? "Not Found" :
        (http_status == 405) ? "Method Not Allowed" :
        (http_status == 503) ? "Service Unavailable" :
                               "Internal Server Error";

    /* body 길이를 Content-Length로 명시해야 클라이언트가 응답 끝을 정확히 알 수 있다. */
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        http_status, status_str, body_len);

    /* write는 한 번에 전부 쓰지 못할 수 있으므로 남은 바이트가 없을 때까지 반복한다. */
    if (hlen > 0) {
        ssize_t sent = 0;
        while (sent < (ssize_t)hlen) {
            ssize_t n = write(fd, header + sent, (size_t)(hlen - sent));
            if (n <= 0) return;
            sent += n;
        }
    }

    /* header와 같은 이유로 body도 partial write를 고려해 반복 전송한다. */
    ssize_t sent = 0;
    while (sent < (ssize_t)body_len) {
        ssize_t n = write(fd, body + sent, (size_t)(body_len - (size_t)sent));
        if (n <= 0) return;
        sent += n;
    }
}
