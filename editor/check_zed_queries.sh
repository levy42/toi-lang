#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SAMPLE="$ROOT/test.pua"
PTML_SAMPLE="$ROOT/examples/showcase.ptml"
REV="main"

TMP_DIR="$(mktemp -d /tmp/pua-zed-query-XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

if ! command -v tree-sitter >/dev/null 2>&1; then
  echo "tree-sitter CLI not found"
  exit 1
fi

status=0
prepare_grammar_from_rev() {
  local rel_path="$1"
  local out_dir="$2"
  local fallback_dir="${3:-}"
  mkdir -p "$out_dir"
  if git cat-file -e "$REV:$rel_path/grammar.js" 2>/dev/null; then
    git show "$REV:$rel_path/grammar.js" > "$out_dir/grammar.js"
  elif [[ -n "$fallback_dir" && -f "$fallback_dir/grammar.js" ]]; then
    cp "$fallback_dir/grammar.js" "$out_dir/grammar.js"
  else
    echo "Missing grammar.js for $rel_path"
    return 1
  fi
  cat > "$out_dir/package.json" <<'JSON'
{"name":"tree-sitter-temp","version":"0.0.0"}
JSON
  (cd "$out_dir" && tree-sitter generate >/dev/null)
}

run_query_check() {
  local grammar="$1"
  local query="$2"
  local sample="$3"
  if XDG_CACHE_HOME=/tmp tree-sitter query -p "$grammar" "$query" "$sample" >/dev/null 2>&1; then
    echo "OK   $query"
  else
    echo "FAIL $query"
    XDG_CACHE_HOME=/tmp tree-sitter query -p "$grammar" "$query" "$sample" || true
    status=1
  fi
}

PUA_GRAMMAR="$TMP_DIR/pua"
PTML_GRAMMAR="$TMP_DIR/ptml"
prepare_grammar_from_rev "editor/treesitter" "$PUA_GRAMMAR" "$ROOT/editor/treesitter"
prepare_grammar_from_rev "editor/treesitter-ptml" "$PTML_GRAMMAR" "$ROOT/editor/treesitter-ptml"

for q in \
  "$ROOT/editor/zed-extension-pua/languages/pua/highlights.scm" \
  "$ROOT/editor/zed-extension-pua/languages/pua/indents.scm" \
  "$ROOT/editor/zed-extension-pua/languages/pua/injections.scm" \
  "$ROOT/editor/zed-extension-pua/languages/pua/brackets.scm"
do
  [[ -f "$q" ]] && run_query_check "$PUA_GRAMMAR" "$q" "$SAMPLE"
done

for q in \
  "$ROOT/editor/zed-extension-pua/languages/ptml/highlights.scm" \
  "$ROOT/editor/zed-extension-pua/languages/ptml/injections.scm"
do
  [[ -f "$q" ]] && run_query_check "$PTML_GRAMMAR" "$q" "$PTML_SAMPLE"
done

exit $status
