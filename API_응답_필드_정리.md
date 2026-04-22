# API 응답 필드 정리

## 공통 필드 (모든 응답에 포함)

| 필드 | 타입 | 설명 | 예시 |
|------|------|------|------|
| `ok` | boolean | 요청 성공 여부 | `true` / `false` |
| `http_status` | number | HTTP 상태코드 | `200`, `404` |
| `code` | string | 상태를 나타내는 문자열 코드 | `"OK"`, `"UNSUPPORTED_ROUTE"` |
| `error` | string | 에러 메시지. 성공 시 빈 문자열 | `""`, `"unsupported route"` |

---

## 쿼리 결과 필드 (SELECT / INSERT 응답)

| 필드 | 타입 | 설명 | 예시 |
|------|------|------|------|
| `row_count` | number | SELECT로 조회된 row 수 | `1`, `553` |
| `rows_affected` | number | INSERT로 실제 써진 row 수 | `1` |
| `has_payload` | boolean | `rows` 배열 존재 여부. SELECT면 true, INSERT면 false | `true` / `false` |
| `rows` | array | 실제 조회 결과 배열 | `[{ "id": "5001", ... }]` |

---

## profile 필드 (쿼리 실행 정보)

| 필드 | 타입 | 설명 | 예시 |
|------|------|------|------|
| `elapsed_ms` | number | 쿼리 실행 시간 (밀리초) | `0.136` |
| `tree_io` | number | B+Tree 노드 방문 횟수. linear 경로면 0 | `2`, `0` |
| `row_count` | number | 조회된 row 수 (위의 row_count와 동일) | `1` |
| `access_path` | string | 쿼리가 실행된 경로 (아래 표 참고) | `"index:id:eq"` |

---

## access_path 종류

| 값 | 사용 조건 | 설명 |
|----|-----------|------|
| `index:id:eq` | `WHERE id = ?` | id B+Tree로 단건 조회. 가장 빠름. tree_io = 트리 높이 |
| `index:id:range` | `WHERE id BETWEEN ? AND ?` | id B+Tree로 범위 조회. leaf를 옆으로 순회하므로 tree_io가 높이보다 클 수 있음 |
| `index:age:range` | `WHERE age BETWEEN ? AND ?` | age B+Tree로 범위 조회. 동일 age 값이 많을 경우 결과 row fetch 비용이 커서 linear보다 느릴 수 있음 |
| `linear` | 위 세 가지 외 모든 경우 | 파일 전체를 처음부터 끝까지 스캔. tree_io = 0 |

> `linear`가 되는 대표적인 경우: `WHERE age = 25` (BETWEEN이 아닌 equality), `WHERE name = 'alice'`, WHERE 절 없는 전체 조회

---

## 응답 예시

### 헬스체크
```json
{
    "ok": true,
    "http_status": 200,
    "code": "OK",
    "message": "healthy"
}
```

### INSERT 성공
```json
{
    "ok": true,
    "http_status": 200,
    "code": "OK",
    "row_count": 0,
    "rows_affected": 1,
    "has_payload": false,
    "error": ""
}
```

### SELECT — 인덱스 경로 (id 단건)
```json
{
    "ok": true,
    "http_status": 200,
    "code": "OK",
    "row_count": 1,
    "rows_affected": 0,
    "has_payload": true,
    "error": "",
    "rows": [{ "id": "5001", "name": "postman-user", "age": "27", "email": "postman-user@example.com" }],
    "profile": {
        "elapsed_ms": 0.136,
        "tree_io": 2,
        "row_count": 1,
        "access_path": "index:id:eq"
    }
}
```

### SELECT — linear 경로
```json
{
    "ok": true,
    "http_status": 200,
    "code": "OK",
    "row_count": 553,
    "rows_affected": 0,
    "has_payload": true,
    "error": "",
    "rows": [ "..." ],
    "profile": {
        "elapsed_ms": 0.636,
        "tree_io": 0,
        "row_count": 553,
        "access_path": "linear"
    }
}
```

### 에러 응답
```json
{
    "ok": false,
    "http_status": 404,
    "code": "UNSUPPORTED_ROUTE",
    "row_count": 0,
    "rows_affected": 0,
    "has_payload": false,
    "error": "unsupported route"
}
```
