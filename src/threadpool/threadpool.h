#ifndef THREADPOOL_H
#define THREADPOOL_H

/* =========================================================
 * threadpool.h — 스레드 풀 내부 계약
 *
 * 담당: Role D
 * 용도: src/server 가 이 헤더를 include 해서 스레드 풀을 사용한다.
 *       엔진 계층(executor/service)은 이 헤더를 알지 못한다.
 *
 * 설계:
 *   - 고정 N개 worker thread
 *   - 링버퍼(ring buffer) 기반 job 큐
 *   - pthread mutex + condvar 동기화
 * ========================================================= */

#include "../../include/api_contract.h"

#define THREADPOOL_QUEUE_MAX 256

/*
 * 스레드 풀 잡 하나.
 * worker 가 처리할 정보를 담는다.
 * conn_fd: 응답을 써야 할 TCP 연결 fd (worker가 처리 후 close)
 */
typedef struct {
    int     conn_fd;
    char    sql[API_SQL_MAX_LEN];
    char    request_id[API_REQUEST_ID_MAX];
    int     force_linear;
    int     include_profile;
    int     emit_log;
} ThreadpoolJob;

/* 불투명 핸들: threadpool 구조체 상세는 threadpool.c 안에만 있다 */
typedef struct Threadpool Threadpool;

/*
 * threadpool_create: num_threads 개 worker를 생성하고 핸들을 반환한다.
 * 반환 NULL 이면 실패.
 */
Threadpool *threadpool_create(int num_threads);

/*
 * threadpool_submit: 잡을 큐에 넣는다.
 * 큐가 가득 찬 경우 0 반환 (드롭).
 * 성공 시 1 반환.
 */
int threadpool_submit(Threadpool *tp, const ThreadpoolJob *job);

/*
 * threadpool_destroy: 모든 worker를 안전하게 종료하고 메모리를 해제한다.
 * 큐에 남은 잡은 처리하지 않고 conn_fd를 닫는다.
 */
void threadpool_destroy(Threadpool *tp);

#endif /* THREADPOOL_H */
