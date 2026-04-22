/* =========================================================
 * server.c — TCP accept loop + 요청 디스패치
 *
 * 담당: Role D
 *
 * 책임 (Role D 소유):
 *   - 소켓 생성/바인드/리슨/accept (TCP 프레이밍)
 *   - HTTP 헤더 읽기, Content-Length 파싱, body 분리
 *   - threadpool 에 잡 제출
 *   - SIGINT/SIGTERM 종료 처리
 *
 * Role C 위임:
 *   - http_parse_query_request()  : method/path/body → ApiQueryRequest
 *   - http_serialize_*_response() : ApiResponseMeta/DBServiceResult → JSON
 *   JSON 직렬화/파싱 로직은 이 파일에 두지 않는다.
 * ========================================================= */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "../threadpool/threadpool.h"
#include "../http/http_message.h"
#include "../../include/api_contract.h"
#include "../../include/index_manager.h"

/* ── 전역 상태 ──────────────────────────────────────────── */
static volatile int g_running   = 1;
static int          g_server_fd = -1;

/* ── 내부 헬퍼 선언 ──────────────────────────────────────── */
static int  read_http_request(int fd, char *buf, size_t buf_size,
                              int *out_body_offset);
static int  parse_request_line(const char *buf,
                               char *method, size_t mlen,
                               char *path,   size_t plen);
static int  extract_content_length(const char *headers, int *out_len);
static void send_response(int fd, int http_status, const char *body);
static void sigint_handler(int sig);

/* ── 공개 API ─────────────────────────────────────────────── */

int server_run(int port, int num_threads) {
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    Threadpool *tp = threadpool_create(num_threads);
    if (!tp) {
        fprintf(stderr, "[server] threadpool_create failed\n");
        return -1;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("[server] socket");
        threadpool_destroy(tp);
        return -1;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[server] bind");
        close(g_server_fd);
        threadpool_destroy(tp);
        return -1;
    }

    if (listen(g_server_fd, 128) < 0) {
        perror("[server] listen");
        close(g_server_fd);
        threadpool_destroy(tp);
        return -1;
    }

    fprintf(stderr,
            "[server] miniDBMS HTTP server listening on port %d "
            "with %d worker thread(s)\n", port, num_threads);
    fprintf(stderr, "[server] POST /query  — SQL 실행\n");
    fprintf(stderr, "[server] GET  /health — 서버 상태\n\n");

    /* ── accept 루프 ── */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(g_server_fd,
                             (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;
            perror("[server] accept");
            continue;
        }

        /* ── HTTP 요청 읽기 (TCP 프레이밍: Role D 소유) ── */
        char raw[65536];
        int  body_offset = -1;
        int  raw_len = read_http_request(conn_fd, raw, sizeof(raw), &body_offset);

        if (raw_len <= 0) {
            close(conn_fd);
            continue;
        }

        /* method/path 추출 (HTTP 요청 라인 파싱: Role D 소유) */
        char method[API_METHOD_MAX_LEN + 1];
        char path[API_PATH_MAX_LEN + 1];
        if (parse_request_line(raw, method, sizeof(method),
                               path, sizeof(path)) < 0) {
            close(conn_fd);
            continue;
        }

        const char *body = (body_offset >= 0) ? (raw + body_offset) : "";

        /* ── Role C: method/path/body → ApiQueryRequest ── */
        ApiQueryRequest  req;
        ApiResponseMeta  parse_err;
        int ok = http_parse_query_request(method, path, body,
                                          &req, &parse_err);

        if (!ok) {
            /* 파싱 실패: Role C 가 채운 error_meta 로 즉시 응답 */
            char buf[512];
            buf[0] = '\0';
            http_serialize_query_response(&parse_err, NULL, buf, sizeof(buf));
            send_response(conn_fd, parse_err.http_status, buf);
            close(conn_fd);
            continue;
        }

        /* ── /health → Role C 직렬화 후 즉시 응답 ── */
        if (req.route == API_ROUTE_KIND_HEALTH) {
            char buf[256];
            http_serialize_health_response(buf, sizeof(buf));
            send_response(conn_fd, 200, buf);
            close(conn_fd);
            continue;
        }

        /* ── POST /query → threadpool 에 제출 ── */
        ThreadpoolJob job;
        memset(&job, 0, sizeof(job));
        job.conn_fd = conn_fd;
        strncpy(job.sql, req.sql, sizeof(job.sql) - 1);
        job.options = req.options;   /* ApiQueryOptions 그대로 복사 */
        strncpy(job.request_id, req.request_id, sizeof(job.request_id) - 1);

        if (!threadpool_submit(tp, &job)) {
            /* 큐 가득 참 */
            ApiResponseMeta full_meta;
            memset(&full_meta, 0, sizeof(full_meta));
            full_meta.http_status = 503;
            full_meta.code        = API_CODE_QUEUE_FULL;
            full_meta.ok          = 0;
            strncpy(full_meta.error, "server queue full",
                    sizeof(full_meta.error) - 1);

            char buf[256];
            http_serialize_query_response(&full_meta, NULL, buf, sizeof(buf));
            send_response(conn_fd, 503, buf);
            close(conn_fd);
        }
        /* 성공 시 conn_fd 는 worker 가 처리하고 close */
    }

    fprintf(stderr, "\n[server] shutting down...\n");
    close(g_server_fd);
    g_server_fd = -1;

    index_cleanup();
    threadpool_destroy(tp);

    fprintf(stderr, "[server] stopped.\n");
    return 0;
}

