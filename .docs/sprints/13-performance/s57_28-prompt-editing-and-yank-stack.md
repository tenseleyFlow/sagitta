# Sprint 57.28: Prompt Editing and the Yank Stack

## Prerequisites

- **The Insert-mode readline commands** (`src/edit/readline_cmds.[ch]`):
  `ed.edit.kill.word_prev|word_next|to_home|to_end|yank`,
  `ed.edit.transpose.chars|words`, `ed.edit.case.upper_word|lower_word|
  cap_word`. Aggregate commands over every cursor. Two lines matter here:
  `rl_kill_apply` writes the kill with `yew_reg_delete(&cx->ed->regs, …)`
  (`readline_cmds.c:175`) and the yank reads the unnamed register with
  `yew_reg_get(…, '"')` (`:292`).
- **The prompt is a Win.** `yew_cmdline_target(ed)` is a one-line `Win`
  with its own `TextBuf` and cursor set; commands dispatched while the
  prompt is active receive it as `cx->win` (`src/edit/ed.c:1845`). The
  prompt's own delete commands (`yew_cmdline_cmd_delete_word_prev` etc.,
  `src/ui/cmdline.c:1552–1590`) already operate on it that way. `Cursor`
  carries `anchor` (`src/text/cursor.h:62`).
- **Sprint 57.26** — the prompt ghost: `yew_cmdline_ghost`,
  `ed.cmdline.ghost.accept` (`<right>`), `ed.cmdline.ghost.accept_word`
  (`A-f`, `A-<right>`), the history snapshot.
- The register ring (`Registers.ring`, `yew_reg_ring_push/cycle`,
  `src/text/register.h:60–104`) — the thing the yank stack is NOT.
- Binding: invariants 1, 4, 5, 9, 10.

## Goals

The `:` / `:!` prompt has five readline keys (`C-a C-e C-w C-u C-k`); the
user reaches for all of them and for fish's. This sprint gives the prompt
the full readline editing set and a **yank stack**: a readline kill ring,
shared by the prompt and Insert mode, **fully separate** from yew's
registers and the system clipboard. Decisions already made with the user
(2026-09-23), not to be relitigated:

- Readline wins the three clashes. `C-y` = yank (redo moves to `A-/`,
  fish's key); `C-v` = paste (literal-next moves to `C-q`, readline's
  quoted-insert); insert-register gains `A-r` (and keeps `C-r` until 57.30
  gives `C-r` to history search).
- The yank stack is shared across the prompt and Insert mode, consecutive
  kills append (readline), `C-y` yanks, `A-y` cycles in place.
- Kills never write registers: `p` in normal mode does not paste a kill.

Campaign: 57.28 (this) → 57.29 shift-selection and clipboard (`C-c`
copy, `C-x` cut) → 57.30 history and table navigation (`C-r`, Up/Down
redesign, `C-p`/`C-n`) → 57.31 fish extras (`A-s`, `A-e`, `A-h`). 57.32
(cd-aware completion) runs in parallel. 57.27 stays reserved.

## Deliverables

### 1. The yank stack — `src/text/yankstack.h`, `src/text/yankstack.c`

```c
typedef enum { YEW_KILL_FORWARD, YEW_KILL_BACKWARD } YewKillDir;

typedef struct YewYankStack {
    Bytebuf entry[YEW_YANK_MAX];  /* YEW_YANK_MAX = 32; newest at head */
    u32 head, len;
    u64 bytes, bytes_max;         /* bytes_max = 1 MiB; oldest evicted  */
    u64 kill_seq;                 /* ed->cmd_seq of the last kill        */
} YewYankStack;

void yew_yank_init(YewYankStack *y);
void yew_yank_free(YewYankStack *y);
/* Record a kill.  `seq` is the dispatcher's sequence number of the
 * command doing it: when it is exactly one past `kill_seq` the kill
 * EXTENDS the newest entry -- appended for FORWARD, prepended for
 * BACKWARD, the way readline joins C-k C-k or M-DEL M-DEL. */
void yew_yank_kill(YewYankStack *y, const u8 *b, size_t n, YewKillDir d,
                   u64 seq);
/* Entry `k` back from the newest (0 = newest), or NULL. */
const Bytebuf *yew_yank_at(const YewYankStack *y, u32 k);
```

