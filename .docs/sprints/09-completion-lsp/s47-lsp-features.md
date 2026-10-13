# Sprint 47: LSP Features — Completion, Hover, Navigation, Rename, Symbols

**MILESTONE: LSP live.** clangd navigates yew's own source in CI (§9).

## Prerequisites

- Sprint 2 — `yew_utf8_decode`, `yew_gb_next_bytes`, `yew_str_clip`,
  `yew_cluster_width`, the invalid-byte escape policy (hover and diagnostic
  text from a server is rendered through it like any other text).
- Sprint 5 — `Cell`, `attrs` (bold 0, dim 1, italic 2, underline 3),
  `yew_grid_put/puts/fill`, the wide-glyph rules and the last-column
  refusal — the floating panel obeys all three.
- Sprint 7/8/9/10 — `TextBuf`, `TextIter`, `FileMeta`, `MarkId`/`MarkBias`,
  `EditCtx`, `yew_edit_insert`/`yew_edit_delete`, `yew_undo_begin/end/abort`,
  `yew_undo`, `yew_undo_current`, and `YEW_TXN_LSP` — reserved by s10 and
  **wired here** (s10 DoD 11 pins that it hard-errors naming this sprint
  until it is).
- Sprint 13 — the command registry, `YEW_CMD_*` flags, the `lsp` domain,
  transient keymap layers pushed by `YEW_CMD_PROMPTS` commands.
- Sprint 15/16 — `yew_msg`, `yew_msg_hint` (s46 §8), `Viewport`,
  `yew_unit_word` (the identifier under the cursor), `yew_unit_of_mode`.
- Sprint 18 — `CmdLine` and `YEW_PROMPT_INPUT` (rename's new-name prompt is
  an instance; s18's law is that there is exactly one text editor in this
  program).
- Sprint 21 — `yew_jump_push(Win *w, ByteOff from)` and the push-rule table,
  whose row *"LSP goto-def / references (Sprint 47) — yes, through this same
  API"* is this sprint's contract; `MatchOverlay` for documentHighlight; the
  back-to-front application order for multi-edit runs.
- Sprint 22 — `Rect`, `yew_layout_compute`, `yew_region_add`,
  `YEW_REGION_BLOCK`, and the layout/draw/hit-test single-source law.
- Sprint 26 — `yew_fz_score` / `FzMatch` / `YEW_FZ_NO_MATCH`, and **the one
  list picker**: `PickItem {label, detail, i32 payload, u8 flags}`,
  `PickerSpec {title, items(ctx,*n), preview, accept(ed,ctx,payload,how),
  path_mode, ctx}`, `yew_picker_open/key/draw/close`. s26's Goals already
  name this sprint's two lists as instances — *"LSP symbol picker (47) …
  `PickerSpec` instances requiring no new machinery"* — and §5 keeps that
  promise: `pickers.c` supplies sources, not chrome.
- Sprint 43 — `ShadowProvider`/`yew_shadow_register`/`yew_shadow_deliver`,
  the staleness law, the menu-vs-ghost arbitration rule (this sprint
  registers the LSP shadow provider and may not re-decide the rule).
- Sprint 44 — `ComplItem`, `ComplSource`, `ComplMenu`, `yew_compl_open`,
  `yew_compl_push`, `yew_compl_close`, the kind-glyph table, the
  documentation-panel **slot** this sprint fills.
- Sprint 45 — `JsonValue` + accessors, `JsonW`, `yew_rpc_call/notify/cancel`,
  the error-code table (`MethodNotFound` clearing a capability bit),
  `yew_mod_require` and `shim.c`.
- Sprint 46 — `LspServer`, `LspCaps`/`yew_lsp_has`, `LspDoc.version`,
  `LspGen`/`yew_lsp_gen_current`, `lsp_pos_of_off` and its inverse,
  `yew_lsp_uri_of_path`/`yew_lsp_path_of_uri`, `DiagStore`, and the nine
  `YEW_CMD_DEFERRED` commands this sprint implements.
