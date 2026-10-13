# Sprint 45: Bespoke JSON and JSON-RPC — The Module Boundary Opens

## Prerequisites

- Sprint 0 — `Arena` (`arena_alloc`, `arena_free_all`), `Vec(T)`, `Bytebuf`
  (`bytebuf_append`, `bytebuf_printf`, `bytebuf_push_u8`), `Strmap`
  (insertion-ordered), `yew_sort_stable`, `yew_log`, `YEW_BUG`, the exit
  codes — **and the Makefile's `MODULES=` plumbing**: `KNOWN_MODS`,
  `MODDIR_lsp := lsp`, `-DYEW_WITH_LSP=…`, `$(BUILD)/mods.stamp`, the
  per-module `shim.c` rule, and
  `bool yew_mod_require(YewMod m, char *err, size_t errsz)` with its pinned
  message `this build has no lsp module; rebuild with 'make MODULES="… lsp"'`.
- Sprint 1 — the unit harness, `make test`, `make fuzz`, `scripts/bans.sh`,
  the CI lanes (gcc/clang/ASan/UBSan/determinism).
- Sprint 2 — `yew_utf8_validate(s, len)` (first bad offset, else `len`),
  `yew_utf8_encode`, `yew_utf8_decode`, `yew_utf8_len`, `YEW_CP_ESC_LO`
  /`YEW_CP_ESC_HI` (U+DC80–DCFF), `yew_utf8_escape_of/escape_byte`, and the
  **`setlocale`/`wchar.h` ban** in `scripts/bans.sh` — §4 leans on it.
- Sprint 19 — `YewJobSpec`, `YewJobSink`, `YewJob`, `yew_job_spawn`,
  `yew_job_collect_fds`, `yew_job_pump`, `yew_job_reap`, `yew_job_signal`,
  `YEW_JOB_READ_BUDGET`, `yew_job_safe_prefix`, the `pipe2(O_CLOEXEC)`
  discipline, the exec-status pipe, and its §10 promise that *"LSP/AI/git
  reuse of `yew_job_*`: Sprints 45/46, 48, 51 add framed sinks; the
  spawn/reap/poll core is final here."* This sprint adds that sink.
- Binding: `00-decisions.md` — *"No JSON lib — all bespoke"*, *"LSP: bespoke
  JSON + JSON-RPC over stdio pipes"*, *"Excluded modules hard-error with a
  clear message, never silently no-op"*; invariants 2 (no byte confusion),
  3 (no silent stubs), 5 (determinism), 7 (bespoke first), 8 (single-
  threaded core); `01-architecture.md` (`src/mod/lsp/ json.c jsonrpc.c
  client.c features.c`; process model).

Assumption beyond earlier index entries: Sprint 14's event loop offers
`i64 yew_now_ms(void)` on `CLOCK_MONOTONIC` (s13 uses monotonic ms for chord
deadlines; if the accessor is spelled differently, `jsonrpc.c` adapts at two
call sites).

## Goals

Open the LSP module boundary and land the two layers underneath every later
LSP sprint: a bespoke JSON reader/writer (`src/mod/lsp/json.c|h`) over an
arena-allocated `JsonValue` tree, with UTF-8 validation through s02,
surrogate-pair recombination and a pinned lone-surrogate policy, a number
policy that never silently loses an integer, a depth limit, and a fuzzer;
and JSON-RPC 2.0 framing (`src/mod/lsp/jsonrpc.c|h`) as a **resumable state
machine** over `Content-Length` pipes, with the request/response/notification
correlation table, id generation, error-object mapping, and a transport
registered with the poll loop through a new `YEW_SINK_FRAMED` job sink.
`src/mod/lsp/shim.c` lands beside them so `MODULES=""` still builds and
`:lsp.*` hard-errors. Server lifecycle, capabilities and document sync are
Sprint 46; features are Sprint 47. Nothing in this sprint knows what a
`TextBuf` is — `json.c` and `jsonrpc.c` speak bytes and are unit-testable
without an editor.

## Deliverables

### 1. The decision: a dedicated `JsonValue`, not `FlValue`

Fletch already has a tagged value union with maps, lists, strings, i64 and
f64 (`02-fletch.md`), and reusing it would save perhaps 600 lines. We are
not reusing it. The reasoning, written out because it will be questioned:

1. **The module boundary is a real boundary.** `MODULES=lsp` must build in a
   configuration where Fletch is absent. Fletch is core *today*, but a
   dependency edge from `src/mod/lsp/` into `src/fl/` makes the LSP module
   permanently unexcisable *in the other direction*: you could no longer
   build a stripped profile without a bytecode VM and a garbage collector.
   Sprint 57's `x86_64-linux-musl` size budget is exactly the place that
   bites, and by then the edge would be load-bearing in four files.
