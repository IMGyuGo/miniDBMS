# AGENT.md — miniDBMS 작업 진입점

이 파일은 이 저장소에서 작업하기 전에 가장 먼저 읽는 기준 문서다.

현재 작업 기준안은 [docs/4인_역할분담_및_진행계획_3차2.md](docs/4인_역할분담_및_진행계획_3차2.md) 이고,
실제 역할별 작업 범위와 침범 방지 규칙은 `AGENT.md` 와 `docs/roles/*.md` 를 기준으로 운영한다.

---

## 1. 프로젝트 현재 방향

현재 `miniDBMS` 는 CLI 기반 SQL 엔진이 이미 있고, 다음 단계로 아래 영역을 확장하는 중이다.

- 기존 엔진 유지: `input -> parser -> schema -> executor -> index -> bptree`
- 신규 서버 계층 추가: `http -> service -> server runtime/threadpool`
- 목표: 기존 엔진을 버리지 않고 API 서버와 동시성 계층을 얹는다.

---

## 2. 역할 및 1차 소유 범위

| 역할 | 담당 범위 | 1차 소유 파일/디렉터리 |
|------|-----------|------------------------|
| Role A | B+Tree 저장 코어 | `include/bptree.h`, `src/bptree/**`, `tests/test_bptree.c` |
| Role B | 인덱스/파일 일관성 | `include/index_manager.h`, `src/index/**`, `tests/test_index.c` |
| Role C | SQL 입력/검증 + API 계약/HTTP 메시지 | `src/input/**`, `src/parser/**`, `src/schema/**`, `src/http/**`, `include/api_contract.h`, `tests/test_parser.c`, `tests/test_schema.c`, `tests/test_http.c` |
| Role D | 실행/CLI + 서버 런타임 | `src/executor/**`, `src/main.c`, `src/server/**`, `src/threadpool/**`, `src/service/**`, `tests/test_executor.c`, `tests/test_threadpool.c`, `tests/test_api.c`, `tests/test_concurrency.c` |

규칙:

1. 각 역할은 자기 소유 파일의 1차 책임자다.
2. 다른 역할 소유 파일은 임의로 수정하지 않는다.
3. 새 파일은 가능한 한 해당 역할의 소유 디렉터리 안에 만든다.
4. 테스트도 구현과 같이 올라가야 하며, 단위 테스트는 담당 역할 소유로 본다.

---

## 3. 공통 파일과 합의 규칙

아래 파일은 저장소 전체 계약에 영향을 주므로 공통 합의가 필요하다.

- `AGENT.md`
- `include/interface.h`
- `Makefile`
- `scripts/check_ownership.sh`
- `docs/roles/**`
- `include/db_service.h` (신규 공용 서비스 계약)
- `include/server_api.h` (신규 서버 공용 설정 계약)

추가 규칙:

1. 공통 파일 변경은 단독 판단으로 밀어넣지 않는다.
2. 함수 시그니처, 구조체, 소유권 정책이 바뀌면 먼저 합의하고 수정한다.
3. 가능하면 한 커밋에는 한 역할 소유 파일과 필요한 공통 파일만 함께 담는다.

---

## 4. 아키텍처 및 모듈 의존 관계

현재 엔진 흐름:

```text
SQL 파일
  -> src/input/*
  -> src/parser/*
  -> src/schema/*
  -> src/executor/*
  -> src/index/*
  -> src/bptree/*
  -> data/{table}.dat
```

앞으로의 서버 흐름:

```text
HTTP request
  -> src/http/*          (Role C)
  -> src/service/*       (Role D)
  -> src/executor/*      (Role D)
  -> src/index/*         (Role B)
  -> src/bptree/*        (Role A)
```

중요한 경계:

- Role C는 메시지/계약을 맡고, 런타임 루프를 맡지 않는다.
- Role D는 서버 런타임을 맡고, 메시지 규약을 독단으로 바꾸지 않는다.
- Role B는 저장 계층 보호 구간을 정의하고, Role D와 락 정책을 맞춘다.
- Role A의 B+Tree 공개 API는 B가 직접 소비하므로, 헤더 변경은 A 단독 판단으로 끝내지 않는다.

---

## 5. 인터페이스 규약

### 5-1. 기존 핵심 인터페이스

#### `include/bptree.h`

- Role A 1차 소유
- B+Tree 공개 API 계약
- Role B, Role D는 이 API를 통해서만 트리 기능을 사용한다.
- `src/index/**` 또는 `src/executor/**` 에서 트리 내부 구조를 직접 가정하면 안 된다.

