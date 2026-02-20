# Getting Started

## Build

```bash
make
make release
make test
```

## Run

```bash
./pua
./pua path/to/script.pua
```

## Format Source

```bash
./pua fmt file.pua
./pua fmt -w file.pua
./pua fmt --check file.pua
```

## Hello World

```pua
print "hello, pua"
```

## Quick Example

```pua
math = import math

fn hyp(a, b)
  return math.sqrt(a * a + b * b)

print hyp(3, 4)
```

## Testing Convention

Project tests are executable `.pua` programs in `tests/`.

```bash
make test
```
