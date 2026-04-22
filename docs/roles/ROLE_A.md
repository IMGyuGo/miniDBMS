# Role A — B+Tree 저장 코어

## 1차 소유 범위

- `include/bptree.h`
- `src/bptree/**`
- `tests/test_bptree.c`

## 핵심 책임

- B+Tree insert/search/range 정확성
- split, 높이 변화, 중복 key 정책 정리
- tree height / I/O 측정 포인트 유지
- B+Tree 단위 테스트 작성

## 인터페이스 경계

- 외부에는 `include/bptree.h` 공개 API만 노출한다.
- Role B는 이 API를 통해서만 트리를 사용한다.
- Role D는 B+Tree 내부 구조를 직접 의존하지 않는다.

## 수정하면 안 되는 영역

- `src/index/**`
- `src/executor/**`
- `src/http/**`
- `src/server/**`
- `src/threadpool/**`
- `src/service/**`

## 협업 규칙

1. `include/bptree.h` 변경 시 Role B에 먼저 영향 범위를 공유한다.
2. 트리 문제를 index/executor 쪽 우회 수정으로 덮지 않는다.
3. API가 바뀌면 테스트와 문서를 같이 갱신한다.
