CC     = gcc
CFLAGS = -std=c99 -Wall -Wextra -Iinclude

ifeq ($(OS),Windows_NT)
RUN_PREFIX = .\\
CLEAN_CMD = cmd /C del /Q /F
else
RUN_PREFIX = ./
CLEAN_CMD = rm -f
endif

ENGINE_SRCS = src/input/input.c       \
              src/input/lexer.c       \
              src/parser/parser.c     \
              src/schema/schema.c     \
              src/executor/executor.c \
              src/bptree/bptree.c     \
              src/index/index_manager.c

SRCS = src/main.c                  \
       $(ENGINE_SRCS)              \
       src/http/http_message.c     \
       src/service/db_service.c    \
       src/threadpool/threadpool.c \
       src/server/server.c

TARGET = sqlp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

sim: $(SRCS)
	$(CC) $(CFLAGS) -DBPTREE_SIMULATE_IO=1 -o sqlp_sim $^ -lpthread

PERF_SRCS = tests/test_perf.c       \
            src/input/input.c       \
            src/input/lexer.c       \
            src/parser/parser.c     \
            src/schema/schema.c     \
            src/executor/executor.c \
            src/bptree/bptree.c     \
            src/index/index_manager.c

perf: $(PERF_SRCS)
	$(CC) $(CFLAGS) -o test_perf $^

perf_sim: $(PERF_SRCS)
	$(CC) $(CFLAGS) -DBPTREE_SIMULATE_IO=1 -o test_perf_sim $^

gen_data: tools/gen_data.c
	$(CC) $(CFLAGS) -o gen_data $^

# Role A/B 테스트는 해당 파일이 올라오면 아래 주석 해제
# TEST_BINS_AB = test_bptree test_index
TEST_BINS = test_parser test_schema test_executor test_http test_threadpool

test: $(TEST_BINS)
	@echo ""
	@echo "========== Running Tests =========="
	@$(RUN_PREFIX)test_parser      && echo "[PASS] parser"      || echo "[FAIL] parser"
	@$(RUN_PREFIX)test_schema      && echo "[PASS] schema"      || echo "[FAIL] schema"
	@$(RUN_PREFIX)test_executor    && echo "[PASS] executor"    || echo "[FAIL] executor"
	@$(RUN_PREFIX)test_http        && echo "[PASS] http"        || echo "[FAIL] http"
	@$(RUN_PREFIX)test_threadpool
	@echo "==================================="

test_bptree: tests/test_bptree.c \
             src/bptree/bptree.c
	$(CC) $(CFLAGS) -o $@ $^

test_index: tests/test_index.c   \
            src/bptree/bptree.c  \
            src/index/index_manager.c \
            src/schema/schema.c
	$(CC) $(CFLAGS) -o $@ $^

test_parser: tests/test_parser.c \
             src/input/input.c   \
             src/input/lexer.c   \
             src/parser/parser.c
	$(CC) $(CFLAGS) -o $@ $^

test_schema: tests/test_schema.c \
             src/schema/schema.c \
             src/input/input.c   \
             src/input/lexer.c   \
             src/parser/parser.c
	$(CC) $(CFLAGS) -o $@ $^

test_http: tests/test_http.c         \
           src/http/http_message.c   \
           src/input/input.c         \
           src/input/lexer.c         \
           src/parser/parser.c       \
           src/schema/schema.c       \
           src/executor/executor.c   \
           src/bptree/bptree.c       \
           src/index/index_manager.c \
           src/service/db_service.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_executor: tests/test_executor.c    \
               src/schema/schema.c      \
               src/executor/executor.c  \
               src/bptree/bptree.c      \
               src/index/index_manager.c \
               src/input/input.c        \
               src/input/lexer.c        \
               src/parser/parser.c      \
               src/service/db_service.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

# 역할 D — 스레드 풀 동시성 스모크 테스트
test_threadpool: tests/test_threadpool.c    \
                 src/threadpool/threadpool.c \
                 src/http/http_message.c    \
                 src/service/db_service.c   \
                 src/input/input.c          \
                 src/input/lexer.c          \
                 src/parser/parser.c        \
                 src/schema/schema.c        \
                 src/executor/executor.c    \
                 src/bptree/bptree.c        \
                 src/index/index_manager.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

clean:
	-$(CLEAN_CMD) $(TARGET) $(TARGET).exe \
	             sqlp_sim sqlp_sim.exe \
	             $(TEST_BINS) $(addsuffix .exe,$(TEST_BINS)) \
	             test_perf test_perf.exe \
	             test_perf_sim test_perf_sim.exe \
	             gen_data gen_data.exe

.PHONY: all sim perf perf_sim gen_data test clean
