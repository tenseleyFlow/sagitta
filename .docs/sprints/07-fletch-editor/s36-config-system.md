# Sprint 36: The Config System — init.fl, Options, `bind()`, Reload

## Prerequisites

- Sprint 34 — `flapi.c` bindings, origins (`fl_origin_register`,
  `fl_origin_of_frame`, `fl_cap_check`), the **registration ledger** in `Ed`
  (`{u32 origin_id; RegKind kind; u32 handle;}`, `REG_HOOK` live,
  `REG_BIND`/`REG_OPTION`/`REG_CMD` defined), `fl_hook_*` and the
  `Ed.origin_mask` teardown mask, the `OptProvider` seam, `ed.opt.get/set`,
  spec amendment A2.
- Sprint 13 — `BindRow`, `yew_keymap_build` (tables are **frozen; mutation
  is rebuild**), `KeyStack` layer precedence (mode → user → plugin, top-down
  ownership), `yew_key_parse_seq`/`format_seq` and their round-trip law,
  the six build validations, `keys_default.c` (data only),
  `yew_keys_default_install`, `chord_timeout_ms`.
- Sprint 35 — the recorder and `ed.macro.*`; the default keymap must bind
  `q`/`@`/`@@`.
- Sprint 29 — `fl_parse_literal` (spec §12 pure-literal entry point; s29's
  DoD 7 already asserts it accepts the s25 workspace-state schema example
  verbatim). Sprints 30–33 — VM, stdlib, `yew fl`, conformance + the
  **config-load < 1 ms** bench gate.
- Sprint 25 — dir=workspace, `$XDG_STATE_HOME/yew/workspaces/<hash>/`,
  the **frozen** workspace-state schema and its hand-written emitter/parser.
  **Assumptions beyond its index entry** (s25's file is not yet written; the
  index pins scope): the emitter lives in `src/ws/state.c` behind
  `yew_state_save(Workspace*, const char *path)` /
  `yew_state_load(Workspace*, const char *path)`, the format is Fletch data
  literals with a top-level `schema:` integer, and s10's `undo: { file:
  "<hash>.sagu", version: 1 }` reference is one of its keys. If the landed
  names differ, adapt — the *format* is frozen and this sprint changes only
  the implementation behind it.
- Sprint 0 — `--clean` reserved (`s00` hard-errors naming this sprint), XDG
  helpers, atomic-write primitive (s08 tmp+fsync+rename+dir-fsync).
  Sprint 18 — `:set` reaches E mode through `YEW_COMP_OPTION` /
  `YEW_COMP_VALUE`, whose provider is finally non-empty here.
- Sprint 54 (forward contract, already written): trust db is
  `$XDG_STATE_HOME/yew/trust.fl`, pure literal, atomic write, versioned
  `schema:`; **schema 1 is this sprint's** (dirs only) and s54 adds
  `plugins:` as schema 2. s54 also depends on this sprint's ledger-based
  teardown being reusable per origin.
- Binding: `00-decisions.md` Config row, invariants 1, 3, 4, 5;
  `02-fletch.md` requirements 6 and 8.

## Goals

**MILESTONE: self-config** — yew is configured by its own language.
Land `src/fl/flconf.c|h` (load order, origins, reload), `src/edit/option.c|h`
(the typed option table that `set()` from Fletch and `:set` from E mode both
hit), `src/ws/trust.c|h` (the trusted-directory prompt and its persisted db
with move/change invalidation), `bind()`/`unbind()` onto s13's layers, and
`runtime/init.fl` — **the good default config**, which is also the executable
documentation of the default keymap and replaces s13's C default tables
except for a minimal panic keymap. Finally, `src/ws/state.c` swaps its
hand-written parser and emitter for the real Fletch pure-literal path with a
differential test proving the two implementations agree byte-for-byte on the
frozen schema. Plugin origins and their capability prompts are Sprint 54
(this sprint's ledger and trust db are what it extends); `--batch` config
handling is Sprint 37; macro UX is Sprint 38.

## Deliverables

### 1. Load order and origins — `src/fl/flconf.c`

| # | Source | Origin | Caps | Skipped by |
|---|---|---|---|---|
| 0 | **panic keymap** (C, `keys_default.c`) | — | — | never |
| 1 | `$YEW_RUNTIME_DIR/init.fl` (default `<prefix>/share/yew/runtime/init.fl`) | `FL_ORIGIN_BUILTIN` | all | `--clean` |
| 2 | `$XDG_CONFIG_HOME/yew/init.fl` (`~/.config` fallback) | `FL_ORIGIN_CONFIG` (**id 0**) | all | `--clean`, replaced by `--config PATH` |
| 3 | `<workspace>/.yew.fl` | `FL_ORIGIN_WORKSPACE` | all **after trust** | `--clean`, untrusted, `--no-workspace-config` |

```c
typedef enum { YEW_CFG_OK, YEW_CFG_MISSING, YEW_CFG_PARSE, YEW_CFG_RUN,
               YEW_CFG_UNTRUSTED } CfgStatus;