#### `include/index_manager.h`

- Role B 1차 소유
- executor/service 계층이 인덱스를 사용할 때 거치는 유일한 공개 진입점이다.
- Role D는 `src/index/**` 내부 구현을 우회하지 말고 `index_*` API만 사용한다.

#### `include/interface.h`

- 전원 합의 필요 공통 계약
- SQL 토큰, AST, 스키마, `ResultSet` 구조를 정의한다.
- Role C와 Role D 사이의 핵심 데이터 계약이므로 단독 수정 금지다.

### 5-2. 신규 서버 계층 인터페이스

#### `include/api_contract.h`

- Role C 1차 소유
- HTTP 요청/응답 DTO, 오류 코드, 메시지 포맷 계약을 정의한다.
- Role D가 서버 런타임에 사용하므로 수정 시 Role D 확인이 필요하다.

#### `include/db_service.h`

- 공통 합의 파일
- 서버가 엔진을 호출하는 공용 서비스 경계다.
- Role D가 주 구현을 맡되, Role C는 이 계약을 기준으로 HTTP 메시지 계층을 연결한다.

#### `include/server_api.h`

- 공통 합의 파일
- 서버 설정, 포트, 런타임 옵션 같은 범용 서버 계약이 필요할 때만 추가한다.
- 아직 없으면 섣불리 만들지 말고, 필요성이 분명할 때 합의 후 추가한다.

---

## 6. 역할 침범 방지 규칙

1. 빠른 해결을 위해 다른 역할 파일을 직접 수정하는 방식은 금지한다.
2. 필요한 동작이 없으면 먼저 자기 역할의 공개 계약을 검토하고, 그래도 부족하면 공통 헤더 변경을 제안한다.
3. Role C는 `src/http/**` 만 맡고, `src/server/**`, `src/threadpool/**`, `src/service/**` 를 건드리지 않는다.
4. Role D는 `src/http/**` 메시지 파싱/포맷 계층을 직접 구현하지 않는다.
5. Role B는 락 정책을 문서화하고 D와 맞추지만, 서버 런타임 루프 구현은 맡지 않는다.
6. Role A는 트리 내부 정확성과 API를 책임지며, index/executor 쪽 우회 수정으로 문제를 덮지 않는다.

금지 예시:

- Role D가 응답 포맷을 바꾸려고 `src/parser/**` 나 `src/http/**` 를 직접 수정하는 것
- Role C가 서버 런타임 편의를 위해 `src/server/**` 또는 `src/threadpool/**` 를 직접 수정하는 것
- Role B가 인덱스 문제를 빠르게 해결하려고 `src/bptree/**` 내부를 직접 변경하는 것

---

## 7. 테스트 소유 규칙

단위 테스트는 아래처럼 역할 소유를 따른다.

- `tests/test_bptree.c` -> Role A
- `tests/test_index.c` -> Role B
- `tests/test_parser.c`, `tests/test_schema.c`, `tests/test_http.c` -> Role C
- `tests/test_executor.c`, `tests/test_threadpool.c`, `tests/test_api.c`, `tests/test_concurrency.c` -> Role D

원칙:

1. 구현 파일을 올릴 때 같은 역할의 단위 테스트도 같이 올린다.
2. 통합 테스트는 Role D가 리드하지만, 계약 검증은 Role C와 함께 확인한다.
3. 성능 비교 자료는 Role A + Role B가 공동 정리한다.

---

## 8. 필수 작업 규칙

1. `data/{table}.dat` 접근은 기존처럼 binary 모드를 유지한다.
2. 결과 row는 `stdout`, 타이밍/진단은 `stderr` 분리를 유지한다.
3. `schema/users.schema` 는 팀 계약으로 보고 임의 변경하지 않는다.
4. 새 서버 파일을 추가할 때는 메시지 계층과 런타임 계층을 섞지 않는다.

---

## 9. 문서 참조

- 기준 분담안: [docs/4인_역할분담_및_진행계획_3차2.md](docs/4인_역할분담_및_진행계획_3차2.md)
- AI 작업 규약: [docs/ai_convention.md](docs/ai_convention.md)
- 역할 빠른 참고: [docs/roles/README.md](docs/roles/README.md)
- Role A: [docs/roles/ROLE_A.md](docs/roles/ROLE_A.md)
- Role B: [docs/roles/ROLE_B.md](docs/roles/ROLE_B.md)
- Role C: [docs/roles/ROLE_C.md](docs/roles/ROLE_C.md)
- Role D: [docs/roles/ROLE_D.md](docs/roles/ROLE_D.md)