2. **GC lifetime is wrong for the pump phase.** `FlValue` is managed by a
   mark-sweep collector with an explicit root set, collecting *at opcode
   boundaries*. A JSON message is parsed in the poll-loop pump phase, where
   there is no VM frame to root it; every parsed message would need a
   temporary root, and a collection triggered between two frames could free
   a half-built tree. An arena freed per message is O(1) teardown, cannot
   leak into GC pressure, and has no interaction with the VM's schedule.
3. **The interner is append-only and the input is not ours.** Fletch strings
   are interned, and s00's `Interner` has no delete. A buggy or hostile
   language server can mint unbounded distinct object keys; interning them
   grows a structure that never shrinks, for the life of the process. Arena
   strings die with the message.
4. **The semantics genuinely differ.** JSON has one lexical number type with
   no int/float distinction and no bound on precision — a conforming reader
   must decide, from the *lexeme*, whether an i64 is exact (§3), and must
   record that it could not. Fletch has separate `int`/`float` types with
   language-level conversion rules that are not JSON's. JSON also permits
   duplicate keys with implementation-defined behaviour (RFC 8259 §4);
   Fletch's map is insertion-ordered with put-replaces semantics and no
   notion of "the key appeared twice". Encoding those choices into `FlValue`
   would mean either changing Fletch to suit a wire format or lying about
   what arrived.
5. **Cost is bounded and one-directional.** `json.c` is ~600 lines with a
   fuzzer. The coupling it avoids is permanent and would be discovered at
   the worst moment (a size gate, or a Fletch value-model change in Sprint
   30 breaking the LSP client).

Recorded here so nobody "de-duplicates" the two value models later.

### 2. Value model — `src/mod/lsp/json.h`

```c
typedef enum { YEW_JS_NULL = 0, YEW_JS_BOOL, YEW_JS_INT, YEW_JS_REAL,
               YEW_JS_STR, YEW_JS_ARR, YEW_JS_OBJ } JsonKind;

enum {  /* JsonValue.flags */
    YEW_JSF_LOSSY_NUM = 1u << 0,  /* an integer lexeme did not fit i64    */
    YEW_JSF_RAW_BYTE  = 1u << 1,  /* string carries a §4 escaped raw byte */
    YEW_JSF_DUP_KEY   = 1u << 2,  /* object had a duplicate key           */
};

typedef struct JsonValue JsonValue;
typedef struct JsonMember { const u8 *key; u32 klen; JsonValue *val; }
    JsonMember;

struct JsonValue {
    u8 kind, flags; u16 _pad;
    union {
        bool   b;
        i64    i;
        double d;
        struct { const u8 *p; u32 len; } s;   /* NOT NUL-terminated       */
        struct { JsonValue **v; u32 n; } arr;
        struct { JsonMember *m; u32 n; } obj; /* INSERTION ORDER          */
    };
};
_Static_assert(sizeof(JsonValue) <= 24, "json value bloat");
```

- **Strings are length-carrying and never NUL-terminated.** `\u0000` is
  legal JSON and produces a NUL byte in the middle of a string. A
  NUL-terminated representation silently truncates it, and the truncation
  lands in a rename's `newText`. Pitfall pinned in the header; the fuzzer
  drives it.
- **Objects preserve insertion order**, in a flat `JsonMember` array with a
  linear `yew_json_get`. Determinism doctrine: no hashing, no sorting, and
  therefore no iteration order that depends on a seed or a compiler. LSP
  objects have single-digit member counts; the linear scan is faster than a
  hash for every message we will ever see.
- **Duplicate keys: last value wins, first slot keeps its position.** A
  second `"id"` replaces the value in the slot the first one created, sets
  `YEW_JSF_DUP_KEY` on the object, and logs at WARN. Chosen over
  "first wins" and over "error" because it matches every other JSON consumer
  a server will have been tested against, and over silence because a
  duplicate key in a protocol message means the server is malformed and we
  want that in the log when a bug is filed.

Accessors — total, never assert on a type mismatch, so parse code is
straight-line:

```c
const JsonValue *yew_json_get   (const JsonValue *o, const char *key);
const JsonValue *yew_json_at    (const JsonValue *a, u32 i);
u32              yew_json_len   (const JsonValue *v);      /* arr/obj/str  */
i64              yew_json_int   (const JsonValue *v, i64  dflt);
double           yew_json_real  (const JsonValue *v, double dflt);
bool             yew_json_bool  (const JsonValue *v, bool dflt);
const u8        *yew_json_str   (const JsonValue *v, u32 *len); /* NULL ok */
bool             yew_json_streq (const JsonValue *v, const char *s);
/* Dotted path: yew_json_path(root, "result.range.start.line"). Array
 * indices are not supported — LSP's shapes never need one mid-path.     */
const JsonValue *yew_json_path  (const JsonValue *root, const char *path);
```

