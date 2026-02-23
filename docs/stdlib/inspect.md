# `inspect` Module

Import:

```toi
inspect = import inspect
```

## Functions

- `inspect.signature(fn_or_native) -> table`

## Signature Fields

Observed fields include:

- `kind` (`"function"` or `"native"`)
- `name`
- `arity`
- `variadic`
- `defaults_count`
- `params` (array of per-parameter tables)

Parameter entries can include:

- `index`
- `name`
- `type`
- `has_default`
- `variadic`
