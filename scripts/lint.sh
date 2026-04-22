#!/usr/bin/env bash
# =============================================================
# lint.sh — 현재 역할 구조 기준 C 소스 컴파일 검증
#
# 사용법:
#   bash scripts/lint.sh
#   bash scripts/lint.sh [추가 소스파일...]
#
# 검증 방식:
#   - Role A/B/C/D 소유 디렉터리에서 현재 존재하는 C 파일을 자동 수집한다.
#   - 테스트 파일도 역할 기준 이름으로 수집해 개별 진단한다.
#   - 추가 소스파일 인자를 넘기면 함께 검사한다.
#
# 종료 코드:
#   0 = 성공 (에러 없음)
#   1 = 실패 (컴파일 에러 존재)
# =============================================================

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFLAGS="-std=c99 -Wall -Wextra -Iinclude -fsyntax-only"

# ── 컴파일러 탐색: 환경변수 CC → gcc → clang → cc ──────────────
find_compiler() {
    if [ -n "$CC" ] && command -v "$CC" >/dev/null 2>&1; then
        echo "$CC"; return
    fi
    for candidate in gcc clang cc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"; return
        fi
    done
    echo ""
}

CC=$(find_compiler)

if [ -z "$CC" ]; then
    echo "[LINT] SKIP — C 컴파일러를 찾을 수 없습니다 (gcc / clang 미설치)"
    echo "         컴파일러 설치 후 재실행하거나, CC 환경변수로 경로를 지정하세요."
    echo "         예) CC=/usr/bin/gcc bash scripts/lint.sh"
    exit 0   # 도구 부재는 커밋을 막지 않는다
fi

collect_dir_c_files() {
    local dir="$1"

    if [ -d "$dir" ]; then
        find "$dir" -type f -name '*.c' | sort
    fi
}

append_existing_file() {
    local file="$1"
    local label="$2"

    if [ -f "$file" ]; then
        echo "$file"
        return
    fi

    if [ -n "$label" ]; then
        echo "[LINT] WARN — $label 파일이 없어 건너뜁니다: $file" >&2
    fi
}

collect_role_sources() {
    append_existing_file "src/main.c" ""
    collect_dir_c_files "src/executor"
    collect_dir_c_files "src/server"
    collect_dir_c_files "src/threadpool"
    collect_dir_c_files "src/service"
}

cd "$ROOT" || exit 1

mapfile -t ROLE_A_SRCS < <(collect_dir_c_files "src/bptree")
mapfile -t ROLE_B_SRCS < <(collect_dir_c_files "src/index")
mapfile -t ROLE_C_SRCS < <(
    collect_dir_c_files "src/input"
    collect_dir_c_files "src/parser"
    collect_dir_c_files "src/schema"
    collect_dir_c_files "src/http"
)
mapfile -t ROLE_D_SRCS < <(collect_role_sources)

MAIN_SRCS=(
    "${ROLE_A_SRCS[@]}"
    "${ROLE_B_SRCS[@]}"
    "${ROLE_C_SRCS[@]}"
    "${ROLE_D_SRCS[@]}"
)

ROLE_A_TESTS=()
ROLE_B_TESTS=()
ROLE_C_TESTS=()
ROLE_D_TESTS=()

if [ -f "tests/test_bptree.c" ]; then ROLE_A_TESTS+=("tests/test_bptree.c"); fi
if [ -f "tests/test_index.c" ]; then ROLE_B_TESTS+=("tests/test_index.c"); fi
if [ -f "tests/test_parser.c" ]; then ROLE_C_TESTS+=("tests/test_parser.c"); fi
if [ -f "tests/test_schema.c" ]; then ROLE_C_TESTS+=("tests/test_schema.c"); fi
if [ -f "tests/test_http.c" ]; then ROLE_C_TESTS+=("tests/test_http.c"); fi
if [ -f "tests/test_executor.c" ]; then ROLE_D_TESTS+=("tests/test_executor.c"); fi
if [ -f "tests/test_threadpool.c" ]; then ROLE_D_TESTS+=("tests/test_threadpool.c"); fi
if [ -f "tests/test_api.c" ]; then ROLE_D_TESTS+=("tests/test_api.c"); fi
if [ -f "tests/test_concurrency.c" ]; then ROLE_D_TESTS+=("tests/test_concurrency.c"); fi

