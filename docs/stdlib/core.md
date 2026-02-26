# Core Built-ins

These are global functions available without importing a module.

## Conversion and Type

- `type(value) -> string`
- Generator-backed coroutines report as `"generator"` (other coroutines remain `"thread"`).
- `istype(value, type_name, ...) -> boolean` (true if `type(value)` matches any provided type string; aliases: `bool` -> `boolean`, `int`/`float` -> `number`)
- `bool(value) -> boolean`
- `int(value) -> number` (integer conversion)
- `float(value) -> number`
- `str(value) -> string` (alias to `string` module call behavior)

## Input/Output and Errors

- `print ...`
- `input(prompt=nil) -> string|nil`
- `error(message, type="Error") -> throws`
- `error(table_exception) -> throws` (rethrows/propagates the given table unchanged)
- `exit(code=0)`

## Iteration Helpers

- `next(state, key) -> next_key, value`
- `inext(table, index) -> next_index, value`
- `gen_next(thread, control) -> key, value` (generator helper)

## Range and Slice

- `range(stop)`
- `range(start, stop)`
- `range(start, stop, step)`
- `range_iter(state, control)`
- `slice(table_or_string, start, stop, step=1)`

## Numeric Helpers

- `min(...)`
- `max(...)`
- `sum(...)`
- `sum(table)`
- `divmod(a, b) -> {quotient, remainder}`

## Metatable Helpers

- `setmetatable(table, mt|nil) -> table`
- `getmetatable(table) -> table|nil`

## Runtime

- `mem() -> number` (bytes allocated)
