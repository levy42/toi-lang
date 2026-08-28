# Toi Libraries

These libraries are implemented in Toi and shipped under `lib.*`.

## Testing And Validation

### `lib.test`

Testing helpers used throughout `tests/*.toi`:

- `assert_eq(actual, expected, msg=nil)`
- `assert_true(cond, msg=nil)`
- `expect_error(fn_)`
- timing helpers such as `timeit(...)`

### `lib.types`

Runtime validation and schema composition:

- primitive types: `String`, `Number`, `Boolean`, `Integer`
- containers: `List`, `Optional`, `Record`
- combinators: `Union`, `Literal`, `Constraints`
- helpers: `validate`, `coerce`, `from_hint`

```toi
types = import lib.types

profile_t = types.Record({
  name = types.String(default = "Anonymous"),
  age = types.Integer(min = 18)
})
```

### `lib.records`

Helpers for record-like table shapes.

## Files, Paths, And Formatting

### `lib.path`

POSIX-style path manipulation:

- `join`
- `split`
- `dirname`
- `basename`
- `normalize`
- `is_abs`

### `lib.markdown`

Markdown-to-HTML rendering used by the docs toolchain.

### `lib.html`

HTML construction helpers.

### `lib.doc_builder`

Static documentation-site builder used by `tools/build_docs.toi`.

## CLI And Logging

### `lib.cli`

CLI application framework with decorator-style command registration:

- `App(opts)`
- `app.command(...)`
- `app.help()`
- `app.run([argv])`
- terminal helpers such as `printc`, `status`, and `progress`

### `lib.log`

Small logging toolkit with logger instances, levels, handlers, and formatters.

## Concurrency And Scheduling

### `lib.selector`

Readiness helper over `poll` or `socket.select`.

### `lib.scheduler`

Cooperative scheduling utilities for coroutine-driven workflows.

## Data And Application Libraries

### `lib.db`

Embedded table-oriented database helpers with typed records, indexes, filtering, and persistence.

### `lib.http_server`

HTTP server framework built on native socket, poll, thread, and HTTP helpers.

Common features:

- route decorators such as `@get(...)`
- static file serving
- TLS support when available
- threaded worker options

```toi
http_server = import lib.http_server

app = http_server(host = "127.0.0.1", port = 8080)
get = app.get

@get("/")
fn root(req)
  return "ok"

app.run()
```

### `lib.loadtest`

HTTP/HTTPS load testing utility focused on requests per second.

### `lib.settings`

Configuration helpers.

### `lib.admin`

Admin UI helpers on top of `lib.db` and `lib.http_server`.