/* ── TCP 프레이밍 헬퍼 (Role D 소유) ─────────────────────── */

/*
 * read_http_request: fd 에서 HTTP 요청 헤더+body 를 읽어 buf 에 채운다.
 * 헤더 끝(\r\n\r\n) 이후 Content-Length 만큼 body 를 추가로 읽는다.
 * out_body_offset: body 시작 위치 인덱스, 없으면 -1
 */
static int read_http_request(int fd, char *buf, size_t buf_size,
                             int *out_body_offset) {
    *out_body_offset = -1;
    size_t total      = 0;
    int    header_end = -1;

    while (total < buf_size - 1) {
        ssize_t n = read(fd, buf + total, 1);
        if (n <= 0) break;
        total++;
        buf[total] = '\0';

        if (total >= 4 &&
            buf[total-4] == '\r' && buf[total-3] == '\n' &&
            buf[total-2] == '\r' && buf[total-1] == '\n') {
            header_end = (int)total;
            break;
        }
    }

    if (header_end < 0) return (int)total;

    int content_length = 0;
    extract_content_length(buf, &content_length);
    *out_body_offset = header_end;

    int remaining = content_length;
    while (remaining > 0 && total < buf_size - 1) {
        size_t want = (size_t)remaining < (buf_size - total - 1)
                      ? (size_t)remaining : (buf_size - total - 1);
        ssize_t n = read(fd, buf + total, want);
        if (n <= 0) break;
        total     += (size_t)n;
        remaining -= (int)n;
        buf[total]  = '\0';
    }

    return (int)total;
}

/*
 * parse_request_line: "METHOD /path HTTP/1.1\r\n..." 에서 method, path 추출.
 * 반환: 성공 0, 실패 -1
 */
static int parse_request_line(const char *buf,
                              char *method, size_t mlen,
                              char *path,   size_t plen) {
    const char *p  = buf;
    const char *sp = strchr(p, ' ');
    if (!sp) return -1;

    size_t ml = (size_t)(sp - p);
    if (ml >= mlen) ml = mlen - 1;
    memcpy(method, p, ml);
    method[ml] = '\0';

    p  = sp + 1;
    sp = strchr(p, ' ');
    if (!sp) { sp = strchr(p, '\r'); }
    if (!sp) { sp = strchr(p, '\n'); }
    if (!sp) return -1;

    size_t pl = (size_t)(sp - p);
    if (pl >= plen) pl = plen - 1;
    memcpy(path, p, pl);
    path[pl] = '\0';

    return 0;
}

/* Content-Length 헤더 값 추출 */
static int extract_content_length(const char *headers, int *out_len) {
    *out_len = 0;
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            p += 15;
            while (*p == ' ') p++;
            *out_len = atoi(p);
            return 1;
        }
        p++;
    }
    return 0;
}

/* HTTP 응답 전송 (소켓 write: Role D 소유) */
static void send_response(int fd, int http_status, const char *body) {
    if (!body) body = "";
    size_t body_len = strlen(body);

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

    if (hlen > 0) write(fd, header, (size_t)hlen);
    if (body_len > 0) write(fd, body, body_len);
}

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}