CfgStatus yew_config_load_all(Ed *ed, DiagCtx *dc);
CfgStatus yew_config_reload  (Ed *ed, DiagCtx *dc);   /* §5 */
```

- **The panic keymap** is what `keys_default.c` shrinks to: `<esc>`, `q`,
  `q !`, `ed.nop`, and `:` — enough to quit and to type a command. It is
  installed first, always, and every later layer stacks above it. Why it
  exists: if `init.fl` fails to parse, a user with a broken config must
  still be able to quit and edit the file. Vim's `-u NONE` as a permanent
  floor rather than a flag you have to know about.
- Missing file at rows 2/3 is **not** an error (`YEW_CFG_MISSING`, one
  `yew_log(INFO)` line). Missing row 1 **is** an error naming
  `YEW_RUNTIME_DIR` — the defaults are a shipped artifact, and silently
  running without them is the "why are my keys gone" bug (invariant 3).
- Each source is compiled and run as an s31 module under its origin, in
  order. A parse error in one source aborts **that source only**, reports
  with caret diagnostics, and continues with the next: a broken workspace
  config never costs a user their personal config.
- `--clean` loads row 0 only, and additionally forces an in-memory command
  history (s18) and skips workspace state. `yew --clean` is the bug-report
  reproduction command and is named as such in `--help`.

**Cold start.** Rows 1–3 are parsed and executed on the startup path before
first paint (20 ms budget). `02-fletch.md` requirement 7 and s33's gate cap
config load at **< 1 ms**; the shipped `runtime/init.fl` is measured against
that budget in this sprint's perf lane (DoD 11). Pitfall: `import`ing
modules from `init.fl` is legal but each import is a file read — the shipped
config imports nothing, and the budget is stated in its header comment so
the first person to add an import sees the cost.

### 2. The option table — `src/edit/option.h`, `option.c`

```c
typedef enum { YEW_OPT_BOOL, YEW_OPT_INT, YEW_OPT_STR, YEW_OPT_ENUM } OptType;
typedef enum { YEW_OPT_GLOBAL, YEW_OPT_BUFFER, YEW_OPT_WINDOW } OptScope;

typedef struct OptVal { u8 type; union { bool b; i64 i;
                        const char *s; u32 e; } v; } OptVal;

typedef struct OptDesc {
    const char *name;              /* "tabwidth", "undo.bytes_max"        */
    u8 type, scope;
    OptVal      dflt;
    const char *const *enums;      /* NUL-terminated list; ENUM only      */
    i64         imin, imax;        /* INT only; inclusive                 */
    bool      (*validate)(const OptVal *v, const char **err);  /* or NULL */
    void      (*on_change)(Ed *, const OptDesc *, const OptVal *old,
                           const OptVal *nu);                  /* or NULL */
    const char *help;              /* one line: :set completion, man page */
} OptDesc;

extern const OptDesc yew_opts[];  extern const u32 yew_opts_len;

bool yew_opt_get(Ed *, Buffer *, Win *, const char *n, u32 nl, OptVal *out);
bool yew_opt_set(Ed *, u8 scope_hint, const char *n, u32 nl,
                 const OptVal *v, const char **err);
