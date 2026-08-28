# Networking And Concurrency

This group covers sockets, HTTP helpers, readiness polling, threads, coroutines, and request-local context.

## `socket`

The low-level networking module.

Module entry points:

- `socket.tcp()`
- `socket.udp()`
- `socket.select(read, write, timeout=nil)`
- `socket.tls_available()`

Common socket methods:

- `connect`
- `bind`
- `listen`
- `accept`
- `send`
- `recv`
- `settimeout`
- `close`
- `fileno`
- `getpeername`
- `getsockname`
- `tls(...)`
- `tls_server(...)`

TLS support is optional and depends on an OpenSSL-enabled build.

## `http`

HTTP parsing and request helpers.

Common entry points:

- `http.parse(...)`
- `http.response(...)`
- `http.urldecode(...)`
- `http.parsequery(...)`
- `http.fetch(...)`
- `http.request(...)`

## `poll`

Readiness polling for file descriptors and sockets.

## `coroutine`

Coroutine helpers:

- `create`
- `resume`
- `yield`
- `sleep`
- `status`

## `thread`

OS-thread based concurrency:

- `spawn`
- `join`
- `yield`
- `sleep`
- `mutex`
- `channel`

This is the module to use for background workers, channels, and blocking concurrency.

## `context`

Small request- or task-local context storage:

- `set`
- `get`
- `exists` / `has`
- `unset` / `delete`
- `clear`
- `copy`
- `items`
- `keys`

## Typical Stack

For application code, the common layering is:

- `socket` and `poll` for low-level networking
- `http` for parsing and request helpers
- `thread` and `context` for concurrent servers
- `lib.http_server` for a higher-level web framework
