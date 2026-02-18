#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

build_one() {
  local grammar_dir="$1"
  local wasm_name="$2"

  pushd "$grammar_dir" >/dev/null
  tree-sitter generate
  tree-sitter build --wasm
  popd >/dev/null

  cp "$grammar_dir/tree-sitter-${wasm_name}.wasm" "$ROOT/editor/zed-extension-pua/grammars/${wasm_name}.wasm"
}

build_one "$ROOT/editor/treesitter" "pua"
build_one "$ROOT/editor/treesitter-ptml" "ptml"

echo "Updated wasm grammars in editor/zed-extension-pua/grammars"
