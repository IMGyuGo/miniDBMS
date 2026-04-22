# miniDBMS AI Convention

## 1. 목적

이 문서는 AI 에이전트가 `miniDBMS` 저장소에서 작업할 때 따라야 하는 공통 작업 규칙을 정리한 문서다.

이 문서의 목적은 아래 3가지다.

1. 역할별 파일 침범을 막는다.
2. 공통 인터페이스 계약을 함부로 바꾸지 않게 한다.
3. API 서버 확장 시 계층이 섞이지 않도록 판단 기준을 고정한다.

이 문서는 구현 문서가 아니라 작업 규약 문서다.

---

## 2. 읽는 순서

AI는 작업 전에 아래 순서로 문서를 확인한다.

1. `AGENT.md`
2. `docs/4인_역할분담_및_진행계획_3차2.md`
3. `docs/roles/README.md`
4. 자기 역할에 해당하는 `docs/roles/ROLE_*.md`
5. 관련 공통 헤더
   - `include/interface.h`
   - `include/bptree.h`
   - `include/index_manager.h`
   - `include/api_contract.h`
   - `include/db_service.h`

---

## 3. 기본 원칙

1. 이미 있는 엔진 구조를 버리지 않는다.
2. 역할별 1차 소유 범위를 존중한다.
3. 다른 역할 파일을 직접 고쳐서 빠르게 해결하지 않는다.
4. 공통 인터페이스는 필요할 때만, 합의 후 바꾼다.
5. 새 서버 기능은 계약 계층과 런타임 계층을 섞지 않는다.
6. 테스트는 구현과 같이 올린다.

---

## 4. 역할별 수정 범위

| 역할 | 수정 가능 범위 |
|------|----------------|
| Role A | `include/bptree.h`, `src/bptree/**`, `tests/test_bptree.c` |
| Role B | `include/index_manager.h`, `src/index/**`, `tests/test_index.c` |
| Role C | `src/input/**`, `src/parser/**`, `src/schema/**`, `src/http/**`, `include/api_contract.h`, `tests/test_parser.c`, `tests/test_schema.c`, `tests/test_http.c` |
| Role D | `src/executor/**`, `src/main.c`, `src/server/**`, `src/threadpool/**`, `src/service/**`, `tests/test_executor.c`, `tests/test_threadpool.c`, `tests/test_api.c`, `tests/test_concurrency.c` |

공통 합의 파일:

- `AGENT.md`
- `include/interface.h`
- `Makefile`
- `scripts/check_ownership.sh`
- `docs/roles/**`
- `include/db_service.h`
- `include/server_api.h`

---

## 5. 인터페이스 판단 기준

### `include/interface.h`

- SQL 엔진 공통 계약만 둔다.
- 토큰, AST, 스키마, `ResultSet` 같은 엔진 도메인 구조만 유지한다.
- HTTP 헤더, status code, 소켓 fd, threadpool 상태는 넣지 않는다.

### `include/api_contract.h`

- HTTP 요청/응답 메시지 계약만 둔다.
- route, method, request option, application-level error code를 둔다.
- 엔진 내부 구조(`AST`, `ResultSet`)는 직접 넣지 않는다.

### `include/db_service.h`

- 서버가 엔진을 호출하는 공용 서비스 경계를 둔다.
- transport 비의존적이어야 한다.
- parser/executor를 직접 우회 호출하지 않도록 service API로 고정한다.

### `include/bptree.h`

- Role A 공개 API다.
- Role B, Role D는 이 헤더를 통해서만 트리 기능을 사용한다.
- 트리 내부 구조를 외부 계층이 직접 가정하면 안 된다.

### `include/index_manager.h`

- Role B 공개 API다.
- Role D는 `index_*` API를 통해서만 인덱스를 사용한다.
- executor/service가 저장 계층 내부 구현을 직접 만지면 안 된다.

---

## 6. API 서버 확장 규칙

API 서버를 추가할 때 AI는 아래 계층 분리를 지킨다.

