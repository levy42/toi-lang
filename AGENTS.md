# Repository Guidelines

## Project Structure & Module Organization
- `src/`: C99 interpreter and runtime (core VM, compiler, lexer, REPL).
- `src/lib/`: native built-in modules (e.g., `math`, `time`, `io`, `string`).
- `lib/`: Toi standard library helpers implemented in `.toi` (e.g., `lib.types`).
- `tests/`: `.toi` test programs executed by the VM.
- `examples/`: sample scripts.
- `editor/`: editor support files.
- `toi`: built interpreter binary (created by `make`).

## Build, Test, and Development Commands
- `make`: build the `toi` binary with debug flags.
- `make release`: optimized build (`-Os`, `-DNDEBUG`, LTO) and strip.
- `make clean`: remove object files and the binary.
- `make test`: run all `tests/*.toi` with a 30s timeout each. The Makefile uses `gtimeout` (install via `brew install coreutils` on macOS).
- `./toi`: start the REPL.
- `./toi path/to/file.toi`: run a script.

## Coding Style & Naming Conventions
- Language: C99, compiled with `-Wall -Wextra`.
- Indentation: 4 spaces, no tabs. Keep lines readable; prefer early returns.
- Naming: `snake_case` for functions/locals, `UpperCamelCase` for struct types (e.g., `ObjFunction`), `UPPER_SNAKE` for macros.
- File layout: keep public APIs in `*.h` next to implementation `*.c`.
- Formatting/linting: no enforced formatter or linter; match existing style in `src/`.
- Prefer data-driven token/category membership (tables/sets) over long boolean chains like `a == x or a == y or ...`. If chunk constant limits apply, split lists into smaller chunks or parse from compact strings.
- In Toi, `has` checks value containment, not key presence. For membership checks, store tokens as value lists and use `list has token`; avoid map-key membership patterns and avoid defensive helper wrappers that hide type errors.

## Toi Style Preferences
- Toi is not released yet. If something in the language/runtime feels wrong, awkward, or inconvenient, call it out explicitly and suggest a concrete change instead of silently working around it.
- Write clear, compact Toi. Prefer idiomatic syntax and simple control flow.
- Prefer readability first, then brevity.
- Use Toi language features directly instead of verbose patterns.
- Keep functions focused; return early to reduce nesting.
- Assume types in normal code paths; avoid repetitive runtime type checks where type hints and calling conventions already define expectations.
- Use table compahensions: `{v for v in table}` or `{k=v for k,v in table}` 
- Prefer `if var` over `if var == true`, `if var == false`, `var != ""`, or `var != {}`.
- Prefer `match/case` over long `if/elif/else` chains when matching by value or shape.
- Use guard clauses and early `return`/`continue` instead of deep indentation.
- Prefer `for k, v in table` or `for v in table` over index loops when the index is not required.
- Use table append for push: `items <+ value`.
- Use `table.sort`, `table.concat`, and iterator-style APIs over manual loops when clearer.
- Prefer method-call style when available: `"hello".upper()` over `string.upper("hello")`; `", ".join({"a", "b"})` over `string.join`.
- Prefer slice syntax for strings/tables when clearer: `s[1..5]`, `s[3..]`, `s[..5]`.
- For larger string/HTML generation, build parts and `table.concat(parts, "")` instead of repeated concatenation.
- Prefer interpolation for dynamic strings: `f"hello {name}"`; use `f[[...]]` for multiline templates.
- Prefer direct operators for common patterns: `items <+ value`, `a ? b : c` when readable.
- Prefer native helpers over re-implementing utilities (`string.*`, `path.*`, `os.mkdir(path, true)` for recursive creation).
- Use `::` only for explicit metatable-only method dispatch; prefer `obj.method(...)` for normal lookups.
- If a table is a structured object, define methods in the literal when practical instead of post-assignment.
- Use `error(...)` for unrecoverable programmer/runtime errors.
- For expected failures in library-style functions, prefer `nil, err` returns.
- Keep error messages specific and actionable.
- Use `snake_case` for variables and functions.
- Use clear names over abbreviations unless conventional (`i`, `k`, `v` in short loops).
- Use `UPPER_SNAKE` for module-level constants.

## Toi Usage Guardrails
- Prefer idiomatic Toi primitives first: use `table[key]` for key membership/maps and `has` for value containment; use method-call style where available.
- Do not add defensive `type(...)` wrappers by default. At internal call sites, fail fast on invalid values instead of silently handling misuse.
- If a workaround seems necessary, explicitly call out the language/runtime friction and propose a concrete change before coding around it.
- Prefer data-driven designs (tables/specs) over long `if/or` chains for token/category classification.
- Before adding fallback logic, verify assumptions with a minimal repro script.
- If a workaround is unavoidable, keep it localized and add a TODO with reason and removal condition.
- Final review gate for Toi changes: remove unnecessary type checks, ensure membership style is idiomatic (`table[key]` vs `has`), and document any remaining workaround explicitly.

## Testing Guidelines
- Tests are Toi scripts in `tests/*.toi`.
- Naming: keep tests descriptive and script-like (e.g., `tests/vm_tables.toi`).
- Run with `make test`. Add a new test whenever you fix a bug or add a language feature.

## Commit & Pull Request Guidelines
- Commit messages in history are short and imperative (e.g., “Remove debug printf…”). Keep subject lines concise and action-oriented.
- PRs should include: a clear description, rationale, relevant issue links, and test results (`make test` output summary is enough). Include before/after examples for language or stdlib changes.

## Security & Configuration Notes
- No external configuration is required. Avoid introducing new runtime dependencies without documenting them in `README.md` and the Makefile.