### 3. Parser — `src/mod/lsp/json.c`

```c
typedef struct JsonErr { u32 line, col; u64 off; char msg[96]; } JsonErr;

#define YEW_JSON_MAX_DEPTH  128
#define YEW_JSON_MAX_BYTES  (64u * 1024u * 1024u)   /* == s19 collect cap */
#define YEW_JSON_MAX_NODES  (4u * 1024u * 1024u)

/* Arena-only. Returns NULL and fills err on any failure; the arena may
 * hold garbage on failure and the caller frees it wholesale.            */
JsonValue *yew_json_parse(Arena *a, const u8 *s, u64 len, JsonErr *err);
```

Recursive descent with an explicit depth counter checked on entry to every
array and object. `YEW_JSON_MAX_DEPTH = 128` bounds the C stack at a few
kilobytes and produces a clean `nesting deeper than 128` error at
`line:col`, never a crash — the difference between a bad server and a
segfault in the editor.

**UTF-8 validation** is s02's, done **once per message body** by the framing
layer (§6) before the parser runs: `yew_utf8_validate(body, n) != n` is a
protocol error, not a text-recovery situation. The lossless escape policy of
s02 §4 governs *user files*; a protocol frame that is not valid UTF-8 is a
broken server, and silently repairing it would let corrupt bytes into a
rename edit. The parser therefore assumes valid UTF-8 in the raw string
bytes and does not re-scan them; only `\uXXXX` sequences are decoded (§4).

**Grammar strictness** — RFC 8259 exactly, with every rejection tested:

| Input | Verdict |
|---|---|
| `01`, `+1`, `.5`, `1.`, `1.e3`, `-` | reject (`malformed number`) |
| `Infinity`, `NaN`, `1e999` | reject (`number out of range`) |
| trailing comma, single quotes, unquoted key, comment | reject |
| control byte `< 0x20` raw inside a string | reject (`control byte in string`) |
| trailing bytes after the top-level value (other than whitespace) | reject |
| top-level scalar (`5`, `"x"`, `true`) | **accept** (RFC 8259 allows it; the framing layer rejects it as a JSON-RPC message separately) |
| empty input | reject (`empty document`) |

**Number policy**, pinned:

```c
/* 1. Scan the lexeme per the RFC grammar; remember whether it contained
 *    '.', 'e' or 'E'.
 * 2. Integer lexeme that fits i64 exactly -> YEW_JS_INT. Accumulate with
 *    an overflow check per digit; do NOT strtoll a 30-digit string and
 *    inspect errno, because that conflates ERANGE with a real value.
 * 3. Integer lexeme that does not fit -> YEW_JS_REAL + YEW_JSF_LOSSY_NUM.
 * 4. Any lexeme with a fraction or exponent -> YEW_JS_REAL.
 * 5. strtod producing +-HUGE_VAL with errno == ERANGE -> REJECT
 *    ("number out of range"): JSON has no infinity, so a server emitting
 *    1e400 is broken and we say so rather than propagating an inf into a
 *    line number. Underflow to 0 is ACCEPTED with YEW_JSF_LOSSY_NUM.
 * 6. "-0" has no fraction and fits i64 -> YEW_JS_INT 0. Documented; the
 *    protocol has no field where the sign of zero is meaningful.        */
```

**Pitfall — `strtod` is locale-dependent.** Its decimal separator follows
`LC_NUMERIC`, so in a `de_DE` locale `"1.5"` parses as `1`. We may use it
only because `setlocale` is banned repo-wide by s02 §10 (`scripts/bans.sh`)
and a C program that never calls `setlocale` stays in the `"C"` locale for
its whole life. `json.c` carries that sentence as a comment above the call,
and DoD 9 re-greps the ban.

`yew_json_parse` also enforces `YEW_JSON_MAX_NODES` so a 60 MiB body of
`[[[[...` cannot allocate unboundedly inside the depth limit (`[1,1,1,…]` is
depth 2 and millions of nodes).

### 4. `\uXXXX`, surrogate pairs, and the lone-surrogate policy

Decoding rules, in order, each a test row:

| Sequence | Result |
|---|---|
| `\uXXXX` with `XXXX ∉ D800–DFFF` | that scalar, UTF-8 encoded via `yew_utf8_encode` |
| `\uHHHH\uLLLL` with `HHHH ∈ D800–DBFF`, `LLLL ∈ DC00–DFFF` | `0x10000 + ((hi−0xD800) << 10) + (lo−0xDC00)`, UTF-8 encoded (4 bytes) |
| high surrogate not followed by `\u` + a low surrogate | **reject**: `lone high surrogate U+D8xx` |
| lone low surrogate in **`DC80`–`DCFF`** | emit the single raw byte `lo − 0xDC00`; set `YEW_JSF_RAW_BYTE` |
| any other lone low surrogate (`DC00`–`DC7F`, `DD00`–`DFFF`) | **reject**: `lone low surrogate U+DCxx` |
| `\u0000` | one NUL byte in the string (legal; §2) |
| `\b \f \n \r \t \" \\ \/` | the obvious byte |
| any other `\x` | reject: `unknown escape '\x'` |

The `DC80`–`DCFF` row is the interesting one and it is deliberate. That
range is exactly s02's lossless invalid-byte escape. Our **writer** emits an
invalid byte as `\uDCxx` (§5), so this rule makes the whole stack
round-trip: a file containing `0xFF`, sent to a server in a `didChange` and
echoed back in a completion item's `newText`, arrives as the same `0xFF`.
Without it, the only alternatives are refusing to send such buffers at all
or mangling them — both violate invariant 2. Every *other* lone surrogate is
an error precisely so this rule stays unambiguous: nothing but our own
escape ever lands in that range.

The consequence is stated plainly in `json.h`: a `YEW_JS_STR` carrying
`YEW_JSF_RAW_BYTE` **is not valid UTF-8**. That is correct — `TextBuf` is
byte-oriented and s08 pins byte-verbatim storage — but any code that
displays such a string (a diagnostic message, a hover body) must route it
through the s02 escape policy like any other buffer text, and s46/s47 do.

### 5. Writer — `src/mod/lsp/json.c`

```c
typedef struct JsonW {
    Bytebuf *out;
    u8   need_comma[YEW_JSON_MAX_DEPTH / 8];   /* one bit per depth level */
    u16  depth;
    bool key_pending;
} JsonW;

void yew_jsonw_init (JsonW *w, Bytebuf *out);
void yew_jsonw_obj  (JsonW *w);  void yew_jsonw_obj_end(JsonW *w);
void yew_jsonw_arr  (JsonW *w);  void yew_jsonw_arr_end(JsonW *w);
void yew_jsonw_key  (JsonW *w, const char *k);
void yew_jsonw_str  (JsonW *w, const u8 *s, u32 n);
void yew_jsonw_cstr (JsonW *w, const char *s);
void yew_jsonw_int  (JsonW *w, i64 v);
void yew_jsonw_real (JsonW *w, double v);
void yew_jsonw_bool (JsonW *w, bool v);
void yew_jsonw_null (JsonW *w);
void yew_jsonw_raw  (JsonW *w, const u8 *json, u32 n);  /* pre-built blob */
void yew_jsonw_value(JsonW *w, const JsonValue *v);     /* re-emit a tree */
```

- **Deterministic key order = insertion order.** The writer emits keys in
  the order `yew_jsonw_key` is called, and `yew_jsonw_value` emits an
  object's members in `obj.m[]` order, which is the order they were parsed.
  There is no sort and no hash iteration anywhere in the emitter. Two runs
  of the same code produce byte-identical bytes — the determinism doctrine
  applied to the wire, which is what makes the golden-message tests in §7
  and Sprint 46 possible at all.
- No pretty-printing, no spaces: one canonical form.
- String escaping: `"` → `\"`, `\` → `\\`, bytes `< 0x20` → `\b \f \n \r \t`
  where defined else `\u00XX` (lowercase hex digits — pinned, because
  `\u001f` vs `\u001F` is a golden-file diff); `/` is **not** escaped; bytes
  `≥ 0x80` pass through raw **except** that a byte which s02's validator
  rejects is emitted as `\uDCxx` (§4's inverse). Determining that requires
  one `yew_utf8_validate` pass over the string; `yew_jsonw_str` does it and
  takes the fast path (memcpy of the whole run) when the string is clean.
- `yew_jsonw_real` is the **only** floating-point formatting in the module
  and uses `snprintf("%.17g")` — safe for the same locale reason as §3, and
  reachable only from `yew_jsonw_value` re-emitting a parsed real, because
  every field yew *originates* is an int, string, bool, array or object.
  `json.h` says so; DoD 8 greps for other `%g`/`%f` uses.
- Debug builds assert structural correctness (`obj_end` matching `obj`, a
  key before every object value, depth ≤ `YEW_JSON_MAX_DEPTH`) with
  `YEW_BUG` — a malformed message we *generate* is our bug (exit 4), not a
  protocol error.

### 6. JSON-RPC framing — the resumable state machine

Wire format (LSP's base protocol):

```
Content-Length: 1234\r\n
Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n   (optional)
\r\n
<exactly 1234 bytes of UTF-8 JSON>
```

```c
typedef enum { YEW_RPCRX_HDR, YEW_RPCRX_BODY, YEW_RPCRX_DEAD } RpcRxState;

