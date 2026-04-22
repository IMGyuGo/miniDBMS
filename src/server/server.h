#ifndef SERVER_H
#define SERVER_H

/* =========================================================
 * server.h — HTTP 서버 내부 계약
 *
 * 담당: Role D
 * 노출 범위: src/server 디렉터리 내부 + main.c 에서만 include 한다.
 *
 * 주의:
 *   - HTTP 메시지 파싱/포맷 계약은 Role C의 src/http 가 담당한다.
 *     현재 src/http 가 없으므로 server.c 에서 최소 인라인 파싱을 쓴다.
 *     Role C 가 src/http 를 제공하면 교체 예정.
 *     TODO(RoleD): src/http/http_parser.h 완성 후 해당 API 로 교체
 * ========================================================= */

/* server_run: 포트를 열고 블로킹 accept 루프를 시작한다.
 * num_threads: worker 스레드 수 (1 이상)
 * 반환: 정상 종료 0, 오류 -1
 */
int server_run(int port, int num_threads);

#endif /* SERVER_H */
