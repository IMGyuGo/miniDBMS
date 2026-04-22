/* =========================================================
 * server.c — TCP accept loop + 요청 디스패치
 *
 * 담당: Role D
 *
 * 책임:
 *   - 소켓 생성/바인드/리슨
 *   - accept 루프 (연결 수락)
 *   - HTTP 요청 최소 파싱 (method, path, body)
 *     TODO(RoleD): Role C 의 src/http 완성 후 http_parse_request() 로 교체
 *   - threadpool 에 잡 제출
 *
 * 이 파일이 하지 않는 것:
 *   - SQL 파싱/실행 (service 계층 담당)
 *   - JSON 직렬화 (threadpool.c 의 worker 담당)
 *   - 메시지 계약 정의 (api_contract.h 담당)
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
#include "../../include/api_contract.h"
#include "../../include/index_manager.h"

/* ── 전역 상태 (서버 수명 동안 유지) ─────────────────────── */
static volatile int g_running = 1;
static int          g_server_fd = -1;

/* ── 내부 헬퍼 선언 ──────────────────────────────────────── */
static int  read_http_request(int fd, char *buf, size_t buf_size, int *out_body_offset);
static int  parse_request_line(const char *buf, char *method, size_t mlen,
                               char *path, size_t plen);
static int  extract_content_length(const char *headers, int *out_len);
static int  extract_json_string(const char *json, const char *key,
                                char *out, size_t out_size);
static int  extract_json_int(const char *json, const char *key, int *out);
static void handle_health(int conn_fd);
static void handle_queue_full(int conn_fd);
static void handle_not_found(int conn_fd);
static void handle_method_not_allowed(int conn_fd);
static void send_plain_response(int fd, int status,
                                const char *status_str, const char *body);
static void sigint_handler(int sig);

/* ── 공개 API ─────────────────────────────────────────────── */

int server_run(int port, int num_threads) {
    /* SIGINT / SIGTERM 핸들러 등록 */
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);
    /* 클라이언트 연결 끊김 시 SIGPIPE 무시 */
    signal(SIGPIPE, SIG_IGN);

    /* ── 스레드 풀 생성 ── */
    Threadpool *tp = threadpool_create(num_threads);
    if (!tp) {
        fprintf(stderr, "[server] threadpool_create failed\n");
        return -1;
    }

    /* ── 소켓 생성 및 바인드 ── */
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
    fprintf(stderr, "[server] GET  /health — 서버 상태\n");
    fprintf(stderr, "[server] SIGINT(Ctrl+C) 으로 종료\n\n");

    /* ── accept 루프 ── */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(g_server_fd,
                             (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            if (!g_running) break;   /* SIGINT 로 인한 accept 중단 */
            if (errno == EINTR) continue;
            perror("[server] accept");
            continue;
        }

        /* ── HTTP 요청 읽기 ── */
        char raw[65536];
        int body_offset = 0;
        int raw_len = read_http_request(conn_fd, raw, sizeof(raw), &body_offset);

        if (raw_len <= 0) {
            close(conn_fd);
            continue;
        }

        /* ── 요청 라인 파싱 ── */
        char method[API_METHOD_MAX_LEN + 1];
        char path[API_PATH_MAX_LEN + 1];
        if (parse_request_line(raw, method, sizeof(method),
                               path, sizeof(path)) < 0) {
            close(conn_fd);
            continue;
        }

        /* ── 라우팅 ── */
        if (strcmp(path, API_ROUTE_HEALTH) == 0) {
            handle_health(conn_fd);
            close(conn_fd);
            continue;
        }

        if (strcmp(path, API_ROUTE_QUERY) != 0) {
            handle_not_found(conn_fd);
            close(conn_fd);
            continue;
        }

        if (strcmp(method, "POST") != 0) {
            handle_method_not_allowed(conn_fd);
            close(conn_fd);
            continue;
        }

        /* ── POST /query 처리: JSON body 에서 sql 추출 ── */
        /*
         * TODO(RoleD): 아래 최소 JSON 파싱은 Role C 의 src/http 가 완성되면
         *   http_parse_request() 호출로 교체한다.
         */
        const char *body = (body_offset >= 0) ? (raw + body_offset) : NULL;
        if (!body || *body == '\0') {
            send_plain_response(conn_fd, 400, "Bad Request",
                                "{\"ok\":false,\"code\":1,\"error\":\"empty body\"}");
            close(conn_fd);
            continue;
        }

        ThreadpoolJob job;
        memset(&job, 0, sizeof(job));
        job.conn_fd = conn_fd;

        /* SQL 추출 */
        if (extract_json_string(body, "sql", job.sql, sizeof(job.sql)) < 0) {
            send_plain_response(conn_fd, 400, "Bad Request",
                                "{\"ok\":false,\"code\":1,\"error\":\"missing 'sql' field\"}");
            close(conn_fd);
            continue;
        }

        /* 옵션 추출 (없으면 기본값 0) */
        extract_json_int(body, "force_linear",    &job.force_linear);
        extract_json_int(body, "include_profile", &job.include_profile);

        /* request_id 추출 (없으면 빈 문자열) */
        extract_json_string(body, "request_id",
                            job.request_id, sizeof(job.request_id));

        /* ── 스레드 풀에 제출 ── */
        if (!threadpool_submit(tp, &job)) {
            /* 큐 가득 참 → conn_fd는 여기서 처리하고 닫는다 */
            handle_queue_full(conn_fd);
            close(conn_fd);
        }
        /* 성공 시 conn_fd 는 worker 가 처리하고 close 한다 */
    }

    /* ── 정리 ── */
    fprintf(stderr, "\n[server] shutting down...\n");
    close(g_server_fd);
    g_server_fd = -1;

    /* threadpool 종료 전 엔진 인덱스 정리 */
    index_cleanup();
    threadpool_destroy(tp);

    fprintf(stderr, "[server] stopped.\n");
    return 0;
}