- Binding: `00-decisions.md` — the 1.0 LSP scope (*"Diagnostics,
  completion, hover, goto-def, references, rename, symbols"*), invariants 1
  (no data loss), 3 (no silent stubs), 4, 5, 9 (every feature
  keyboard-reachable); `01-architecture.md` §Async subsystems.

## Goals

Turn the client into an editor feature set: `src/mod/lsp/features.c`
(the request/response layer for nine methods), `src/mod/lsp/pickers.c`
(location list, document symbols), `src/mod/lsp/rename.c` (the multi-buffer
workspace-edit transaction), and **`src/ui/panel.c|h`** — the first floating
panel widget, which is *core* code with no LSP dependency because
diagnostics and signature help reuse it and it must exist in a
`MODULES=""` build. Every feature is gated on the server's advertised
capability and degrades to a plain sentence when it is absent. Snippets are
**downgraded, not expanded**, and the policy is pinned and documented
rather than half-implemented. The sprint closes with the milestone job: a
CI lane where clangd, driven by real yew, navigates and renames across
yew's own source.

## Deliverables

### 1. The method matrix

Every row is implemented here unless marked. "Surface built in" names the
sprint that owns the widget, so nothing on this list invents UI it could
reuse.

| Method | Dir | UI surface | Surface built in | Capability gate |
|---|---|---|---|---|
| `textDocument/completion` | → | completion menu (explicit) + ghost text (passive) | s44 §5 / s43 | `completionProvider` |
| `completionItem/resolve` | → | the menu's documentation panel slot | s44 §5 | `completionProvider.resolveProvider` |
| `textDocument/hover` | → | **floating panel** | this sprint §4 | `hoverProvider` |
| `textDocument/signatureHelp` | → | floating panel, anchored above the cursor | this sprint §4 | `signatureHelpProvider` |
| `textDocument/definition` | → | jump, or location picker on N > 1 | s21 jumplist / s26 picker | `definitionProvider` |
| `textDocument/declaration` | → | same | " | `declarationProvider` |
| `textDocument/typeDefinition` | → | same | " | `typeDefinitionProvider` |
| `textDocument/implementation` | → | same | " | `implementationProvider` |
| `textDocument/references` | → | location picker | s26 picker | `referencesProvider` |
| `textDocument/documentSymbol` | → | symbol picker with breadcrumbs | s26 picker | `documentSymbolProvider` |
| `textDocument/documentHighlight` | → | `MatchOverlay` | s21 §3 | `documentHighlightProvider` |
| `textDocument/rename` | → | confirm summary → multi-buffer transaction | s18 prompt / s10 txn | `renameProvider` |
| `textDocument/publishDiagnostics` | ← | gutter, underline, message row, picker | s46 §8 | always |
| `window/showMessage` | ← | `yew_msg` at the mapped severity | s15 | — |
| `window/logMessage` | ← | `yew_log` | s00 | — |
| `workspace/configuration` | ← | answered from the option model | s36 | — |
| `$/cancelRequest` | → | — | s45 | — |
| `$/progress`, `window/workDoneProgress/create` | ← | answered, rendered nowhere | — | deferred, s46 §10 |

Gating is one helper, used identically by all nine:

```c
/* Returns false and messages the user once per (server, capability):
 *   "clangd does not support rename"
 * Also the landing place for a -32601 MethodNotFound response, which
 * clears the bit so the message appears exactly once (s45 §7).        */
static bool feat_require(Ed *ed, LspServer *s, u32 cap, const char *what);
```

The message is a plain sentence naming the server and the feature — never
"not supported", never silence, never a spinner that stops. Invariant 3
applies to a *server's* missing feature exactly as it does to ours.

### 2. Completion — menu, ghost, resolve, and the snippet downgrade

```c
extern const ComplSource yew_compl_src_lsp;    /* fed to yew_compl_open  */
static bool lsp_shadow_request(Ed *ed, const ShadowReq *r);
static const ShadowProvider yew_shadow_lsp = {
    .name = "lsp", .prov = YEW_SHADOW_LSP, .debounce_ms = 120,
    .request = lsp_shadow_request, .cancel = lsp_shadow_cancel,
};
```

The **same** `textDocument/completion` response feeds both surfaces, and
which one it lands on was decided in s43 §9 and is not reopened here:
`yew_compl_push` when the menu is open (explicit trigger), `yew_shadow_deliver`
otherwise (passive). `lsp_shadow_cancel` sends `$/cancelRequest` for
superseded requests — the reason `ShadowProvider.cancel` exists.

Item mapping. LSP `CompletionItemKind` (1–25) → s44's glyph table, extended
with the kinds s44 reserved:

| LSP kind | `SymKind`/glyph | LSP kind | glyph |
|---|---|---|---|
| 3 Function, 2 Method | `f` | 6 Variable | `v` |
| 7 Class, 8 Interface, 22 Struct, 25 TypeParameter | `t` | 21 Constant, 20 EnumMember | `c` |
| 5 Field, 10 Property | `F` | 13 Enum | `e` |
| 9 Module, 19 Folder, 18 Reference | `M` | 14 Keyword | `k` |
| 15 Snippet | `s` (see below) | anything else | `w` |

Field precedence, pinned because servers disagree: the inserted text is
`textEdit.newText` if present (and its range replaces `ComplMenu.replace`),
else `insertText`, else `label`. Sorting uses `sortText` when the server
gives one, falling back to `filterText`, falling back to `label`; our fuzzy
score orders *within* the server's `sortText` groups rather than replacing
them — a server that has ranked its own results knows things we do not, and
throwing that away is how LSP completion ends up worse than the local index.
`preselect` sets the initial `sel`. `additionalTextEdits` (an import the
server wants added) are applied **in the same transaction as the accept**,
back-to-front, or the accept is refused if any of them fails to validate —
half an import is worse than none.

**Lazy resolve.** `ComplSource.resolve` sends `completionItem/resolve` for
the selected item only, and only when the documentation panel is open. The
response fills `detail`/`doc`; a resolve for an item that is no longer
selected is dropped by `yew_lsp_gen_current` plus a selection check. Rate:
at most one resolve in flight per window, cancelled on selection change.
Servers that do not advertise `resolveProvider` show whatever came in the
original item, and `(no documentation)` when that is nothing.

**THE SNIPPET DOWNGRADE POLICY — pinned, documented, not half-built.**

yew 1.0 does **not** expand snippets. There is no tab-stop mode, no
placeholder navigation, no mirrored fields. We advertise
`snippetSupport: false` (s46 §3), which is the honest thing and makes most
servers send plain text. But servers ignore that flag, so every item with
`insertTextFormat == 2` is downgraded on arrival:

```c
/* Strip LSP snippet syntax to plain text. Returns the byte offset where
 * $0 was (the final cursor stop), or the output length when absent.
 * Grammar handled: $N  ${N}  ${N:default}  ${N|a,b,c|}  $VAR  ${VAR}
 * ${VAR:default}  and the escapes \$ \\ \} \, \|                       */
u32 yew_lsp_snippet_strip(const u8 *in, u32 n, Bytebuf *out);
```

| Construct | Becomes | Why |
|---|---|---|
| `$0`, `$1`, `${3}` | *(nothing)* | a tab stop with no expansion engine is an artifact |
| `${1:int argc}` | `int argc` | the default *is* the useful plain text |
| `${1\|red,green\|}` | `red` | the first choice, deterministically |
| `$TM_FILENAME`, `${TM_LINE_NUMBER}` | *(nothing)* | variables we do not implement must not appear literally |
| `\$`, `\\`, `\}` | `$`, `\`, `}` | |
| nested `${1:${2:x}}` | `x` | recursive strip, depth-capped at 8 |

The cursor lands at the recorded `$0` position if the snippet had one,
otherwise at the end of the inserted text. That single line of behaviour is
90 % of what snippet expansion is *for* (`foo(|)` rather than `foo()|`) at
0 % of its complexity. The policy is stated in the man page (Sprint 59), in
`features.c`'s header comment, and in `:lsp.info`, so a user seeing a
completion insert `int argc` instead of a tab stop knows it is a decision.
Snippet expansion is a **post-1.0** item, not a stub, not a TODO.

### 3. The floating panel — `src/ui/panel.c|h` (core)

Hover is the first consumer, so the widget is specified generally. It is
**core**, not module code: signature help, hover, and the s15 message
expansion (`ed.ui.message_expand`, which s15 already describes as a ≤ 5-row
overlay) all want it, and it must compile in a `MODULES=""` build.

```c
typedef enum { YEW_PANEL_BELOW = 0,  /* prefer below the anchor          */
               YEW_PANEL_ABOVE,      /* prefer above (signature help)    */
               YEW_PANEL_CURSOR }    /* wherever there is room           */
    PanelPlace;

