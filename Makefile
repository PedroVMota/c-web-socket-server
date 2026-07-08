CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := chat-server
SRC := src/chat_server.c
TEST_BIN := unit-tests
TEST_SRC := tests/unit_tests.c

.PHONY: all clean run test

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

$(TEST_BIN): $(TEST_SRC) $(SRC)
	$(CC) $(CFLAGS) -I. $(TEST_SRC) -o $(TEST_BIN) $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(TEST_BIN)