u32  yew_opt_list(const char **out, u32 max);        /* declaration order */
```

**Resolution chain**: window override → buffer override → global value.
Overrides live in small `Strmap`s on `Win` and `Buffer`; the global values
live in one `OptVal[yew_opts_len]` array on `Ed`. An option declared
`YEW_OPT_GLOBAL` refuses a buffer/window set (`err` names the scope).

**One table, two front doors.** `set({...})` and `opt.set()` from Fletch and
`:set name value` from E mode both call `yew_opt_set`. Grep gate (DoD 5):
`grep -rn 'yew_opt_set' src/` hits only `option.c`, `flapi.c`, and
`cmdline.c`'s `:set` handler — nothing reads a config value out of a Fletch
map or a private cache. Why the teeth: an option with two writers has two
`on_change` stories, and the one used less often stops firing.

`set` is an ordinary **function** taking a map, not a statement:

```fletch
set({ tabwidth: 4, wrap: false, theme: "quiver-dark" })
```

Pinned, with the reason: spec §2's grammar has no `set_stmt`, and
`02-fletch.md`'s brace sketch predates the s28 freeze (which explicitly made
`{` at statement position an error). A map argument gives the same shape
with zero new grammar. `set(map)` applies keys in **insertion order** (spec
§4 pins insertion-ordered maps), so `on_change` order is deterministic;
an unknown key raises kind `"name"` with near-match suggestions and
**aborts the whole call** — a config typo must not half-apply.

The shipped option set (declaration order = `:set` completion order):

| Option | Type | Scope | Default | Owner sprint |
|---|---|---|---|---|
| `tabwidth` | int 1–16 | buffer | 4 | s02/s15 |
| `expandtab` | bool | buffer | false | s14 |
| `wrap` | bool | window | false | s15 |
| `scrolloff` | int 0–99 | window | 3 | s15 |
| `number` | enum `off\|abs\|rel\|both` | window | `both` | s15 |
| `statusline.column` | enum `gcol\|gcol_ccol` | global | `gcol` | s15 |
| `errorbells` | bool | global | false | s15 |
| `ambiguous_wide` | bool | global | false | s02 |
| `subword` | bool | buffer | false | s16 |
| `chord_timeout_ms` | int 0–5000 | global | 500 | s13 |
| `undo.break_on_newline` | bool | buffer | true | s10 |
| `undo.bytes_max` | int | buffer | 64 MiB | s10 |
| `undo.min_nodes` | int | buffer | 200 | s10 |
| `undo.persist_bytes_max` | int | global | 16 MiB | s10 |
| `registers.ring_depth` | int 0–256 | global | 32 | s12 |
| `registers.ring_bytes_max` | int | global | 8 MiB | s12 |
| `registers.clip_read_max` | int | global | 64 MiB | s12 |
| `clipboard.sync` | enum `off\|yank\|all\|unnamed` | global | `yank` | s12 |
| `search.ignorecase` | bool | global | false | s21 |
| `search.smartcase` | bool | global | true | s21 |
| `hooks.error_limit` | int 1–100 | global | 5 | s34 |
| `theme` | str | global | `"quiver-dark"` | s41 (validator accepts any name; unknown themes warn at s41) |
| `macro.dir` | str | global | `""` (→ XDG) | s38 |

Each row's default **must equal** the C constant the owning sprint shipped;
a unit test asserts equality for every row, so the table cannot silently
change behavior that earlier sprints' tests pinned.

`on_change` examples that make the table earn its keep: `tabwidth` and
`ambiguous_wide` set `full_damage` and invalidate width caches;
`chord_timeout_ms` rearms `yew_dispatch_deadline`; `undo.bytes_max` runs a
trim pass at the next commit; `clipboard.sync` re-detects the backend.
**Pitfall:** `on_change` runs *after* the value is stored and must not call
`yew_opt_set` on the same option — re-entrancy is guarded by a per-option
in-flight flag and the second set is refused with kind `"user"`.

### 3. `bind()` / `unbind()` — Fletch onto s13's layers

```fletch
bind("L", "g g", "ed.move.buf.home")            # command by name
bind("H", "s", fn() surround("(", ")"))         # a closure
bind("L", "space f", "ed.find.files", { count: 1 })
unbind("L", "g g")
```

```c
u32  yew_bind_add(Ed *ed, u32 origin, Mode m, const char *seq,
                  CmdId cmd, i64 iarg, const char *sarg, FlValue fn);
bool yew_bind_remove(Ed *ed, u32 ledger_id);
void yew_bind_rebuild(Ed *ed);        /* frozen tables: mutation = rebuild */
```

- Rows accumulate in a `Vec(BindRow)` per (layer, origin) and the layer is
  rebuilt via `yew_keymap_build` — s13's pinned design, so this is a change
  of *row source*, nothing else. Rebuild is deferred to the end of a config
  run (one rebuild per load/reload, not per `bind` call).
- **Closures need a command.** A bound `FlValue` closure is wrapped by an
  auto-registered command `ed.fl.closure` with the closure's ledger id in
  `iarg`. Why: s13's `Binding` holds a `CmdId`, and the one-dispatch-surface
  doctrine says a key must resolve to a registry command — so a Fletch
  closure becomes one instead of becoming an exception. Consequence worth
  stating: closures are dispatched, recordable (Sprint 35 emits the
  closure's *effects*, since the tap sees the commands it invokes), and
  visible to `:map`.
- Every `bind` appends `REG_BIND` to the ledger with the origin, so §5's
  teardown removes it. `unbind` on a sequence owned by a *different* origin
  is refused with a message naming the owner — a plugin cannot silently
  steal the user's binding (s54 depends on this).
- Mode names are the s13 `ModeDesc.name` strings (`"L" "W" "B" "H" "I" "E"
  "F"`), plus `"*"` meaning every unit mode (L/W/B). An unknown mode raises
  kind `"name"`.
- All six `yew_keymap_build` validations still apply and now report with
  **Fletch source positions**: `init.fl:41:5: error: <esc> may not be a
  chord prefix`. Pitfall: s13 pinned that `sarg` is arena-duplicated at
  build time and never aliased into the row — Fletch-owned strings are
  GC-managed and *will* move or die; duplicating is not optional here.

### 4. Workspace trust — `src/ws/trust.h`, `trust.c`

`<workspace>/.yew.fl` executes arbitrary code, so it is gated:

```
this directory ships a .yew.fl config:
  /home/u/proj/.yew.fl  (2.1 KiB, modified 3 days ago)
[t]rust always   [o]nce   [n]ever   [v]iew   [s]kip
```

- `view` opens the file in a scratch buffer and re-prompts on close. The
  prompt runs on s18's cmdline machinery inside the event loop — never a
  nested read loop (s54 states the same requirement for its prompts).
- Answers: `trust always` → persisted `trusted`; `once` → session only;
  `never` → persisted `denied` (never prompts again for this directory);
  `skip` → session only, prompts again next time.
- **No tty (Sprint 37 `--batch`, or stdin not a terminal): never prompt.**
  Untrusted → skipped with a stderr line. `--trust-workspace` pre-grants for
  scripts, documented with the batch docs.

Trust db — `$XDG_STATE_HOME/yew/trust.fl`, pure literal, atomic write,
keys sorted bytewise on write (determinism), unknown keys preserved on
rewrite (forward compatibility with s54's schema 2):

```fletch
# yew trust database — hand-editable; delete a line to be asked again.
{
  schema: 1,
  dirs: {
    "/home/u/proj": { state: "trusted", hash: "9f2ac1d4e07b5533",
                      dev: 66306, ino: 4213771, at: 1785600000 },
    "/home/u/junk": { state: "denied" },
  },
}
```

**Invalidation — the "directory moved or changed" story, pinned:**

| Change | Detected by | Action |
|---|---|---|
| `.yew.fl` content changed | `fnv64(bytes)` ≠ `hash` | **re-prompt**, message says "the config changed since you trusted it" |
| directory moved or renamed | the key is the `realpath`; no entry at the new path | prompt as new (the old entry ages out, §below) |
| same path, different filesystem object (bind-mount, restored backup, replaced checkout) | `dev`/`ino` mismatch | **re-prompt** |
| `.yew.fl` deleted | no file to load | entry retained, harmless |
| entry older than `trust.prune_days` (default 365) with no matching dir | on write | pruned |

The hash is over the file's bytes, not its mtime: touching a file must not
cost a prompt, and rewriting it with identical content is not a change.
The `dev`/`ino` check is what catches the genuinely dangerous case — a
directory you trusted replaced by a different one at the same path.

**Pitfall — trust is per realpath, and the realpath is resolved once.** A
symlinked workspace resolves to its target before lookup, so trusting
`~/work` (a symlink) trusts the target, and re-pointing the symlink at a
different directory does **not** carry the grant across. Stated in
`trust.h`'s header comment because the opposite behavior is the exploit.

`schema: 1` entries whose value is a **bare string** (`"trusted"`) are
accepted on read as `{state: <s>}` with no fingerprint, and are upgraded in
place on the next write by recording the current fingerprint — the grant was
already given; this only adds the invalidation data. That keeps s54's
abbreviated example valid.

### 5. Reload with atomic teardown — the machinery Sprint 54 reuses

```c
CfgStatus yew_config_reload(Ed *ed, DiagCtx *dc);   /* ed.config.reload */
void      yew_origin_teardown(Ed *ed, u32 origin);  /* the ledger walk  */
```

Reload, in order, and the order is the deliverable:

1. **Compile everything first.** Parse and compile rows 1–3 into chunks. A
   syntax error at this stage aborts the reload with diagnostics and the
   **old config stays live** — a typo must never leave a user with no
   keymap.
2. **Tear down**, origins builtin/user/workspace, each by
   `yew_origin_teardown`: set `Ed.origin_mask` for that origin (so hooks
   mid-teardown do not fire back into it — s34's mask), walk the ledger
   **in reverse** (LIFO: keymap layers must unstack in the order they
   stacked), removing `REG_BIND`, `REG_HOOK`, `REG_OPTION` (restores the
   default or the previously-set value), `REG_CMD`; drop the module root;
   run one GC cycle; in debug builds assert every registry's length
   returned to its pre-load size.
3. **Reset options** to defaults before re-running, so a removed `set()`
   call actually removes its effect. (Without this, deleting a line from
   `init.fl` and reloading keeps the old value — the single most confusing
   reload bug in every editor that has one.)
4. **Run** the chunks in load order. A *runtime* error in one source is
   contained: that source's origin is torn down again and the reload
   continues with the others, with a persistent error message.
5. One `yew_bind_rebuild`, one relayout, one full damage.

**Pitfall — ghosts.** Hooks and binds registered by the old config must be
removed *as a unit*. A reload that re-runs `init.fl` without tearing down
leaves the previous run's `on("buf.save", …)` alive, so the hook fires twice,
then three times, then the user reports "yew formats my file N times
where N is how often I reloaded". The ledger is the only registration path
(§3, s34 §6, and s54's `ctx.*`) precisely so that teardown is a loop and not
a checklist. The zero-residue test is the same shape s54 uses: snapshot
every registry length before load, reload 20 times, assert equality.

`ed.config.reload` and `ed.config.edit` (opens `init.fl`) are registered
here; the default keymap binds neither by default (a chord for a rare
operation is a wasted key) — both are E-mode commands.

### 6. `runtime/init.fl` — the good default config

The shipped default, and simultaneously the executable documentation of the
default keymap. Style per spec §15 (4-space indent, `snake_case`, `#`
comment sentences). Structure, complete:

