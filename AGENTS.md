# Repository Guidelines

## Project Structure & Module Organization
- `src/`: C99 interpreter and runtime (core VM, compiler, lexer, REPL).
- `src/lib/`: native built-in modules (e.g., `math`, `time`, `io`, `string`).
- `lib/`: Pua standard library helpers implemented in `.pua` (e.g., `lib.types`).
- `tests/`: `.pua` test programs executed by the VM.
- `examples/`: sample scripts.
- `editor/`: editor support files.
- `pua`: built interpreter binary (created by `make`).

## Build, Test, and Development Commands
- `make`: build the `pua` binary with debug flags.
- `make release`: optimized build (`-Os`, `-DNDEBUG`, LTO) and strip.
- `make clean`: remove object files and the binary.
- `make test`: run all `tests/*.pua` with a 30s timeout each. The Makefile uses `gtimeout` (install via `brew install coreutils` on macOS).
- `./pua`: start the REPL.
- `./pua path/to/file.pua`: run a script.

## Coding Style & Naming Conventions
- Language: C99, compiled with `-Wall -Wextra`.
- Indentation: 4 spaces, no tabs. Keep lines readable; prefer early returns.
- Naming: `snake_case` for functions/locals, `UpperCamelCase` for struct types (e.g., `ObjFunction`), `UPPER_SNAKE` for macros.
- File layout: keep public APIs in `*.h` next to implementation `*.c`.
- Formatting/linting: no enforced formatter or linter; match existing style in `src/`.

## Testing Guidelines
- Tests are Pua scripts in `tests/*.pua`.
- Naming: keep tests descriptive and script-like (e.g., `tests/vm_tables.pua`).
- Run with `make test`. Add a new test whenever you fix a bug or add a language feature.

## Commit & Pull Request Guidelines
- Commit messages in history are short and imperative (e.g., “Remove debug printf…”). Keep subject lines concise and action-oriented.
- PRs should include: a clear description, rationale, relevant issue links, and test results (`make test` output summary is enough). Include before/after examples for language or stdlib changes.

## Security & Configuration Notes
- No external configuration is required. Avoid introducing new runtime dependencies without documenting them in `README.md` and the Makefile.
