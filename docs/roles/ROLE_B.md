# Role B — 인덱스/파일 일관성

## 1차 소유 범위

- `include/index_manager.h`
- `src/index/**`
- `tests/test_index.c`

## 핵심 책임

- `.dat` 파일과 인덱스 offset 경로 정리
- 서버 시작 시 인덱스 재구성 전략 정리
- 저장 계층 보호 구간 도출
- 인덱스 단위 테스트 작성

## 인터페이스 경계

- Role A의 트리 기능은 `include/bptree.h` 공개 API로만 사용한다.
- Role D는 `include/index_manager.h` 를 통해서만 인덱스를 사용한다.
- 락 정책은 Role B가 보호 구간을 정의하고, 실제 런타임 적용은 Role D와 합의한다.

## 수정하면 안 되는 영역

- `src/bptree/**`
- `src/http/**`
- `src/server/**`
- `src/threadpool/**`
- `src/service/**`

## 협업 규칙

1. 저장 계층 문제가 보여도 B+Tree 내부를 직접 수정하지 않는다.
2. 락이 필요한 지점은 코드보다 먼저 문서화해서 Role D와 맞춘다.
3. 인덱스 rebuild 정책이 바뀌면 관련 테스트를 같이 올린다.