**Consecutive** means "the previous command the dispatcher ran was a kill
in the same Win". Implement it with a monotonic `u64 cmd_seq` that
`yew_ed_invoke` increments once per invoked command (it already sees every
command) and a `Win *` of the last kill: a motion, an insert, a Tab, a
mode change — anything — breaks the chain for free. Do not add a "clear
the flag" call to every command.

**Pitfall — multi-cursor kills.** A kill over N cursors is ONE entry (the
spans in document order, as `rl_kill_apply` concatenates them today) and
never extends a previous entry: joining a 3-cursor kill onto a 1-cursor
kill has no meaning.

**Session-only.** Not saved in workspace state; say so in the header.

### 2. Kills and yanks use the stack — `src/edit/readline_cmds.c`

- `rl_kill_apply` → `yew_yank_kill`, direction from the command
  (`word_prev`, `to_home` BACKWARD; `word_next`, `to_end` FORWARD), and
  **no** `yew_reg_delete`. Remove the header paragraph claiming kills feed
  the register ring; replace it with this sprint's rule.
- `ed.edit.kill.yank` inserts `yew_yank_at(0)` at every cursor.
- New `ed.edit.kill.yank_pop` (`A-y`): valid only when the previous
  command in this Win was a yank or yank-pop; replaces the text that yank
  inserted with the next older entry, wrapping. Track the inserted span(s)
  and the `cmd_seq` of the yank. Otherwise: message "A-y follows a yank"
  and do nothing. With more than one cursor, replace at every cursor from
  the recorded spans (they are the same text, so the spans are known).
- One undo step per kill, yank and yank-pop, as today.

### 3. Prompt bindings — `runtime/init.fl`

Verify, before binding each, that the target command works on the prompt
Win (drive it through `yew_ed_handle_key` in a test). Where an Insert-mode
command does not (e.g. it assumes a document `Buffer`), make it work on the
prompt rather than adding a prompt-only twin.

| Key | Command | Notes |
|---|---|---|
| `C-b` / `C-f` | char prev / next | `C-f` at the end with a ghost accepts the whole ghost (fish), same as `<right>` |
| `A-b` / `A-<left>` | word prev | |
| `A-f` / `A-<right>` | `ed.cmdline.ghost.accept_word` | **contextual**: with a ghost and the caret at the end, take one ghost word; otherwise word NEXT. Today it does nothing without a ghost |
| `C-<left>` / `C-<right>` | line home / end | synonyms of `C-a` / `C-e` (user's call) |
| `C-e` | line end | at the end with a ghost, accept the whole ghost (fish) |
| `C-d` | delete grapheme forward | never cancels the prompt |
| `C-h` | delete grapheme left | terminals that send `^H` for Backspace |
| `C-w` | **unix-word-rubout**: kill back to the previous WHITESPACE | bash semantics; `a/b/c` goes whole. The current `ed.del.word_prev` (word-char based) moves to `A-<bs>` |
| `A-<bs>` | `ed.edit.kill.word_prev` | word-char based |
| `A-d` / `A-<del>` | `ed.edit.kill.word_next` | |
| `C-u` / `C-k` | `ed.edit.kill.to_home` / `to_end` | replace today's `ed.del.*` so they feed the stack |
| `C-t` / `A-t` | transpose chars / words | |
| `A-u` `A-l` `A-c` | upper / lower / capitalize word | |
| `C-y` / `A-y` | yank / yank-pop | `C-y` was redo |
| `C-_` `C-/` `C-z` | undo | `C-z` exists |
| `A-/` | redo | moved from `C-y` |
| `C-q` | literal-next | moved from `C-v` |
| `C-v` | `ed.clip.paste` | system clipboard; verify it lands in the prompt |
| `A-r` | insert register | `C-r` keeps it until 57.30 |
| `A-.` | last argument, §4 | |

`ed.del.word_prev`, `ed.del.to_home`, `ed.del.to_end`: once unbound,
delete them if nothing else references them (grep `runtime/`, `src/`,
tests); a registered command nobody can reach is dead weight in a
size-gated binary.

**Insert mode parity** in the same file: `A-<del>` → kill word next,
`A-y` → yank-pop. (`A-.` is prompt-only: Insert mode has no history.)

Mirror every binding change in `tests/unit/test_runtime_defaults.c` and
RECOUNT `yew_bind_active_count` by running the test.

### 4. Last argument — `A-.`, `src/ui/cmdline.c`

Inserts the last WORD of the previous history entry at the caret; each
further `A-.` (consecutive, via `cmd_seq`) replaces what the last one
inserted with the last word of the next OLDER entry. For a bang entry the
word is found with `yew_shctx_at` at the entry's end (so quotes and
escapes are honoured and the inserted text is the RAW word as typed); for
other entries, the last whitespace-delimited token. Entries whose last
word is empty are skipped. History source: the prompt's own history
(`line->history`); no shell history files.

