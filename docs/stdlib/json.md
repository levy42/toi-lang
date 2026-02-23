# `json` Module

Import:

```toi
json = import json
```

## Functions

- `json.encode(value) -> string`
- `json.decode(string) -> value`

## Notes

- Encodes Toi tables as JSON arrays or objects depending on shape.
- Decoder raises runtime errors for invalid JSON.