TEST_SRCS=(
    "${ROLE_A_TESTS[@]}"
    "${ROLE_B_TESTS[@]}"
    "${ROLE_C_TESTS[@]}"
    "${ROLE_D_TESTS[@]}"
)

TOOL_SRCS=()
if [ -d "tools" ]; then
    mapfile -t TOOL_SRCS < <(collect_dir_c_files "tools")
fi

EXTRA_SRCS=()
for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "[LINT] WARN — 추가 파일이 없어 건너뜁니다: $f"
        continue
    fi

    case "$f" in
        *.c) EXTRA_SRCS+=("$f") ;;
        *)
            echo "[LINT] WARN — C 소스가 아니어서 건너뜁니다: $f"
            ;;
    esac
done

echo "[LINT] 컴파일 검증 시작 (CC=$(command -v "$CC"))"
echo "[LINT] CFLAGS: $CFLAGS"
echo "[LINT] 역할별 앱 소스: A=${#ROLE_A_SRCS[@]} B=${#ROLE_B_SRCS[@]} C=${#ROLE_C_SRCS[@]} D=${#ROLE_D_SRCS[@]}"
echo "[LINT] 역할별 테스트: A=${#ROLE_A_TESTS[@]} B=${#ROLE_B_TESTS[@]} C=${#ROLE_C_TESTS[@]} D=${#ROLE_D_TESTS[@]}"
echo "[LINT] 도구 소스: ${#TOOL_SRCS[@]}  추가 소스: ${#EXTRA_SRCS[@]}"
echo ""

FAILED=0
APP_SRCS=("${MAIN_SRCS[@]}" "${EXTRA_SRCS[@]}")

# ── 1. 전체 빌드 대상을 한 번에 검증 ──────────────────────────
if [ "${#APP_SRCS[@]}" -eq 0 ]; then
    echo "[1] 앱 소스 전체 검증..."
    echo "    → SKIP (검사할 src/*.c 없음)"
else
    echo "[1] 앱 소스 전체 검증..."
    if $CC $CFLAGS "${APP_SRCS[@]}" 2>&1; then
        echo "    → OK"
    else
        echo "    → FAILED"
        FAILED=$((FAILED + 1))

        # 실패 시 파일별로 개별 검증해 원인 특정
        echo ""
        echo "    [개별 파일 진단 중...]"
        for f in "${APP_SRCS[@]}"; do
            if [ ! -f "$f" ]; then
                echo "    [SKIP] $f (파일 없음)"
                continue
            fi
            if ! $CC $CFLAGS "$f" >/dev/null 2>&1; then
                echo "    [ERROR] $f"
            fi
        done
    fi
fi

run_individual_group() {
    local label="$1"
    shift
    local files=("$@")
    local ok=0
    local skip=0

    echo ""
    echo "$label"

    if [ "${#files[@]}" -eq 0 ]; then
        echo "    → SKIP: 검사 대상 없음"
        return
    fi

    for f in "${files[@]}"; do
        if [ ! -f "$f" ]; then
            skip=$((skip + 1))
            continue
        fi
        if $CC $CFLAGS "$f" >/dev/null 2>&1; then
            ok=$((ok + 1))
        else
            echo "    [ERROR] $f"
            $CC $CFLAGS "$f" 2>&1 | sed 's/^/            /'
            FAILED=$((FAILED + 1))
        fi
    done

    echo "    → OK: $ok  SKIP: $skip"
}

run_individual_group "[2] 테스트 파일 검증..." "${TEST_SRCS[@]}"
run_individual_group "[3] 도구/추가 파일 검증..." "${TOOL_SRCS[@]}" "${EXTRA_SRCS[@]}"

# ── 결과 ───────────────────────────────────────────────────────
echo ""
if [ $FAILED -eq 0 ]; then
    echo "[LINT] PASSED — 컴파일 에러 없음"
    exit 0
else
    echo "[LINT] FAILED — $FAILED 개 에러 발생"
    exit 1
fi
