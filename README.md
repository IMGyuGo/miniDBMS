# miniDBMS

> C로 구현한 파일 기반 SQL 실행기 + 스레드 풀 HTTP API 서버

---

## 프로젝트 소개

SQL 처리기와 B+Tree 인덱스를 처음부터 직접 구현하고, 그 위에 스레드 풀 기반 HTTP 서버를 얹어 외부 클라이언트에서 SQL을 요청할 수 있는 미니 DBMS입니다.

- 지원 SQL: `INSERT`, `SELECT` (단건 / 범위 / 전체 스캔)
- 인덱스: `id`, `age` 기준 B+Tree
- 동시성: 스레드 풀 + rwlock으로 SELECT 병렬 처리
- 구현 언어: C

---

## 아키텍처

![아키텍처 다이어그램](docs/arch_diagram.svg)

서버와 엔진은 `db_service_execute_sql()` 하나로만 연결됩니다. 스레드는 이 함수를 통해서만 엔진에 접근하며, Executor는 B+Tree를 직접 건드리지 않고 Index Manager를 통해서만 접근합니다.

---

## 스레드 풀

### 개념

스레드 풀은 미리 만들어둔 고정 개수의 스레드가 잡 큐에서 작업을 꺼내 처리하는 구조입니다. 요청마다 스레드를 새로 생성하고 파괴하는 비용을 없애고, 동시에 처리할 수 있는 요청 수를 워커 수로 제한해 서버를 안정적으로 유지합니다.

| 개념 | 설명 |
|------|------|
| 워커 스레드 | 잡 큐에서 작업을 꺼내 실행하는 스레드. 서버 시작 시 고정 개수(4개) 생성 |
| 잡 큐 | 링버퍼(ring buffer) 구조. 요청이 들어오면 큐에 적재, 워커가 순서대로 소비 |
| mutex | 큐 접근 보호. 여러 스레드가 동시에 큐를 건드리지 못하도록 잠금 |
| cond_wait | 잡이 없을 때 워커를 재움. 잡이 들어오면 signal로 깨움 |
| rwlock | 엔진 접근 보호. SELECT는 동시 읽기 허용, INSERT는 단독 쓰기 |

### 요청 처리 흐름

![스레드 풀 요청 처리 흐름](docs/threadpool_flow.svg)

### 워커 스레드 대기 구조

워커는 잡이 없을 때 `pthread_cond_wait`으로 실제로 잠듭니다. 루프를 돌며 CPU를 태우는 스핀락이 아닙니다.

```c
while (tp->queue_count == 0 && !tp->stop)
    pthread_cond_wait(&tp->cond_not_empty, &tp->mutex);
```

### 엔진 접근 — rwlock

| 요청 | Lock | 동작 |
|------|------|------|
| SELECT | rdlock | 여러 스레드 동시 처리 가능 |
| INSERT | wrlock | 단독 쓰기, 나머지 대기 |

---

## 빌드 및 실행

```bash
make                        # 빌드
./minidbms_server           # 서버 실행 (포트 8080)
make test                   # 단위 테스트 전체 실행
./test_threadpool 64        # 스레드 풀 병렬성 테스트
```

## 동시성 read, write 병렬 작업 테스트
![alt text](병렬작업테스트.png)

- INSERT 작업의 경우 worker 쓰레드가 하나의 작업을 끝내고 다음 작업으로 순차적으로 진행
- SELECT 작업의 경우 worker 쓰레드가 병렬적으로 작업을 시작하는 것을 확인 가능

---

## 테스트 케이스

| 구분 | 테스트 | 검증 내용 |
|------|--------|-----------|
| Thread Pool | 병렬 처리 | N개 잡을 worker 수별로 실행 → elapsed 감소, speedup 수치 확인 |
| Thread Pool | 안전 종료 | `threadpool_destroy()` 후 모든 워커 정상 종료 |
| Executor | INSERT | row 파일 기록, 인덱스 등록 |
| Executor | SELECT (index) | `index:id:eq`, `index:id:range`, `index:age:range` 결과 정확성 |
| Executor | SELECT (linear) | 인덱스 조건 외 쿼리 → 전체 스캔, 결과 일치 |
| Executor | 경로 일치 | 동일 쿼리를 인덱스/linear 두 경로로 실행 → 결과 동일 |
| B+Tree | 삽입 / 조회 | 삽입 후 단건·범위 조회 값 일치 |
| B+Tree | 노드 분할 | leaf 꽉 참 → 분할 후 height 증가, 기존 키 정상 조회 |

---

## 엣지 케이스

| 구분 | 상황 | 처리 방식 |
|------|------|-----------|
| Thread Pool | 잡 큐 포화 | `threadpool_submit()` 0 반환, 잡 버림 |
| Thread Pool | 종료 시 잠든 워커 | `cond_broadcast()`로 전체 깨운 후 join |
| B+Tree | 중복 키 (age) | 같은 key, 다른 offset → offset 오름차순 정렬 저장 |
| B+Tree | 노드 분할 후 탐색 | 분할 전 키 모두 정상 조회 |
| Executor | 빈 SQL / 잘못된 SQL | `BAD_REQUEST` 반환, 서버 크래시 없음 |
| HTTP | 미등록 라우트 | `{ "ok": false, "code": "UNSUPPORTED_ROUTE" }` 반환 |

---

## 설계 쟁점

**mutex 대신 rwlock** — mutex는 SELECT끼리도 직렬로 처리된다. rwlock을 쓰면 SELECT는 동시에, INSERT만 단독으로 실행할 수 있다.

**스레드 고정 4개** — 요청마다 스레드를 생성하면 OS 자원 비용이 생기고 요청이 몰릴 때 스레드가 폭발한다. 미리 4개를 만들어두고 재사용한다.

**`WHERE age = 25` → linear** — age 인덱스는 BETWEEN 범위 탐색용으로 만들었다. equality는 같은 age를 가진 row가 많을 때 offset을 전부 fetch해야 해서 전체 스캔보다 오히려 느릴 수 있다.

**재시작 시 인덱스 복구** — B+Tree는 메모리 구조라 서버가 꺼지면 사라진다. 재시작 시 `.dat` 파일을 처음부터 읽어 인덱스를 다시 올린다.

**동시성 개선 여지** — 현재는 전역 rwlock 하나로 DB 엔진 전체를 막는다. INSERT 하나가 들어오면 모든 SELECT가 대기한다. 실제 DB는 이를 단계적으로 개선한다.

| 단계 | 방식 | 효과 |
|------|------|------|
| Table lock | 테이블마다 별도 rwlock | users INSERT 중에도 orders SELECT 가능 |
| Lock 범위 축소 | 파싱/스키마 검증은 lock 없이, 파일 접근 구간만 lock | lock 보유 시간 단축 |
| Row/Page lock | B+Tree leaf 단위 lock | 다른 row 동시 접근 가능 |
| MVCC | writer는 새 버전 생성, reader는 스냅샷 읽기 | reader가 writer를 기다리지 않음 |