| Section | Contents |
|---|---|
| header | version comment, the < 1 ms budget note, "this file is documentation you can edit" |
| 1. options | one `set({…})` call per concern group, every non-default choice commented with *why* |
| 2. unit-mode keymap | L/W/B tables — motions, counts, mode entry |
| 3. I mode | insert, delete, Esc, literal tab |
| 4. H mode | selection, lift to multi-cursor, operators |
| 5. global chords | `space`-led leader: files, buffers, tabs, groups, search |
| 6. macros | `q`/`@`/`@@` (Sprint 35), `Q` for the browser (Sprint 38) |
| 7. hooks | two exemplary hooks, both commented as examples to delete |
| 8. functions | `surround`, `dup_line`, `sort_block` — the composability demo |

```fletch
# yew default configuration.  This file IS the keymap: every default
# binding below is real, and copying a line into your own init.fl and
# changing it is the whole customization story.
#
# Budget: this file is parsed and run before the first paint and must stay
# under 1 ms (see Sprint 33's config-load gate).  It imports nothing on
# purpose — every import is a file read on the startup path.

# -- 1. options ---------------------------------------------------------
set({
    tabwidth: 4, expandtab: false,       # bytes are sacred: no auto-retab
    wrap: false, scrolloff: 3, number: "both",
    clipboard.sync: "yank",              # yanks reach the system clipboard;
                                         # deletes do not clobber it
    search.ignorecase: false, search.smartcase: true,
    chord_timeout_ms: 500,
    theme: "quiver-dark",
})

# -- 2. unit modes (L line, W word, B block) ----------------------------
# The arrows are the vocabulary the recorder emits (fletch-spec §3).
for m in ["L", "W", "B"] {
    bind(m, "<right>", "ed.move.unit.next")
    bind(m, "<left>",  "ed.move.unit.prev")
    bind(m, "<up>",    "ed.move.unit.up")
    bind(m, "<down>",  "ed.move.unit.down")
    bind(m, "A-<right>", "ed.move.unit.next.alt")   # WORD / grow
    bind(m, "A-<left>",  "ed.move.unit.prev.alt")
    bind(m, "l", "ed.mode.enter", { sarg: "L" })
    bind(m, "w", "ed.mode.enter", { sarg: "W" })
    bind(m, "b", "ed.mode.enter", { sarg: "B" })
    bind(m, "i", "ed.mode.enter", { sarg: "I" })
    bind(m, "h", "ed.mode.enter", { sarg: "H" })
    bind(m, ":", "ed.mode.enter", { sarg: "E" })
    bind(m, "x", "ed.edit.delete.unit")
    bind(m, "y", "ed.edit.yank")
    bind(m, "p", "ed.edit.paste")
    bind(m, "u", "ed.edit.undo")
    bind(m, "C-r", "ed.edit.redo")
    bind(m, "/", "ed.search.forward")
    bind(m, "n", "ed.search.next")
    bind(m, "g g", "ed.move.buf.home")
    bind(m, "g e", "ed.move.buf.end")
    bind(m, "z z", "ed.view.center")
}
bind("L", "<esc>", "ed.mode.escape")
bind("L", "q",     "ed.quit")
bind("L", "q !",   "ed.quit_force")

# -- 3. I mode ----------------------------------------------------------
bind("I", "<esc>", "ed.mode.escape")
bind("I", "<bs>",  "ed.edit.delete.prev")
bind("I", "<tab>", "ed.edit.insert.tab")

# -- 4. H mode (selection) ----------------------------------------------
bind("H", "<esc>", "ed.mode.escape")
bind("H", "y", "ed.edit.yank")
bind("H", "d", "ed.edit.delete.sel")
bind("H", "c", "ed.cursor.lift")          # selection -> multi-cursor
bind("H", "s", fn() surround("(", ")"))   # a closure is a first-class bind

# -- 5. leader chords ---------------------------------------------------
bind("*", "<space> f", "ed.find.files")
bind("*", "<space> b", "ed.find.buffers")
bind("*", "<space> t", "ed.tab.new")
bind("*", "<space> g", "ed.group.picker")
bind("*", "<space> w", "ed.file.save")

# -- 6. macros (Sprint 35) ----------------------------------------------
bind("*", "q", "ed.macro.record")         # q<reg> starts, q stops
bind("*", "@", "ed.macro.replay")
bind("*", "@ @", "ed.macro.replay_last")

# -- 7. hooks (examples — delete these two lines to turn them off) ------
on("buf.saved", fn(b) msg("wrote " + b.path()))
on("mode.enter", fn(m) if m == "I" { set({ number: "abs" }) })

# -- 8. functions: the terse layer and the structured layer compose -----
fn surround(open, close) {
    for c in win.cursors() {
        let s = c.span() or c.word()
        edit { s.prepend(open); s.append(close) }   # one undo step
    }
}

macro dup_line = @[ l< H(lv) yank v paste esc ]

fn sort_block() {
    @[ b^ H(bv) ]
    let s = win.selection()
    edit { s.replace_lines(list.sort(s.lines())) }
}
```

