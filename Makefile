CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS = -lpthread

SRC = src/main.c src/lexer.c src/object.c src/table.c src/value.c src/chunk.c src/debug.c src/vm.c src/compiler.c src/opt.c src/repl.c src/linenoise.c \
      src/lib/math.c src/lib/time.c src/lib/io.c src/lib/os.c src/lib/coroutine.c src/lib/string.c src/lib/core.c src/lib/libs.c src/lib/table.c src/lib/socket.c src/lib/thread.c src/lib/json.c src/lib/template.c src/lib/http.c
OBJ = $(SRC:.c=.o)
TARGET =pua

all: $(TARGET)

# Release build for minimal size
release: CFLAGS = -Wall -Wextra -std=c99 -Os -DNDEBUG -flto
release: LDFLAGS = -Wl,-dead_strip -flto
release: clean $(TARGET)
	strip $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

test: $(TARGET)
	@passed=0; failed=0; timedout=0; \
	for f in tests/*.pua; do \
		printf "Testing $$f... "; \
		if gtimeout 30s ./$(TARGET) "$$f" > /dev/null 2>&1; then \
			printf "PASS\n"; \
			passed=$$((passed + 1)); \
		elif [ $$? -eq 124 ]; then \
			printf "TIMEOUT\n"; \
			timedout=$$((timedout + 1)); \
		else \
			printf "FAIL\n"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	printf "\n=== Results ===\n"; \
	printf "Passed: $$passed\n"; \
	printf "Failed: $$failed\n"; \
	printf "Timeout: $$timedout\n"; \
	[ $$failed -eq 0 ] && [ $$timedout -eq 0 ]

.PHONY: all clean release test
