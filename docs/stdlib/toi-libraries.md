# Toi Libraries (`lib/*.toi`)

These are standard helper modules implemented in Toi itself.

## `lib.test`

Testing helpers used by `tests/*.toi`:

- `assert_eq(actual, expected, [msg])`
- `assert_true(cond, [msg])`
- `expect_error(fn_)`
- `timeit(msg="timer", echo=false)`

## `lib.types`

Runtime type-validation/composition helpers.

Provided constructors and helpers include:

- Primitive type defs: `String`, `Number`, `Boolean`, `Integer`
- Containers: `List(inner)`, `Optional(inner)`, `Record(schema, [opts])`
- Combinators: `Union(...)`, `Literal(...)`, `Constraints(inner, opts)`
- Validation/coercion helpers: `validate(type_def, value)`, `coerce(type_def, value)`, `from_hint(name)`

Primitive types are now callable with constraint/default metadata, which is sugar for wrapping with `Constraints(...)`.

Examples:

```toi
types = import lib.types

short_name_t = types.String(max_len = 30, pattern = "^[A-Z]")
adult_t = types.Integer(min = 18, max = 120)

profile_t = types.Record({
  name = types.String(default = "Anonymous"),
  age = types.Integer(default = 18)
})
```

When a record field is missing, `Record.check(...)` applies the field's `default` (value or function) before validation.

See `tests/20_toi_libs.toi` and `tests/70_types_options_defaults.toi` for detailed usage.

## `lib.cli`

CLI application framework:

- `App(opts)`
- `app.command(...)` decorator/registration
- `app.help()`
- `app.run([argv])`
- `printc(text)` ANSI-color helper
- `status(msg, [opts])` spinner-style status line helper
- `progress(total, [opts])` single-line progress bar helper
  - status and progress auto-spawn background updates by default when `thread` is available.
  - status still supports manual `tick()` and explicit `start_auto([interval])`/`stop(...)` control.
  - both expose context-manager hooks `__enter/__exit` for `with` blocks.

## `lib.scheduler`

Cooperative scheduling utilities for coroutine-driven workflows.

## `lib.path`

POSIX-style path helpers for common string path manipulation.

API:

- `path.join(a, b)`
- `path.split(path)` (returns non-empty segments)
- `path.dirname(path)`
- `path.basename(path)`
- `path.normalize(path)` (collapses duplicate `/`, trims trailing `/` except root)
- `path.is_abs(path)`

## `lib.selector`

Small readiness selector helper over native `poll` (with `socket.select` fallback).

Primary API:

- `selector.new() -> sel`
- `selector.add_read(sel, sock)`
- `selector.add_write(sel, sock)`
- `selector.remove(sel, sock)`
- `selector.clear(sel)`
- `selector.wait(sel, [timeout_seconds]) -> ready_read, ready_write`

## `lib.log`

Minimal logging toolkit implemented in Toi.

Main API:

- `log.Logger(name="root", opts={...})`
- `log.get_logger(name="root")`
- `log.configure(level=nil, handlers=nil, formatter=nil, context=nil)` (named args)
- `log.trace/debug/info/warn/error(msg, [fields])` (root logger shortcuts)
- `log.log(level, msg, [fields])`

Options:

- `level` (`"trace"|"debug"|"info"|"warn"|"error"` or numeric)
- `handlers` (array of `fn(record, line)` callbacks)
- `formatter` (`fn(record) -> string`)
- `context` (table merged into all log records)

Built-in handlers:

- `log.console_handler(record, line)` (prints line)
- `log.file_handler(path, [append=true]) -> handler`

## `lib.http_server`

Simple HTTP server framework composed from:

- `lib/http_server/__.toi`
- `lib/http_server/route.toi`
- `lib/http_server/response.toi`
- `lib/http_server/transport.toi`

Provides routing helpers and response generation on top of native modules.

Constructor:

- `http_server(opts={...})` (also exported as callable module)
- Backward-compatible: `http_server.new(port, [handler])`, `http_server.app(port)`

Common options:

- `host` (default `"0.0.0.0"`)
- `port` (default `8080`)
- `handler` (custom request handler function)
- `ssl` (`true` to enable TLS)
- `ssl_cert` / `ssl_key` (required when `ssl=true`)
- certificate/key aliases: `cert` or `cert_path`, `key` or `key_path`
- `worker_threads`, `worker_select_timeout`, `worker_queue_capacity`
- `stop_grace_seconds`
- `gc_every_requests`, `log_every_requests`, `trim_after_gc`

Routing and lifecycle methods:

- `app.get(path)`, `app.post(path)`, `app.put(path)`, `app.patch(path)`, `app.delete(path)`
- `app.route(method, path)` (decorator-style)
- `app.serve_dir(dir_path, path, [gzip=false])` (static files)
- `app.run()`
- `app.stop([grace_sec])`
- `app.is_running()`

HTTPS example:

```toi
http_server = import lib.http_server

app = http_server(
  host = "127.0.0.1",
  port = 8443,
  ssl = true,
  ssl_cert = "tests/fixtures/tls/server.crt",
  ssl_key = "tests/fixtures/tls/server.key"
)

get = app.get
@get("/")
fn root(req)
  return "ok over tls"

app.run()
```

Notes:

- TLS requires a build with OpenSSL support (`socket.tls_available()`).
- In TLS mode, accepted client sockets are handshaked with `sock.tls_server(...)` before request handling.

## `lib.loadtest`

Minimal HTTP/HTTPS load tester focused on requests-per-second.

File:

- `lib/loadtest.toi`

CLI command:

- `rps(url, duration_sec=10, concurrency=10, timeout_ms=2000, verify_tls=false)`

Example:

```bash
./toi lib/loadtest.toi rps http://127.0.0.1:8080/ 10 20 2000 false
```

Behavior notes:

- Uses plain TCP for `http://` and `socket.tls(...)` for `https://`.
- Classifies `2xx`/`3xx` responses as `ok`; everything else increments `fail`.
- Uses `thread` workers when available; otherwise falls back to concurrency `1`.
