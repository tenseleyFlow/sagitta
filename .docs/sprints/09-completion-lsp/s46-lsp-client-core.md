# Sprint 46: LSP Client Core — Lifecycle, Sync, Positions, Diagnostics

## Prerequisites

- Sprint 2 — the UTF-8 DFA, `yew_utf8_decode`, `yew_utf8_len`,
  `yew_cp_width`, the U+DC80–DCFF escape policy, and the architecture law
  *"Conversions live in `src/unicode/` and are the only place allowed to do
  width math."* §5 lands a new coordinate family under that law.
- Sprint 5 — `attrs` bits (`undercurl` = 4, `underline` = 3, `dim` = 1),
  `YewColor`, the note that `4:3` is emitted only when `caps.truecolor` and
  degrades to plain underline otherwise.
- Sprint 7 — `TextBuf`, `TextIter`, `tb->gen`, `yew_textbuf_line_span`,
  `yew_textbuf_line_start`, `yew_textbuf_line_of`, `yew_textbuf_line_count`.
- Sprint 8 — `FileMeta` (`eol`, `had_bom`, `binary`, `had_invalid_utf8`),
  the atomic-save contract, canonical paths.
- Sprint 9 — `Cursor`, `MarkSet`, `MarkId`, `MarkBias` and the bias table.
  Its §3 already says: *"Sprint 21's jumplist and **Sprint 46's LSP
  positions** are mark consumers; getting bias wrong here surfaces as
  off-by-one rename edits two campaigns later."* This is that sprint.
- Sprint 10 — `EditCtx`, `yew_edit_insert`/`yew_edit_delete` and their
  pinned step order, `yew_undo_begin/end/abort`, `YewTxnReason` with
  `YEW_TXN_LSP` reserved.
- Sprint 15 — `Viewport`, `yew_gutter_width`, `yew_msg`, `yew_msg_clear`,
  the statusline segment/priority table (the `badges` row is reserved for
  LSP here), `yew_win_damage_lines`.
- Sprint 19 — `yew_job_spawn`, `YewJobSpec` (argv form, `cwd`),
  `yew_job_collect_fds`, `yew_job_pump`, `yew_job_reap`, `yew_job_signal`,
  `YEW_JOB_TERM_GRACE_MS`, the exec-status pipe, the environment contract,
  the argv-array-never-string-interpolation rule.
