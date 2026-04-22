# miniDBMS Roles

이 폴더는 역할별 소유 범위와 인터페이스 경계를 빠르게 확인하기 위한 실무용 문서 모음이다.

운영 기준:

- 저장소 진입점: [AGENT.md](../../AGENT.md)
- 기준 분담안: [../4인_역할분담_및_진행계획.md](../4인_역할분담_및_진행계획.md)

## 빠른 소유권 요약

| 역할 | 핵심 책임 | 1차 소유 범위 |
|------|-----------|----------------|
| Role A | B+Tree 코어 | `include/bptree.h`, `src/bptree/**`, `tests/test_bptree.c` |
| Role B | 인덱스/파일 일관성 | `include/index_manager.h`, `src/index/**`, `tests/test_index.c` |
| Role C | SQL 전단 + API 계약/HTTP 메시지 | `src/input/**`, `src/parser/**`, `src/schema/**`, `src/http/**`, `include/api_contract.h`, `tests/test_parser.c`, `tests/test_schema.c`, `tests/test_http.c` |
| Role D | 실행/CLI + 서버 런타임 | `src/executor/**`, `src/main.c`, `src/server/**`, `src/threadpool/**`, `src/service/**`, `tests/test_executor.c`, `tests/test_threadpool.c`, `tests/test_api.c`, `tests/test_concurrency.c` |

## 공통 합의 파일

아래 파일은 역할 문서와 상관없이 공통 합의 후 수정한다.

- `AGENT.md`
- `include/interface.h`
- `Makefile`
- `scripts/check_ownership.sh`
- `docs/4인_역할분담_및_진행계획.md`
- `docs/ai_convention.md`
- `docs/roles/**`
- `include/db_service.h`
- `include/server_api.h`

## 역할 문서

- [ROLE_A.md](ROLE_A.md)
- [ROLE_B.md](ROLE_B.md)
- [ROLE_C.md](ROLE_C.md)
- [ROLE_D.md](ROLE_D.md)
