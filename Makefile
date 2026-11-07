CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
LDFLAGS =
LDLIBS ?=
EXTRA_CFLAGS = -D_POSIX_C_SOURCE=200809L
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

SRC = src/main.c src/lexer.c src/object.c src/table.c src/value.c src/chunk.c src/debug.c src/jit.c src/jit_call_runtime.c src/jit_ir.c src/jit_pipeline.c src/jit_native_trace.c src/jit_backend.c src/jit_x64.c src/jit_arm64.c src/vm.c src/vm/build_string.c src/vm/ops_arith.c src/vm/ops_arith_const.c src/vm/ops_compare.c src/vm/ops_control.c src/vm/ops_exception.c src/vm/ops_float.c src/vm/ops_has.c src/vm/ops_import.c src/vm/ops_import_star.c src/vm/ops_iter.c src/vm/ops_local_const.c src/vm/ops_local_set.c src/vm/ops_meta.c src/vm/ops_mod.c src/vm/ops_power.c src/vm/ops_print.c src/vm/ops_state.c src/vm/ops_table.c src/vm/ops_unary.c src/compiler.c src/compiler/fstring.c src/compiler/stmt_control.c src/compiler/stmt.c src/opt.c src/repl.c src/toi_lineedit.c \
      src/lib/math.c src/lib/time.c src/lib/io.c src/lib/sys.c src/lib/os.c src/lib/stat.c src/lib/dir.c src/lib/signal.c src/lib/mmap.c src/lib/poll.c src/lib/coroutine.c src/lib/context.c src/lib/string.c src/lib/core.c src/lib/libs.c src/lib/table.c src/lib/socket.c src/lib/thread.c src/lib/json.c src/lib/template.c src/lib/http.c src/lib/url.c src/lib/regex.c src/lib/fnmatch.c src/lib/glob.c \
      src/lib/inspect.c src/lib/binary.c src/lib/structlib.c src/lib/btree.c src/lib/uuid.c src/lib/gzip.c src/lib/csv.c src/lib/toml.c

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

stats:
	@src_lines=$$(find src -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 awk 'END { print NR }'); \
	lib_lines=$$(find lib -type f \( -name '*.toi' -o -name '*.tpl' -o -name '*.css' \) -print0 | xargs -0 awk 'END { print NR }'); \
	total_size=$$(find src lib -type f \( -name '*.c' -o -name '*.h' -o -name '*.toi' -o -name '*.tpl' -o -name '*.css' \) -print0 | xargs -0 wc -c | awk 'END { print $$1 }'); \
	printf "src lines: %s\n" "$$src_lines"; \
	printf "lib lines: %s\n" "$$lib_lines"; \
	printf "total size: %s bytes\n" "$$total_size"

test: $(TARGET)
	@passed=0; failed=0; timedout=0; \
	for f in tests/test_*.toi; do \
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

.PHONY: all clean release release-perf test test-jit verify docs wasm wasm-release pi stats perf perf-audit peft peft-comp perf-comp benchmark-comp

# Differential JIT check: every test must produce identical stdout with the JIT
# on (aggressively lowered hot threshold, so small loops get compiled too) and
# off. Tests that print timings, memory statistics or time-based progress bars
# are skipped because their output is not deterministic.
JIT_DIFF_SKIP = test_gc_soak_mmap_io_iter test_json_poll_csv test_package_import_db_perf test_template_glob_table_clone test_cli_framework_walrus

test-jit: $(TARGET)
	@passed=0; failed=0; skipped=0; \
	for f in tests/test_*.toi; do \
		base=$$(basename $$f .toi); \
		skip=0; \
		for s in $(JIT_DIFF_SKIP); do \
			if [ "$$base" = "$$s" ]; then skip=1; fi; \
		done; \
		if [ $$skip -eq 1 ]; then \
			printf "Verifying $$f... SKIP (non-deterministic output)\n"; \
			skipped=$$((skipped + 1)); \
			continue; \
		fi; \
		printf "Verifying $$f... "; \
		base_out=$$($(TIMEOUT) 30s ./$(TARGET) "$$f" < /dev/null 2>/dev/null); base_rc=$$?; \
		if [ $$base_rc -ne 0 ]; then \
			printf "SKIP (fails without JIT)\n"; \
			skipped=$$((skipped + 1)); \
			continue; \
		fi; \
		jit_out=$$(TOI_JIT_LOOP_HOT=8 $(TIMEOUT) 30s ./$(TARGET) "$$f" < /dev/null 2>/dev/null); jit_rc=$$?; \
		if [ $$jit_rc -ne 0 ]; then \
			printf "FAIL (jit exit %s)\n" "$$jit_rc"; \
			failed=$$((failed + 1)); \
		elif [ "$$base_out" != "$$jit_out" ]; then \
			printf "FAIL (output differs)\n"; \
			failed=$$((failed + 1)); \
		else \
			printf "PASS\n"; \
			passed=$$((passed + 1)); \
		fi; \
	done; \
	printf "\n=== JIT diff results ===\n"; \
	printf "Passed: $$passed\n"; \
	printf "Failed: $$failed\n"; \
	printf "Skipped: $$skipped\n"; \
	[ $$failed -eq 0 ]

verify: $(TARGET)
	@./benchmarks/verify_results.sh

perf: $(TARGET)
	@for f in tests/peft/*.toi; do \
		printf "Running $$f...\n"; \
		./$(TARGET) "$$f"; \
	done

perf-audit: $(TARGET)
	@echo "Running core perf suite..."
	@./$(TARGET) benchmarks/perf.toi
	@./$(TARGET) benchmarks/string_ops_compare.toi
	@./$(TARGET) benchmarks/json_regex_bench.toi
	@for f in tests/peft/*.toi; do \
		printf "Running $$f...\n"; \
		./$(TARGET) "$$f"; \
	done

peft: perf

docs: $(TARGET)
	./$(TARGET) tools/build_docs.toi
