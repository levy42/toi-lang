# Types and Values

Runtime `type(x)` strings include:

- `nil`
- `boolean`
- `number`
- `string`
- `table`
- `function`
- `thread`
- `userdata`

## Truthiness

- `nil` is falsey.
- `false` is falsey.
- Other values are truthy.

## Tables

Tables combine array and hash behavior.

## Userdata and Metatables

Native modules expose userdata objects with metatable-driven methods (for example: `io` files, sockets, thread handles).

Common metamethod keys used in the runtime include:

- `__index`
- `__newindex`
- `__str`
- `__call`
- `__append`
- `__next`

Metatable-only member access is available with `::`:

- `obj::method(args...)` looks up `method` directly on `getmetatable(obj)`, not through `__index`.
- If the looked-up value is callable, `obj` is passed automatically as the first argument.