typedef struct RpcRx {
    u8      state;
    bool    pending_cr;    /* a '\r' ended the last read; §6 pitfall 2   */
    Bytebuf line;          /* current header line, CR/LF stripped        */
    Bytebuf body;          /* accumulating body                          */
    u64     want;          /* Content-Length                             */
    bool    have_len;
    u16     nhdr;          /* header lines seen; cap YEW_RPC_MAX_HDRS 32 */
    u64     bytes_in, msgs_in;
    char    err[96];       /* set when state == YEW_RPCRX_DEAD           */
} RpcRx;

typedef void (*RpcMsgFn)(void *ctx, const u8 *body, u64 n);

void yew_rpcrx_init(RpcRx *rx);
/* Feed whatever read(2) returned. Returns the number of complete messages
 * delivered. NEVER blocks, NEVER loops waiting for more bytes, and may be
 * called with n == 1. On protocol error sets state = DEAD and err, and the
 * caller kills the server (§8 restart policy is s46's).                 */
u32  yew_rpcrx_feed(RpcRx *rx, const u8 *b, u64 n, RpcMsgFn on_msg,
                    void *ctx);
void yew_rpcrx_free(RpcRx *rx);
```

**The pitfall this design exists for.** A `read(2)` on a pipe returns
whatever bytes happen to be there — s19 §3 already pins that `PIPE_BUF`
guarantees the *reader* nothing. So:

1. **The header can split anywhere**, including inside the literal
   `Content-Length`, between the colon and the digits, or between two
   digits. The state machine accumulates into `rx->line` byte by byte and
   only parses on a completed line.
2. **`\r\n` can split across reads.** A read ending in `\r` followed by a
   read starting with `\n` is the classic failure: a machine that tests
   `b[i] == '\r' && b[i+1] == '\n'` walks off the end of the buffer or
   treats the `\r` as data. `pending_cr` carries the state across the call
   boundary. This is one boolean and it is the difference between "works on
   my machine" and "works under load".
3. **The body can split mid-UTF-8 sequence.** It does not matter, because
   `Content-Length` is a byte count and we accumulate exactly that many
   bytes before parsing — but it matters *enormously* that s19's
   `yew_job_safe_prefix` (which holds back an incomplete UTF-8 sequence and
   an incomplete grapheme cluster, for text buffers) is **not** applied to a
   framed sink. Holding back three bytes of a body we are counting means the
   frame never completes and the server appears to hang. `YEW_SINK_FRAMED`
   bypasses `yew_job_safe_prefix` entirely; §7 pins it and a test proves it.
4. **There is no "read until you have it all" loop.** Any such loop blocks
   the single-threaded event loop on a process we do not control, which is
   invariant 4's failure mode. `yew_rpcrx_feed` is called from
   `yew_job_pump` with whatever arrived, and returns.

Header parsing rules: names are matched **ASCII-case-insensitively**; the
value is everything after the first `:` with leading spaces/tabs trimmed;
unknown headers are ignored; a header line before any `Content-Length` is
fine (order is not fixed); the blank line ends the headers. Errors, each
setting `YEW_RPCRX_DEAD`:

| Condition | `err` |
|---|---|
| blank line with no `Content-Length` | `frame has no Content-Length` |
| `Content-Length` not a plain decimal, or > `YEW_RPC_MAX_BODY` (64 MiB) | `bad Content-Length '<value>'` |
| header line longer than `YEW_RPC_MAX_HDRLINE` (4 KiB) | `header line too long` |
| more than 32 header lines | `too many headers` |
| body not valid UTF-8 (`yew_utf8_validate`) | `body is not UTF-8 at offset N` |
| body parses but is not a JSON object | `frame is not a JSON-RPC object` |

The transmit side is trivial and synchronous-looking but is not:

```c
/* Appends the framed message to the job's outbound Bytebuf; the poll loop
 * drains it via POLLOUT. NEVER write(2)s directly — a server whose stdin
 * pipe is full would block the editor (s19 §5's deadlock, mirrored).    */
void yew_rpctx_send(RpcTx *tx, const u8 *body, u64 n);
```

`RpcTx` holds `Bytebuf pending` and `u64 sent`; `yew_job_collect_fds` adds
`in_fd` with `POLLOUT` while `sent < pending.len` — the same interleave
discipline s19 §5 pins for filters.

### 7. Correlation, ids, error mapping, transport

```c
typedef enum { YEW_RPC_REQUEST, YEW_RPC_RESPONSE, YEW_RPC_ERROR,
               YEW_RPC_NOTIFY,  YEW_RPC_SRV_REQUEST, YEW_RPC_MALFORMED }
    RpcMsgKind;