### 5. Size

This adds code to a binary with ~2.4 KB of headroom on the full cap
(Amendment S57.26-A2), so the size lane WILL fail. **Do not touch
`tests/size/`**: the orchestrator runs the CI rebaseline (pinned ledgers
from CI's artifact, a `size: rebaseline` commit with `Baseline-reason:`
and `Baseline-delta:` lines, and a decisions amendment). Report your own
estimate of the added bytes (`size` on the new/changed `.o` files).

### 6. Defer

- `C-c` copy / `C-x` cut and shift-selection → 57.29.
- `C-r` history search, the Up/Down/Tab redesign, `C-p`/`C-n` → 57.30
  (`C-p`/`C-n` keep their completion meaning until then).
- `A-s`, `A-e`, `A-h` → 57.31.
- Numeric arguments (`A-3 A-.`, `A-2 C-w`): non-goal.

## Testing Strategy

- **Unit, the stack (`tests/unit/test_yankstack.c`)**: forward append and
  backward prepend across consecutive kills; a motion between kills starts
  a new entry; a multi-cursor kill never extends; eviction by count and by
  bytes; `yew_yank_at` bounds.
- **Unit, separation**: after `C-k` in Insert mode the unnamed register
  and the register ring are byte-identical to before; after `yy`/`dd` the
  yank stack is unchanged.
- **Unit, prompt keys** (`test_navkeys.c` / `test_modes.c` style, real keys
  through `yew_ed_handle_key` in E mode): every row of §3, including the
  contextual `A-f`/`A-<right>` with and without a ghost, `C-e`/`C-f`
  ghost acceptance, `C-w` whitespace semantics on `a/b/c`, `A-<bs>` word
  semantics, `C-y` then `A-y` then `A-y` wrapping, `A-y` without a yank
  (message, no change), `A-.` twice walking two entries.
- **Unit, Insert parity**: `A-<del>`, `A-y` in Insert mode; multi-cursor
  yank-pop.
- **Unit, undo**: each kill/yank/yank-pop is one undo step in the prompt.
- **PTY**: `s57_28_prompt_kill_and_yank`, `s57_28_prompt_yank_pop`,
  `s57_28_prompt_last_arg`, `s57_28_prompt_alt_arrow_contextual`.
- **Roundtrip / invariant 9**: new commands registered, motion words or
  non-recordable treatment as the existing patterns require.

## Definition of Done

1. Every §3 key does what its row says in the built editor's prompt, each
   proven by a real-key unit test.
2. The yank stack tests pass; kills leave registers and the clipboard
   byte-identical (asserted).
3. `C-y` yanks and `A-/` redoes in the prompt; `C-q` inserts the next key
   literally; `C-v` pastes the system clipboard (fakeclip in tests).
4. `A-.` walks older entries; `A-y` walks older kills; both wrap or stop
   as specified.
5. `yew_bind_active_count` recounted by running the test.
6. The four `s57_28_*` goldens pass; full `make test-pty` green.
7. gcc AND clang warning-free (`make CC=gcc-16 BUILD=build-gcc
   FAULTSHIM_ARCH_FLAGS=` builds everything, `-Werror=format-truncation`
   included — CI is Linux GCC, and Apple clang misses what GCC rejects);
   `MODULES=""` builds and passes; ASan/UBSan filtered runs clean.
8. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
9. `tests/size/` untouched; added-bytes estimate in the report.