Pinned constraints on this file, each mechanically checked:

- It must express **every row** of the default binding tables s13/s14/s16/
  s17 shipped in C; a test diffs the rebuilt layers against those tables and
  fails on any missing or extra binding. That is what makes it documentation
  rather than a plausible-looking sample.
- Every option it sets must equal the table default in §2 (so the file
  documents the defaults without changing them); a test asserts it.
- It must pass the spec §15 style rules and parse with zero diagnostics.
- It must run under the < 1 ms budget.

### 7. Workspace state on the real Fletch path — `src/ws/state.c`

s25 froze the *format* and hand-wrote the implementation. This sprint swaps
the implementation:

```c
/* read: pure literal only — grants nothing, runs nothing (spec §12) */
bool yew_state_load(Workspace *ws, const char *path, DiagCtx *dc);
/* write: canonical pure-literal emitter, shared with trust.c and s54 */
void fl_data_write(Bytebuf *out, const FlValue v, u32 indent);
FlValue fl_data_read(FlVm *vm, const char *src, size_t len, DiagCtx *dc);
```

- Reading goes through `fl_parse_literal` (s29) then a struct mapper; the
  hand-written recursive-descent reader is deleted.
- Writing goes through `fl_data_write`, the canonical emitter: 2-space
  indent, one entry per line, bare identifier keys where they match
  `[a-z_][a-z0-9_]*`, quoted otherwise, insertion order preserved (spec §4),
  no trailing commas at the top level, `\xNN`/`\u{…}` escapes per spec §1.
  Deterministic by construction — the same state emits the same bytes.
