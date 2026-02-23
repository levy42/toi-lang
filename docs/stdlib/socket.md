# `socket` Module

Import:

```toi
socket = import socket
```

## Module Functions

- `socket.tcp() -> sock`
- `socket.udp() -> sock`
- `socket.select(read_list, write_list, [timeout]) -> ready_read, ready_write`
- `socket.tls_available() -> bool`

## Socket Methods

- `sock.connect(host, port)`
- `sock.bind(host, port)`
- `sock.listen([backlog])`
- `sock.accept() -> client_sock|nil, err?`
- `sock.send(data) -> bytes_sent|nil, err?`
- `sock.recv([size]) -> data|nil, err?`
- `sock.settimeout(nil|seconds)`
- `sock.tls([servername], [verify=false]) -> true|nil, err?` (client handshake)
- `sock.tls_server(cert_path, key_path) -> true|nil, err?` (server handshake)
- `sock.close()`
- `sock.getpeername() -> ip, port`
- `sock.getsockname() -> ip, port`
- `sock.fileno() -> fd|nil`

## `socket.select`

- `read_list` and `write_list` are tables (typically arrays) of socket userdata.
- Returns two tables: ready-to-read sockets, ready-to-write sockets.

## TLS Notes

- TLS support is optional and enabled when Toi is built with OpenSSL available via `pkg-config`.
- `socket.tls_available()` returns whether TLS is compiled in.
- Once TLS is enabled on a socket, `sock.send`/`sock.recv` use TLS records transparently.