RpcMsgKind yew_rpc_classify(const JsonValue *msg);
```

| `id` | `method` | `result`/`error` | Kind | What we do |
|---|---|---|---|---|
| — | present | — | notification (either direction) | dispatch; never answered |
| present | present | — | **server → client request** | must answer, always, even if only with `MethodNotFound` — a server blocked on an unanswered request stops serving |
| present | — | `result` | response | look up `id` in the pending table |
| present | — | `error` | error response | §error table |
| present, `null` | — | `error` | error for a request we could not parse | log only |
| anything else | | | `YEW_RPC_MALFORMED` | log the first 200 bytes, count it; 8 malformed frames kill the server |

```c
typedef struct RpcPending {
    u64  id;
    u32  method;        /* interned method name, for the log             */
    u64  gen;           /* opaque generation the caller supplied (s46)   */
    u32  buf_id;        /* 0 when not buffer-scoped                      */
    i64  sent_ms, deadline_ms;
    void (*cb)(Ed *ed, void *ctx, const JsonValue *result,
               const JsonValue *error);
    void *ctx;
} RpcPending;

u64  yew_rpc_call  (RpcConn *c, const char *method, const u8 *params,
                    u32 nparams, const RpcPending *p);   /* returns id   */
void yew_rpc_notify(RpcConn *c, const char *method, const u8 *params,
                    u32 nparams);
void yew_rpc_reply (RpcConn *c, const JsonValue *id, const u8 *result,
                    u32 nresult);
void yew_rpc_reply_error(RpcConn *c, const JsonValue *id, i32 code,
                    const char *msg);
