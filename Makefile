CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -march=native -Iinclude -DUSE_PICO -Iexternal/picohttpparser
LDFLAGS = -luring -lpthread

BUILDDIR = build

SRC      = src/main.c src/worker.c src/http.c src/ws.c src/stats.c src/tui.c src/history.c
OBJ      = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
PICO_OBJ = $(BUILDDIR)/picohttpparser.o
BIN      = gcannon

all: $(BIN)

$(BIN): $(OBJ) $(PICO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/picohttpparser.o: external/picohttpparser/picohttpparser.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(BIN)

.PHONY: all clean
