# Sprint 57.16: Insert-Mode Comfort — Indent and Pairs

## Prerequisites

- Sprint 10/11 — `yew_edit_insert` / `yew_edit_delete` (`src/text/edit.h`), the
  `EditCtx` by-value contract and `yew_ed_finish_edit` on EVERY exit path,
  `YewTxnReason` and the refcounted `yew_undo_begin` whose nested reason
  mismatch is a hard `YEW_BUG`.
- Sprint 16/17 — `UnitOps`, H mode, `yew_sel_cmd_indent` / `_dedent`.
- Sprint 40s — the syntax engine: `yew_syn_in_string_or_comment(const Buffer*,
  ByteOff)` (`src/edit/block.h:19`, impl `src/syn/synblock.c:347`) is
  SYNCHRONOUS and callable from the edit path; `yew_syn_stack_at`;
  `yew_block_match` (`src/edit/block.h:20`) for `()[]{}` only.
- Sprint 36 — the option table (`src/edit/option.c`) and its frozen-order tests.
- Binding: invariants 1 (no data loss), 2 (no byte confusion), 4, 5, 9.

## Goals

Typing in yew is deliberately literal: Enter inserts the bare EOL and nothing
else, Tab inserts one `'\t'`, and no bracket or quote is ever completed for
you. Every comparable editor carries indent and closes pairs, and the absence
is felt on every line of code written in it.

There is also a latent defect to close: **`expandtab` is declared in the
option table with the help text "Insert spaces when indentation emits a tab"
and has ZERO consumers anywhere in `src/`.** It is a switch wired to nothing.
Either it starts working here or it is removed; this sprint makes it work.

Deferred, named: re-indent on paste; a language-aware `indentexpr`; format-on-
save; surround-with-pair over a selection; auto-close in the command line.

## Deliverables

### 1. The indent vocabulary — `src/edit/indent.[ch]` (new)

One module owning every "what is this line's indentation" question, so the
four private near-duplicates the survey found (`src/text/register.c:572`,
`src/edit/block.c:145`, `src/edit/motion.c:108`, `src/edit/sel_actions.c:713`)
stop multiplying.

```c
typedef struct IndentInfo {
    ByteOff first;   /* first non-blank byte; line end when blank */
    CCol    width;   /* indent WIDTH in cells, tab-expanded */
    bool    blank;   /* line is entirely whitespace */
    bool    tabs;    /* leading whitespace begins with '\t' */
} IndentInfo;

bool yew_indent_info(const TextBuf *tb, Span line, u32 tabwidth,
                     IndentInfo *out);
/* Bytes one indent level emits, honouring expandtab and tabwidth. */
u32  yew_indent_unit(const Buffer *b, u8 *out, u32 cap);
/* Where one Backspace inside leading whitespace should land. */
ByteOff yew_indent_back(const TextBuf *tb, Span line, u32 tabwidth,
                        ByteOff from);
```

`yew_indent_info` takes the ASCII fast path first and falls back to grapheme
iteration exactly as `line_info` in `block.c` does; that static may be deleted
in favour of this, but ONLY if `tests/unit/test_block.c`'s hand-computed level
fixtures stay byte-identical.

**`expandtab` acquires its meaning here and nowhere else**: `yew_indent_unit`
emits `tabwidth` spaces when set, one `'\t'` when clear. Every indent-emitting
site calls it.

### 2. Auto-indent on Enter — `src/edit/edit_cmds.c`

New buffer-scope option `autoindent`, `YEW_OPT_BOOL`, default **true**.

`yew_edit_cmd_insert_newline` gains, when `autoindent` is on:

| Situation | Result |
|---|---|
| ordinary line | new line carries the previous line's indent verbatim |
| previous line's last non-blank byte is an opener `{`, `[`, `(` | carries indent **plus one level** |
| caret sits directly between a matched pair, e.g. `{|}` | THREE lines: opener line, an indented empty line with the caret, and the closer on its own line at the opener's indent |
| the line being left is now blank | its whitespace is removed, so no trailing blanks are committed |