typedef struct PanelSpec {
    const char *title;        /* optional; drawn in the top border       */
    const u8   *body; u32 len;/* plain text, LF-separated                */
    u16   x, y;               /* anchor cell, absolute in the grid       */
    u8    place;              /* PanelPlace                              */
    u16   max_w, max_h;       /* 0 = the built-in caps                   */
    const char *role;         /* theme role for the border/background    */
    Vec_Span   *emph;         /* optional byte spans drawn bold (§signature) */
} PanelSpec;

typedef struct Panel {
    bool  open;
    Rect  rect;               /* computed ONCE; draw and hit-test share it */
    u16   scroll;             /* first visible body row                  */
    u32   nrows;              /* wrapped body rows                       */
    Vec(Span) rows;           /* byte span per wrapped row               */
} Panel;

bool yew_panel_open (Ed *ed, Panel *p, const PanelSpec *spec);
void yew_panel_close(Ed *ed, Panel *p);
bool yew_panel_key  (Ed *ed, Panel *p, const Key *k);  /* false = not ours */
void yew_panel_draw (Ed *ed, const Panel *p, Grid *g); /* LAST in the frame */
```

Rules, each with a test:

1. **Placement is computed once** in `yew_panel_open` (and re-computed on
   resize), stored in `rect`, and consumed by the drawing pass *and* by
   `yew_region_add` — s22's law. A panel never recomputes its position from
   text content.
2. Size: content width clamped to `YEW_PANEL_MAX_W = 72` cells and to the
   window rect; height to `YEW_PANEL_MAX_H = 16` rows. Width is the widest
   wrapped row, measured with `yew_str_width`, never `strlen`.
3. Placement resolution: try the preferred side; if it does not fit, try
   the other; if neither fits, take the side with more room and scroll.
   Horizontally, the panel is left-aligned to the anchor and shifted left
   just enough to fit — never clipped mid-cluster (`yew_str_clip` does the
   truncation when it must).
4. **Wrapping is at grapheme boundaries with a whitespace preference**, the
   same simple rule s15 uses (UAX #14 is a 1.0 non-goal, s02 §11). A word
   longer than the panel is hard-broken at a cluster boundary, never inside
   one.
5. Border: `┌ ─ ┐ │ └ ┘` from the s22 glyph vocabulary, in `spec->role`;
   the title is clipped into the top border; a `▲`/`▼` marker sits in the
   right border when the body scrolls.
6. Input: `<up>`/`<down>`/`<pgup>`/`<pgdn>` scroll, `<esc>` and any other
   key close **and re-dispatch** the key (a panel never swallows a
   keystroke — s44's menu rule, applied here). A panel is a transient
   keymap layer (s13) so this is one `Keymap`, not an if-chain.
7. Regions: `YEW_REGION_BLOCK` over `rect`, so clicks do not fall through
   to text that is not visually there.
8. Body text is **plain**. Servers are asked for `plaintext` (s46 §3), but
   a server that sends markdown anyway is rendered verbatim minus fenced
   code-fence lines (` ``` ` alone on a line is dropped) — the one
   concession, because those lines are pure noise and dropping them is not
   rendering markdown.