- `fl_data_write`/`fl_data_read` become the single data-literal path for
  workspace state, the trust db (§4), and — per its Prerequisites — s54's
  plugin manifests.

**Differential test** (`tests/unit/test_state_diff.c`), the sprint's proof
that "s36 swaps the implementation, not the format":

- the s25 emitter is retained as `src/ws/state_legacy.c`, compiled **only**
  into the unit-test binary behind `YEW_STATE_LEGACY`;
- for 500 generated `Workspace` states (0–200 tabs, deferred tabs, nested
  pane trees to depth 8, groups, cursor sets to 8 cursors, jumplists,
  paths containing spaces/quotes/newlines/CJK/invalid UTF-8, the s10 undo
  reference), assert `emit_legacy(s)` and `fl_data_write(s)` are
  **byte-identical**, and that both parsers produce structurally equal
  states from either output (a 2×2 matrix, so a bug in one emitter cannot
  hide behind the matching parser);
- plus the fixed corpus of s25's committed state files, which must load and
  re-emit byte-identically (round-trip on real data, not just generated).

Pitfall: the legacy emitter is not a reference implementation forever — it
is deleted in Sprint 58's audit pass, and `state_legacy.c` carries a comment
saying so with the sprint number, so it does not become permanent furniture.

### 8. Defer