```text
HTTP request
  -> src/http/*       : message parsing / response formatting
  -> src/service/*    : engine call boundary
  -> src/server/*     : accept loop / runtime control
  -> src/threadpool/* : worker / queue
  -> src/executor/*   : SQL execution
  -> src/index/*      : storage index
  -> src/bptree/*     : tree core
```

규칙:

1. `src/http/**` 는 메시지 규약만 다룬다.
2. `src/server/**` 는 소켓 accept loop, 요청 분배, 종료 흐름을 맡는다.
3. `src/threadpool/**` 는 worker lifecycle과 queue만 맡는다.
4. `src/service/**` 는 transport와 executor 사이의 단일 공용 실행 경계다.
5. `src/http/**` 와 `src/server/**` 의 책임을 한 파일에 섞지 않는다.

---

## 7. 역할 충돌 방지 규칙

1. Role C는 `src/http/**` 만 맡고 `src/server/**`, `src/threadpool/**`, `src/service/**` 를 직접 수정하지 않는다.
2. Role D는 메시지 규약을 바꾸기 위해 `src/http/**` 또는 `include/api_contract.h` 를 직접 수정하지 않는다.
3. Role B는 인덱스 문제를 해결하려고 `src/bptree/**` 를 직접 수정하지 않는다.
4. Role A는 트리 문제를 index/executor 우회 수정으로 덮지 않는다.
5. 공통 계약이 부족하면 남의 소유 파일을 건드리지 말고 공통 헤더 변경을 제안한다.

---

## 8. 새 필드/새 구조체 추가 규칙

AI가 새 필드나 새 구조체를 추가하려고 할 때는 아래 질문을 먼저 확인한다.

1. 이 값이 SQL 엔진 의미 자체에 필요한가?
2. transport/runtime 정보가 아니라 parser/schema/executor 공통 의미인가?
3. 요청 단위 데이터인가, 런타임 공유 상태인가?
4. free 책임이 명확한가?
5. 역할 경계가 흐려지지 않는가?

판단 기준:

- 엔진 공통 의미면 `include/interface.h`
- HTTP 요청/응답 의미면 `include/api_contract.h`
- 엔진 호출 결과/프로파일 의미면 `include/db_service.h`
- 저장/트리 의미면 해당 역할 헤더
- 런타임 공유 상태면 `src/server/**` 또는 `src/threadpool/**` 내부 구조

---

## 9. 출력 및 I/O 규칙

1. 결과 row는 `stdout`
2. 타이밍/진단은 `stderr`
3. 데이터 파일은 binary 모드 유지
4. `schema/users.schema` 는 임의 변경 금지

---

## 10. 테스트 규칙

1. 단위 테스트는 역할별 소유를 따른다.
2. 구현 파일을 수정하면 관련 테스트도 같이 수정한다.
3. 통합 테스트는 Role D가 리드하되, 계약 검증은 Role C가 같이 본다.
4. 성능 비교 자료는 Role A + Role B가 같이 정리한다.

---

## 11. 커밋 규칙

1. 한 커밋에는 가능한 한 한 역할의 소유 파일만 담는다.
2. 공통 파일이 섞이면 변경 이유가 분명해야 한다.
3. `scripts/check_ownership.sh` 기준을 우회하려고 파일을 섞지 않는다.

---

## 12. 금지 예시

- HTTP 응답 형식을 바꾸려고 `src/executor/**` 에 JSON 직렬화 코드를 넣는 것
- 서버 성능 문제를 해결하려고 `src/http/**` 에 worker queue를 구현하는 것
- 인덱스 문제를 고치려고 `src/bptree/**` 내부 구조를 Role B가 직접 수정하는 것
- 공통 헤더를 빠르게 바꾸고 관련 역할에게 공유하지 않는 것

---

## 13. 최종 기준

작업 중 규칙이 충돌하면 아래 우선순위를 따른다.

1. `AGENT.md`
2. `include/interface.h`, `include/api_contract.h`, `include/db_service.h`
3. `docs/roles/*.md`
4. `docs/4인_역할분담_및_진행계획_3차2.md`
5. 이 문서
