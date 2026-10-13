# Sprint-File Authoring Brief

Binding rules for writing yew sprint files. Every sprint file must be
sufficient for an agent to implement the sprint **from the file alone**,
with `.docs/plan/*.md` as standing context.

## Required reading before writing a single line

1. `.docs/plan/00-decisions.md` — locked decisions + the 10 invariants
2. `.docs/plan/01-architecture.md` — module map, event loop, data model
3. `.docs/plan/02-fletch.md` — language requirements
4. `.docs/sprints/index.md` — the campaign map; your sprint entries there
   are the contract for scope and file names
5. Format exemplars (quality bar — match their density of signatures,
   tables, pitfalls, and numeric DoD items):
   - `~/GithubOrgs/tenseleyFlow/Cgfried/.docs/sprints/00-scaffolding/s00-repo-and-foundations.md`
   - `~/GithubOrgs/tenseleyFlow/Cgfried/.docs/sprints/04-ir/s17-ir-core.md`

## The five-heading skeleton (mandatory, exact order)

```markdown
# Sprint N: Title
## Prerequisites    — sprints + what they landed; binding plan docs
## Goals            — one paragraph; explicit "X lands in Sprint M" deferrals
## Deliverables     — numbered; exact file paths, function signatures,
                      reference tables, code blocks, pitfalls; final item is
                      usually "Defer: …" naming what must hard-error until
                      its sprint
## Testing Strategy — unit / script / pty / fuzz / perf / regression fixtures
## Definition of Done — checkable, numeric where possible
```

## Thoroughness bar

