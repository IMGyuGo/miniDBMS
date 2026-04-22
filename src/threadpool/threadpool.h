#ifndef THREADPOOL_H
#define THREADPOOL_H

/* =========================================================
 * threadpool.h — 스레드 풀 내부 계약
 *
 * 담당: Role D
 * 용도: src/server 가 이 헤더를 include 해서 스레드 풀을 사용한다.
 *       엔진 계층(executor/service)은 이 헤더를 알지 못한다.
 *
 * Role C 연동:
 *   - ThreadpoolJob 안의 options 는 ApiQueryOptions 를 그대로 쓴다.
 *   - 직렬화/파싱은 threadpool.c 에서 http_message.h 를 include 해서 처리.
 * ========================================================= */

#include "../../include/api_contract.h"

#define THREADPOOL_QUEUE_MAX 256

/*
 * ThreadpoolJob: worker 가 처리할 정보.
 * conn_fd: 응답을 써야 할 TCP 연결 fd (worker 가 처리 후 close).
 * sql/options/request_id 는 Role C 의 http_parse_query_request() 가 채운다.
 */
typedef struct {
    int             conn_fd;
    char            sql[API_SQL_MAX_LEN];
    ApiQueryOptions options;
    char            request_id[API_REQUEST_ID_MAX];
} ThreadpoolJob;

/* 불투명 핸들 */
typedef struct Threadpool Threadpool;

/*
 * threadpool_create: num_threads 개 worker 를 생성한다.
 * 반환 NULL 이면 실패.
 */
Threadpool *threadpool_create(int num_threads);

/*
 * threadpool_submit: 잡을 큐에 넣는다.
 * 큐가 가득 찬 경우 0 반환, 성공 시 1 반환.
 */
int threadpool_submit(Threadpool *tp, const ThreadpoolJob *job);

/*
 * threadpool_destroy: 모든 worker 를 안전하게 종료하고 메모리를 해제한다.
 */
void threadpool_destroy(Threadpool *tp);

#endif /* THREADPOOL_H */
