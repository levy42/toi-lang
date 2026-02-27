# `csv` Module

Import:

```toi
csv = import csv
```

## Functions

- `csv.parse(text, [delimiter]) -> rows`
- `csv.stringify(rows, [delimiter]) -> text`

## Notes

- Default delimiter is `","`.
- `delimiter` must be a single-character string.
- `csv.parse` supports quoted fields, escaped quotes (`""`), CRLF/LF line endings, and newlines inside quoted fields.
- `csv.stringify` expects `rows` as an array of row arrays; cell values must be `string`, `number`, `bool`, or `nil`.