void yew_rpc_cancel(RpcConn *c, u64 id);       /* $/cancelRequest        */
```

**Id generation:** `u64`, monotonic **per connection**, starting at 1, never
reused, emitted as a JSON integer. Reasons: an integer id makes the pending
table an integer-keyed open-addressing map instead of a string map; a
per-connection counter means a restarted server starts over without
colliding with anything (the old connection's table is destroyed with it).
**Pitfall:** the spec allows a string id, and real servers have been seen
echoing an integer id back as `"7"`. `yew_rpc_classify` accepts either shape
and the pending lookup parses a string id as a decimal integer; a
non-numeric string id is `YEW_RPC_MALFORMED`.

The pending table is capped at `YEW_RPC_MAX_PENDING` (256) and swept every
poll iteration: an entry past `deadline_ms` (default 10 s, 60 s for
`initialize`) fires its callback with a synthetic
`{code: -32001, message: "timed out"}` and is removed, so no caller ever
waits forever on a server that lost the message.

Error-object mapping — the table `client.c` and `features.c` both switch on:

| Code | Name | Our action |
|---|---|---|
| −32700 | ParseError | our bytes were bad, or the server is confused: log the sent body at DEBUG, count toward the restart threshold |
| −32600 | InvalidRequest | our bug: `yew_log(ERROR)` + `yew_msg` once per session |
| −32601 | MethodNotFound | clear that capability bit and degrade permanently for this connection (s47's "server does not support X" path) |
| −32602 | InvalidParams | our bug: log with the method name; message the user once |
| −32603 | InternalError | server bug: message the user with the server's text |
| −32800 | RequestCancelled | **silently drop** — we asked for it |
| −32801 | ContentModified | **silently drop** — this *is* the generation race, arriving as a protocol answer instead of one of ours |
| −32802 | ServerCancelled | silently drop |
| −32803 | RequestFailed | message the user with the server's text |
| −32099 … −32000 | server-reserved | log at WARN |
| anything else | — | log at WARN, treat as RequestFailed |

**Transport** — the new sink in `src/edit/job.h` (s19's enum grows one row):

```c
YEW_SINK_FRAMED,   /* out_fd -> RpcRx; err_fd -> a scratch log buffer   */
```

- `yew_job_pump` routes a framed job's stdout bytes straight into
  `yew_rpcrx_feed` **without** `yew_job_safe_prefix` (§6 pitfall 3) and
  keeps s19's `YEW_JOB_READ_BUDGET` (256 KiB per job per iteration): a
  server dumping a 40 MiB workspace-symbol response must not starve
  keystrokes.
- stderr goes to a per-server `Bytebuf` mirrored into
  `$XDG_STATE_HOME/yew/log` at DEBUG and, on a crash, into the message
  the user sees (s46). It is **never** parsed; a language server's stderr is
  not protocol.
- Per-server buffers: one `RpcRx`, one `RpcTx`, one arena reset per message.
  `RpcConn` owns all four plus the pending table, and lives in `YewJob`'s
  owner slot, so `yew_job_reap` tears the connection down in one place.

### 8. Module boundary — `src/mod/lsp/shim.c` and the hard error

Following s00's pattern exactly: every function in `lsp.h` has a real
implementation in the module sources and a shim implementation compiled
**instead** when `MODULES` lacks `lsp`. Each shim body is:

```c
bool yew_lsp_start(Ed *ed, Buffer *b) {
    char err[160];
    if (!yew_mod_require(YEW_MOD_LSP, err, sizeof err)) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);   /* the pinned s00 text  */
        return false;
    }
    return false;                                 /* unreachable          */
}
```

Every `ed.lsp.*` command is registered in **both** builds (so `:lsp.restart`
completes, `:help` lists it, and the user learns *why* rather than *that*
nothing happened) and, in a shim build, reports
`this build has no lsp module; rebuild with 'make MODULES="… lsp"'`
verbatim. Silent no-ops are an invariant-3 violation. This sprint registers
`ed.lsp.info` and `ed.lsp.log` for real (they need no server) and the rest
as `YEW_CMD_DEFERRED` naming Sprint 46.

### 9. Defer

- Server lifecycle, `initialize`/`initialized`, capability negotiation,
  `positionEncoding`, document sync, diagnostics → **Sprint 46**.
  `yew_lsp_start` in the *real* module hard-errors `LSP servers start in
  Sprint 46` until then; the transport is exercised this sprint by a fake
  server (§Testing).
- Features (completion, hover, definition, references, rename, symbols)
  → **Sprint 47**.
- JSON *emitting* of Fletch values, or reading config as JSON → never:
  Fletch data literals are the config and state format
  (`00-decisions.md`), and this parser is a protocol reader living inside a
  module.
- Streaming/partial-result parsing (`$/progress` partial results) → not in
  1.0; a partial result notification is accepted and ignored, and s47 notes
  it in the method matrix.
- HTTP, TLS, and the AI transport → Sprint 48 (`src/mod/ai/http.c`); it does
  **not** reuse `RpcRx` (different framing) but does reuse `json.c`, which
  is why `json.c|h` has no LSP-specific symbol in it. Grep gate in the DoD.

## Testing Strategy

- **Unit — parser** (`tests/unit/test_json.c`): the §3 rejection table, one
  case per row, each asserting the exact `err.msg` prefix and a plausible
  `line:col`; all eight escape forms; the §4 surrogate table, all seven
  rows; `\u0000` producing a string of length 1 whose byte is 0; nesting at
  127 (accept) and 128 (reject) for both arrays and objects; duplicate keys
  (last wins, slot order kept, flag set); a 4 MiB flat array hitting
  `YEW_JSON_MAX_NODES`; every accessor called on every wrong kind, returning
  the default and not asserting.
- **Unit — numbers** (`test_json_num.c`): ≥ 40 vectors —
  `9223372036854775807` (INT, exact), `9223372036854775808` (REAL + lossy),
  `-9223372036854775808` (INT), `0`, `-0`, `1e308` (REAL), `1e309`
  (reject), `5e-324` (REAL), `1e-400` (REAL + lossy, value 0), `3.14`,
  `1E+2`, and the whole malformed set. Each asserts kind, flags, and exact
  value.
- **Unit — writer** (`test_jsonw.c`): every escape the writer produces,
  with lowercase hex pinned; a raw invalid byte emitted as `\uDCff` and
  re-parsed back to the same byte (the §4 round-trip, the one that keeps
  invariant 2 alive across the wire); insertion-order emission for a 12-key
  object built in a shuffled order; `yew_jsonw_value` re-emitting a parsed
  tree byte-identically for ≥ 20 corpus messages; the debug structural
  asserts firing on a missing `obj_end` and on a value without a key.
- **Unit — framing** (`test_jsonrpc_frame.c`): a 12-message corpus fed at
  **every** split point — the s03/s19 chunking-independence discipline —
  asserting the same message sequence for every split, including splits
  between `\r` and `\n`, inside `Content-Length`, inside a digit run, and
  inside a multi-byte UTF-8 sequence in the body; the six framing-error
  rows; a message with `Content-Type` present and one with headers in the
  other order; a 64 MiB+1 `Content-Length` rejected without allocating.
- **Unit — correlation** (`test_jsonrpc.c`): the §7 classification table;
  id round-trip with an integer id and with a string-echoed id; the pending
  table under 256 outstanding calls; the timeout sweep; every error-code row
  reaching its documented action (asserted via a recording callback);
  `$/cancelRequest` emitted with the right shape.
- **Integration — the fake server** (`tests/unit/test_lsp_transport.c`):
  a tiny helper binary `tests/helpers/fakelsp.c` (built by `make test`)
  that reads frames and replies per a script — including replying in
  deliberately awkward chunk sizes (1 byte at a time, and one giant write),
  writing to stderr, and exiting mid-message. Drives `yew_job_spawn` with
  `YEW_SINK_FRAMED` through the real poll loop and asserts messages arrive
  intact, stderr never reaches the parser, and a mid-message exit is
  reported as a clean protocol error rather than a hang.
- **Regression — the safe-prefix trap**: one labelled test spawns the fake
  server, forces `yew_job_safe_prefix` back on for a framed sink, and
  asserts the frame **fails to complete** — the test documents in its
  comment why the bypass exists, so nobody re-adds the hold.
- **Fuzz** (`tests/fuzz/fuzz_json.c`, `fuzz_jsonrpc.c`, joining `make
  fuzz`): seeded from the message corpus plus s02's byte corpus. Invariants:
  no crash / ASan / UBSan report; no read past the end; depth never exceeds
  the limit; a successful parse followed by `yew_jsonw_value` re-parses to a
  structurally equal tree (`yew_json_eq` provided for tests); the framing
  fuzzer feeds arbitrary byte streams at arbitrary split points and asserts
  the state machine either delivers well-formed bodies or reaches
  `YEW_RPCRX_DEAD` with a message — never a hang (watchdog), never a
  message longer than its declared `Content-Length`.
- **Determinism**: `yew_jsonw_*` output for the corpus is byte-identical
  across two runs and across the gcc and clang lanes; the goldens live in
  `tests/unit/fixtures/lsp/*.json` and are byte-compared.
- **Module lane**: `make MODULES="" && make MODULES="" test` builds and
  passes; the shim path is exercised by a test asserting the exact
  `yew_mod_require` message text.

## Definition of Done

1. `make` and `make test` green on gcc and clang, zero warnings under
   `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, for **both**
   `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind
   lanes clean.
2. `sizeof(JsonValue) <= 24` (`_Static_assert`); parsing is arena-only —
   `grep -n 'malloc\|calloc\|realloc\|free(' src/mod/lsp/json.c` → empty.
3. Every row of the §3 grammar table, the §4 surrogate table, and the §6
   framing-error table has a firing test asserting the exact message.
4. The invalid-byte round trip holds: a buffer byte `0xFF` → `\uDCff` on the
   wire → `0xFF` back, asserted end to end through the fake server.
5. Chunking independence: the 12-message corpus produces the identical
   message sequence for **every** split offset (≥ 40 000 split cases), and
   for the 1-byte-at-a-time and single-giant-write servers.
6. Depth and size limits: 128-deep rejects cleanly with `line:col`; a
   64 MiB+1 `Content-Length` is rejected before any allocation; a
   4M-node body is rejected; none of the three crashes or hangs.
7. Number policy: all ≥ 40 vectors pass, `YEW_JSF_LOSSY_NUM` is set exactly
   where the table says, and `1e309` is a *parse error*, not an infinity.
8. Determinism: writer output for the corpus is byte-identical across two
   runs and both compilers. The scan admits precision-bearing conversion specifiers: `grep -rEn '%[-+ #0-9.*]*[gfe]' src/mod/lsp` matches only the
   single annotated `snprintf` in `yew_jsonw_real`; no sort and no hash in
   the emitter (`grep -n 'yew_sort\|strmap' src/mod/lsp/json.c` → empty).
9. `grep -rn 'setlocale\|wcwidth\|mbrtowc' src/mod/lsp/` → empty; the s02
   ban still passes; the locale rationale comment is present above the
   `strtod` call.
10. `make fuzz FUZZ_ITERS=200000 FUZZ_SEED=1` exits 0 for `fuzz_json` and
    `fuzz_jsonrpc`, plain and under ASan/UBSan; `tests/fuzz/crashes/` empty;
    the parse→write→parse structural-equality invariant holds for every
    successfully parsed input.
11. `json.c|h` contains no LSP-specific identifier —
    `grep -in 'lsp\|textdocument\|rpc' src/mod/lsp/json.c src/mod/lsp/json.h`
    → empty (Sprint 48 links it for HTTP bodies).
12. The framed sink bypasses `yew_job_safe_prefix`: the labelled regression
    test fails when the bypass is removed, and its comment says so.
13. In a `MODULES=""` build, `:lsp.restart` prints exactly
    `this build has no lsp module; rebuild with 'make MODULES="… lsp"'`.
    Sprint 46 delivered full-build server startup: without a configured
    server restart reports that fact, while configured fake-server tests
    exercise start and restart. Neither path is a silent no-op.
14. ≥ 220 assertions across the six new unit files; determinism lane green
    on `tests/unit/fixtures/lsp/*.json`.
