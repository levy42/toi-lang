# `uuid` Module

Import:

```toi
uuid = import uuid
```

## Functions

- `uuid.uid() -> string` (length 16, alphanumeric)
- `uuid.secret() -> string` (default length 64)
- `uuid.secret(length) -> string` (`1..4096`)
