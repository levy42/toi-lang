# `toml` Module

Import:

```toi
toml = import toml
```

## Functions

- `toml.parse(text) -> table`
- `toml.stringify(table) -> text`

## Notes

- Current implementation supports a practical TOML subset:
- Keys: bare keys (`a-z`, `A-Z`, digits, `_`, `-`) and quoted keys.
- Values: strings, numbers, booleans, arrays.
- Tables: `[section]` with dotted paths (`[server.tls]`), `[[array.of.tables]]`, plus dotted keys (`a.b = 1`).
- Inline tables: `{x = 1, y = 2}`.
- Datetime literals are parsed as strings (for example `1979-05-27T07:32:00Z`) and stringified back as bare literals when they match datetime syntax.
- Comments (`# ...`) are supported.
- `toml.stringify` currently supports table trees composed of scalar values and arrays.
