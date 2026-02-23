# REPL

Start interactive mode:

```bash
./toi
```

## Behavior

- Multi-line input is supported.
- Press Enter on an empty line to submit an open block.
- Last expression result is printed automatically (when applicable).
- Syntax highlighting and completion are enabled.

## Completion

Completion includes:

- Keywords
- Built-in globals
- Global identifiers in current VM state
- Table/userdata member completion after `obj.`

## Interrupts

`Ctrl+C` interrupts current execution and returns to prompt.