**Byte and undo discipline.** The EOL and the indent must be ONE
`yew_edit_insert` of one contiguous payload, not two calls. The survey found
that two inserts at non-contiguous offsets produce two undo ops
(`test_undo_explicit_type_keeps_noncontiguous_insert_ops`), and the newline
path is already forced through its own `YEW_TXN_TYPE` by
`undo_break_on_newline`. The three-line brace case is still one insert: build
the whole payload, then place the caret.

**The test that will break, and must be updated deliberately:**
`test_edit_insert_newline_uses_crlf_bytes` pins the exact post-Enter text,
cursor offset AND damage window. With `autoindent` defaulting true its fixture
changes. Keep a variant with `autoindent` off asserting today's bytes exactly,
so the literal path stays pinned.

### 3. Tab navigates existing indent — `src/edit/edit_cmds.c`

`yew_edit_cmd_insert_tab` becomes indent-aware.

| Caret position | Tab does |
|---|---|
| anywhere inside the leading whitespace (before the first non-blank) | MOVES the caret to the first non-blank. No edit, no undo entry. |
| at the first non-blank | indents the LINE by one level (insert at line start), caret keeps its position relative to the text |
| past the first non-blank | inserts one indent unit at the caret (`yew_indent_unit`) |
| line is entirely blank | inserts one indent unit |

A pure caret move must not open an undo transaction. The command is
registered `YEW_CMD_CHANGES_BUFFER`, which makes `yew_ed_invoke` open one
unconditionally; the navigate case must leave it empty so `yew_undo_end`
commits nothing, or the command must be re-shaped. State which you chose and
why in the header.

`Backspace` inside leading whitespace deletes to the previous indent stop via
`yew_indent_back`, one level rather than one byte. Outside leading whitespace
it is unchanged.

### 4. Auto-closing pairs — `src/edit/pairs.[ch]` (new)

New buffer-scope option `autopair`, `YEW_OPT_BOOL`, default **true**.

Typing an opener inserts the pair and places the caret between them. Typing a
closer when the very next byte is that closer and it was auto-inserted
ADVANCES over it instead of inserting. Backspace between a fresh empty pair
deletes both.

**The memory of what was auto-closed is the hard part, and it is per-buffer
state, not a heuristic.** Keep a small stack on the buffer:

```c
enum { YEW_PAIR_STACK_MAX = 32 };
typedef struct PairMark { Mark close; u8 closer; } PairMark;
```

Each entry holds a **Mark**, not an offset — marks are adjusted by the edit
chokepoint (`yew_marks_adjust`), so the remembered closer survives arbitrary
typing, deletion and multi-cursor edits between the delimiters. That is what
makes "type between the pair, then type the closer" still skip rather than
duplicate. An offset would go stale on the first insert.

Entries are dropped when: the mark is deleted, the caret leaves the pair's
span, the stack overflows (oldest first), or the buffer is reloaded. Undo does
not restore the stack — say so in the header; a restored pair that no longer
knows it was auto-closed merely behaves literally, which is safe.

**Syntax awareness.** Before inserting a pair, ask
`yew_syn_in_string_or_comment(buf, off)`. Inside a string or comment, do not
pair. The survey confirms this is synchronous and answers correctly for the
caret's own line immediately after the user's own edit, and FAILS OPEN
(returns false) when the settle wave has not reached the line or the buffer
has no language. Fail-open is the right default: pairing in unsettled text is
a cosmetic annoyance, refusing to type is not.

**Cost.** That query heap-allocates a 4096-span array and re-lexes the line
per call. Calling it on every keystroke is not acceptable. Call it only when
the typed byte is actually an opener or closer in the table, and cache the
answer for the caret's line invalidated by any edit to that line.

