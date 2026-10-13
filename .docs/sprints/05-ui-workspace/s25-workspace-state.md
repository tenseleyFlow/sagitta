# Sprint 25: Workspace State — Dir Is the Workspace, Schema Frozen

## Prerequisites

- Sprint 22 — `Rect`, the pane tree (`Pane`: `is_leaf`, `dir`, `ratio`,
  `a`, `b`, `win`), `yew_layout_compute`, and **`yew_pane_tree_walk(root,
  fn, ctx)`**, exposed there precisely so this sprint serializes without
  reaching into node internals.
- Sprint 23 — `Tab` (`tab_id`, `buffer_id`, `path`, `root`, `focus`,
  `group_id`, `group_ordinal`), `Tabs`, `yew_tab_index_of_id`,
  `yew_tab_find_by_path`, `yew_tab_switch`, the 512-tab cap.
- Sprint 24 — `TabGroup` (`id`, `label`, `dir_path`, `last_active_member`;
  **no member list**), `yew_group_create/add_member/set_ordinal/members/
  prune_empty`, and lazy hydration (`yew_tab_is_resident/hydrate/defer`) —
  residency is asked of the allocation, never a flag.
- Sprint 21 — `JumpList` (per **window**), `ChangeList` (per **buffer**),
  and `yew_jumplist_serialize/deserialize`, written there against "the
  schema shape Sprint 25 freezes" and called by nobody. This sprint calls
  them. Sprint 15 — `Viewport` (`top`, `top_sub`, `left`, `wrap`),
  `yew_vp_clamp`. Sprint 14 — `Ed`, `Win`, `Buffer`, `Workspace`, the poll
  loop and its `TimerHeap`. Sprint 13 — the `"ed.*"` registry.
- Sprint 10 — the `.sagu` sidecar under
  `$XDG_STATE_HOME/yew/workspaces/<ws_hash>/undo/<fnv64(realpath)>.sagu`,
  its three-way restore validation, and its statement that *this* sprint
  freezes the enclosing state format. Sprint 9 — `Cursor{pos, anchor,
  goal_col}`, `YEW_GCOL_EOL`, `MarkSet` and its bias table.
- Sprint 8 — `yew_xdg_state_dir()`, `FileMeta`, journal replay, and the
  atomic tmp+fsync+rename+dir-fsync primitive. **Assumption beyond the
  index entries:** Sprint 10 factored that primitive out as
  `YewSaveErr yew_write_atomic(const char *path, const u8 *b, u64 n)` to
  write `.sagu`. If it did not, it is factored out here — this sprint must
  not own a second copy of the no-corruption path.
- Sprint 18 — `CmdHist`; its deferral *"per-workspace history → Sprint
  25"* closes in §8.
