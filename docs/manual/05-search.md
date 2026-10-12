# Search, replace, and the jump list

`/` searches forward and `?` backward. The preview is incremental and
cooperative: literal scans yield in bounded slices so a large file cannot
freeze input. Escape restores the cursor, sticky goal, and viewport; Enter
accepts the pattern into the search register. `n` repeats in the accepted
direction and `N` reverses it.

`*` and `#` quote the word under the cursor and search forward or backward.
`search.ignorecase` defaults to false. `search.smartcase` defaults to true:
when ignoring case, an uppercase literal codepoint restores case sensitivity;
uppercase bytes inside escapes or character-class syntax do not.

## Regular expressions

yew's bespoke engine is leftmost-first and linear-time by design. It supports
literals, `.`, classes and negated classes, `^`/`$`, grouping, alternation,
greedy and lazy repetition, bounded repetition, Unicode properties, simple
case folding, and capture replacement. `^` and `$` are always line anchors;
on CRLF, `$` matches before the `\r`.

The Unicode shorthand definitions are fixed:

| Class | Meaning |
|---|---|
| `\d` | Unicode decimal number (`Nd`) |
| `\s` | Unicode White_Space property |
| `\w` | letters, marks, decimal numbers, connector punctuation, plus `_` |
| `\b` | a boundary between `\w` and `\W` |

There is no lookaround, backreference, recursion, normalization, or locale
folding. These are design boundaries, not partially implemented syntax. The
complete grammar and limits are in `yew(1)`; the `yew-regex(7)` name redirects
there.

## Replacement

`:substitute/PATTERN/REPLACEMENT/FLAGS` (abbreviation `:s`) defaults to the
current line. A range such as `:%s/old/new/g` selects a broader span. Captures
use `\1` through `\9`; `&` is literal rather than an alias for the whole
match. Unknown replacement escapes are errors.

The entire replacement run is one undo transaction, including confirm mode.
Zero-width matches always force grapheme-safe progress. `q` during confirmation
stops and commits the replacements already accepted; an error rolls the run
back.

## Jump and change history

Navigation that leaves the local context pushes the jump list. Ctrl-O walks
back and Ctrl-I forward. `g;` and `g,` walk older and newer change locations.
History stores buffer identity and grapheme-safe marks, so ordinary edits move
the remembered location rather than leaving a stale byte offset.
