CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS =
LDLIBS ?=
UNAME_S := $(shell uname -s)
TIMEOUT := $(shell command -v gtimeout >/dev/null 2>&1 && echo gtimeout || echo timeout)

SRC = src/main.c src/lexer.c src/object.c src/table.c src/value.c src/chunk.c src/debug.c src/vm.c src/vm/build_string.c src/vm/ops_arith.c src/vm/ops_arith_const.c src/vm/ops_compare.c src/vm/ops_control.c src/vm/ops_exception.c src/vm/ops_float.c src/vm/ops_has.c src/vm/ops_import.c src/vm/ops_import_star.c src/vm/ops_iter.c src/vm/ops_local_const.c src/vm/ops_local_set.c src/vm/ops_meta.c src/vm/ops_mod.c src/vm/ops_power.c src/vm/ops_print.c src/vm/ops_state.c src/vm/ops_table.c src/vm/ops_unary.c src/compiler.c src/compiler/fstring.c src/compiler/stmt_control.c src/compiler/stmt.c src/opt.c src/repl.c src/linenoise.c \
      src/lib/math.c src/lib/time.c src/lib/io.c src/lib/os.c src/lib/coroutine.c src/lib/string.c src/lib/core.c src/lib/libs.c src/lib/table.c src/lib/socket.c src/lib/thread.c src/lib/json.c src/lib/template.c src/lib/http.c \
      src/lib/inspect.c src/lib/binary.c src/lib/structlib.c src/lib/btree.c
OBJ = $(SRC:.c=.o)
TARGET =pua

all: $(TARGET)

# Release build for minimal size
release: CFLAGS = -Wall -Wextra -std=c99 -Os -DNDEBUG -flto -ffunction-sections -fdata-sections
ifeq ($(UNAME_S),Darwin)
release: LDFLAGS = -Wl,-dead_strip -flto
else
release: LDFLAGS = -Wl,--gc-sections -flto
endif
release: clean $(TARGET)
	strip $(TARGET)

# Release build optimized for speed (host-specific CPU tuning).
release-perf: CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG -flto -march=native -mtune=native -fomit-frame-pointer
ifeq ($(UNAME_S),Darwin)
release-perf: LDFLAGS = -Wl,-dead_strip -flto
else
release-perf: LDFLAGS = -Wl,--gc-sections -flto
endif
release-perf: clean $(TARGET)
	strip $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread -lm $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

pi:
	@echo "Syncing.."
	rsync -az --delete . pi@pi3:/home/pi/pua

test: $(TARGET) test-fmt
	@passed=0; failed=0; timedout=0; \
	for f in tests/*.pua; do \
		printf "Testing $$f... "; \
		if $(TIMEOUT) 30s ./$(TARGET) "$$f" > /dev/null 2>&1; then \
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

.PHONY: all clean release release-perf test pi perf peft test-fmt peft-comp perf-comp

perf: $(TARGET)
	@for f in tests/peft/*.pua; do \
		printf "Running $$f...\n"; \
		./$(TARGET) "$$f"; \
	done

peft: perf

peft-comp:
	@echo "\nPua\n"
	@./$(TARGET) examples/perf.pua
	@echo "\n\nPython\n"
	python3.14 examples/perf.py
	@echo "\n\nLua\n"
	lua examples/perf.lua
	@echo "Done!"

perf-comp: peft-comp

test-fmt: $(TARGET)
	@tests/fmt_regression.sh