**The pair table is per language, and its home is a decision this sprint
makes.** `SynLangDesc` already carries a `SynComment` that NOTHING consumes —
the precedent exists and is inert. Adding a `pairs` sibling touches
`parse_language`'s key whitelist (unknown keys are rejected), the cache blob,
`YEW_SYN_TABLE_VERSION`, `scripts/gen-langtab`, the regenerated
`src/syn/langs_gen.c` (`scripts/check-syn-assets.sh` fails on staleness), and
potentially all 48 `runtime/syntax/*.fl`. Either do that properly or ship a
built-in default table keyed by language id with a documented follow-up. Pick
one, justify it, and do not half-do the schema change.

Default pairs: `()`, `[]`, `{}`, `""`, `''`, and `` `` `` where the language
uses it. Quote pairs only auto-close when the caret is not immediately after a
word character, so `don't` does not become `don''t`.

### 5. Options and keyboard reach

Three new buffer-scope options: `autoindent` (bool, true), `autopair` (bool,
true), and `expandtab` acquires consumers. Every one must be added to
`runtime/init.fl`'s `set` block with its default, to `test_option.c`'s frozen
name list, AND to the second copy in `test_fl_runtime.c`. No new bindings, so
`test_runtime_defaults.c`'s binding count stays 230 — verify rather than
assume.

Any new `YEW_CMD_RECORDABLE` command must be added to `tests/roundtrip/gen.c`'s
`gen_cmds[]` or given a named `D("name", "reason")` exclusion, or the
roundtrip coverage gate fails.

## Testing Strategy

- **Unit (`tests/unit/test_indent.c`, new)**: `yew_indent_info` over tabs,
  spaces, mixed, blank, CRLF, and non-ASCII leading whitespace;
  `yew_indent_unit` under both `expandtab` values; `yew_indent_back` stops.
- **Unit (`test_edit_cmds.c`)**: Enter carries indent; opener adds a level;
  `{|}` expands to three lines; the abandoned line loses its whitespace; with
  `autoindent` off every existing byte-exact assertion still holds; Tab
  navigates then indents then inserts per §3; Backspace eats a level; each
  case asserts the undo node count, not just the text.
- **Unit (`tests/unit/test_pairs.c`, new)**: insert pair and caret placement;
  type-over; type-over still works after editing between the delimiters (the
  Mark test); no pairing inside a string or comment; fail-open on an unsettled
  line; quote suppressed after a word character; stack overflow drops oldest;
  multi-cursor pairing is one MULTI transaction.
- **Unit (`test_option.c`, `test_fl_runtime.c`)**: both frozen lists updated.
- **PTY**: `s57_16_autoindent_block` (type a brace block, snapshot the shape),
  `s57_16_pair_typeover`, `s57_16_tab_navigates_indent`.
- **Perf**: a new case in the existing perf suite typing 1000 characters
  including openers and closers, asserting the syntax query is not called per
  ordinary character and the per-keystroke budget (invariant 4) holds.
- **Fuzz**: extend the text fuzz corpus so random typing with pairing on never
  corrupts the buffer, never leaves an unbalanced stack, and always round-trips
  through undo to the original bytes.

## Definition of Done

1. Enter carries indent, adds a level after an opener, expands `{|}` to three
   lines, and leaves no trailing whitespace on the line it left.
2. Tab inside leading whitespace moves the caret to the first non-blank and
   makes no edit; at the content it indents the line; past it, it inserts.
3. Backspace in leading whitespace removes one indent level.
4. Pairs auto-close, type-over works after arbitrary editing between the
   delimiters, and nothing pairs inside a string or comment on a settled line.
5. `expandtab` has consumers and changes what indentation emits.
6. With `autoindent` and `autopair` off, insert-mode behaviour is byte-identical
   to today, proven by the preserved assertions.
7. gcc and clang warning-free; unit, ASan/UBSan, PTY, fuzz, perf green;
   `MODULES=""` builds; roundtrip coverage gate green.
