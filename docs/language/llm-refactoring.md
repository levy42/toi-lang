# Toi Refactoring Reference for LLMs

Use this page when modernizing existing Toi code. The compiler and current repository examples are the source of truth: Toi is evolving, so verify unfamiliar syntax against nearby code or a focused test. Preserve behavior and public APIs while refactoring.

## Core syntax

- Blocks use indentation (no braces); comments start with `--`.
- Bind/assign with `name = value`; tuple assignment works: `a, b = pair`.
- Functions: `fn name(arg, optional = default) -> type`; type hints are metadata, not static checking.
- Imports: `import math`, `import os, io`, `from lib.test import assert_eq`.
- `nil` is the empty value. Only `nil` and `false` are falsey.

## Tables and iteration

Tables serve as lists, maps, and records. Numeric list indices start at 1.

```toi
items = {"a", "b"}
items <+ "c"                    -- append
by_name = {name = "Ana", [key] = 7}
value = by_name[key]             -- map lookup
names = {row.name for row in rows} -- comprehension

for item in items
  print item
for key, value in by_name
  print key, value
```

Use `for value in table` for values and `for key, value in table` for entries. Use `#items` for length. Prefer iteration over index loops unless the index is needed. `table[key]` is the direct lookup; `has` tests value containment, not key presence.

## Expressions and control flow

```toi
if ready
  run()
elif pending
  wait()
else
  stop()

match kind
case "text"
  handle_text()
case "image"
  handle_image()
else
  handle_other()

label = valid ? "ok" : "bad"
if (line := input("> ")) != ""
  print line
```

Boolean operators are `and`, `or`, `not`. `match/case` is a good fit for dispatch. Use guard clauses (`if not valid; return`) to avoid deep nesting. Loops support `break` and `continue`.

## Strings and collection building

```toi
message = f"hello {name}"
formatted = f"{value|.2f}"
upper = text.upper()
fields = text.split(",")
combined = ", ".join(fields)
head = text[..5]
tail = items[2..]
```

Interpolation uses `f"...{expr}..."`; format specifiers follow `|`. Slice ranges use `..`, including negative bounds from the end. For larger generated strings, append pieces to a table and join once (`"".join(parts)` or `table.concat(parts, "")`). Prefer common string methods over manual character loops.

## Useful operators and idioms

| Intent | Toi |
|---|---|
| Append to array portion | `items <+ value` |
| Value containment | `items has value` or `text has part` |
| Length | `#items` |
| Inclusive range | `1..n` |
| Ternary | `condition ? yes : no` |
| Optional chaining | `value?.field`, `value?[key]` (verify when unfamiliar) |
| Metatable-only method dispatch | `obj::method(args)` |

Do not use `in` for general membership; it is for loop/import syntax. Do not use `has` to check a map key: use `map[key]` and handle a missing (`nil`) value as the surrounding API expects.

## Functions, errors, and methods

```toi
fn split_pair(text)
  parts = text.split(":")
  return parts[1], parts[2]

try
  risky_operation()
except err
  print err
```

Functions can return multiple values and accept defaults or named arguments. Method calls such as `text.trim()` are normal lookup; `obj::method(...)` explicitly requests metatable-only dispatch. Use `nil, err` for expected library failures when that is the established API; use `error(...)`/`throw` for unrecoverable errors.

## Refactoring checklist

1. Read the target code and nearby callers/tests first; infer compatibility constraints.
2. Look for manual list pushes, index-only loops, string concatenation loops, long dispatch chains, and verbose conditionals that current idioms can simplify.
3. Prefer table comprehensions, `for value in table`, `match/case`, string methods, `..` slices, interpolation, and guard clauses where they make intent clearer.
4. Keep explicit index loops when indices matter; keep manual code when it is clearer or materially faster.
5. Do not add defensive `type(...)` wrappers or helper layers without a real boundary that needs them.
6. Verify uncertain syntax from current parser behavior and repo examples; run the relevant `.toi` test after changes.
7. Report behavior changes separately from syntax modernization. Avoid unrelated redesign.
