# `http` Module

Import:

```pua
http = import http
```

## Functions

- `http.parse(raw_request) -> table|nil`
  - returns a table with keys like `method`, `path`, `version`, `headers`, optional `query`, optional `body`.
- `http.response(status, headers_table, body_string) -> string`
- `http.urldecode(str) -> string`
- `http.parsequery(str) -> table`
