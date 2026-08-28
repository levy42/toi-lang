# Repository Guidelines

## Project Structure & Module Organization

This is a compact experimental compiler/interpreter project. Core Python modules live at the repository root:

- `lex.py` tokenizes source text.
- `parser.py` defines AST node dataclasses and parser logic.
- `compiler.py` defines shared IR dataclasses.
- `ast_to_ir.py` lowers parsed AST nodes into IR.
- `arm_backend.py` emits and runs ARM64 machine code from IR.
- `common.py` contains shared exceptions.
- `test.py` is a small executable smoke test for lexing, parsing, IR generation, and backend execution.

Runtime files are also root-level: `runtime.asm` is the assembly source, and `runtime` is the built executable. Do not edit generated files in `__pycache__/`.

## Build, Test, and Development Commands

- `python test.py` runs the current smoke test and prints generated IR.
- `make` builds `runtime` from `runtime.asm` with `clang` and executes it.

There is no package manager configuration or formal test runner yet, so use direct Python commands for local checks.

## Coding Style & Naming Conventions

Use standard Python style with 4-space indentation. Keep AST and IR structures as `@dataclass` types with concise names such as `BinOp`, `JumpIfZero`, and `FuncCall`. Use `snake_case` for functions, methods, and variables, and `PascalCase` for classes.

The lexer currently treats indentation as two spaces for the language being parsed (`INDENT_SPACES = 2`), but Python source files should still use normal 4-space indentation. Keep parser token names short and explicit, for example `"NAME"`, `"INT"`, `"INDENT"`, and `"EOF"`.

## Testing Guidelines

Add focused examples to `test.py` or introduce a `tests/` directory if coverage grows. Tests should exercise the full path when possible: source string -> `lex()` -> `Parser(...).parse()` -> `AstToIr(...).compile()`.

Name future test files with the `test_*.py` pattern so `pytest` or `unittest` can discover them later. For now, run `python test.py` before committing compiler, lexer, or parser changes.

## Commit & Pull Request Guidelines

The current git history has only one short commit message (`my`), so there is no established convention. Use concise imperative commit subjects, such as `Add while-loop parsing` or `Fix IR stack allocation`.

Pull requests should include a brief description, behavior changed, and exact verification commands. Include sample input/output when changing syntax, parser behavior, generated IR, or runtime assembly.

## Agent-Specific Instructions

Keep changes small and local. Do not rewrite unrelated compiler stages while fixing one stage. Avoid committing generated `__pycache__/` files, and verify whether changes affect both the Python compiler pipeline and `runtime.asm` before reporting completion.