9. At most **one** panel per window is open at a time; opening a second
   closes the first. The completion menu's documentation panel is *not* a
   `Panel` (it is part of the menu's own rectangle, s44 §5) — pinned so the
   two do not fight for the "one panel" slot.

Hover (`ed.lsp.hover`, `K` in L mode) sends `textDocument/hover` at the
cursor, opens a `YEW_PANEL_BELOW` panel with the `contents` flattened to
plain text and the server's `range` (when given) drawn with the
`lsp.hover_range` role in the buffer. An empty/null result messages
`no hover information here`, which is information.

Signature help (`ed.lsp.signature`, `C-k` in I mode, and auto-triggered on
`caps.sig_trigger` characters) opens a `YEW_PANEL_ABOVE` panel showing the
active signature, with `emph` set to the active parameter's label range so
it renders bold. `activeParameter` may be per-signature or global; both
shapes are handled, and an out-of-range index emphasises nothing rather
than asserting.

### 4. Navigation — definition family and references

```c
typedef struct LspLoc { char *path; u32 line, chr, end_line, end_chr; }
    LspLoc;

static void nav_goto(Ed *ed, Win *w, const char *method, u32 cap,
                     const char *what);
```

One function serves `definition`, `declaration`, `typeDefinition` and
`implementation` — four commands, one code path, four capability bits.
Response handling:

| Result | Behaviour |
|---|---|
| `null` / `[]` | `yew_msg(INFO, "clangd: no definition found")` |
| one `Location` | **`yew_jump_push(w, cursor)` first**, then open the file (reusing an existing buffer or hydrating a new tab per the current `lsp.open_in` option: `here` \| `split` \| `tab`, default `here`) and place the cursor at the converted position |
| `Location[]` with N > 1, or `LocationLink[]` | the location picker (§5) |

`yew_jump_push` is called **before** the cursor moves, per s21's contract,
and only for the actual jump — a picker that the user cancels pushes
nothing. `n`/`N`-style repeats do not exist here.

**Pitfall — the position comes back in the server's encoding.** Every
`Position` in a response goes through `lsp_off_of_pos` (s46 §5's inverse),
against **the target file's** buffer, not the current one. A goto-def into a
file with different content (or one not yet open) must hydrate the buffer
first and convert against it; converting against the wrong buffer lands the
cursor at a plausible-looking wrong column, which is the hardest class of
bug in this sprint to notice.

`textDocument/documentHighlight` runs on an idle timer (300 ms after the
cursor stops) when the capability is present, painting occurrences of the
symbol under the cursor into an LSP-owned `MatchOverlay` (s21 §3) with the
`lsp.highlight_read` / `lsp.highlight_write` roles. Cleared on any edit.

### 5. Pickers — locations and document symbols

Both are `PickerSpec` instances over `yew_picker_open`; neither invents
chrome, geometry, keys or a filter line. `src/mod/lsp/pickers.c` supplies
only an `items()` source, an `accept()`, and an optional `preview()`:

```c
typedef struct LocSrc { Vec(LspLoc) locs; Vec(PickItem) rows; Arena a; }
    LocSrc;                       /* PickerSpec.ctx for both pickers      */
```

`PickItem.payload` is the location's index into `LocSrc.locs` — an
**identity, never a row index** (s26's law), so the picker's incremental
re-filtering can reorder rows without an `accept` landing on the wrong
target. `path_mode = true` for the location picker, so s26's two-pass
basename rule ranks `piece.c` above a path that merely contains those
letters; `false` for symbols, whose labels are not paths.

**Location picker** (`references`, and multi-result navigation). Rows —
`PickItem.label` on the left, `PickItem.detail` on the right:

```
src/text/piece.c:412:5   yew_textbuf_insert(tb, at, bytes, len);
src/edit/shell.c:308:5   yew_textbuf_insert(tb, region.lo, out, n);
```

Path shortened relative to `yew_ws_root()`; line and column **1-based for
display** (s46 §5's rule: 0-based internally, +1 only at the display edge);
the source line's text is read from the file (or its open buffer) with
leading whitespace stripped and clipped by `yew_str_clip`. `yew_fz_score`
runs over `label`, and `FzMatch.pos` drives the accent highlighting the
picker already does. `Enter` (the spec's `accept`) pushes the jumplist and
jumps; the spec's `preview` fills the picker's preview slot with the
target's context. Results are sorted by
(path, line, column) with `yew_sort_stable` — servers return them in
arbitrary order and a picker that re-orders between two runs of the same
query is a determinism failure the user experiences as "the list moved".

**Document-symbol picker with breadcrumbs.** `documentSymbol` returns either
a `DocumentSymbol[]` tree (we asked for it) or a flat `SymbolInformation[]`;
both are handled, and the flat form's `containerName` becomes a one-level
breadcrumb. Rows carry the full path through the tree:

```
t  Grid                        src/term/grid.h:120
F  Grid › rows                 src/term/grid.h:121
f  yew_grid_put                src/term/grid.c:88
f  yew_render_frame › row_run  src/term/render.c:214
```

The kind glyph is s44's table; the breadcrumb separator is `›` (U+203A, one
cell) with an ASCII `>` fallback under the same one-cell assertion s43 §6
pins for gutter glyphs. Filtering matches the **leaf name** first and the
full breadcrumb second, so typing `put` finds `yew_grid_put` before
`Grid › put_something_else` — implemented by scoring both strings and
taking the better, not by a second widget. `accept` pushes the jumplist and
jumps. `ed.lsp.symbols` is the command; with no server it falls back to the s44
symbol index restricted to the current buffer, so `:lsp.symbols` is useful
in a `.txt` — the no-LSP tier earning its keep again.

### 6. Rename — a multi-buffer transaction

```c
typedef struct RenamePlan {
    Arena  arena;
    Vec(RenameFile) files;    /* one per URI, sorted by path             */
    u32    nedits;
    char  *old_name, *new_name;
} RenamePlan;

typedef struct RenameFile {
    char  *path;  u32 buf_id;      /* buf_id 0 until hydrated            */
    Vec(RenameEdit) edits;         /* sorted DESCENDING by offset        */
    bool   was_open, was_dirty;
} RenameFile;

typedef struct RenameEdit { ByteOff lo, hi; const u8 *text; u32 len; }
    RenameEdit;

bool yew_lsp_rename(Ed *ed, Win *w);          /* ed.lsp.rename           */
```

The flow, in four phases, and **nothing touches a buffer before phase 3**:

**Phase 1 — ask.** The identifier under the cursor comes from
`yew_unit_word.span()` (s16), seeding a `YEW_PROMPT_INPUT` `CmdLine` (s18).
Empty or unchanged input cancels silently. The request carries `LspGen` so
a response describing a buffer the user has since edited is dropped (s46 §7).

**Phase 2 — validate, all of it, before anything.** The `WorkspaceEdit`
(`documentChanges` preferred, `changes` accepted) is turned into a
`RenamePlan`, and the plan is rejected as a whole if **any** of these fails:

| Check | Failure message |
|---|---|
| every URI has scheme `file:` and decodes to a path (s46 §4) | `rename touches a non-file URI; refusing` |
| no `CreateFile`/`RenameFile`/`DeleteFile` operations appear | `server asked to create or delete files; refusing (not supported in 1.0)` |
| every path is inside `yew_ws_root()` | `rename would edit outside the workspace: <path>` |
| every file is readable and, if it exists on disk, regular (not a symlink to outside the workspace) | `cannot edit <path>: <errno>` |
| ≤ `YEW_RENAME_MAX_FILES` (200) files and ≤ `YEW_RENAME_MAX_EDITS` (20 000) edits | `rename spans 412 files; refusing (limit 200)` |
| each document's `version` (when the server sent one) equals ours | `rename is stale; the buffer changed. try again` |
| every range converts to a valid in-range byte span on a grapheme boundary | `rename produced an invalid range in <path>:<line>` |
| no two ranges within one file overlap | `server sent overlapping edits for <path>; refusing` |

Every affected file is **hydrated as a buffer** during validation (that is
where the conversion happens: a position is meaningless without the text it
indexes). Files opened by the rename are ordinary buffers in ordinary tabs.

**Phase 3 — confirm.** A summary panel (§3's widget, `YEW_PANEL_CURSOR`),
listing files and counts, sorted by path:

```
 rename 'yew_cluster_width' → 'yew_cluster_cells'
 34 edits in 6 files, 2 already modified

   src/unicode/width.c        11
   src/unicode/width.h         2
   src/term/grid.c             9 *
   src/term/render.c           7
   src/ui/statusline.c         3 *
   tests/unit/test_width.c     2

 enter apply · esc cancel · d show diff
```

`*` marks a buffer with unsaved changes, because a rename that lands on top
of the user's in-flight edits is something they should decide about
knowingly. `d` opens a scratch buffer with a unified diff of the plan. This
confirmation is not optional and there is no "don't ask again" — invariant 1
outranks convenience, and a rename is the single most destructive thing an
LSP server can ask us to do.

**Phase 4 — apply, all or nothing.**

```c
for each file f in plan.files:                  /* path order: determinism */
    yew_undo_begin(f->buf->undo, YEW_TXN_LSP);  /* s10: LSP txns are ATOMIC,
                                                   never merged with typing */
    for each edit e in f->edits:                /* DESCENDING offsets —
                                                   s21's rule, so earlier
                                                   edits never shift later
                                                   ones' coordinates       */
        yew_edit_delete(&f->ec, (Span){ e.lo.v, e.hi.v });
        yew_edit_insert(&f->ec, e.lo, e.text, e.len);
    yew_undo_end(f->buf->undo);
    applied[napplied++] = f;
```

- **One undo node per buffer.** `YEW_TXN_LSP` is atomic by s10's grouping
  law (only `TYPE` and `ERASE` merge), so one `ed.undo` in any affected
  buffer takes that buffer's whole share of the rename back.
- **All or nothing.** If any file fails mid-apply, that file's transaction
  is `yew_undo_abort`ed and every already-committed file is rolled back with
  exactly one `yew_undo` each. That rollback is sound because each buffer
  received exactly one node and, in a single-threaded editor (invariant 8),
  nothing could have edited them in between — stated in the code comment so
  nobody "improves" it into a partial-apply. The user sees
  `rename aborted: <reason>; 4 files rolled back` and the workspace is
  byte-identical to before.
- **Nothing is written to disk.** Buffers are left dirty and the message
  says so: `renamed 34 occurrences in 6 files (unsaved — :wa to write)`.
  A rename that silently rewrites 200 files on disk is exactly the surprise
  invariant 1 exists to prevent; the user reviews and saves.
- Marks, cursors and diagnostics in every touched buffer follow through the
  choke point automatically (s09 bias rules, s46 §8's diagnostic marks) —
  the rename does no bookkeeping of its own, which is the payoff for having
  one edit choke point.

### 7. Commands and default bindings

All nine s46 `YEW_CMD_DEFERRED` commands become real. Bindings live in the
L-mode layer unless noted; every one is keyboard-reachable (invariant 9).

| Command | Binding | Flags |
|---|---|---|
| `ed.lsp.goto_def` | `g d` | `NEEDS_WIN` |
| `ed.lsp.goto_decl` | `g D` | `NEEDS_WIN` |
| `ed.lsp.goto_type` | `g y` | `NEEDS_WIN` |
| `ed.lsp.goto_impl` | `g i` | `NEEDS_WIN` |
| `ed.lsp.references` | `g r` | `NEEDS_WIN`, `PROMPTS` |
| `ed.lsp.hover` | `K` | `NEEDS_WIN`, `PROMPTS` |
| `ed.lsp.signature` | `C-k` (I layer) | `NEEDS_WIN`, `PROMPTS` |
| `ed.lsp.rename` | `g R` | `NEEDS_WIN`, `PROMPTS`, `CHANGES_BUFFER` |
| `ed.lsp.symbols` | `g s` | `NEEDS_WIN`, `PROMPTS` |
| `ed.lsp.complete` | `C-<space>` (I layer) | opens s44's menu with `yew_compl_src_lsp` |

`ed.lsp.complete` and `ed.compl.open` are the same surface with different
sources; when no server serves the buffer's language, `ed.lsp.complete`
falls back to the s44 index source with an INFO message naming why. In a
`MODULES=""` build all ten exist and report s00's `yew_mod_require` text.

### 8. Defer

- **Snippet expansion** → post-1.0, by policy (§2), documented in
  `features.c`, `:lsp.info`, and the man page. Not a stub: the strip
  function is the finished behaviour.
- `workspace/symbol`, code actions, formatting, inlay hints, semantic
  tokens, call/type hierarchy, folding ranges, selection ranges →
  **not in 1.0** (s46 §10 already lists them); `features.c` repeats the list
  with reasons so a reader of this file does not have to find s46.
- `prepareRename` → never used: we compute the identifier span with s16 and
  a server disagreeing about the range is a validation failure, not a
  negotiation.
- Server-initiated `workspace/applyEdit` → refused; we advertise
  `applyEdit: false` (s46 §3) and answer any such request with
  `{applied: false}` plus a log line.
- Multi-server merged results for one buffer → post-1.0 (s46 §10).
- Markdown rendering in the panel → never for 1.0 (§3 rule 8).
- AI provider on the shadow surface → Sprint 49.

### 9. MILESTONE — clangd navigates yew's own source in CI

The proof that the whole campaign works, run as a dedicated CI lane (`lsp`)
on the designated runner. It uses our *own* repository as the corpus, which
is the only fixture that cannot go stale.

**`make compile_commands.json`.** We have one hand-written Makefile and no
cmake, so we emit the compilation database ourselves rather than adding
`bear`/`compiledb` (invariant 7). One target, ~15 lines, emitting a
deterministic JSON array through `src/mod/lsp`'s own writer discipline
(sorted by file, one entry per object, `arguments` = the exact `CFLAGS` the
build uses, `directory` = the repo root):

```make
compile_commands.json: Makefile
	$(BUILD)/gen-compdb $(CC) "$(CFLAGS)" $(SRCS) > $@
```

It is generated, not committed (it embeds an absolute path), listed in
`.gitignore`, and regenerating it twice produces byte-identical output.

**`tests/lsp/clangd_ci.sh` + `tests/lsp/test_clangd_live.c`.** The lane:

1. Assert `clangd --version` exists. On the `lsp` CI lane a missing clangd
   is a **failure**, not a skip — a silently skipped milestone rots. For a
   local `make test` it is a loud `SKIP: clangd not installed`.
2. `git worktree add` a scratch copy of `HEAD`, so nothing edits the
   checkout; `make compile_commands.json` inside it.
3. Launch yew headless (`yew --batch tests/lsp/nav.fl`, s37; until s37
   lands, the C driver `test_clangd_live.c` drives the same code paths
   directly) and wait for the server to reach `YEW_LSP_READY` and for
   `--background-index` to quiesce, with a 180 s ceiling.
4. **Goto-definition fixtures.** For each row, request
   `textDocument/definition` at the given *use* site and assert the result's
   file and line. Expected lines are **resolved at test time** by searching
   the target file for the definition's own anchor regex, not hardcoded —
   so an ordinary refactor does not break CI, while a wrong *file* or a
   wrong line still does:

| # | Use site (file, symbol) | Must land on | Anchor regex |
|---|---|---|---|
| 1 | `src/text/edit.c`, `yew_textbuf_insert` | `src/text/piece.c` | `^void yew_textbuf_insert` |
| 2 | `src/term/render.c`, `yew_cluster_width` | `src/unicode/width.c` | `^int yew_cluster_width` |
| 3 | `src/edit/dispatch.c`, `yew_ed_invoke` | `src/edit/ed.c` | `^CmdStatus yew_ed_invoke` |
| 4 | `src/unicode/coords.c`, `yew_gb_boundary` | `src/unicode/grapheme.c` | `^bool yew_gb_boundary` |
| 5 | `src/term/grid.c`, `Cell` (a type) | `src/term/grid.h` | `^} Cell;` |
| 6 | `src/mod/lsp/client.c`, `yew_json_parse` | `src/mod/lsp/json.c` | `^JsonValue \*yew_json_parse` |
| 7 | `src/ui/statusline.c`, `yew_str_width` | `src/unicode/width.c` | `^int yew_str_width` |
| 8 | a use inside a `#ifdef YEW_WITH_LSP` block | the right file | — (proves the compdb's `-D` flags reached clangd) |

  Row 8 is the one that catches a broken `compile_commands.json`: without
  the module defines, clangd sees dead code and returns nothing.

5. **References fixture.** `textDocument/references` on `yew_cluster_width`
   returns ≥ 6 locations spanning ≥ 3 distinct files, including
   `src/unicode/width.h` and `src/term/grid.c`.
6. **The rename round-trip, across the indexed call graph.** In the scratch worktree:
   rename `yew_cluster_width` → `yew_cluster_cells` at its definition;
   assert the plan covers **at least** `src/unicode/width.h` and
   `src/unicode/width.c` (the two-file requirement) plus every caller; apply
   it; `:wa`; assert `git diff --name-only` lists exactly the plan's files
   and that the exact identifier `yew_cluster_width` is absent from
   executable code. Comment-only mentions and longer symbols such as
   `yew_cluster_width_init` are not rename targets and may remain. Then
   **restart clangd on the renamed tree**, rename back, and assert
   `git diff --exit-code` is clean — the round trip is what proves ranges
   were converted correctly in both directions, because an off-by-one
   anywhere leaves a residue the second rename cannot undo. Finally,
   `make` in the worktree, proving the renamed tree still compiles and
   providing the stronger guard against a missed executable reference.
7. **Diagnostics fixture.** Append `int x = undefined_symbol_xyz;` to a
   scratch copy of `src/util/base.c`, assert a `publishDiagnostics` arrives
   within 60 s with `YEW_DIAG_ERROR` on that line and a message containing
   `undefined_symbol_xyz`, and that the gutter cell for that line carries
   the error glyph.
8. **Hover fixture.** Hover on `yew_textbuf_insert` returns a non-empty
   panel body containing the string `TextBuf`.
9. Tear down: shut the server down gracefully, assert zero zombies
   (`waitpid(-1,…,WNOHANG)` → `ECHILD`), zero leaked fds, and
   `git worktree remove`.

The lane writes a one-line summary the CI log surfaces:
`clangd: 8/8 goto, 6 refs in 3 files, rename round-trip clean, 1 diagnostic`.

## Testing Strategy

- **Unit — snippets** (`tests/unit/test_lsp_snippet.c`): every row of §2's
  table plus nesting to depth 8 and 9 (capped), unbalanced `${`, a literal
  `$` at end of input, `\|` inside a choice, and a 4 KiB snippet; assert the
  stripped text **and** the returned `$0` offset for each. ≥ 30 vectors.
- **Unit — item mapping** (`test_lsp_compl.c`): all 25 `CompletionItemKind`
  values → glyphs; the `textEdit`/`insertText`/`label` precedence with all
  eight presence combinations; `sortText` grouping preserved under our fuzzy
  re-order (a hand-built case where the server's order must win); `preselect`;
  `additionalTextEdits` applied in the same transaction (undo depth 1) and
  refused as a whole when one is invalid.
- **Unit — panel** (`test_panel.c`): placement resolution for all four
  fit/no-fit combinations at three terminal sizes; wrapping at grapheme
  boundaries over the s02 corpus (a CJK line, a ZWJ family, a 200-char
  unbroken token); width measured with `yew_str_width` and the panel never
  exceeding its `Rect`; `yew_panel_key` re-dispatching an unknown key;
  region registration matching `rect` exactly; opening a second panel
  closing the first. Runs in a `MODULES=""` build (the widget is core).
- **Unit — rename planning** (`test_lsp_rename.c`): every §6 Phase-2
  rejection row, each asserting the exact message and that **zero** bytes
  changed; overlapping edits; a `documentChanges` form and a `changes` form
  producing identical plans; descending-order application verified by a
  three-edit-per-line fixture where ascending order would corrupt; one undo
  node per buffer; the mid-apply failure path rolling back N committed
  buffers to byte-identity (fault-injected on the third file); the 200-file
  and 20 000-edit caps.
- **Unit — navigation** (`test_lsp_nav.c`): the result-shape table
  (`null`, `[]`, one, many, `LocationLink[]`); jumplist pushed exactly once
  and only on an actual jump; a cancelled picker pushing nothing; a
  definition in a not-yet-open file hydrating the buffer and converting
  against **it** (the pitfall test: the same position converted against the
  current buffer yields a different offset, and the test asserts we used the
  right one).
- **Unit — capability gating** (`test_lsp_gate.c`): every one of the nine
  features with its capability absent → the exact sentence, once; a
  `-32601` response clearing the bit so the second invocation messages
  without a round trip.
- **Script/pty** (`tests/pty/lsp_feat_*.golden`, driven by s45's `fakelsp`
  with scripted responses — deterministic, no real server): the completion
  menu with LSP items and a resolved documentation panel; the hover panel
  below and flipped above; signature help with the active parameter bold;
  the location picker; the symbol picker with breadcrumbs; the rename
  confirmation summary including the `*` dirty markers; the "server does not
  support X" message.
- **Milestone lane** (§9): the real clangd job. Separate from the pty
  goldens on purpose — golden files must stay deterministic, and a real
  language server is not.
- **Fuzz** (`tests/fuzz/fuzz_lsp_resp.c`): mutated `CompletionList`,
  `Hover`, `Location[]`, `DocumentSymbol[]` and `WorkspaceEdit` payloads fed
  to the real handlers. Invariants: no crash; no buffer is ever modified by
  a malformed `WorkspaceEdit` (the strongest one — assert byte-identity
  after every iteration); every range clamped rather than asserted; the
  panel never draws outside its `Rect`.
- **Perf**: the completion path from keystroke to menu paint ≤ 8 ms with a
  fake server replying instantly (our share of the budget, server latency
  excluded); a 20 000-edit rename plan validates in ≤ 300 ms; the symbol
  picker over a 5 000-symbol file filters within the keystroke budget.

## Definition of Done

1. `make test` green on gcc and clang for `MODULES="lsp …"` **and**
   `MODULES=""` (the panel widget is core and is tested in both), zero
   warnings; ASan/UBSan/valgrind lanes clean.
2. **MILESTONE:** the `lsp` CI lane is green — all 8 goto-definition
   fixtures land on the right file and the anchor-resolved line, references
   returns ≥ 6 hits across ≥ 3 files, the two-file rename round-trips to
   `git diff --exit-code` clean and the renamed tree still compiles, the
   diagnostic fixture fires, and the hover fixture returns text. The lane
   **fails** when clangd is absent.
3. `make compile_commands.json` is reproducible: two runs are byte-identical,
   it is gitignored, and the `#ifdef YEW_WITH_LSP` fixture (row 8) passes,
   proving the module defines reached the server.
4. Snippets are downgraded, never expanded: all ≥ 30 vectors pass including
   the `$0` cursor offset; `grep -rn 'tabstop\|placeholder' src/` returns
   only comments in the policy paragraph; the policy text appears in
   `:lsp.info` output (asserted by a script test).
5. Rename is all-or-nothing: every Phase-2 rejection row leaves the
   workspace byte-identical, and the fault-injected mid-apply rolls back
   every committed buffer to byte-identity (checked by hash over all
   affected buffers).
6. One undo node per buffer per rename: after a 6-file rename, each
   buffer's `yew_undo_current` advanced by exactly 1, and one `ed.undo` in
   each restores it exactly.
7. Nothing is written to disk by a rename: the fd-accounting/`open` trace
   over a rename script shows zero writes to any source path, and every
   affected buffer is dirty.
8. `YEW_TXN_LSP` no longer hard-errors (closing s10 DoD 11) and is used in
   exactly one place — `grep -rn 'YEW_TXN_LSP' src/` shows the enum, the
   rename apply loop, and nothing else.
9. Jumplist discipline: `yew_jump_push` is called before the cursor moves,
   exactly once per completed jump, zero times for a cancelled picker —
   asserted for all five navigation commands.
10. Capability gating: all nine features produce the exact
    `"<server> does not support <feature>"` sentence when the bit is absent,
    exactly once per (server, feature); a `-32601` reply achieves the same
    state.
11. The panel obeys its `Rect`: fuzz + unit prove it never writes a cell
    outside `rect`, never splits a cluster, never leaves an orphan wide-glyph
    continuation (s05's rules re-checked from this side), and computes its
    geometry exactly once per open/resize.
12. Determinism: the location and symbol pickers produce identical row order
    for the same response delivered in three different array orders;
    determinism lane green on the new pty goldens.
13. Malformed responses never edit a buffer: 4 × 50k fuzz iterations with
    byte-identity asserted after each.
14. ≥ 240 assertions across the six new unit files; the milestone lane's
    summary line is present in CI output.
