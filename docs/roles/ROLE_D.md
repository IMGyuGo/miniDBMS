# Role D — 실행/CLI + 서버 런타임

## 1차 소유 범위

- `src/executor/**`
- `src/main.c`
- `src/server/**`
- `src/threadpool/**`
- `src/service/**`
- `tests/test_executor.c`
- `tests/test_threadpool.c`
- `tests/test_api.c`
- `tests/test_concurrency.c`

## 핵심 책임

- CLI 실행 흐름을 공용 서비스 함수로 정리
- executor 결과를 서버 응답에 실을 수 있게 연결
- 서버 런타임, 스레드풀, 서비스 계층 구현
- 통합 테스트와 end-to-end 시연 경로 정리

## 인터페이스 경계

- 메시지 규약과 DTO는 Role C가 정의한 계약을 따른다.
- 엔진 호출은 공용 서비스 경계를 통해 수행한다.
- 저장 계층 락 범위는 Role B 문서를 기준으로 합의 후 적용한다.

## 수정하면 안 되는 영역

- `src/http/**`
- `src/parser/**`
- `src/schema/**`
- `src/input/**`
- `src/index/**`
- `src/bptree/**`

## 협업 규칙

1. HTTP 메시지 포맷을 바꾸려고 `src/http/**` 를 직접 수정하지 않는다.
2. index/bptree 문제를 빠르게 해결하려고 저장 계층 파일을 직접 수정하지 않는다.
3. 공용 실행 경계가 바뀌면 관련 테스트와 문서를 같이 갱신한다.