- Plugin origins, `ctx.*` registration, capability prompts, trust db
  **schema 2** → **Sprint 54**; the ledger, `yew_origin_teardown`, and the
  trust db are built here to be reused verbatim.
- `--batch` interaction with config (which sources load, `--trust-workspace`
  enforcement, never-prompt) → **Sprint 37**; this sprint implements the
  no-tty branch and 37 wires the flag.
- Macro library autoload from `macro.dir` → **Sprint 38** (the option
  exists; nothing reads it yet — `yew_log` says so at startup once).
- Theme *contents* and validation of `theme` values → **Sprint 41**; the
  option accepts any string and warns for unknown names there.
- `:set` UI polish, option value enumerator completion → s18's surface,
  fed by `yew_opt_list` and `OptDesc.enums` from now on.
- Per-language/per-filetype option overrides (`autocmd`-shaped config) →
  express them today with `on("buf.open", …)`; a declarative form is
  post-1.0 and is pinned as *not* a 1.0 feature so nobody half-builds it.

## Testing Strategy

- **Unit** (`test_option.c`): every row of §2's table — default equals the
  owning sprint's C constant; type/range/enum rejection; scope refusal;
  window→buffer→global resolution; `on_change` fires once with correct
  old/new; re-entrant `yew_opt_set` from `on_change` refused; `set(map)`
  applying in insertion order and aborting whole on an unknown key.