/* ── 내부 구현 ────────────────────────────────────────────── */

/*
 * read_http_request: fd 에서 HTTP 요청을 읽어 buf 에 채운다.
 * 헤더 끝(\r\n\r\n) + Content-Length 바이트만큼 body 를 읽는다.
 * out_body_offset: body 시작 위치 (없으면 -1)
 * 반환: 읽은 총 바이트 수, 실패 -1
 */
static int read_http_request(int fd, char *buf, size_t buf_size, int *out_body_offset) {
    *out_body_offset = -1;

    size_t total = 0;
    int header_end = -1;

    /* 헤더 끝까지 읽기 */
    while (total < buf_size - 1) {
        ssize_t n = read(fd, buf + total, 1);
        if (n <= 0) break;
        total++;
        buf[total] = '\0';

        /* \r\n\r\n 탐지 */
        if (total >= 4 &&
            buf[total-4] == '\r' && buf[total-3] == '\n' &&
            buf[total-2] == '\r' && buf[total-1] == '\n') {
            header_end = (int)total;
            break;
        }
    }

    if (header_end < 0) return (int)total;

    /* Content-Length 파싱 */
    int content_length = 0;
    extract_content_length(buf, &content_length);

    *out_body_offset = header_end;

    /* body 읽기 */
    int remaining = content_length;
    while (remaining > 0 && total < buf_size - 1) {
        ssize_t n = read(fd, buf + total,
                         (size_t)remaining < buf_size - total - 1
                         ? (size_t)remaining : buf_size - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        remaining -= (int)n;
        buf[total] = '\0';
    }

    return (int)total;
}

/*
 * parse_request_line: "METHOD /path HTTP/1.1" 를 파싱한다.
 * 반환: 성공 0, 실패 -1
 */
static int parse_request_line(const char *buf, char *method, size_t mlen,
                              char *path, size_t plen) {
    const char *p = buf;
    /* method */
    const char *sp = strchr(p, ' ');
    if (!sp) return -1;
    size_t ml = (size_t)(sp - p);
    if (ml >= mlen) ml = mlen - 1;
    memcpy(method, p, ml);
    method[ml] = '\0';

    /* path */
    p = sp + 1;
    sp = strchr(p, ' ');
    if (!sp) sp = strchr(p, '\r');
    if (!sp) sp = strchr(p, '\n');
    if (!sp) return -1;
    size_t pl = (size_t)(sp - p);
    if (pl >= plen) pl = plen - 1;
    memcpy(path, p, pl);
    path[pl] = '\0';

    return 0;
}

/*
 * extract_content_length: 헤더 문자열에서 Content-Length 값을 추출한다.
 * 반환: 성공 시 1, 없으면 0
 */
static int extract_content_length(const char *headers, int *out_len) {
    *out_len = 0;
    /* 대소문자 무관하게 탐색 */
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

/*
 * extract_json_string: JSON 문자열에서 "key":"value" 를 단순 추출한다.
 * 완전한 JSON 파서가 아니며, MVP 용 최소 구현이다.
 * TODO(RoleD): Role C 의 http 파서 완성 후 제거 예정.
 * 반환: 성공 0, 키 없음 -1
 */
static int extract_json_string(const char *json, const char *key,
                               char *out, size_t out_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);

    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 0;
}

/*
 * extract_json_int: JSON 에서 "key": number 를 추출한다.
 * 반환: 성공 0, 없으면 -1
 */
static int extract_json_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);

    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = atoi(p);
        return 0;
    }
    return -1;
}

/* ── 정적 응답 헬퍼 ──────────────────────────────────────── */

static void handle_health(int conn_fd) {
    send_plain_response(conn_fd, 200, "OK",
        "{\"ok\":true,\"status\":\"running\","
        "\"engine\":\"miniDBMS\"}");
}

static void handle_queue_full(int conn_fd) {
    send_plain_response(conn_fd, 503, "Service Unavailable",
        "{\"ok\":false,\"code\":7,\"error\":\"server queue full\"}");
}

static void handle_not_found(int conn_fd) {
    send_plain_response(conn_fd, 404, "Not Found",
        "{\"ok\":false,\"code\":2,\"error\":\"unsupported route\"}");
}

static void handle_method_not_allowed(int conn_fd) {
    send_plain_response(conn_fd, 405, "Method Not Allowed",
        "{\"ok\":false,\"code\":3,\"error\":\"method not allowed\"}");
}

static void send_plain_response(int fd, int status,
                                const char *status_str, const char *body) {
    size_t body_len = strlen(body);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_str, body_len);

    if (hlen > 0) write(fd, header, (size_t)hlen);
    if (body_len > 0) write(fd, body, body_len);
}

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
    /* accept 를 깨우기 위해 서버 fd 닫기 */
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}
