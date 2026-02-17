# Tree-sitter source for Pua

This directory is the minimal grammar source needed for development.

Included:
- `grammar.js`
- `queries/highlights.scm`
- `package.json`

Intentionally not included:
- generated `src/*` files
- nested `.git` checkout

Regenerate parser files when needed:

```bash
cd editor/treesitter
tree-sitter generate
```