- **Unit** (`test_trust.c`): all five invalidation rows; bare-string entry
  upgraded in place; symlinked workspace resolving to its target; sorted
  write; unknown top-level key preserved across a rewrite; atomic write
  proven by a truncated-file fixture failing to load without clobbering.
- **Unit** (`test_state_diff.c`): §7's 500-state 2×2 matrix plus the s25
  corpus.
- **Unit** (`test_bind.c`): closure binds dispatch and appear in `:map`;
  `unbind` across origins refused; all six `yew_keymap_build` validations
  reported with Fletch source positions; one rebuild per load regardless of
  bind count.
- **Script** (`tests/script/config_*.fl`): load order and precedence (a
  workspace config overriding a user option, and a user bind shadowed by
  nothing); parse error in one source leaving the others live; **reload
  zero-residue** — snapshot registry lengths, reload 20×, assert equality
  and that hooks fire exactly once; deleting a `set()` line and reloading
  restores the default; runtime error during reload contained.
- **Golden**: `runtime/init.fl` rebuilt layers diffed against the s13/s14/
  s16/s17 C tables — zero missing, zero extra bindings; its `set()` values
  diffed against §2 defaults.
- **Pty**: the trust prompt (all five answers), a broken `init.fl` showing
  the caret diagnostic and leaving the panic keymap live enough to quit,
  `:set tabwidth 8` visibly retabbing.
- **Fuzz**: `fl_data_read` over a corpus of mutated state and trust files
  (truncated, deep nesting, huge strings, non-UTF-8 keys) — no crash, no
  hang, every failure a diagnostic.
- **Perf**: `runtime/init.fl` parse+run measured on the CI runner with a
  committed baseline; config reload ≤ 5 ms with 500 binds.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/
   determinism lanes clean.
2. **Self-config milestone**: `keys_default.c` contains only the panic
   keymap (≤ 6 rows); every other default binding comes from
   `runtime/init.fl`, proven by the golden diff against s13/s14/s16/s17's
   tables (zero missing, zero extra).
3. `runtime/init.fl` parses with zero diagnostics, passes spec §15 style
   checks, imports nothing, and its parse+run is **< 1 ms** on the CI runner
   with a committed baseline.
4. Every §2 option row: default equals the owning sprint's C constant
   (asserted), validator fires, scope enforced, `on_change` tested.
5. Grep gate: `grep -rn 'yew_opt_set' src/` hits only `option.c`,
   `flapi.c`, and the `:set` handler; a seeded fourth call site fails
   `make test`.
6. `:set tabwidth 4` and `set({tabwidth: 4})` are observably identical
   (same table entry, same `on_change`, asserted by a script test).
7. Trust: all five invalidation rows fire; a moved directory re-prompts; a
   same-path/different-inode directory re-prompts; a content change
   re-prompts; `never` never prompts again; the db round-trips sorted with
   unknown keys preserved; no prompt is ever issued without a tty.
8. **Reload zero-residue**: registry lengths equal across 20 reloads; hooks
   fire exactly once after any number of reloads; a deleted `set()` line
   restores the default; a parse error leaves the previous config live and
   fully functional (script-tested by binding a key, breaking the file,
   reloading, and pressing the key).
9. Differential test green: 500 generated states + the s25 corpus,
   byte-identical emitters and cross-parsed matrix.
10. `--clean` loads only the panic keymap, skips workspace state and the
    workspace config, and is documented in `--help`; `yew --clean` no longer
    hard-errors naming this sprint.
11. Perf: config load < 1 ms; reload with 500 binds ≤ 5 ms; baselines
    committed.
12. Deferred surfaces hard-error naming their sprint: macro library → 38,
    plugin config → 54, `--batch` config flags → 37.
13. ≥ 180 assertions across the four unit files; ≥ 20 script tests; pty
    goldens byte-stable across the determinism lane's double run.
