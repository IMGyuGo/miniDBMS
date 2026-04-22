#!/usr/bin/env bash
# =============================================================
# check_ownership.sh — 파일 소유권 규칙 검증
#
# 역할:
#   git diff --cached 의 스테이징된 파일들이 단일 소유자의
#   영역만 수정하는지 확인한다.
#   여러 역할의 소유 파일이 한 커밋에 섞이면 차단한다.
#
# 사용법:
#   bash scripts/check_ownership.sh
#
# 종료 코드:
#   0 = 통과
#   1 = 소유권 위반
# =============================================================

# ── 파일 → 소유자 매핑 ────────────────────────────────────────
# 반환값: "Role A (...)" | "Role B (...)" | "Role C (...)" | "Role D (...)" | "" (공통)
get_owner() {
    case "$1" in
        include/bptree.h|src/bptree/*|tests/test_bptree.c)
            echo "Role A (B+Tree 저장 코어)" ;;
        include/index_manager.h|src/index/*|tests/test_index.c)
            echo "Role B (인덱스/파일 일관성)" ;;
        src/input/*|src/parser/*|src/schema/*|src/http/*|include/api_contract.h|tests/test_parser.c|tests/test_schema.c|tests/test_http.c)
            echo "Role C (SQL 입력/검증 + API 계약/HTTP 메시지)" ;;
        src/executor/*|src/main.c|src/server/*|src/threadpool/*|src/service/*|tests/test_executor.c|tests/test_threadpool.c|tests/test_api.c|tests/test_concurrency.c)
            echo "Role D (실행/CLI + 서버 런타임)" ;;
        *)
            echo "" ;; # 공통 파일 (누구나 수정 가능)
    esac
}

# ── 공통 파일이지만 전원 합의 필요한 파일 ──────────────────────
needs_consensus() {
    case "$1" in
        include/interface.h|Makefile|AGENT.md|scripts/check_ownership.sh|docs/4인_역할분담_및_진행계획.md|docs/ai_convention.md|docs/roles/*|include/db_service.h|include/server_api.h)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

append_unique_owner() {
    local owner="$1"

    if [ -z "$owner" ]; then
        return
    fi

    if printf '%s\n' "$OWNERS_FOUND" | grep -Fxq "$owner"; then
        return
    fi

    if [ -n "$OWNERS_FOUND" ]; then
        OWNERS_FOUND="${OWNERS_FOUND}"$'\n'"$owner"
    else
        OWNERS_FOUND="$owner"
    fi
}

# ── 스테이징된 파일 목록 확인 ──────────────────────────────────
STAGED=$(git diff --cached --name-only 2>/dev/null)

if [ -z "$STAGED" ]; then
    echo "[OWNERSHIP] 스테이징된 파일 없음. 검증 스킵."
    exit 0
fi

echo "[OWNERSHIP] 스테이징된 파일 검증 중..."

# ── 소유자 수집 ────────────────────────────────────────────────
OWNERS_FOUND=""
WARNED_CONSENSUS=0

while IFS= read -r f; do
    owner=$(get_owner "$f")

    append_unique_owner "$owner"

    # 전원 합의 필요 파일 경고
    if needs_consensus "$f"; then
        echo "  [WARN] '$f' 는 전원 합의 필요 파일입니다."
        WARNED_CONSENSUS=$((WARNED_CONSENSUS + 1))
    fi

done <<< "$STAGED"

# ── 소유자 수 계산 ─────────────────────────────────────────────
OWNER_COUNT=$(printf '%s\n' "$OWNERS_FOUND" | sed '/^$/d' | wc -l | tr -d ' ')

if [ "$OWNER_COUNT" -gt 1 ]; then
    echo ""
    echo "  [OWNERSHIP VIOLATION] 한 커밋에 여러 소유자의 파일이 섞여 있습니다:"
    printf '%s\n' "$OWNERS_FOUND" | sed '/^$/d' | while IFS= read -r o; do
        echo "    - $o"
    done
    echo ""
    echo "  스테이징 내용:"
    while IFS= read -r f; do
        owner=$(get_owner "$f")
        if [ -n "$owner" ]; then
            echo "    [$owner] $f"
        else
            echo "    [공통]   $f"
        fi
    done <<< "$STAGED"
    echo ""
    echo "  → 한 번에 한 역할의 파일만 커밋하세요."
    exit 1
fi

# ── 통과 ───────────────────────────────────────────────────────
if [ "$OWNER_COUNT" -eq 1 ]; then
    ROLE=$(printf '%s\n' "$OWNERS_FOUND" | sed '/^$/d')
    echo "  → 소유자: $ROLE"
fi
if [ "$WARNED_CONSENSUS" -gt 0 ]; then
    echo "  → 전원 합의 파일 포함. 팀 동의 확인 후 커밋."
fi
echo "[OWNERSHIP] PASSED"
exit 0
