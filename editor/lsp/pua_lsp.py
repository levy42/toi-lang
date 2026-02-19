#!/usr/bin/env python3
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from urllib.parse import unquote, urlparse

KEYWORDS = [
    "if", "elif", "else", "while", "for", "in", "break", "continue", "yield",
    "with", "as", "try", "except", "finally", "local", "global", "import", "from",
    "fn", "return", "del", "throw", "print", "gc", "and", "or", "not", "has",
    "true", "false", "nil",
]

HEADER_RE = re.compile(r"^\s*(fn\b|if\b|elif\b|else\b|while\b|for\b|with\b|try\b|except\b|finally\b)")
FUNC_RE = re.compile(r"^\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
ASSIGN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")
WORD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass
class Symbol:
    name: str
    line: int
    col: int
    kind: str  # function | variable


def uri_to_path(uri: str) -> str:
    parsed = urlparse(uri)
    if parsed.scheme == "file":
        return unquote(parsed.path)
    return uri


def path_to_uri(path: str) -> str:
    return Path(path).resolve().as_uri()


def pos_to_offset(text: str, line: int, character: int) -> int:
    lines = text.splitlines(keepends=True)
    if line >= len(lines):
        return len(text)
    return min(sum(len(lines[i]) for i in range(line)) + character, len(text))


def word_at_position(text: str, line: int, character: int) -> Optional[str]:
    offset = pos_to_offset(text, line, character)
    for m in WORD_RE.finditer(text):
        if m.start() <= offset <= m.end():
            return m.group(0)
    return None


def collect_symbols(text: str) -> List[Symbol]:
    out: List[Symbol] = []
    seen = set()
    for i, line in enumerate(text.splitlines()):
        m = FUNC_RE.match(line)
        if m:
            name = m.group(1)
            col = line.find(name)
            key = (name, "function")
            if key not in seen:
                out.append(Symbol(name=name, line=i, col=col, kind="function"))
                seen.add(key)
            continue

        m = ASSIGN_RE.match(line)
        if m:
            name = m.group(1)
            col = line.find(name)
            key = (name, "variable")
            if key not in seen:
                out.append(Symbol(name=name, line=i, col=col, kind="variable"))
                seen.add(key)
    return out


def format_pua(text: str) -> str:
    def update_multiline_string_state(s: str, in_ml: bool) -> bool:
        i = 0
        n = len(s)
        while i + 1 < n:
            pair = s[i : i + 2]
            if not in_ml and pair == "[[":
                in_ml = True
                i += 2
                continue
            if in_ml and pair == "]]":
                in_ml = False
                i += 2
                continue
            i += 1
        return in_ml

    out: List[str] = []
    indent_stack: List[int] = [0]
    in_multiline_string = False
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if in_multiline_string:
            out.append(line)
            in_multiline_string = update_multiline_string_state(line, in_multiline_string)
            continue

        stripped = line.lstrip()
        if not stripped:
            out.append("")
            continue

        col = 0
        for ch in line:
            if ch == " ":
                col += 1
            elif ch == "\t":
                col += 4
            else:
                break

        if col > indent_stack[-1]:
            indent_stack.append(col)
        elif col < indent_stack[-1]:
            while len(indent_stack) > 1 and col < indent_stack[-1]:
                indent_stack.pop()
        normalized = ("  " * (len(indent_stack) - 1)) + stripped
        out.append(normalized)
        in_multiline_string = update_multiline_string_state(normalized, in_multiline_string)

    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


def build_diagnostics(text: str) -> List[dict]:
    diagnostics: List[dict] = []
    lines = text.splitlines()

    # Basic indentation checks.
    for i, line in enumerate(lines):
        if "\t" in line:
            diagnostics.append(
                {
                    "range": {
                        "start": {"line": i, "character": 0},
                        "end": {"line": i, "character": max(1, len(line))},
                    },
                    "severity": 2,
                    "source": "pua-lsp",
                    "message": "Tab indentation detected; use 2 spaces.",
                }
            )
            continue

        stripped = line.lstrip(" ")
        if not stripped:
            continue
        indent = len(line) - len(stripped)
        if indent % 2 != 0:
            diagnostics.append(
                {
                    "range": {
                        "start": {"line": i, "character": 0},
                        "end": {"line": i, "character": indent},
                    },
                    "severity": 2,
                    "source": "pua-lsp",
                    "message": "Indentation should be a multiple of 2 spaces.",
                }
            )

    # Header lines should be followed by a more-indented code line.
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if not HEADER_RE.match(stripped):
            continue
        curr_indent = len(line) - len(stripped)
        j = i + 1
        found = False
        while j < len(lines):
            nxt = lines[j]
            nxt_stripped = nxt.lstrip()
            if not nxt_stripped:
                j += 1
                continue
            found = True
            nxt_indent = len(nxt) - len(nxt_stripped)
            if nxt_indent <= curr_indent:
                diagnostics.append(
                    {
                        "range": {
                            "start": {"line": i, "character": 0},
                            "end": {"line": i, "character": len(line)},
                        },
                        "severity": 1,
                        "source": "pua-lsp",
                        "message": "Expected an indented block after this header.",
                    }
                )
            break
        if not found:
            diagnostics.append(
                {
                    "range": {
                        "start": {"line": i, "character": 0},
                        "end": {"line": i, "character": len(line)},
                    },
                    "severity": 1,
                    "source": "pua-lsp",
                    "message": "Header without following block.",
                }
            )

    # Bracket balance check.
    pairs = {")": "(", "]": "[", "}": "{"}
    opening = set(pairs.values())
    stack: List[Tuple[str, int, int]] = []
    in_string: Optional[str] = None
    escape = False

    for ln, line in enumerate(lines):
        for ch_i, ch in enumerate(line):
            if in_string is not None:
                if escape:
                    escape = False
                    continue
                if ch == "\\":
                    escape = True
                    continue
                if ch == in_string:
                    in_string = None
                continue

            if ch in ('"', "'"):
                in_string = ch
                continue

            if ch in opening:
                stack.append((ch, ln, ch_i))
            elif ch in pairs:
                if not stack or stack[-1][0] != pairs[ch]:
                    diagnostics.append(
                        {
                            "range": {
                                "start": {"line": ln, "character": ch_i},
                                "end": {"line": ln, "character": ch_i + 1},
                            },
                            "severity": 1,
                            "source": "pua-lsp",
                            "message": f"Unmatched closing '{ch}'.",
                        }
                    )
                else:
                    stack.pop()

    for ch, ln, ch_i in stack:
        diagnostics.append(
            {
                "range": {
                    "start": {"line": ln, "character": ch_i},
                    "end": {"line": ln, "character": ch_i + 1},
                },
                "severity": 1,
                "source": "pua-lsp",
                "message": f"Unclosed '{ch}'.",
            }
        )

    return diagnostics


class LspServer:
    def __init__(self) -> None:
        self.docs: Dict[str, str] = {}
        self.running = True
        self.shutdown_requested = False
        self.workspace_roots: List[str] = []
        self.workspace_symbols: Dict[str, List[Tuple[str, Symbol]]] = {}

    def read_message(self) -> Optional[dict]:
        headers: Dict[str, str] = {}
        while True:
            line = sys.stdin.buffer.readline()
            if not line:
                return None
            line = line.decode("utf-8", errors="replace").strip("\r\n")
            if not line:
                break
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()

        length = int(headers.get("content-length", "0"))
        if length <= 0:
            return None
        body = sys.stdin.buffer.read(length)
        return json.loads(body.decode("utf-8", errors="replace"))

    def send(self, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        sys.stdout.buffer.write(header)
        sys.stdout.buffer.write(body)
        sys.stdout.buffer.flush()

    def respond(self, msg_id, result=None, error=None) -> None:
        payload = {"jsonrpc": "2.0", "id": msg_id}
        if error is not None:
            payload["error"] = error
        else:
            payload["result"] = result
        self.send(payload)

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def publish_diagnostics(self, uri: str) -> None:
        text = self.docs.get(uri)
        if text is None:
            return
        self.notify(
            "textDocument/publishDiagnostics",
            {"uri": uri, "diagnostics": build_diagnostics(text)},
        )

    def _index_text(self, uri: str, text: str) -> None:
        # Drop old refs for this uri.
        for name in list(self.workspace_symbols.keys()):
            entries = [e for e in self.workspace_symbols[name] if e[0] != uri]
            if entries:
                self.workspace_symbols[name] = entries
            else:
                del self.workspace_symbols[name]

        for sym in collect_symbols(text):
            self.workspace_symbols.setdefault(sym.name, []).append((uri, sym))

    def refresh_workspace_index(self) -> None:
        # Start from open docs.
        self.workspace_symbols.clear()
        for uri, text in self.docs.items():
            self._index_text(uri, text)

        # Add files from workspace roots.
        for root in self.workspace_roots:
            root_path = Path(root)
            if not root_path.exists() or not root_path.is_dir():
                continue
            for p in root_path.rglob("*.pua"):
                try:
                    uri = path_to_uri(str(p))
                    if uri in self.docs:
                        continue
                    text = p.read_text(encoding="utf-8", errors="replace")
                    self._index_text(uri, text)
                except Exception:
                    continue

    def update_doc(self, uri: str, text: str) -> None:
        self.docs[uri] = text
        self._index_text(uri, text)
        self.publish_diagnostics(uri)

    def find_symbol_location(self, current_uri: str, name: str) -> Optional[dict]:
        entries = self.workspace_symbols.get(name, [])
        if not entries:
            return None

        # Prefer current file, then workspace order.
        entries = sorted(entries, key=lambda e: (0 if e[0] == current_uri else 1, e[0]))
        uri, sym = entries[0]
        return {
            "uri": uri,
            "range": {
                "start": {"line": sym.line, "character": sym.col},
                "end": {"line": sym.line, "character": sym.col + len(sym.name)},
            },
        }

    def handle_request(self, method: str, params: dict):
        if method == "initialize":
            init_opts = params or {}
            workspace_folders = init_opts.get("workspaceFolders") or []
            root_uri = init_opts.get("rootUri")
            self.workspace_roots = []
            for wf in workspace_folders:
                uri = wf.get("uri")
                if uri:
                    self.workspace_roots.append(uri_to_path(uri))
            if not self.workspace_roots and root_uri:
                self.workspace_roots.append(uri_to_path(root_uri))
            self.refresh_workspace_index()

            return {
                "capabilities": {
                    "textDocumentSync": {
                        "openClose": True,
                        "change": 1,
                        "save": {"includeText": True},
                    },
                    "completionProvider": {"resolveProvider": False},
                    "definitionProvider": True,
                    "documentSymbolProvider": True,
                    "documentFormattingProvider": True,
                },
                "serverInfo": {"name": "pua-lsp", "version": "0.2.0"},
            }

        if method == "shutdown":
            self.shutdown_requested = True
            return None

        if method == "textDocument/completion":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            items = []
            for kw in KEYWORDS:
                items.append({"label": kw, "kind": 14})

            seen = set()
            for sym_name, entries in self.workspace_symbols.items():
                if not entries:
                    continue
                kind = 6 if any(e[1].kind == "function" for e in entries) else 6
                if sym_name not in seen:
                    seen.add(sym_name)
                    items.append({"label": sym_name, "kind": kind})

            # Prefer local symbols by adding them once more with sortText.
            text = self.docs.get(uri, "")
            for sym in collect_symbols(text):
                items.append({"label": sym.name, "kind": 6, "sortText": f"0_{sym.name}"})

            return {"isIncomplete": False, "items": items}

        if method == "textDocument/definition":
            text_doc = params.get("textDocument", {})
            pos = params.get("position", {})
            uri = text_doc.get("uri", "")
            text = self.docs.get(uri, "")
            sym = word_at_position(text, int(pos.get("line", 0)), int(pos.get("character", 0)))
            if not sym:
                return None
            return self.find_symbol_location(uri, sym)

        if method == "textDocument/documentSymbol":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            text = self.docs.get(uri, "")
            symbols = []
            for sym in collect_symbols(text):
                kind = 12 if sym.kind == "function" else 13
                symbols.append(
                    {
                        "name": sym.name,
                        "kind": kind,
                        "range": {
                            "start": {"line": sym.line, "character": 0},
                            "end": {"line": sym.line, "character": 10_000},
                        },
                        "selectionRange": {
                            "start": {"line": sym.line, "character": sym.col},
                            "end": {"line": sym.line, "character": sym.col + len(sym.name)},
                        },
                    }
                )
            return symbols

        if method == "textDocument/formatting":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            text = self.docs.get(uri, "")
            formatted = format_pua(text)
            line_count = max(0, len(text.splitlines()))
            return [
                {
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": line_count + 1, "character": 0},
                    },
                    "newText": formatted,
                }
            ]

        return None

    def handle_notification(self, method: str, params: dict) -> None:
        if method == "exit":
            self.running = False
            return

        if method == "initialized":
            return

        if method == "textDocument/didOpen":
            text_doc = params.get("textDocument", {})
            self.update_doc(text_doc.get("uri", ""), text_doc.get("text", ""))
            return

        if method == "textDocument/didChange":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            changes = params.get("contentChanges", [])
            if changes:
                self.update_doc(uri, changes[-1].get("text", self.docs.get(uri, "")))
            return

        if method == "textDocument/didSave":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            text = params.get("text")
            if text is not None:
                self.update_doc(uri, text)
            else:
                self.publish_diagnostics(uri)
            self.refresh_workspace_index()
            return

        if method == "textDocument/didClose":
            text_doc = params.get("textDocument", {})
            uri = text_doc.get("uri", "")
            self.docs.pop(uri, None)
            self.refresh_workspace_index()
            return

        if method == "workspace/didChangeWorkspaceFolders":
            event = params.get("event", {})
            for added in event.get("added", []):
                uri = added.get("uri")
                if uri:
                    self.workspace_roots.append(uri_to_path(uri))
            for removed in event.get("removed", []):
                uri = removed.get("uri")
                if uri:
                    path = uri_to_path(uri)
                    self.workspace_roots = [r for r in self.workspace_roots if r != path]
            self.refresh_workspace_index()
            return

    def run(self) -> int:
        while self.running:
            msg = self.read_message()
            if msg is None:
                break
            method = msg.get("method")
            params = msg.get("params", {})
            msg_id = msg.get("id")
            if method is None:
                continue
            try:
                if msg_id is not None:
                    result = self.handle_request(method, params)
                    self.respond(msg_id, result=result)
                else:
                    self.handle_notification(method, params)
            except Exception as exc:
                if msg_id is not None:
                    self.respond(msg_id, error={"code": -32603, "message": str(exc)})
        return 0 if self.shutdown_requested else 1


def main() -> int:
    return LspServer().run()


if __name__ == "__main__":
    raise SystemExit(main())
