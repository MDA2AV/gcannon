CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -march=native -Iinclude -DUSE_PICO -Iexternal/picohttpparser
LDFLAGS = -luring -lpthread -lssl -lcrypto

BUILDDIR = build

SRC      = src/main.c src/worker.c src/http.c src/ws.c src/stats.c src/tui.c src/history.c \
           src/ktls.c
OBJ      = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      = $(OBJ:.o=.d)
PICO_OBJ = $(BUILDDIR)/picohttpparser.o
BIN      = gcannon

all: $(BIN)

$(BIN): $(OBJ) $(PICO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/picohttpparser.o: external/picohttpparser/picohttpparser.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(BIN)

# ── tests ───────────────────────────────────────────────────────────

TESTDIR   = tests
TEST_SRC  = $(TESTDIR)/test_http_parser.c src/http.c external/picohttpparser/picohttpparser.c
TEST_BIN  = $(BUILDDIR)/test_http_parser
RING_TEST_BIN = $(BUILDDIR)/test_buf_ring
RING_TEST_OBJ = $(filter-out $(BUILDDIR)/main.o $(BUILDDIR)/worker.o,$(OBJ))

$(TEST_BIN): $(TEST_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

$(RING_TEST_BIN): $(TESTDIR)/test_buf_ring.c src/worker.c include/worker.h include/constants.h $(RING_TEST_OBJ) $(PICO_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(RING_TEST_OBJ) $(PICO_OBJ) $(LDFLAGS)

KTLS_TEST_SRC = $(TESTDIR)/test_ktls.c src/ktls.c
KTLS_TEST_BIN = $(BUILDDIR)/test_ktls

$(KTLS_TEST_BIN): $(KTLS_TEST_SRC) include/ktls.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(KTLS_TEST_SRC) -lcrypto -luring

# Run every unit suite even if an earlier one fails, then report the
# combined result.
test-unit: $(TEST_BIN) $(RING_TEST_BIN) $(KTLS_TEST_BIN)
	@rc=0; \
	 ./$(TEST_BIN)      || rc=1; \
	 ./$(RING_TEST_BIN) || rc=1; \
	 ./$(KTLS_TEST_BIN) || rc=1; \
	 exit $$rc

# Every executable tests/test_*.sh is an integration test; adding one needs no
# Makefile change.
test-integration: $(BIN)
	@rc=0; for t in $(TESTDIR)/test_*.sh; do \
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
