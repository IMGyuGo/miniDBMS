#!/usr/bin/env bash
# =============================================================
# check_binary_mode.sh — fopen binary 모드 규칙 검증
#
# 규칙:
#   data/*.dat 파일에 접근하는 fopen() 은 반드시 binary 모드를 사용해야 한다.
#   - INSERT 쓰기: "ab" (binary append)
#   - SELECT 읽기: "rb" (binary read)
#   - text 모드 ("r", "a", "w") 는 Windows에서 ftell 오프셋 오류를 일으킨다.
#
# 종료 코드:
#   0 = 통과
#   1 = 위반 발견
# =============================================================

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

echo "[BINARY_MODE] fopen() 모드 검증 중..."

FAILED=0
mapfile -t CANDIDATE_FILES < <(
    if [ -d "src" ]; then
        find src -type f -name '*.c' -exec grep -l -E '\.dat|data/' {} + 2>/dev/null | sort -u
    fi
)

if [ "${#CANDIDATE_FILES[@]}" -eq 0 ]; then
    echo "  [SKIP] data/*.dat 접근 소스를 찾지 못했습니다."
    echo ""
    echo "[BINARY_MODE] PASSED"
    exit 0
fi

for f in "${CANDIDATE_FILES[@]}"; do
    # text 모드 패턴 탐지: "r", "w", "a", "r+", "w+", "a+"
    # binary 모드가 포함된 "rb", "wb", "ab", "r+b" 등은 제외된다.
    VIOLATIONS=$(grep -nE 'fopen[[:space:]]*\([^,]+,[[:space:]]*"(r\+?|w\+?|a\+?)"\)' "$f")

    if [ -n "$VIOLATIONS" ]; then
        echo "  [FAIL] $f — text mode fopen 감지:"
        echo "$VIOLATIONS" | while IFS= read -r line; do
            echo "         $line"
        done
        echo "         → \"rb\" / \"ab\" / \"wb\" 등 binary 모드로 변경하세요 (AGENT.md 섹션 8 참조)"
        FAILED=$((FAILED + 1))
    else
        echo "  [OK]   $f"
    fi
done

echo ""
if [ $FAILED -eq 0 ]; then
    echo "[BINARY_MODE] PASSED"
    exit 0
else
    echo "[BINARY_MODE] FAILED — $FAILED 개 파일에 text mode fopen 있음"
    exit 1
fi