- Sprint 21 — `MatchOverlay` (*"the overlay type is deliberately
  pattern-agnostic"*) and `yew_win_damage_lines`.
- Sprint 43 — `yew_edit_notify_pre/post` and their pinned call order
  (`yew_lsp_note_edit` is a `_pre` consumer, and §5 is why),
  `yew_gutter_sign_set` / `YEW_SIGN_DIAG` (cell 0).
- Sprint 45 — `JsonValue`, `yew_json_parse`, the accessors, `JsonW`,
  `RpcRx`/`RpcTx`/`RpcConn`, `yew_rpc_call/notify/reply/cancel`,
  `RpcPending` (with its `gen` and `buf_id` fields), the error-code table,
  `YEW_SINK_FRAMED`, `src/mod/lsp/shim.c`, `yew_mod_require`.
- Binding: `00-decisions.md` — *"LSP: bespoke JSON + JSON-RPC over stdio
  pipes. Incremental didChange. Diagnostics, completion, hover, goto-def,
  references, rename, symbols for 1.0"*; invariants 1, 2, 3, 4, 5, 8;
  `01-architecture.md` — *"LSP: framed JSON-RPC; requests carry generation
  counters so stale responses (post-edit) are dropped, never applied."*

Assumptions beyond earlier index entries: **(a)** Sprint 39/40 give each
`Buffer` a language id (`b->lang`, an interner id like `"c"`, `"rust"`)
derived from the syntax-definition registry; **(b)** Sprint 36's option
model can carry the §1 server table as Fletch data, with the C table in
this sprint as the compiled-in default; **(c)** `yew_ws_root()` (s18/s19)
returns the workspace realpath.

## Goals

Land the LSP client: `src/mod/lsp/client.c|h` (per-language server config,
spawn → `initialize` → `initialized`, shutdown/exit, crash detection with
backoff and a give-up threshold), document synchronisation (`didOpen`,
**incremental `didChange` translated from the s10 edit choke point**,
`didSave`, `didClose`), generation counters that drop responses describing a
buffer state that no longer exists, and diagnostics end to end
(`publishDiagnostics` → `src/mod/lsp/diag.c` per-buffer store → gutter
signs, underline attrs, message-row echo, and a diagnostic-list picker).
And, as a deliverable in its own right because a wrong answer here silently
corrupts every edit and rename the editor will ever make: `src/unicode/u16.c|h`,
the UTF-16 code-unit coordinate family, with `positionEncoding` negotiation
so we mostly do not have to use it. Completion, hover, definition,
references, rename and symbols are **Sprint 47**; this sprint registers
their commands as `YEW_CMD_DEFERRED` naming it.

## Deliverables

### 1. Server configuration and root resolution — `src/mod/lsp/client.h`

```c
typedef struct LspServerCfg {
    const char *id;              /* "clangd" — the log/statusline name    */
    const char *lang;            /* Buffer.lang, interned                 */
    const char *cmd;             /* argv[0]; looked up on PATH            */
    const char *const *args;     /* NULL-terminated                       */
    const char *const *roots;    /* root markers, in priority order       */
    const char *init_options;    /* raw JSON object text, or NULL         */
    i32   init_timeout_ms;       /* default 60000                         */
} LspServerCfg;
```

The shipped default table (overridable in `init.fl` from Sprint 36; a
language absent from it simply has no server and the s44 index tier is the
whole answer):

| `lang` | `id` | command | args | root markers (in order) |
|---|---|---|---|---|
| `c`, `cpp`, `objc` | clangd | `clangd` | `--background-index`, `--clang-tidy`, `--offset-encoding=utf-8` | `compile_commands.json`, `.clangd`, `compile_flags.txt`, `.git` |
| `rust` | rust-analyzer | `rust-analyzer` | — | `Cargo.toml`, `.git` |
| `python` | pyright | `pyright-langserver` | `--stdio` | `pyproject.toml`, `setup.py`, `setup.cfg`, `.git` |
| `go` | gopls | `gopls` | — | `go.mod`, `.git` |
| `js`, `ts`, `jsx`, `tsx` | tsserver | `typescript-language-server` | `--stdio` | `tsconfig.json`, `package.json`, `.git` |
| `fortran` | fortls | `fortls` | — | `.fortls`, `.git` |
| `sh` | bashls | `bash-language-server` | `start` | `.git` |
| `fletch` | — | *(none)* | | the s44 index tier is the answer for our own language |

`--offset-encoding=utf-8` on the clangd row is belt and braces: clangd
honours the standard `positionEncodings` negotiation (§5), and the flag
makes the outcome visible in `ps` when something goes wrong.

Root resolution, pinned: walk up from `dirname(buffer->path)` to
`yew_ws_root()`; the **first directory containing any marker** wins, markers
tested in the config's order at each level; if none is found, the root is
`yew_ws_root()`. One server instance per `(cfg->id, root)` pair, shared by
every buffer under that root — not one per buffer, not one per workspace.

**Pitfall — never build the command line as a string.** s19 §9 makes this a
CI gate: the spawn uses `YewJobSpec.argv`, never `cmdline`, so a root path
containing a space, a quote or a newline cannot become shell syntax. The
root is passed as a `rootUri`, and the URI encoder (§4) is the only place
that path bytes are transformed.

### 2. Lifecycle: spawn → initialize → initialized → shutdown → exit

```c
typedef enum { YEW_LSP_SPAWNING, YEW_LSP_INITIALIZING, YEW_LSP_READY,
               YEW_LSP_SHUTTING_DOWN, YEW_LSP_DEAD } LspState;

typedef struct LspServer {
    const LspServerCfg *cfg;
    char      *root;              /* canonical, arena-owned              */
    u32        job;               /* s19 job id                          */
    RpcConn    rpc;               /* s45                                 */
    u8         state;
    LspCaps    caps;              /* §3                                  */
    u8         pos_enc;           /* YEW_POSENC_UTF8 | YEW_POSENC_UTF16  */
    Vec(u32)   docs;              /* buf_ids we have didOpen'd           */
    u32        restarts;  i64 first_restart_ms, next_try_ms;
    Bytebuf    stderr_tail;       /* last 8 KiB, for the crash message   */
} LspServer;

bool yew_lsp_start(Ed *ed, Buffer *b);        /* idempotent per (id,root) */
void yew_lsp_stop (Ed *ed, LspServer *s, bool graceful);
void yew_lsp_pump (Ed *ed);                   /* poll-loop pump phase     */
```

Handshake, exactly this order:

1. `yew_job_spawn` with `sink = YEW_SINK_FRAMED`, `cwd = root`, argv from
   the config, `timeout_ms = 0`. An exec failure arrives on s19's
   exec-status pipe and is reported precisely
   (`clangd: No such file or directory — install clangd or set
   lsp.servers.c.cmd`), then the language is marked "no server" for the
   session. A missing language server is a missing optional tool, not an
   error: the s44 tier keeps working.
2. `initialize` request (§3's params), `state = YEW_LSP_INITIALIZING`.
   Every buffer-scoped message is **queued**, not sent, while initializing.
3. On the response: record capabilities and `positionEncoding` (§5), send
   the `initialized` notification, `state = YEW_LSP_READY`, then flush the
   queue — which begins with a `didOpen` for every already-open buffer of
   that language under this root.
4. `shutdown` request → on its response, `exit` notification → close stdin
   → wait up to `YEW_JOB_TERM_GRACE_MS` (200 ms) → SIGTERM the process
   group → 200 ms → SIGKILL. s19's group-kill rule applies: always
   `-pgid`, never `pid`.
5. On editor quit, every server is shut down **before** the terminal is
   restored, with a hard 500 ms total budget; past it they are killed. A
   server that will not die must not delay the user's exit.

**Crash detection and backoff.** A framed connection dies when the job
reaps, when `RpcRx` reaches `YEW_RPCRX_DEAD`, or after 8
`YEW_RPC_MALFORMED` frames (s45 §7).

| Restart # | Delay before retry | User-visible |
|---|---|---|
| 1 | 250 ms | nothing (a startup race is not news) |
| 2 | 1 s | `clangd restarted` (INFO) |
| 3 | 4 s | `clangd restarted (3)` (WARN) |
| 4 | 16 s | `clangd restarted (4)` (WARN) |
| 5th within 5 min | — | **give up** |

The give-up message is plain and actionable, includes the last stderr line,
and names the way back:

```
clangd crashed 5 times in 5 minutes; LSP is off for this workspace.
last error: ../src/foo.c:1: fatal: cannot find compile_commands.json
run :lsp.restart to try again, or :lsp.log to read the server's output.
```

The counter resets after 5 minutes without a crash. `restarts` and the
timestamps are per `LspServer`, so one broken server never disables another.

### 3. The `initialize` params we send — written out and annotated

```json
{ "processId": 4711,
  "clientInfo": { "name": "yew", "version": "1.0.0" },
  "rootUri": "file:///home/u/proj",
  "workspaceFolders": [ { "uri": "file:///home/u/proj", "name": "proj" } ],
  "initializationOptions": { },
  "capabilities": {
    "general": { "positionEncodings": ["utf-8", "utf-16"] },
    "textDocument": {
      "synchronization": { "dynamicRegistration": false,
                           "willSave": false, "willSaveWaitUntil": false,
                           "didSave": true },
      "completion": { "contextSupport": true,
        "completionItem": { "snippetSupport": false,
                            "documentationFormat": ["plaintext"],
                            "insertReplaceSupport": false,
                            "resolveSupport": { "properties":
                                ["documentation", "detail"] } },
        "completionItemKind": { "valueSet": [1,2,3,4,5,6,7,8,9,10,11,12,
                                             13,14,15,16,17,18,19,20,21,
                                             22,23,24,25] } },
      "hover":          { "contentFormat": ["plaintext"] },
      "signatureHelp":  { "signatureInformation": {
                            "documentationFormat": ["plaintext"],
                            "parameterInformation": {
                              "labelOffsetSupport": true } } },
      "definition":     { "linkSupport": false },
      "declaration":    { "linkSupport": false },
      "typeDefinition": { "linkSupport": false },
      "implementation": { "linkSupport": false },
      "references":     { },
      "documentHighlight": { },
      "documentSymbol": { "hierarchicalDocumentSymbolSupport": true },
      "rename":         { "prepareSupport": false },
      "publishDiagnostics": { "relatedInformation": true,
                              "versionSupport": true,
                              "tagSupport": { "valueSet": [1, 2] } }
    },
    "workspace": { "applyEdit": false,
                   "configuration": true,
                   "workspaceFolders": true,
                   "workspaceEdit": { "documentChanges": true,
                                      "failureHandling": "abort",
                                      "resourceOperations": [] } },
    "window": { "showMessage": { }, "workDoneProgress": false }
  } }
```

Every non-obvious line, and why:

| Field | Value | Why |
|---|---|---|
| `positionEncodings` | `["utf-8","utf-16"]`, in that order | preference order; §5. Servers that ignore it default to utf-16 and we convert |
| `snippetSupport` | **false** | Sprint 47 pins the downgrade policy: no snippet expansion in 1.0. Advertising true and then stripping placeholders would make servers send `${1:int}` we throw away — asking for plain text gets a better `insertText` from most servers |
| `documentationFormat` | `["plaintext"]` only | there is no markdown renderer in 1.0. A server told we accept markdown sends us backticks and tables to display raw |
| `insertReplaceSupport` | false | we compute our own replace span from the stem (s44 §5); accepting the server's two-range form adds a second source of truth for the same decision |
| `linkSupport` | false | `LocationLink` adds an origin range we do not use; the flat `Location` shape is one less parse path |
| `hierarchicalDocumentSymbolSupport` | true | Sprint 47's breadcrumbs need the tree; the flat `SymbolInformation` fallback is still handled |
| `prepareSupport` | false | `prepareRename` is an extra round trip to learn a range we recompute anyway from the word under the cursor (s16) |
| `versionSupport` (diagnostics) | true | lets us drop diagnostics for a document version we have moved past (§7) instead of showing squiggles under the wrong text |
| `tagSupport` | `[1,2]` | Unnecessary and Deprecated; rendered as `dim` and `strike` respectively (s05 attr bits 1 and 8) |
| `applyEdit` | **false** | a server may not push edits into the user's buffers unprompted. Every edit we apply is one the user asked for and confirmed (invariant 1). A server that wants to edit must answer a request we made |
| `resourceOperations` | `[]` | no create/rename/delete of *files* from a server in 1.0. Sprint 47's rename applies text edits only and says so |
| `failureHandling` | `"abort"` | matches Sprint 47's all-or-nothing rule; we tell the server what we will actually do |
| `workDoneProgress` | false | no progress UI in 1.0; `$/progress` notifications are accepted and dropped |
| `processId` | our pid | lets the server exit if we die without saying goodbye. Omitting it is the classic orphaned-clangd bug |

The response's `capabilities` object is flattened into a bitfield the
feature layer switches on:

```c
typedef struct LspCaps {
    u32  bits;                  /* YEW_LSPC_* per method                 */
    u8   sync_kind;             /* 0 none, 1 full, 2 incremental         */
    bool resolve_completion;
    char trigger_chars[16];     /* completion trigger characters         */
    char sig_trigger[8];
    bool save_include_text;
} LspCaps;
bool yew_lsp_has(const LspServer *s, u32 cap);
```

**Pitfall — a capability may be `true`, an options object, or absent.**
`{"hoverProvider": true}`, `{"hoverProvider": {"workDoneProgress": true}}`
and no key at all are three shapes for two answers. `yew_lsp_has` treats
"present and not `false` and not `null`" as supported; a test drives all
three shapes for every capability. Getting this wrong disables half the
features against clangd and none against gopls, which is how it ships
unnoticed.

If the server answers `sync_kind == 0` (no sync), we log it and treat the
server as unusable — every feature needs a document. If it answers `1`
(full), §6 sends whole-document changes instead of ranges; the code path
exists and is tested, because two of the servers in §1's table have shipped
`full`-only versions.

### 4. Documents, URIs, and the version counter

```c
typedef struct LspDoc {
    u32   buf_id;
    char *uri;             /* file://… , arena-owned                     */
    i64   version;         /* monotonic; ++ on every didChange           */
    u64   sent_gen;        /* tb->gen at the last didChange we sent       */
    bool  open;
    Vec(LspChange) pending;/* §6                                          */
} LspDoc;

void yew_lsp_uri_of_path(Bytebuf *out, const u8 *path, u32 n);
bool yew_lsp_path_of_uri(Bytebuf *out, const u8 *uri, u32 n);
```

URI encoding, pinned: `file://` + the canonical absolute path with every
byte outside `A–Z a–z 0–9 - . _ ~ /` percent-encoded as `%XX` with
**uppercase** hex. Path separators stay literal. Decoding accepts lowercase
hex and both `file://` and `file:///`. Bytes ≥ 0x80 are encoded per byte,
so a filename with invalid UTF-8 round-trips (invariant 2 again).
**Pitfall:** a Windows-style `file:///C:/…` is not a case we support and
`yew_lsp_path_of_uri` returns false for any URI whose scheme is not `file`
— a server returning an `untitled:` or `jdt:` URI in a rename is refused
rather than written to a path we invented.

`version` starts at 1 on `didOpen` and increments on every `didChange`
notification we send. It is the **document generation** the server echoes in
`publishDiagnostics` and it is what §7 filters on.

### 5. Positions — the UTF-16 pitfall

LSP `Position` is `{line, character}` where `line` is a 0-based line index
(**the same 0-based `LineNo` we use internally — there is no ±1 anywhere in
this sprint; display adds 1, and only s15 does that**) and `character` is,
by default, an offset in **UTF-16 code units** within the line. We speak
bytes and graphemes. A wrong conversion is not a cosmetic bug: it moves
every rename edit, every goto-def, and every diagnostic range by a silent
offset that only shows on lines containing non-ASCII, which is exactly the
input nobody tests by hand.

Two defences, in order:

**Defence 1 — negotiate it away.** We advertise `["utf-8","utf-16"]`. If the
server's `capabilities.positionEncoding` is `"utf-8"`, `character` is a
**byte** offset within the line and conversion is `pos.v − line.lo` in both
directions: no scan, no tables, no risk. clangd, rust-analyzer and gopls all
support it. `s->pos_enc` records the outcome and `:lsp.info` shows it.

**Defence 2 — one conversion site, in `src/unicode/`.** When the server
insists on utf-16, we convert — and the conversion lives with every other
coordinate conversion, under the architecture law, beside `GCol`/`CCol`:

```c
/* src/unicode/u16.h — a new member of the typed-coordinate family. */
typedef struct { u64 v; } U16Col;      /* UTF-16 code units from line start */

U16Col  yew_off_to_u16col(const TextBuf *tb, Span line, ByteOff pos);
ByteOff yew_u16col_to_off(const TextBuf *tb, Span line, U16Col c); /* clamps */
```

The whole table, and every row is a test:

| Decoded codepoint | UTF-16 code units | UTF-8 bytes |
|---|---|---|
| U+0000–U+007F | 1 | 1 |
| U+0080–U+07FF | 1 | 2 |
| U+0800–U+FFFF (non-surrogate) | 1 | 3 |
| U+10000–U+10FFFF | **2** (a surrogate pair) | 4 |
| U+DC80–U+DCFF (s02 invalid-byte escape) | **1** | **1** |
| U+0009 TAB | 1 | 1 |

- The astral row is the whole problem: an emoji is 4 bytes, 1 codepoint, 1
  grapheme, **2** UTF-16 units, and 2 cells. Five different numbers for one
  character, and code that reaches for the wrong one compiles fine.
- The escape row is the subtle one. Our invalid-byte escape *is* a lone low
  surrogate, so it is exactly **one** UTF-16 code unit — while
  `yew_cp_width` says **4** (it renders as `<FF>`). Display width and UTF-16
  length are different functions of the same codepoint; conflating them
  corrupts every position on any line containing an invalid byte.
- TAB is 1 unit here. There is no tab-stop arithmetic in this file — a
  protocol position is not a display column, and `tabw` must not appear in
  `u16.c`. Grep gate in the DoD.
- **Clamping:** a `character` landing between the two units of a surrogate
  pair (a server that computed a position with a different string
  representation) resolves to the **start** of that character, never inside
  it, and logs once per server. A `character` past the line's content
  clamps to the line end **before** the EOL bytes — LSP line content
  excludes the line terminator, and a CRLF buffer must not produce a
  position on the `\r` (s09 pins `\r\n` as one cluster; the same rule
  applies here).
- Cost: `yew_off_to_u16col` is one decode pass over the line prefix, driven
  from `yew_textiter_*`. §6's budget note covers it, and Defence 1 means it
  is usually not on the path at all.

```c
/* The single place a Position is built. There is no other. */
static void lsp_pos_of_off(const LspServer *s, const TextBuf *tb,
                           ByteOff off, i64 *line, i64 *ch) {
    LineNo l = yew_textbuf_line_of(tb, off);
    Span   sp = yew_textbuf_line_span(tb, l);
    *line = (i64)l.v;                      /* 0-based BOTH sides; no +1  */
    *ch   = (s->pos_enc == YEW_POSENC_UTF8)
          ? (i64)(off.v - sp.lo)           /* bytes: identity            */
          : (i64)yew_off_to_u16col(tb, sp, off).v;
}
```

### 6. Document sync — didOpen, incremental didChange, didSave, didClose

```c
typedef struct LspChange {          /* one contentChanges element         */
    i64 sl, sc, el, ec;             /* start/end line+character, PRE-op   */
    const u8 *text; u32 len;        /* arena-owned copy; empty for delete */
} LspChange;

void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len);
```

`yew_lsp_note_edit` is a **`yew_edit_notify_pre` consumer** (s43 §3), and
the order in that fixed call list is load-bearing: it runs *before*
`yew_textbuf_insert`/`_delete` mutates the tree, so the positions it
computes describe the document the server currently believes in. That is
precisely what LSP's `contentChanges` array wants — the spec says the
changes "are applied in order", each expressed against the state produced by
the previous ones, which is exactly what a sequence of pre-op snapshots
gives you for free.

| Edit | `contentChanges` element |
|---|---|
| insert of `n` bytes at `a` | `{ range: { start: P(a), end: P(a) }, text: <the n bytes> }` |
| delete of `[a, a+n)` | `{ range: { start: P(a), end: P(a+n) }, text: "" }` — both endpoints in **pre-op** coordinates |

Both endpoints of a delete are computed before the deletion, which is the
whole trick and the whole pitfall: computing `end` afterwards yields a
position in a document that no longer contains the text being deleted, and
the resulting range is short by exactly the deleted length. The unit test
for this deletes a multi-line region and compares the emitted range against
a hand-computed one.

Pending changes accumulate on `LspDoc.pending` and are flushed as **one**
`didChange` per buffer per event-loop iteration, in the pump phase, with
`version++`. Batching is what keeps a 40-keystroke burst from becoming 40
notifications, and the array preserves order so the server reconstructs the
same bytes.

Other rules:

- **`sync_kind == 1` (full)**: the pending array is discarded and one
  `{ text: <whole buffer> }` element is sent, materialised through
  `yew_textiter_*` into the outbound `Bytebuf` with no intermediate copy.
- **Overflow**: `YEW_LSP_PENDING_MAX` = 512 elements. Past it we drop the
  array and send a full-document change for that flush, logging once. A
  degraded-but-correct sync always beats a clever-but-wrong one.
- `didOpen` sends the whole buffer text and `languageId`. A buffer with
  `meta.binary` is never opened.
- `didSave` is sent only when `caps.save_include_text` is known;
  `includeText` is sent only when the server asked for it.
- `didClose` on buffer close, on language change, and before any server
  restart (the new process knows nothing; every doc is re-`didOpen`ed).
- Budget: `yew_lsp_note_edit` is on the keystroke path. With
  `pos_enc == utf-8` it is two `yew_textbuf_line_of` calls (O(log n)) and a
  subtraction. With utf-16 it adds a decode of the line prefix; the perf
  gate is p99 ≤ 100 µs on a 4 KiB line, and `:lsp.info` shows the encoding
  so a user seeing latency can see why.

### 7. Generation counters — dropping answers about a vanished past

Every request that describes a position or a document carries the state it
was asked about:

```c
typedef struct LspGen { u32 buf_id; i64 version; u64 tb_gen; } LspGen;
bool yew_lsp_gen_current(const Ed *ed, const LspGen *g);
```

`yew_rpc_call`'s `RpcPending.gen`/`buf_id` (s45 §7) carry it. The pump-phase
dispatcher checks `yew_lsp_gen_current` **before** invoking a response
callback, and on failure drops the response, counts it in
`server.dropped_stale`, and never calls the callback at all. Consequences of
putting the check there rather than in each feature: Sprint 47 cannot forget
it, and the drop is uniform across nine methods.

| Response kind | Dropped when | Why |
|---|---|---|
| position-bearing (completion, hover, signatureHelp, definition, references, documentHighlight) | `tb_gen != tb->gen` | the offsets in the answer index text that has moved |
| `documentSymbol` | `version != doc->version` | a whole-file answer about an older file |
| `rename` (a WorkspaceEdit) | any affected buffer's `tb_gen` changed since the request | applying it would edit the wrong bytes — Sprint 47's validation pass re-checks anyway, belt and braces |
| `publishDiagnostics` | notification's `version` present and `< doc->version` | squiggles under text that has since been retyped |
| `publishDiagnostics` with no `version` | never dropped, but marked `stale` if `doc->version` advanced since the last flush; rendered `dim` until refreshed | the server did not tell us; showing nothing is worse than showing something slightly old, but we say so |

A server also has its own word for this race: `-32801 ContentModified`,
which s45's error table already silently drops. Both paths exist because
either side can notice first.

### 8. Diagnostics — store, gutter, underline, echo, picker

```c
typedef enum { YEW_DIAG_ERROR = 1, YEW_DIAG_WARN, YEW_DIAG_INFO,
               YEW_DIAG_HINT } DiagSev;
enum { YEW_DIAGT_UNNECESSARY = 1u << 0, YEW_DIAGT_DEPRECATED = 1u << 1 };

typedef struct Diagnostic {
    MarkId lo, hi;        /* s09 marks: the range survives edits          */
    Span   cache;         /* resolved byte span, revalidated per frame    */
    u32    line;          /* first line, for the gutter and the picker    */
    u8     sev, tags;
    u32    server;        /* which LspServer produced it                  */
    char  *code, *source, *message;   /* arena-owned                      */
} Diagnostic;

typedef struct DiagStore {
    Vec(Diagnostic) d;    /* sorted by (line, lo) with yew_sort_stable    */
    Arena  arena;
    u32    n[5];          /* counts per severity, index 0 unused          */
    u32    gen;           /* bumped on replace; render cache key          */
    bool   stale;
} DiagStore;              /* one per Buffer                               */

void yew_diag_replace(Ed *ed, Buffer *b, u32 server, const JsonValue *arr,
                      i64 version);
u32  yew_diag_at_line(const Buffer *b, LineNo l, const Diagnostic **out,
                      u32 max);
const Diagnostic *yew_diag_at_point(const Buffer *b, ByteOff p);
u32  yew_diag_list(Ed *ed, PickItem *out, u32 max);      /* s26 picker feed */
```

`publishDiagnostics` is a **whole-file replace** per server, not a merge:
the store keeps one arena per `(buffer, server)` and `yew_diag_replace`
resets it. Ranges become **marks** at receipt (`lo` bias LEFT, `hi` bias
RIGHT) so that when the user types inside a diagnostic's range, the squiggle
grows with the text instead of drifting off it; the `cache` span is
recomputed from the marks whenever `tb->gen` changes.

Severity table — glyph, cell, attrs, roles:

| Severity | Gutter glyph (cell 0) | Underline attr | Theme role | Statusline |
|---|---|---|---|---|
| `ERROR` | `✗` | `undercurl`, → `underline` without truecolor (s05 §2) | `diag.error` | `E:n` |
| `WARN` | `▲` | `undercurl` → `underline` | `diag.warn` | `W:n` |
| `INFO` | `●` | `underline` | `diag.info` | — |
| `HINT` | `·` | none (gutter glyph only) | `diag.hint` | — |
| tag `Unnecessary` | — | plus `dim` | `diag.unnecessary` | — |
| tag `Deprecated` | — | plus `strike` | `diag.deprecated` | — |

- All four glyphs are one cell under the default `ambiguous_wide = false`,
  and s43's `yew_gutter_sign_set` asserts exactly that and falls back to
  ASCII `E W i h` for a theme override that is not. The severity glyphs are
  themable in Sprint 41; the assertion is what keeps a theme from smearing
  the gutter.
- Highest severity on a line wins the gutter cell; the count is not shown
  (a gutter is two cells wide).
- Underline spans are handed to the renderer as byte spans on the s21
  `MatchOverlay` machinery — s21 §7 reserved this explicitly (*"the overlay
  type is deliberately pattern-agnostic"*) — with an LSP-owned instance
  keyed on `DiagStore.gen` instead of `pat_gen`. A zero-width diagnostic
  range (servers emit them for "expected `;` here") is widened to one
  grapheme so it is visible at all.
- **Message-row echo:** the diagnostic under the cursor is shown in the
  message row. It must not fight with real messages, so it does **not** go
  through `yew_msg`; s15's surface grows one function:

```c
/* A low-priority, self-refreshing line. Displaced by any yew_msg and
 * restored when that message expires. Never expires on its own; cleared
 * when the cursor leaves the diagnostic.                                */
void yew_msg_hint(Ed *ed, MsgSev sev, const char *fmt, ...);
```

  Format: `E ✗ [clang] use of undeclared identifier 'foo' (undeclared_var)`
  — severity glyph, `source` in brackets, message, `code` in parentheses
  when present. Truncated with `…` per s15's rules. Multiple diagnostics on
  the cursor's line show the highest severity and ` (+2 more)`.
- **Diagnostic-list picker:** `ed.lsp.diagnostics` (`:lsp.diagnostics`) is
  a `PickerSpec` over `yew_diag_list` — s26's one list picker, no new
  chrome. `PickItem.payload` is the diagnostic's index into an arena array
  the spec's `ctx` owns (s26 pins payload as an **identity, never a row
  index**), so re-filtering cannot mis-route an `accept`. Rows are
  `path:line:col  ✗ message`, sorted by (severity, path, line), filtered by
  the picker's fuzzy stem. `Enter` pushes the jumplist (`yew_jump_push`,
  s21) and jumps. Scope toggles between current buffer and workspace with
  `<tab>`.
- Statusline: the `badges` segment s15 reserved becomes `E:3 W:12` in the
  `diag.error`/`diag.warn` roles, plus the server id and a `…` while any
  server is `YEW_LSP_INITIALIZING`. Priority 5, so it is dropped first on a
  narrow terminal — a badge is never worth the file path.

### 9. Commands

Registered in the `lsp` domain (already in s13's closed list):

| Command | This sprint | Notes |
|---|---|---|
| `ed.lsp.info` | yes | server id, state, root, `positionEncoding`, capability list, restart count, doc versions |
| `ed.lsp.log` | yes | opens the server's stderr scratch buffer |
| `ed.lsp.restart` | yes | restart the server for the current buffer; resets the give-up counter |
| `ed.lsp.stop` | yes | graceful shutdown for this root |
| `ed.lsp.diagnostics` | yes | §8 picker |
| `ed.lsp.diag_next` / `.diag_prev` | yes | jump to the next/previous diagnostic in the buffer, wrapping, pushing the jumplist |
| `ed.lsp.goto_def`, `.goto_decl`, `.goto_type`, `.references`, `.hover`, `.rename`, `.symbols`, `.signature`, `.complete` | **`YEW_CMD_DEFERRED`** | `command not implemented yet: ed.lsp.goto_def lands in Sprint 47` |

In a `MODULES=""` build every one of them exists and reports s00's pinned
`yew_mod_require` message (s45 §8).

### 10. Defer

- All navigation and edit features → **Sprint 47**, by name, through
  `YEW_CMD_DEFERRED` (§9). `YEW_TXN_LSP` still hard-errors naming Sprint 47
  (s10 DoD 11 is unchanged by this sprint — nothing here edits a buffer).
- Completion requests → Sprint 47; the s44 index tier is the completion
  answer today, and `LspCaps.trigger_chars` is parsed and unused.
- `workspace/symbol`, code actions, formatting, inlay hints, semantic
  tokens, call hierarchy → **not in 1.0**. `client.h` lists them as
  permanent non-goals with the reason (each needs a UI surface that does not
  exist, and 1.0's LSP scope is fixed by `00-decisions.md`).
- `$/progress`, `window/workDoneProgress/create`: accepted and answered
  (create → `null` result, so a server is never blocked), rendered nowhere.
- `workspace/configuration`: answered from the option model with `null` per
  requested section until Sprint 36 wires real values.
- Multiple servers per language (a linter beside a compiler) → post-1.0;
  the `(id, root)` map is keyed to allow it, and one server per language is
  what §1's table expresses.

## Testing Strategy

- **Unit — positions** (`tests/unit/test_u16.c`): the §5 table, every row;
  `off→u16col→off` round-trip at **every** grapheme boundary of the s02
  golden corpus (ASCII, CJK, astral emoji, ZWJ family, flag pair, keycap,
  combining stacks, an invalid byte, CRLF); a `character` inside a surrogate
  pair clamping to the character start; `character` past line end clamping
  before `\r`; a line of 100 000 astral characters (the counter must not
  overflow a u32 anywhere).
- **Differential test — the DoD's centrepiece**
  (`tests/unit/test_u16_diff.c`): for every line of a 200-line fixture built
  from the s02 corpus, compute `character` for every byte offset **both
  ways** — through `yew_off_to_u16col`, and through a deliberately naive
  reference that encodes the line prefix to UTF-16 with a separate 30-line
  encoder written in the test file — and assert they agree at every offset.
  Then do the same for utf-8 encoding (identity) and assert that a
  *server-visible round trip* (offset → Position → offset) is the identity
  for both encodings at every offset of every line. A single disagreement
  fails the build. This is what stands between us and silently corrupting
  every rename.
- **Unit — sync** (`test_lsp_sync.c`): the §6 table with a recording
  transport — insert, delete, multi-line delete (the pre-op `end` case),
  insert at line start, insert at EOF, delete crossing CRLF, a 40-keystroke
  burst producing one `didChange` with 40 ordered elements; the pending
  overflow producing one full change; `sync_kind == 1` sending whole text;
  `version` monotonic; `didClose`/`didOpen` around a restart.
- **Unit — capabilities** (`test_lsp_caps.c`): each capability in all three
  shapes (`true`, options object, absent) plus `false` and `null`;
  `positionEncoding` answered as `utf-8`, `utf-16`, absent, and as an
  unknown string (fall back to utf-16 with a WARN).
- **Unit — lifecycle** (`test_lsp_life.c`, driving s45's `fakelsp` helper):
  the full handshake with messages queued during `INITIALIZING` and flushed
  in order; a server that never answers `initialize` (timeout at 60 s,
  simulated via an injectable clock); the five-row backoff table with the
  exact user-visible strings; the give-up message including the last stderr
  line; `restarts` resetting after 5 minutes; shutdown/exit followed by
  SIGTERM then SIGKILL, with `kill(-pgid, 0) == ESRCH` afterwards; exec
  failure reported through s19's exec pipe distinctly from exit 127.
- **Unit — diagnostics** (`test_lsp_diag.c`): whole-file replace semantics;
  marks tracking through inserts and deletes inside/around a range; a
  zero-width range widened to one grapheme; the severity table (glyph, cell,
  attr, role) row by row; highest-severity-wins per line; version filtering
  (older version dropped, missing version marked stale); `yew_diag_at_point`
  on boundaries; counts.
- **Unit — URIs** (`test_lsp_uri.c`): ≥ 25 round-trip vectors including
  spaces, `#`, `?`, `%`, a newline, UTF-8, an invalid byte, and a
  non-`file:` scheme (refused).
- **Pty goldens** (`tests/pty/lsp_diag_*.golden`): gutter glyphs at all four
  severities; undercurl at truecolor and its underline fallback at 256;
  the message-row echo and its displacement by a real `yew_msg` and
  restoration afterwards; the statusline badge appearing, counting, and
  being dropped first on a 40-column terminal; the diagnostic picker.
- **Script** (`tests/script/lsp_*`, `fakelsp`-driven): open → edit → save →
  close producing exactly the expected notification sequence; a stale
  response dropped without reaching its callback.
- **Fuzz** (`tests/fuzz/fuzz_lsp_msg.c`): mutated `publishDiagnostics`,
  `initialize` results and responses fed to the real handlers. Invariants:
  no crash, no unbounded allocation, every string field length-checked, a
  diagnostic range outside the buffer clamped rather than asserted, and the
  server never left in a state other than the five in `LspState`.
- **Perf** (`tests/perf/perf_lsp.c`, baselines committed):
  `yew_lsp_note_edit` p99 ≤ 100 µs on a 4 KiB line in both encodings; a
  10 000-diagnostic store renders a viewport within the frame budget; a
  50 MiB burst of server output keeps keypress p99 ≤ 5 ms (s19's read budget
  doing its job through the framed sink).

## Definition of Done

1. `make test` green on gcc and clang for `MODULES="lsp …"` and
   `MODULES=""`, zero warnings; ASan/UBSan/valgrind lanes clean.
2. **The differential position test passes at every byte offset of every
   line of the corpus fixture, in both encodings, in both directions** —
   zero disagreements. Breaking `yew_off_to_u16col` for astral characters
   fails it (proved once by a deliberately wrong variant in the test file).
3. `grep -n 'tabw\|width\|cell' src/unicode/u16.c` → empty; all position
   conversion lives in `src/unicode/`, and
   `grep -rn 'yew_off_to_u16col\|yew_u16col_to_off' src/mod/lsp/` shows call
   sites only, inside `lsp_pos_of_off` and its inverse.
4. No ±1 on line numbers anywhere in `src/mod/lsp/`:
   `grep -rn 'line + 1\|line - 1\|\.v + 1' src/mod/lsp/` → empty, and a unit
   test asserts a diagnostic on internal line 0 reports LSP line 0 and
   displays as line 1.
5. Incremental `didChange` correctness: every §6 row asserted against a
   hand-computed expectation, including the multi-line delete whose `end` is
   pre-op; a 40-key burst is exactly one notification with 40 ordered
   elements; the overflow path sends one full change.
6. Capability probing: all five shapes × every capability tested;
   `positionEncoding` negotiation verified against `fakelsp` answering
   `utf-8`, `utf-16`, absent, and garbage.
7. Lifecycle: the backoff table's five rows produce the exact messages; the
   give-up message contains the last stderr line; after `stop`,
   `kill(-pgid, 0)` → `ESRCH`, zero zombies, zero leaked fds over 200
   spawn/kill cycles.
8. Stale responses are never applied: with an injected 200 ms server delay
   and a scripted edit in between, `dropped_stale` > 0 and the recording
   callback was never invoked.
9. Diagnostics survive edits: after 1 000 random edits around 50
   diagnostics, every surviving range still covers the same text it did (or
   is collapsed at the deletion point per s09's bias rules), verified
   against a naive oracle.
10. Every severity glyph is one cell (`yew_cluster_width == 1`), asserted
    for the default theme and for an `ambiguous_wide = true` run; a 2-cell
    override falls back to ASCII with one log line.
11. `yew_msg_hint` is displaced by `yew_msg` and restored on its expiry
    (pty golden), and never expires on its own.
12. Perf: `yew_lsp_note_edit` p99 ≤ 100 µs in both encodings; baselines
    committed under `tests/perf/baselines/lsp.txt`.
13. Sprint 47 delivered all nine feature commands; current closeout proves
    each routes through its implementation and exact capability gate rather
    than returning `YEW_CMD_DEFERRED`. `workspace/symbol`, code actions,
    formatting, inlay hints, and semantic tokens remain documented in
    `client.h` as permanent 1.0 non-goals, not stubs.
14. ≥ 220 assertions across the seven new unit files; determinism lane green
    on the new pty goldens.
