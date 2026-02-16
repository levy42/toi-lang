# Pua PyCharm Plugin

This plugin adds Pua language support to PyCharm/IntelliJ Platform:

- `.pua` file type registration
- syntax highlighting
- line comments (`--`)
- brace matching for `()`, `{}`, `[]`
- keyword completion
- basic formatter + code style settings (indent defaults: 2 spaces, tab width: 4)

## Development

From `editor/pycharm-pua`:

```bash
gradle wrapper
./gradlew runIde
```

If you already have a wrapper, run only:

```bash
./gradlew runIde
```

Then open any `.pua` file in the sandbox IDE.
