# Completion, shadow text, and language servers

yew has three completion tiers with explicit provenance:

1. the workspace symbol index, built incrementally from settled syntax;
2. a configured language server, when that server is ready;
3. an AI backend, only after the separate privacy opt-in.

The completion menu labels source and kind. Filtering is stable, preserves a
selected row by identity, and never turns row zero into an implicit choice.
Enter accepts an explicitly selected row; typing refilters; Escape dismisses.
Snippet syntax from an LSP is reduced to safe plain insertion text—there is no
snippet-expansion mode.

## Passive shadow suggestions

`shadow.providers = "index lsp ai"` fixes provider order. A suggestion is
owned bytes plus buffer generation, cursor position, and provider identity;
any stale generation or incompatible edit discards it. Alt-Right accepts a
word, Alt-Shift-Right its alternate word unit, Alt-Shift-Down a line, and
Alt-Enter all. Alt-[ and Alt-] cycle eligible providers.

Inline and multiline shadows use distinct theme attributes and never alter
the viewport merely to make a suggestion fit. Typing a matching prefix keeps
the remainder; deletion, selection, multi-cursor state, undo/redo, replay,
menu conflict, focus change, or buffer switch dismisses it.

The gutter marker identifies the suggestion's source: `·` for the symbol
index, `◇` for the language server, and `✦` for AI. It is not file content.
ASCII terminals use `.`, `>`, and `*`; ambiguous-wide terminals also fall
back where needed to keep the marker inside its one-cell slot. Provider
colors and dim/italic/underline styling remain distinct without color.

## Built-in language-server defaults

Servers start only for a non-binary buffer whose detected language has a
configuration. The root is the nearest matching marker, never an unrelated
parent found by a different buffer.

| Languages | Executable and arguments | Root markers (nearest first) |
|---|---|---|
| C, C++, Objective-C | `clangd` | `compile_commands.json`, `.clangd`, `compile_flags.txt`, `.git` |
| Rust | `rust-analyzer` | `Cargo.toml`, `.git` |
| Python | `pyright-langserver --stdio` | `pyproject.toml`, `setup.py`, `setup.cfg`, `.git` |
| Go | `gopls` | `go.mod`, `.git` |
| JavaScript/TypeScript/JSX/TSX | `typescript-language-server --stdio` | `tsconfig.json`, `package.json`, `.git` |
| Fortran (free or fixed) | `fortls` | `.fortls`, `.git` |
| shell | `bash-language-server start` | `.git` |
| Wolf | `wolf lsp` | `wolf.pkg`, `.git` |

User `lsp.servers` configuration replaces compiled defaults as one validated
set. A missing executable or failed initialize is reported against the
language that requested it; opening an unrelated binary or Wolf file cannot
start `fortls`.

## Protocol behavior

The JSON-RPC transport frames bytes on nonblocking pipes and enforces size,
pending-call, and deadline caps. Buffer versions and generations reject stale
responses. UTF-16 positions convert only at exact codepoint/grapheme
boundaries; inexact server ranges are ignored or refused before mutation.

Completion edits and workspace rename are transactional. Rename preflights
every URI, file identity, version, range, overlap, and workspace boundary
before changing the first buffer; a later commit failure rolls already
changed buffers back. Definitions, references, symbols, hover, signature,
highlights, diagnostics, and rename all use registered commands and the shared
picker/panel surfaces.
