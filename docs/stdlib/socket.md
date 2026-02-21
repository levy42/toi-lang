# `socket` Module

Import:

```pua
socket = import socket
```

## Module Functions

- `socket.tcp() -> sock`
- `socket.udp() -> sock`
- `socket.select(read_list, write_list, [timeout]) -> ready_read, ready_write`

## Socket Methods

- `sock.connect(host, port)`
- `sock.bind(host, port)`
- `sock.listen([backlog])`
- `sock.accept() -> client_sock|nil, err?`
- `sock.send(data) -> bytes_sent|nil, err?`
- `sock.recv([size]) -> data|nil, err?`
- `sock.settimeout(nil|seconds)`
- `sock.close()`
- `sock.getpeername() -> ip, port`
- `sock.getsockname() -> ip, port`
- `sock.fileno() -> fd|nil`

## `socket.select`

- `read_list` and `write_list` are tables (typically arrays) of socket userdata.
- Returns two tables: ready-to-read sockets, ready-to-write sockets.
