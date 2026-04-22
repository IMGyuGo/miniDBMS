# Role C — SQL 입력/검증 + API 계약/HTTP 메시지

## 1차 소유 범위

- `src/input/**`
- `src/parser/**`
- `src/schema/**`
- `src/http/**`
- `include/api_contract.h`
- `tests/test_parser.c`
- `tests/test_schema.c`
- `tests/test_http.c`

## 핵심 책임

- SQL 문자열을 안정적으로 토큰/AST/검증 결과로 변환
- 오류 메시지와 검증 실패 케이스 정의
- API 요청/응답 DTO와 HTTP 메시지 규약 정의
- parser/schema/http 단위 테스트 작성

## 인터페이스 경계

- `include/interface.h` 는 Role C와 Role D가 함께 쓰는 핵심 계약이다.
- `include/api_contract.h` 는 Role C가 1차 소유하지만, Role D가 런타임에서 사용하므로 독단 변경하지 않는다.
- Role D는 메시지 규약을 직접 바꾸지 않고 계약을 통해 서버를 붙인다.

## 수정하면 안 되는 영역

- `src/executor/**`
- `src/server/**`
- `src/threadpool/**`
- `src/service/**`
- `src/index/**`
- `src/bptree/**`

## 협업 규칙

1. HTTP 메시지 파싱/포맷은 Role C가 맡고, accept loop/worker runtime 은 Role D가 맡는다.
2. 응답 구조가 바뀌면 `include/api_contract.h` 와 테스트를 함께 갱신한다.
3. 빠른 서버 연동을 위해 executor 내부를 직접 수정하지 않는다.
