# Pua Libraries (`lib/*.pua`)

These are standard helper modules implemented in Pua itself.

## `lib.test`

Testing helpers used by `tests/*.pua`:

- `assert_eq(actual, expected, [msg])`
- `assert_true(cond, [msg])`
- `expect_error(fn_)`
- `timeit(msg="timer", echo=false)`

## `lib.types`

Runtime type-validation/composition helpers.

Provided constructors and helpers include:

- Primitive type defs: `String`, `Number`, `Boolean`, `Integer`, `Any`
- Containers: `List(inner)`, `Optional(inner)`, `Record(schema, [opts])`
- Combinators: `Union(...)`, `Literal(...)`, `Constraints(inner, opts)`
- Validation/coercion helpers: `check(type_def, value)`, `coerce(type_def, value)`, `from_hint(name)`

See `tests/20_pua_libs.pua` for detailed usage.

## `lib.cli`

CLI application framework:

- `App(opts)`
- `app.command(...)` decorator/registration
- `app.help()`
- `app.run([argv])`
- `printc(text)` ANSI-color helper

## `lib.scheduler`

Cooperative scheduling utilities for coroutine-driven workflows.

## `lib.http_server`

Simple HTTP server framework composed from:

- `lib/http_server/__.pua`
- `lib/http_server/route.pua`
- `lib/http_server/response.pua`
- `lib/http_server/transport.pua`

Provides routing helpers and response generation on top of native modules.