- Binding: `00-decisions.md` Workspace-state row (*"Dir = workspace, keyed
  by realpath hash under `$XDG_STATE_HOME/yew/workspaces/` (never
  pollutes the repo). Format: Fletch data literals (dogfood), atomic
  writes, versioned schema"*), invariants 1, 3, 5, 7; `02-fletch.md`
  requirement 6 (data literals serve as the workspace-state format).

## Goals

Land `src/ws/workspace.c` and `src/ws/state.c`: workspace identity
(realpath → FNV-1a-64 → `$XDG_STATE_HOME/yew/workspaces/<hex>/`, with
a collision policy and a human-readable path record inside), and **the v1
state schema, frozen here** — version, options, tab groups written before
tabs with file-id → runtime-id remapping, tabs with group/ordinal/deferred,
the pane tree per tab, per-window cursor sets + viewport + goal column,
per-file marks, jumplist and changelist, and the undo-tree *reference*.
The format is Fletch pure-literal data; emitter and parser are hand-written
against a frozen fixture corpus, and **Sprint 36 swaps the implementation,
not the format** — that corpus plus the differential test named in §9 is
s36's Definition of Done, not a hope. Corrupt state never blocks startup;
resume exactness is a pty gate. Deferred: the Fletch-VM serializer and the
`.yew.fl` trust prompt (36), F-mode collapsed-tree state (52),
LSP/AI per-workspace state (46/50), multi-session merge (post-1.0 — §5's
lock is the 1.0 answer).

## Deliverables

### 1. Workspace identity and keying — `src/ws/workspace.h|c`

```c
typedef struct WsKey {
    char  realpath[PATH_MAX];   /* canonical workspace dir              */
    u64   hash;                 /* FNV-1a 64 over realpath bytes        */
    u8    probe;                /* collision probe index, 0 = base      */
    char  dir[PATH_MAX];        /* $XDG_STATE_HOME/yew/workspaces/… */
    bool  stateless;            /* true = never read, never write       */
} WsKey;

bool yew_ws_key(WsKey *k, const char *dir);   /* false = run stateless  */
const char *yew_ws_state_path(const WsKey *k);      /* <dir>/state.fl   */
const char *yew_ws_undo_dir  (const WsKey *k);      /* <dir>/undo/      */
```

| Step | Rule |
|---|---|
| canonicalize | `realpath(dir)`; the workspace is the **process's cwd at startup**, or the `--workspace` argument. A path that is not a directory is an error before this point. |
| hash | FNV-1a 64 over the realpath **bytes** (not codepoints — paths are bytes); rendered `%016lx` lowercase. No seed, no randomization: same dir → same key forever (invariant 5). |
| dir | `yew_xdg_state_dir()` + `/workspaces/<hex>[-<probe>]/`, created `0700` with `mkdir -p` on first write. |
| path record | `<dir>/path` — one line, the realpath bytes, LF-terminated, written atomically at directory creation. |
| collision | on open, if `path` exists and differs from our realpath, retry with `probe = 1..15` (`<hex>-1` …). First probe whose `path` matches, or is absent, wins. Past 15 → `yew_log(WARN)` and `stateless = true`. |

**Why the `path` record.** Without it the state tree is a pile of hex
names no human can audit, garbage-collect, or file a bug about, and a
64-bit collision is undetectable rather than merely rare. With it,
`grep -r . ~/.local/state/yew/workspaces/*/path` answers "which
directory is this", and it doubles as the collision detector — one `open`
in the common case, and that is the entire mitigation.

**Never in the repo.** Nothing this sprint writes lands under the
workspace directory itself (DoD grep gate). The `.yew.fl` *config*
file (Sprint 36) is the only file yew ever reads from a workspace, and
it is user-authored. `--clean` (36) and `--batch` (37) set `stateless`
before any I/O.

### 2. The format — Fletch pure literals, v1

Six value kinds and nothing else: **map, list, string, i64, bool, nil**.

```
map    : '{' ( key ':' value ',' )* '}'      key = bare ident or string
list   : '[' ( value ',' )* ']'
string : '"' bytes '"'
int    : ['-'] digits                        i64, no exponent, no leading +
bool   : 'true' | 'false'
nil    : 'nil'
```

- **There are no floats in v1.** The only real number in the model is a
  pane ratio, stored as `ratio_permille: i64` in `[1, 999]`. Why:
  `printf("%f")` honours `LC_NUMERIC`, so a user in a `de_DE` locale emits
  `0,5` and every determinism golden breaks on their machine; and float
  text does not round-trip exactly. Layout converts `ratio = permille /
  1000.0f` once at load (s22's single-rounding-site rule, extended).
- String escapes, pinned exhaustively: `\"` `\\` `\n` `\t` `\r` `\0`
  `\xNN`. The emitter escapes exactly `"`, `\`, bytes `< 0x20` and `0x7F`;
  **every other byte is emitted verbatim**, so UTF-8 rides through and
  invalid bytes round-trip losslessly (paths are bytes, not text —
  invariant 2). The parser rejects any escape outside the table.
- Trailing comma after **every** element. A state diff between two
  sessions is then a line diff, not a line diff plus comma churn.
- Emitter layout, pinned so goldens are byte-comparable: two-space indent
  per depth, one key or element per line, `key: value` with no space
  before the colon, closers on their own line at the parent's indent. Map
  keys are emitted in the fixed order of the §3 tables — never hash order
  (invariant 5, the Cgfried insertion-order doctrine).
- Comments (`#` to end of line) are **accepted on parse, never emitted**.
  A user hand-editing their state file to unstick something must not hit a
  parse error; the notes are lost on the next write, and the manual says
  so rather than the parser discovering it.

### 3. The schema — v1, FROZEN

```fletch
{
  version: 1,
  writer: "yew 1.0.0",
  workspace: { path: "/home/u/src/yew", saved_at: 1753900000, },
  options: { "tabs.group_hover_preview": true, "wrap": false, },
  groups: [
    { id: 1, label: "src/", dir_path: "/home/u/src/yew/src",
      last_active_member: "/home/u/src/yew/src/ui/tabs.c", },
  ],
  tabs: [
    { id: 4, path: "/home/u/src/yew/src/ui/tabs.c",
      group: 1, group_ordinal: 1, deferred: false,
      panes: { split: "h", ratio_permille: 500,
               a: { win: 0, }, b: { win: 1, }, },
      wins: [
        { cursors: [ { pos: 812, anchor: 812, goal: 14, }, ], primary: 0,
          view: { top: 40, top_sub: 0, left: 0, wrap: false, },
          jumps: { cur: 3, entries: [
            { path: "/home/u/src/yew/src/ui/tabs.c", line: 88, col: 4,
              stamp: 1753899102, }, ], }, },
      ], },
  ],
  active_tab: 4,
  files: [
    { path: "/home/u/src/yew/src/ui/tabs.c",
      marks: [ { name: "a", line: 12, col: 0, bias: "left", }, ],
      changes: { cur: 2, entries: [ ], },
      undo: { file: "9f2c1ab0deadbeef.sagu", version: 1, }, },
  ],
}
```

Field table (types, defaults when absent, bounds):

| Path | Type | Default | Bound / note |
|---|---|---|---|
| `version` | i64 | required | `!= 1` → recovery path (§7) |
| `writer` | string | `""` | informational; nothing reads it back |
| `workspace.path` | string | `""` | cross-checked against the live realpath; mismatch logs and continues — a moved checkout is not corruption |
| `workspace.saved_at` | i64 | 0 | `CLOCK_REALTIME` seconds, display only |
| `options` | map | `{}` | keys this build does not know are **kept and re-emitted verbatim** |
| `groups[].id` | i64 | required | **file id**, remapped (§3.1) |
| `groups[].label` | string | `basename(dir_path)/` | ≤ 256 B |
| `groups[].dir_path`, `.last_active_member` | string | `""` | a label of origin and a path, respectively; both may dangle by design (s24) |
| `tabs[].id` | i64 | required | file id, remapped |
| `tabs[].path` | string | required | canonical; `nil` = untitled scratch, dropped on restore |
| `tabs[].group`, `.group_ordinal` | i64 | 0, 0 | file gid (0 = ungrouped); ordinal 1-based, renumbered on restore |
| `tabs[].deferred` | bool | true | a **restore instruction**, not state (§3.3) |
| `tabs[].panes` | map | `{ win: 0 }` | recursive; ≤ 16 leaves (s22 cap), depth ≤ 8 |
| `tabs[].wins[]` | list | one empty win | the index space `panes.win` refers to |
| `…wins[].cursors[]` | list | one at 0 | ≤ 1024 per win; `goal: -1` means `YEW_GCOL_EOL` (§3.2) |
| `…wins[].view` | map | zeros | `top`, `top_sub`, `left`, `wrap` |
| `…wins[].jumps` | map | empty | ≤ 100 entries (`YEW_JUMPLIST_MAX`) |
| `active_tab` | i64 | first tab | file tab id |
| `files[].path` | string | required | one record per canonical path; ≤ 256 records |
| `files[].marks[]` | list | `[]` | `name` ∈ `a`–`z`, `bias` ∈ `"left"`/`"right"` |
| `files[].changes` | map | empty | ≤ 100 entries |
| `files[].undo` | map | absent | `{ file, version }` — §3.4 |

Whole-document caps: file ≤ 8 MiB, parse nodes ≤ 1 000 000, nesting depth
≤ 32, any string ≤ 4096 B, tabs ≤ 512 (the s23 cap; a longer list is
truncated with a log line). A breach is corruption, handled by §7.

#### 3.1 Groups are written before tabs, and ids are remapped

Two rules, both facsimile scars, both load-bearing:

1. **Order.** The restore parser materializes a tab as it finishes that
   tab's record, and `yew_group_add_member` needs a live group at that
   moment. Emitting tabs first would require a fixup pass over ids that
   have already been renumbered — precisely the class of bug the stable
   `tab_id` discipline exists to eliminate. Groups first is a property of
   the *schema*, not of the writer: a v1 document with `tabs` before
   `groups` is malformed.
2. **Remapping.** The ids in the file are the ids of the *writing*
   session. `yew_group_create` hands out fresh monotonic ids on restore,
   so the parser builds a map while it reads `groups:` and every
   `tabs[].group` resolves through it:

```c
typedef struct { u32 file_id, live_id; } IdMap;   /* 8 B per entry */
static u32 map_gid(const Vec_IdMap *m, u32 file_gid) {
    if (file_gid == 0) return 0;                  /* ungrouped stays 0 */
    for (u32 i = 0; i < m->len; i++)
        if (m->data[i].file_id == file_gid) return m->data[i].live_id;
    return 0;   /* group record missing/dropped -> the tab is ungrouped,
                 * never attached to whatever id happens to exist now.  */
}
```

   Reusing the file's number as the live id looks free and is not:
   `next_group_id` would have to be reconstructed by a max scan, a
   plugin-created group in the same session could collide, and any future
   workspace merge becomes impossible. The map costs 8 bytes per group.
   The same discipline applies to `tabs[].id` → live `tab_id`, which is
   how `active_tab` resolves.

   `panes.win: N` is deliberately **not** an id: it is a positional index
   into that tab's `wins` list. Ids are for things that outlive a
   compaction; a window inside one serialized tab does not.

#### 3.2 What lives where: windows own views, files own text facts

Cursor sets, the goal column, the viewport and the jumplist are written
per **window** — two panes on one file legitimately hold different cursors
and different scroll positions, which is the entire point of a split (s23:
state lives where it lives, with no save/restore choreography). Marks, the
changelist and the undo reference are written per **file**, because they
are properties of the text. This is s21's per-window-jumplist /
per-buffer-changelist decision, serialized.

`goal` is an i64 with `-1` meaning `YEW_GCOL_EOL` (`UINT64_MAX`
internally, s09): `18446744073709551615` does not fit i64 and would force
every reader into an unsigned special case.

#### 3.3 `deferred` is an instruction, not a fact

`deferred: true` is written for every tab not resident at save time and
for every group member that is not the active tab. On restore it means
*create this tab with a path and no buffer* — nothing more. Residency
itself is never persisted: it is asked of the allocation (s24 D3), and a
persisted flag would be a second, lying answer to a question that already
has a structural one.

#### 3.4 Undo is a sibling file, not inline — pinned

The state literal carries only `undo: { file: "<fnv64>.sagu", version: 1 }`
and the tree itself lives at `<ws_dir>/undo/<fnv64(realpath)>.sagu`
(s10 §9's location, unchanged). Three reasons, stated in this sprint's
own voice because this is where the decision is frozen:

1. Undo payloads are arbitrary binary — NULs and invalid UTF-8 that s02
   policy says must round-trip. Inlining means `\xNN`-encoding tens of
   MiB at 4× size, through a parser that must then be fast on megabytes
   of escapes.
2. The state file is rewritten **atomically and in full** on every
   workspace change (§5); inline undo makes every tab switch a multi-MiB
   rewrite-and-fsync.
3. Lifetimes differ: the undo tree is validated against buffer content
   (s10's three-way check) and may be dropped alone, while the tab layout
   is always valid. Separate files let one die without the other.

Restore order for a hydrating tab is therefore `yew_file_load` → journal
replay (both s08) → `.sagu` load with s10's validation. A missing `.sagu`
is normal and silent; one that fails validation drops the tree with a log
line and the buffer still opens.

### 4. Emitter and parser — hand-written this sprint

`src/ws/state_emit.c`:

```c
typedef struct FlEmit { Bytebuf *out; u32 depth; } FlEmit;

void yew_fl_map_open (FlEmit *e, const char *key);   /* key NULL at root */
void yew_fl_map_close(FlEmit *e);
void yew_fl_list_open(FlEmit *e, const char *key);
void yew_fl_list_close(FlEmit *e);
void yew_fl_str (FlEmit *e, const char *key, const char *s, u64 n);
void yew_fl_int (FlEmit *e, const char *key, i64 v);
void yew_fl_bool(FlEmit *e, const char *key, bool v);
void yew_fl_nil (FlEmit *e, const char *key);

void yew_state_emit(const Ed *ed, Bytebuf *out);     /* whole document */
```

- Depth is tracked by the emitter, never by callers — unbalanced
  open/close at teardown is `YEW_BUG` (s10's nesting doctrine).
- Pane trees are emitted through `yew_pane_tree_walk`; a leaf emits
  `{ win: N, }` where N comes from a pre-order counter. **Pitfall: that
  counter must be assigned in one pre-order pass used by both the `panes`
  and the `wins` emission.** Two independent traversals that disagree by
  one produce a state file that restores every pane with its neighbour's
  cursor — silent, and entirely plausible-looking.
- The emitter never stats the filesystem and never allocates per field
  (one `Bytebuf`, geometric growth): emitting a 512-tab workspace is
  string formatting and nothing else.

`src/ws/state_parse.c` — a total parser over the §2 subset into an
arena-allocated literal tree, plus a schema layer that reads it:

```c
typedef enum { FL_NIL, FL_BOOL, FL_INT, FL_STR, FL_LIST, FL_MAP } FlKind;
typedef struct FlLit FlLit;                    /* tagged; maps are arrays */
typedef struct { u64 off; u32 line, col; const char *msg; } FlParseErr;

FlLit *yew_fl_parse(Arena *a, const u8 *src, u64 len, FlParseErr *err);
const FlLit *yew_fl_get (const FlLit *map, const char *key);  /* or NULL */
u32          yew_fl_len (const FlLit *list);
const FlLit *yew_fl_at  (const FlLit *list, u32 i);
i64          yew_fl_int_or (const FlLit *v, i64 dflt);   /* type-safe    */
bool         yew_fl_bool_or(const FlLit *v, bool dflt);
const char  *yew_fl_str_or (const FlLit *v, const char *dflt, u64 *n);
```

- Two layers, deliberately: the parser enforces *syntax* and §3's
  structural caps only; ranges, enum spellings and required-field checks
  live in the schema layer, where a failure can be attributed to a field
  name in the log. A parser that also validated semantics would have to be
  rewritten wholesale in Sprint 36.
- **Total, never trusting.** Unknown keys are ignored, retained for
  re-emission, logged once. Missing keys take §3's defaults. A
  wrong-typed value takes its default and logs — a state file is a cache,
  and one bad field must not cost the user their layout.
- `FlParseErr` carries `line:col` for the **log**, not a caret dialog:
  this is not user-authored source (Sprint 36's config is, and that is
  where s29's diagnostics get reused).
- **Pitfall — never validate UTF-8 on strings.** Paths are bytes;
  rejecting or replacing an invalid sequence loses exactly the files
  invariant 2 exists to protect.
- **Pitfall — `yew_fl_get` is a linear scan of an insertion-ordered
  array, on purpose.** At schema sizes it beats hashing, and a hash map's
  iteration order would leak into re-emission and break determinism.

### 5. Saving — triggers, atomicity, and the single-writer lock

```c
void yew_state_mark_dirty(Ed *ed);        /* from state-changing events */
bool yew_state_save(Ed *ed);              /* false = not the writer     */
```

- **State-changing events** (each calls `mark_dirty`): tab open / close /
  switch / reorder; group create / dissolve / membership / ordinal change;
  pane split / close / resize; active-window change; option change.
  Cursor motion, scrolling and typing are *not* triggers — they ride the
  next save.
- Debounce `YEW_STATE_SAVE_DEBOUNCE_MS = 2000` on the s14 timer heap,
  plus an unconditional save on clean quit and on the SIGTERM/SIGHUP exit
  path (s03).
- Writes go through `yew_write_atomic` (s08's primitive, reused —
  invariants 7 and 1: a `kill -9` mid-save leaves either the old document
  or the new one, never a half one the parser must then survive).
- **Never from the crash handler**, which restores the terminal and
  flushes the *journal* (s08). Serializing an `Ed` from a signal handler
  is not async-signal-safe and can capture a half-mutated tabs array —
  writing a plausible wrong state over a good one is worse than writing
  nothing.
- Lock: `<ws_dir>/lock` holds the writer's pid. A second yew in the
  same workspace **reads** state and never writes it (last-writer-wins
  silently discards the other session's tabs), and says so once:
  `workspace state is owned by pid N; this session will not save it`.
  A stale lock (`kill(pid, 0)` → `ESRCH`) is taken over. Pitfall: check
  the pid, never the file's existence — a `kill -9`'d editor otherwise
  locks its own workspace forever.

### 6. Restore — the ordered algorithm

```c
typedef enum { YEW_WS_FRESH, YEW_WS_RESTORED, YEW_WS_RECOVERED } YewWsResult;
YewWsResult yew_ws_restore(Ed *ed);
```

Order, and why each step sits where it does:

1. Read → parse → validate `version`. Anything wrong here is §7.
2. `options` → the workspace option table; unknown keys retained verbatim
   (an older yew must not silently delete a newer one's settings).
   The option *model* is Sprint 36's; this sprint stores typed literals.
3. `groups` → `yew_group_create` each, building the gid map (§3.1).
4. `tabs`, in file order → `yew_tab_open_deferred(ed, path)` (a new entry
   point that creates the tab **without reading the file**); attach via
   `map_gid` + `yew_group_add_member` + `yew_group_set_ordinal`; build the
   pane tree recursively; per win restore cursors (then
   `yew_cset_normalize`), viewport, jumplist.
5. `files` → per canonical path: marks, changelist, undo reference. A
   record with no matching tab is kept anyway (marks for a file you closed
   are still yours), up to the 256-record cap.
6. `active_tab` → `yew_tab_switch`, which hydrates **exactly one** buffer
   (s24 D3). This is the whole point: reopening a 40-tab workspace
   performs one file read.
7. `yew_group_prune_empty` (s24) — a group whose every member was dropped
   must not survive empty.
8. `yew_layout_compute(root, area)` at the *current* terminal size:
   ratios are permille, so a differently-sized terminal restores
   proportionally rather than in stale cells (s22's law).
9. `yew_vp_clamp` on every win — and **not** `yew_vp_follow`. The saved
   viewport already satisfies scrolloff; follow would move `top` and break
   byte-identical resume. Follow resumes at the first cursor motion.

Missing-file handling (`access(path, F_OK)` at restore, never at save —
saving must not stat 500 paths):

| Case | Behavior |
|---|---|
| missing, deferred | tab is kept, flagged orphan, rendered dim (s23 already dims orphans), no read attempted |
| missing, would hydrate | s08's `YEW_LOAD_ENOENT` path: empty buffer, unmodified, orphan flag set; saving it recreates the file — the ordinary new-file path |
| missing, group member | stays a member with its ordinal; a group does not dissolve because a file vanished. A dangling `last_active_member` already falls back to the lowest ordinal (s24) |
| path is now a directory, or unreadable | same as missing, with a distinct log line |
| any of the above | counted; **one** summary message at startup: `3 files in this workspace are no longer on disk` |

A tab is never silently dropped for a missing file. The path is the only
record of what the user was working on, and deleting it because the file
moved is a data loss the user cannot undo.

### 7. Corrupt state never blocks startup

| Failure | Action (every row ends with "the editor starts") |
|---|---|
| `state.fl` absent | fresh workspace, silent |
| unreadable (`EACCES`) | log, fresh, one message |
| > 8 MiB, parse error, depth/node cap, bad `version`, schema type error | rename aside, fresh, one message |
| a single tab or file record malformed | drop that record, keep the rest, count, one summary message |
| `.sagu` invalid | s10's validation drops the tree; the buffer opens |
| state dir not creatable | `stateless` for the session, log once |

Rename-aside form: `state.fl` → `state.fl.corrupt-YYYYMMDDTHHMMSSZ`
(`strftime` `%Y%m%dT%H%M%SZ`, UTC). Message, shown exactly once:
`workspace state was unreadable; saved as state.fl.corrupt-20260731T142211Z`.
The log line carries the parser's `line:col`.

Rules, each with teeth: **never delete the bad file** (the user's bytes
are sacred even when confusing — s08's stale-journal doctrine); **never
prompt at startup** (a modal dialog before first paint, over a *cache*,
is user-hostile); **never exit non-zero** (a corrupt cache is not a failed
edit); **exactly one message** (a wall of warnings trains people to
dismiss the next one, which will matter). Retention: keep at most 5
`state.fl.corrupt-*` per workspace, deleting the lexicographically
smallest — which is the oldest, which is why the timestamp format is
`%Y%m%dT%H%M%SZ` and not anything friendlier.

### 8. Per-workspace history (closing s18's deferral)

`<ws_dir>/history/{cmd,search}` in s18's line format. Option
`history.scope` ∈ `workspace` (default) | `global`. Reads **merge**:
global entries first, workspace entries after (so the most local are
newest under s18's newest-last convention), deduped by s18's rule.
Writes go only to the scope's file. `--clean`/`--batch` keep s18's
in-memory history. Pitfall: merge at load, never at save — appending the
merged list back would migrate every global entry into every workspace.

### 9. Defer — and the contract handed to Sprint 36

- **Sprint 36 replaces `state_emit.c`/`state_parse.c` with the Fletch VM
  data path. The format above is the contract; the code is not.** Both
  handover artifacts land *this* sprint:
  - `tests/unit/fixtures/wsstate/v1/*.fl` — ≥ 20 frozen documents:
    minimal; maximal (512 tabs / 16-leaf trees / 100-entry jumplists);
    every optional field absent, then every one present; a group whose
    file ids are `7, 9, 12`; an orphan-path document; invalid UTF-8 and
    NUL bytes in paths; a `# comment` document; an unknown-key document;
    and the adversarial set from §7.
  - Sprint 36's hand-written-versus-Fletch differential — for every corpus
    document, `emit(parse_hand(d))` and `emit(parse_fletch(d))` were
    byte-identical and both parses structurally equal. Sprint 36 retired the
    hand-written differential after that final 2×2 handover passed; the
    retained corpus round-trip now exercises the shipping Fletch reader and
    writer as the frozen-format guard.
- Deferred elsewhere: `.yew.fl` trust prompt → 36; F-mode collapsed
  tree state → 52; LSP/AI per-workspace state → 46/50; a migration
  framework → the first sprint that needs v2 (v1 is frozen, and
  forward-compat is "unknown keys are preserved", which is enough) —
  `ed.ws.migrate` hard-errors saying no v2 exists; multi-session merge →
  post-1.0.
- Commands landing here: `ed.ws.save_state`, `ed.ws.restore_state`
  (test/dev), `ed.ws.info` (hash, dir, path record, lock owner),
  `ed.ws.forget` (deletes this workspace's state dir, confirm prompt).

## Testing Strategy

- **Unit** (`test_ws_key.c`): hash stability across runs; the probe walk
  against planted collision fixtures (probes 0, 1, 15, exhaustion →
  stateless); the `path` record written atomically and readable;
  `--clean` performs zero state I/O (syscall count via s08's fault shim).
- **Unit** (`test_fl_lit.c`): round-trip over the whole corpus
  (`parse(emit(parse(d)))` structurally equal to `parse(d)`, and
  `emit(parse(emit(x)))` byte-identical to `emit(x)`); every escape row;
  invalid-UTF-8 and NUL paths surviving; each cap breached in turn (8 MiB,
  depth 33, 1 000 001 nodes, 4097-byte string); rejected escapes; comments
  accepted then dropped; unknown keys preserved across parse → emit.
- **Unit** (`test_state_schema.c`): gid remap (`7, 9, 12` → `1, 2, 3` with
  identical membership and ordinals); tab-id remap and `active_tab`
  resolution; `goal: -1` ↔ `YEW_GCOL_EOL`; permille → ratio → permille
  fixpoint for all 999 values; pre-order `win` numbering agreement between
  `panes` and `wins` on a 16-leaf tree (the §4 pitfall); every
  default-when-absent row.
- **Unit** (`test_ws_recover.c`): each §7 row — editor starts, exit 0,
  rename happened, exactly one message, retention capped at 5.
- **Script** (headless): restore of a 40-tab, 3-group state performs
  exactly **1** file read (s24's test hook in `file.c`); the §6 orphan
  rows; per-workspace history merge at both `history.scope` values.
- **Pty** (`tests/pty/resume_exact.golden`) — **the headline gate**: a
  scripted session opens 6 files, groups 3, splits two panes, moves
  cursors (including onto a CJK line, so `goal` is non-trivial), scrolls,
  and pushes two jumplist entries; quit; reopen; assert (a) the grid is
  byte-identical to the pre-quit grid and (b) structurally every tab's
  path/group/ordinal, every pane rect, every cursor `pos`/`anchor`/`goal`
  and every viewport `top`/`top_sub`/`left`. Then resize away and back and
  assert the grid again (ratios, not cells).
- **Fuzz** (`fuzz_state.c`): random bytes and mutated corpus documents
  into `yew_fl_parse` + the schema layer, 100 000 cases × 4 seeds — never
  crash, never loop, never allocate past the caps, always reach a
  `YewWsResult`; ASan/UBSan.
- **Determinism**: emit the same `Ed` twice → byte-identical;
  save → restore → save → byte-identical to the first save.
- **Perf** (`tests/perf/state.c`): a 512-tab / 16-leaf-per-tab document
  emits in ≤ 5 ms and parses in ≤ 10 ms; a 40-tab restore stays inside the
  20 ms cold-start budget.
- **Torture**: `kill -9` through s08's shim during the save loop —
  `state.fl` is always either the old or the new document, and parses.

## Definition of Done

1. `make test` green under gcc and clang at `-std=c11 -pedantic -Wall
   -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism lanes
   green.
2. **Resume exactness**: the `resume_exact` pty test passes — grid
   byte-identical after quit/reopen, plus the structural assertions on
   tabs, panes, cursors (incl. `goal`), and viewports. Two consecutive
   full runs byte-identical (invariant 5).
3. Restoring a 40-tab, 3-group workspace performs exactly **1** file read
   (test-hook counted); switching to a second member performs the 2nd.
4. Group ids are remapped: the `7, 9, 12` fixture restores to live ids
   `1, 2, 3` with identical membership, ordinals and
   `last_active_member` resolution. `grep -n 'group_id = .*file' src/ws/`
   shows no direct assignment of a file id to a live id.
5. Round-trip: for all ≥ 20 corpus documents, `emit(parse(d))` is
   byte-identical to the canonical form, and save→restore→save is a
   fixpoint. Unknown keys and `# comments` survive parse; comments are
   not re-emitted (documented).
6. No floats anywhere in the format: `grep -rn '%f\|%g\|strtod\|atof'
   src/ws/` → empty.
7. Every §7 row exercised: editor starts, exit 0, bad file renamed with
   a UTC timestamp, never deleted, exactly one status message, retention
   caps at 5 corrupt files.
8. Atomicity: the state write issues write → fsync(file) → rename →
   fsync(dir) in that order (shim intercept log asserted, as in s08 DoD
   2). Direct `rename()` and `fsync()` calls remain absent from `src/ws/`;
   state writers call the shared Sprint 8 atomic file API and own no second
   copy of the primitive.
9. Never pollutes the repo: after a scripted session in a git checkout,
   `git status --porcelain` in the fixture workspace is empty, and
   `find <workspace> -name '*.fl' -newer <marker>` finds nothing.
10. Single-writer lock: a second process in the same workspace restores
    normally, shows the ownership message once, and writes no state; a
    stale lock (pid gone) is taken over.
11. Undo is a sibling: `grep -n 'sagu' src/ws/state_emit.c` shows only
    the reference fields; no undo payload bytes appear in any corpus
    document.
12. Fuzz: 100 000 cases × 4 seeds clean; every cap breach classified as
    corruption rather than crashing or truncating silently.
13. Sprint 36 retired the hand-written differential only after its final
    hand-written/Fletch 2×2 matrix passed. The retained state corpus
    round-trip exercises the shipping Fletch reader and writer and preserves
    the frozen-format, rejection, and idempotence coverage.
14. ≥ 130 assertions across the four unit files; per-workspace history
    merge tested at both `history.scope` values.
