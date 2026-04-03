CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -march=native -Iinclude -DUSE_PICO -Iexternal/picohttpparser
LDFLAGS = -luring -lpthread

SRC     = src/main.c src/worker.c src/http.c src/ws.c src/stats.c src/tui.c
OBJ     = $(SRC:.c=.o)
BIN     = gcannon

PICO_OBJ = external/picohttpparser/picohttpparser.o

all: $(BIN)

$(BIN): $(OBJ) $(PICO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

external/picohttpparser/picohttpparser.o: external/picohttpparser/picohttpparser.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(PICO_OBJ) $(BIN)

.PHONY: all clean
