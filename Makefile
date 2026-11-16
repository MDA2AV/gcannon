CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -march=native -Isrc -DUSE_PICO -Iexternal/picohttpparser
LDFLAGS = -luring -lpthread -lssl -lcrypto

BUILDDIR = build

# src/main.c runs the benchmark and src/cli.c reads its options; the folders
# hold what it is built from:
#   core/    the per-thread io_uring engine
#   proto/   what goes over the wire: requests, HTTP/1.1, WebSocket, TLS
#   output/  what a run prints and remembers
SRC      = src/main.c src/cli.c \
           src/core/worker.c src/core/pace.c src/core/stats.c \
           src/proto/template.c src/proto/http.c src/proto/ws.c \
           src/proto/tls.c src/proto/ktls.c \
           src/output/report.c src/output/tui.c src/output/history.c
OBJ      = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      = $(OBJ:.o=.d)
PICO_OBJ = $(BUILDDIR)/picohttpparser.o
BIN      = gcannon

all: $(BIN)

$(BIN): $(OBJ) $(PICO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/picohttpparser.o: external/picohttpparser/picohttpparser.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(BIN)

# ── tests ───────────────────────────────────────────────────────────

UNITDIR        = tests/unit
INTEGRATIONDIR = tests/integration

TEST_SRC  = $(UNITDIR)/test_http_parser.c src/proto/http.c external/picohttpparser/picohttpparser.c
TEST_BIN  = $(BUILDDIR)/test_http_parser
RING_TEST_BIN = $(BUILDDIR)/test_buf_ring
RING_TEST_OBJ = $(filter-out $(BUILDDIR)/main.o $(BUILDDIR)/core/worker.o,$(OBJ))

$(TEST_BIN): $(TEST_SRC) src/proto/http.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

# test_buf_ring includes core/worker.c itself to reach its static helpers
$(RING_TEST_BIN): $(UNITDIR)/test_buf_ring.c src/core/worker.c src/core/worker.h src/core/constants.h $(RING_TEST_OBJ) $(PICO_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(RING_TEST_OBJ) $(PICO_OBJ) $(LDFLAGS)

KTLS_TEST_SRC = $(UNITDIR)/test_ktls.c src/proto/ktls.c
KTLS_TEST_BIN = $(BUILDDIR)/test_ktls

$(KTLS_TEST_BIN): $(KTLS_TEST_SRC) src/proto/ktls.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(KTLS_TEST_SRC) -lcrypto -luring

# Run every unit suite even if an earlier one fails, then report the
# combined result.
test-unit: $(TEST_BIN) $(RING_TEST_BIN) $(KTLS_TEST_BIN)
	@rc=0; \
	 ./$(TEST_BIN)      || rc=1; \
	 ./$(RING_TEST_BIN) || rc=1; \
	 ./$(KTLS_TEST_BIN) || rc=1; \
	 exit $$rc

# Every executable tests/integration/test_*.sh is an integration test; adding
# one needs no Makefile change.
test-integration: $(BIN)
	@rc=0; for t in $(INTEGRATIONDIR)/test_*.sh; do \
	    [ -x "$$t" ] || continue; \
	    "$$t" || rc=1; \
	 done; exit $$rc

# Run both suites even if the first one fails, then report the combined result.
test:
	@rc=0; \
	 $(MAKE) --no-print-directory test-unit        || rc=1; \
	 $(MAKE) --no-print-directory test-integration || rc=1; \
	 exit $$rc

-include $(DEP)

.PHONY: all clean test test-unit test-integration
