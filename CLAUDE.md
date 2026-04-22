# Claude Code 진입점

이 저장소에서 작업하기 전에 **반드시 먼저** 아래 파일을 읽는다.

```text
AGENT.md
```

`AGENT.md` 는 이 저장소의 실제 기준 문서다.
이 파일은 요약 진입점이고, 충돌 시 항상 `AGENT.md` 를 따른다.

---

## 1. AGENT.md 를 먼저 읽어야 하는 이유

`AGENT.md` 에는 아래 내용이 들어 있다.

- 현재 프로젝트 방향
  - CLI SQL 엔진 유지
  - API 서버 + 동시성 계층 확장
- 역할별 1차 소유 범위
  - Role A: `bptree`
  - Role B: `index`
  - Role C: `input/parser/schema/http/api_contract`
  - Role D: `executor/main/server/threadpool/service`
- 공통 파일과 합의 규칙
- 아키텍처 및 모듈 의존 관계
- 핵심 인터페이스 계약
  - `include/interface.h`
  - `include/bptree.h`
  - `include/index_manager.h`
  - `include/api_contract.h`
  - `include/db_service.h`
- 역할 침범 방지 규칙
- 테스트 소유 규칙
- binary I/O, `stdout`/`stderr` 분리 같은 필수 작업 규칙

`AGENT.md` 를 읽기 전에는 어떤 파일도 수정하지 않는다.

---

## 2. 추가로 읽어야 하는 문서

작업 성격에 따라 아래 문서도 함께 읽는다.

### 역할 범위가 걸리면

```text
docs/roles/README.md
docs/roles/ROLE_A.md
docs/roles/ROLE_B.md
docs/roles/ROLE_C.md
docs/roles/ROLE_D.md
```

자기 작업에 해당하는 역할 문서를 읽고, 다른 역할 파일은 직접 수정하지 않는다.

### API 서버 / 인터페이스 / AI 규약이 걸리면

```text
docs/ai_convention.md
include/interface.h
include/api_contract.h
include/db_service.h
```

특히 API 서버 확장 작업에서는 아래 계층 분리를 지켜야 한다.

- `src/http/**` : 메시지 파싱 / 응답 포맷
- `src/service/**` : 엔진 호출 경계
- `src/server/**` : 서버 런타임
- `src/threadpool/**` : 워커 / 큐

---

## 3. 작업 규칙 요약

1. 먼저 `AGENT.md` 를 읽는다.
2. 역할 소유 파일만 수정한다.
3. 공통 파일은 합의 후 수정한다.
4. `interface.h` 는 엔진 도메인 계약만 유지한다.
5. HTTP/transport 규약은 `api_contract.h` 에 둔다.
6. 서버 -> 엔진 호출 경계는 `db_service.h` 를 따른다.
7. 테스트는 구현과 같이 올린다.

---

## 4. 공통 파일 주의

아래 파일은 저장소 전체 계약에 영향을 주므로 단독 수정하지 않는다.

- `AGENT.md`
- `include/interface.h`
- `Makefile`
- `scripts/check_ownership.sh`
- `docs/roles/**`
- `include/db_service.h`
- `include/server_api.h`

---

## 5. 핵심 금지사항

- `AGENT.md` 를 읽기 전에 수정 시작
- 다른 역할 소유 파일 직접 수정
- `interface.h` 에 HTTP 헤더, 상태 코드, 소켓 fd, thread/runtime 상태 추가
- `src/http/**` 와 `src/server/**` 책임 혼합
- 빠른 해결을 위해 저장/트리 계층을 우회 수정

---

## 6. 최종 기준

작업 중 규칙이 충돌하면 아래 우선순위를 따른다.

1. `AGENT.md`
2. 공통 헤더
   - `include/interface.h`
   - `include/api_contract.h`
   - `include/db_service.h`
3. 역할 문서
   - `docs/roles/*.md`
4. AI 작업 규약
   - `docs/ai_convention.md`