- Deliverables name **real paths** (from `01-architecture.md`'s layout) and
  **real C signatures** in code blocks.
- Include reference tables where the domain has them (escape sequences,
  key encodings, porcelain formats, opcode lists, Unicode edge cases,
  LSP method matrices…). The implementer must not need to research
  externally for the sprint's core path.
- Every deliverable that has a classic failure mode gets a **Pitfall:**
  note (like Cgfried's `-MMD -MP`, strict-aliasing, insertion-order notes).
- Sample code blocks for the trickiest 10–20% of each sprint.
- DoD items are mechanically checkable: exact commands, exact expected
  output, counts, budgets. 8–14 items typical.
- Length: 300–500 lines is typical; 650 is fine when the domain's
  reference tables genuinely demand it (Unicode rules, opcode listings,
  escape-sequence tables, porcelain formats). Never drop a required table
  to hit a line count, and never pad to reach one. Density is the target,
  not length — if a sentence does not change what the implementer types,
  cut it.

## Cross-sprint discipline

- You may assume ONLY what earlier sprints' index entries say they landed,
  plus the plan docs. If you need something an earlier sprint plausibly
  built but its entry doesn't name, state the assumption in Prerequisites.
- Anything out of scope hard-errors naming the sprint that lands it
  (invariant 3) — put these in the final "Defer:" deliverable.
- Never renumber sprints or rename files; fractional files (`s24_5-…`)
  are the pressure valve. Do not edit `index.md`.
- Tests land in the same sprint as the surface they cover.

## Shared symbol ledger (authoritative names; refine internals freely)

- Prefixes: `yew_` functions, `YEW_` macros/enums/env; Fletch subsystem
  `fl_` / `FL_`.
- Core types: `Arena`, `Vec(T)` macro, `Strmap` (insertion-ordered),
  `Interner`, `Bytebuf` (byte buffer), `yew_sort_stable`.
- Text: `TextBuf` (piece tree + line index + undo), `Piece`, `Span`
  (byte range `{u64 lo, hi;}`), `Mark`, `Cursor`, `UndoTree`.
- Coordinates (typed, never mixed): `ByteOff` (u64), `LineNo` (u64,
  0-based internal, 1-based display), `GCol` (grapheme col), `CCol`
  (cell col). Conversions only in `src/unicode/`.
- Terminal: `Tty`, `TtyCaps` (probe results: `kitty_kbd`, `kitty_flags`,
  `sync_output`, `truecolor`), `Key` (normalized event: `code`, `mods`,
  `text[]`; non-text keys use codes ≥ `YEW_KEY_BASE` = 0x110000), `Grid`,
  `Cell`, `Damage`.

### Landed APIs — cite these exact names, do not re-invent

Sprints already written pin these. If your sprint touches them, use the
name as spelled here; if you need a variant, add one, don't rename theirs.

- Unicode (Sprint 2, `src/unicode/`) — note the noun-first `_bytes`
  convention for flat-buffer entry points:
  `yew_utf8_decode(s, len, *out)`, `yew_utf8_decode_prev(s, start, pos, *out)`,
  `yew_gb_init/yew_gb_boundary` (incremental),
  `yew_gb_next_bytes/yew_gb_prev_bytes/yew_gb_count_bytes`,
  `yew_cp_width(cp)`, `yew_cluster_width(s, len)`, `yew_str_width(s, len, tabw)`,
  `yew_str_clip(s, len, max_cells, *out_cells)`, `yew_width_set_opts`.
  Invalid bytes ride as U+DC80–DCFF escapes **only** in the decoded stream;
  TAB returns −1 from `yew_cp_width` (caller resolves the tab stop).
- Text (Sprints 7–9): `TextBuf`, `Piece`, `Mark`, `Cursor` and their
  documented operations — read the sprint file before assuming a signature.
- Fletch origins/capabilities (declared Sprint 31, relocated to
  `src/fl/origin.h` by Sprint 34): `FlOriginKind` with `FL_ORIGIN_BUILTIN`
  (= 0), `FL_ORIGIN_CONFIG`, `FL_ORIGIN_WORKSPACE`, `FL_ORIGIN_PLUGIN`,
  `FL_ORIGIN_CLI`, `FL_ORIGIN_REPL`; `FlOrigin {u8 kind; u32 path_id; u32
  caps;}`; `fl_origin_register`, `fl_origin_of_frame`, `fl_cap_check`.
  **Never `YEW_ORIGIN_*` / `OriginKind`** — the ledger prefix is `FL_`.
  The enum *kind* and the registry *id* are different numbers: kind
  `FL_ORIGIN_BUILTIN` is 0, and registry **id 0 is reserved for the user
  config** (s54 depends on that). Conflating them grants a plugin the
  user config's capabilities.
- Undo transactions (Sprint 10 owns these; it is the authority):
  `yew_undo_begin(...)`, `yew_undo_end(...)`, `yew_undo_abort(...)`, with
  `YEW_TXN_*` kind tags. **Do not write `yew_txn_begin/commit/abort`** —
  s21 used that spelling before s10 landed and it is being corrected.
  All buffer mutation funnels through s10's `EditCtx` choke point, which is
  also where marks, cursors, the crash journal, and the syntax line-damage
  record are notified.
- UI: `Win` (view), `Pane`, `Tab` (`tab_id`, `group_id`, `group_ordinal`),
  `TabGroup` (id, label, dir_path, last_active_member — NO member list),
  `Region` (clickable).
- Editor: `Ed` (top-level state), `Workspace`, `Mode` enum
  `YEW_MODE_{L,W,B,H,I,E,F}`; named commands `"ed.*"` in one registry.
- Fletch: `FlVm`, `FlValue`, `FlChunk`, `FlGc`.
- Exit codes: 0 success · 1 error · 2 `--batch` script failure ·
  3 I/O error · 4 internal bug (`yew_bug()`, structured report, never a
  bare crash).
- Logging: `yew_log(level, fmt, …)` to `$XDG_STATE_HOME/yew/log`
  (never stdout/stderr while the TUI owns the terminal); `YEW_BUG(...)`
  wraps `yew_bug` with file:line.

## Style

- Tone and formatting mirror the Cgfried exemplars: terse, declarative,
  no marketing prose, "Why:" notes where a decision needs defending.
- Markdown only, 80-ish column wrap, tables where tables win.
- Comments-carry-the-why applies to sample code too.
