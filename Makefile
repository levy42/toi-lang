CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS =
LDLIBS ?=
EXTRA_CFLAGS =
EXTRA_LDLIBS =
WASM_CC ?= zig cc
WASM_SYSROOT ?=
WASM_ENV ?= ZIG_GLOBAL_CACHE_DIR=/tmp/zig-cache ZIG_LOCAL_CACHE_DIR=/tmp/zig-local
WASM_CFLAGS_BASE = -Wall -Wextra -std=c99 -O2 --target=wasm32-wasi -DTOI_WASM -D_WASI_EMULATED_SIGNAL
WASM_LDFLAGS_BASE = --target=wasm32-wasi
WASM_LDLIBS_BASE = -lwasi-emulated-signal
WASM_RELEASE_CFLAGS = $(WASM_CFLAGS_BASE) -Oz -DNDEBUG -flto -ffunction-sections -fdata-sections
WASM_RELEASE_LDFLAGS = $(WASM_LDFLAGS_BASE) -flto -Wl,--gc-sections -Wl,--strip-all
ifneq ($(strip $(WASM_SYSROOT)),)
WASM_CFLAGS_BASE += --sysroot=$(WASM_SYSROOT)
WASM_LDFLAGS_BASE += --sysroot=$(WASM_SYSROOT)
endif

# Fallback for environments without zig: use clang + distro wasi-libc layout.
ifeq ($(strip $(WASM_CC)),zig cc)
ifeq ($(strip $(wildcard /usr/local/bin/zig /usr/bin/zig)),)
ifneq ($(wildcard /usr/include/wasm32-wasi/stdio.h),)
ifneq ($(wildcard /usr/bin/clang-20),)
WASM_CC := clang-20
else
WASM_CC := clang
endif
WASM_CFLAGS_BASE += -I/usr/include/wasm32-wasi
WASM_LDFLAGS_BASE += -L/usr/lib/wasm32-wasi
endif
endif
endif
UNAME_S := $(shell uname -s)
TIMEOUT := $(shell command -v gtimeout >/dev/null 2>&1 && echo gtimeout || echo timeout)

SRC = src/main.c src/lexer.c src/object.c src/table.c src/value.c src/chunk.c src/debug.c src/vm.c src/vm/build_string.c src/vm/ops_arith.c src/vm/ops_arith_const.c src/vm/ops_compare.c src/vm/ops_control.c src/vm/ops_exception.c src/vm/ops_float.c src/vm/ops_has.c src/vm/ops_import.c src/vm/ops_import_star.c src/vm/ops_iter.c src/vm/ops_local_const.c src/vm/ops_local_set.c src/vm/ops_meta.c src/vm/ops_mod.c src/vm/ops_power.c src/vm/ops_print.c src/vm/ops_state.c src/vm/ops_table.c src/vm/ops_unary.c src/compiler.c src/compiler/fstring.c src/compiler/stmt_control.c src/compiler/stmt.c src/opt.c src/repl.c src/toi_lineedit.c \
      src/lib/math.c src/lib/time.c src/lib/io.c src/lib/os.c src/lib/stat.c src/lib/dir.c src/lib/signal.c src/lib/mmap.c src/lib/poll.c src/lib/coroutine.c src/lib/string.c src/lib/core.c src/lib/libs.c src/lib/table.c src/lib/socket.c src/lib/thread.c src/lib/json.c src/lib/template.c src/lib/http.c src/lib/regex.c src/lib/fnmatch.c src/lib/glob.c \
      src/lib/inspect.c src/lib/binary.c src/lib/structlib.c src/lib/btree.c src/lib/uuid.c src/lib/gzip.c

LDLIBS += -lz
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
ifneq ($(strip $(OPENSSL_LIBS)),)
EXTRA_CFLAGS += $(OPENSSL_CFLAGS) -DTOI_HAVE_TLS
EXTRA_LDLIBS += $(OPENSSL_LIBS)
endif

OBJ = $(SRC:.c=.o)
TARGET =toi
WASM_TARGET = toi.wasm
WASM_SRC = $(filter-out src/repl.c src/toi_lineedit.c src/lib/os.c src/lib/stat.c src/lib/dir.c src/lib/signal.c src/lib/mmap.c src/lib/poll.c src/lib/socket.c src/lib/thread.c src/lib/time.c src/lib/uuid.c src/lib/regex.c src/lib/fnmatch.c src/lib/glob.c src/lib/gzip.c,$(SRC)) src/repl_stub.c
WASM_OBJ = $(WASM_SRC:.c=.wasm.o)

all: $(TARGET)

wasm: CC = $(WASM_CC)
wasm: CFLAGS = $(WASM_CFLAGS_BASE)
wasm: LDFLAGS = $(WASM_LDFLAGS_BASE)
wasm: $(WASM_TARGET)

wasm-release: CC = $(WASM_CC)
wasm-release: CFLAGS = $(WASM_RELEASE_CFLAGS)
wasm-release: LDFLAGS = $(WASM_RELEASE_LDFLAGS)
wasm-release: clean $(WASM_TARGET)
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz -o $(WASM_TARGET) $(WASM_TARGET); \
	fi

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
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread -lm $(LDLIBS) $(EXTRA_LDLIBS)

$(WASM_TARGET): $(WASM_OBJ)
	$(WASM_ENV) $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm $(WASM_LDLIBS_BASE) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

%.wasm.o: %.c
	$(WASM_ENV) $(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET) $(WASM_OBJ) $(WASM_TARGET)

pi:
	@echo "Syncing.."
	rsync -az --delete . pi@pi3:/home/pi/toi

test: $(TARGET) test-fmt
	@passed=0; failed=0; timedout=0; \
	for f in tests/*.toi; do \
		printf "Testing $$f... "; \
		if $(TIMEOUT) 30s ./$(TARGET) "$$f" < /dev/null > /dev/null 2>&1; then \
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

.PHONY: all clean release release-perf test docs wasm wasm-release pi perf peft test-fmt peft-comp perf-comp benchmark-comp

perf: $(TARGET)
	@for f in tests/peft/*.toi; do \
		printf "Running $$f...\n"; \
		./$(TARGET) "$$f"; \
	done

peft: perf

test-fmt: $(TARGET)
	@tests/fmt_regression.sh

docs: $(TARGET)
	./$(TARGET) tools/build_docs.toi
