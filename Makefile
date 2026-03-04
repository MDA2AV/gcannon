CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -march=native -Iinclude
LDFLAGS = -luring -lpthread

SRC     = src/main.c src/worker.c src/http.c src/stats.c
OBJ     = $(SRC:.c=.o)
BIN     = gcannon

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
