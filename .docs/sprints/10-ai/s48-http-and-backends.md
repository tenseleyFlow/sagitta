# Sprint 48: HTTP/1.1, the curl Transport, and the Backend Adapters

## Prerequisites

- Sprint 0 — `Arena`, `Vec(T)`, `Bytebuf` (`bytebuf_append/printf/push_u8`),
  `Strmap`, `yew_log`, `YEW_BUG`, the exit codes, and the `MODULES=`
  plumbing: `KNOWN_MODS`, `MODDIR_ai := ai`, `-DYEW_WITH_AI=…`,
  `$(BUILD)/mods.stamp`, the per-module `shim.c` rule, and
  `bool yew_mod_require(YewMod m, char *err, size_t errsz)` with its pinned
  text `this build has no ai module; rebuild with 'make MODULES="… ai"'`.
- Sprint 1 — the unit harness, `make test`, `make fuzz`, `scripts/bans.sh`,
  the gcc/clang/ASan/UBSan/determinism lanes.
- Sprint 2 — `yew_utf8_validate(s, len)` (first bad offset, else `len`),
  `yew_utf8_decode`, the U+DC80–DCFF escape policy, and the repo-wide
  `setlocale`/`wchar.h` ban.
- Sprint 19 — `YewJobSpec` (**argv form**), `YewJobSink`, `YewJob`,
  `yew_job_spawn/collect_fds/pump/reap`, `yew_job_signal` (to the process
  **group**), `YEW_JOB_READ_BUDGET`, `YEW_JOB_TERM_GRACE_MS`,
  `yew_job_safe_prefix`, the `pipe2(O_CLOEXEC)` discipline, the exec-status
  pipe, the environment contract, the `system()`/`popen()` ban, and the
  pinned rule *"Programmatically constructed commands MUST use argv arrays,
  never string interpolation."* Its §10 promises Sprint 48 a framed sink;
  this sprint adds the streaming one.
- Sprint 36 — `OptDesc`/`yew_opts[]`, `yew_opt_get`/`yew_opt_set` and its
  three-call-site grep gate, the Fletch `set(map)` front door, origins and
  the registration ledger, `fl_data_read`/`fl_data_write`, the trust db.
- Sprint 45 — `JsonValue`, `JsonKind`, `yew_json_parse`, the total
  accessors (`yew_json_get/at/int/real/bool/str/streq/path`), `JsonW` and
  `yew_jsonw_*`, `YEW_JSON_MAX_DEPTH`, the number policy, the `\uDCxx`
  raw-byte round trip, and its **DoD 11**: *"`json.c|h` contains no
  LSP-specific identifier … (Sprint 48 links it for HTTP bodies)."* This
  sprint is the second consumer and adds nothing to `json.c`.
- Binding: `00-decisions.md` — *"AI: Bespoke HTTP/1.1 client for localhost
  (ollama etc.) — no TLS needed; cloud (OpenAI-compatible + Anthropic APIs)
  via optional `curl` subprocess. Off by default, explicit opt-in"*, *"No
  TLS lib"*, *"Optional runtime tools: … `curl` (cloud AI only). All degrade
  gracefully when absent"*; invariants 3 (no silent stubs), 4 (latency is a
  gate), 7 (bespoke first), 8 (single-threaded core);
  `01-architecture.md` (`src/mod/ai/ http.c backend_local.c backend_curl.c
  shadow_ai.c`; process model).

Assumptions beyond earlier index entries: **(a)** `i64 yew_now_ms(void)` on
`CLOCK_MONOTONIC` (s45 makes the same assumption); **(b)** `yew_ws_root()`
returns the workspace realpath (s18/s19). **(c)** `YewJobSpec` grows one
field this sprint — see §7 — because the curl transport must feed the child
arbitrary bytes on stdin, and s19's `in_buf`/`in_span` pair is `TextBuf`
shaped.

## Goals

Land the AI module's transport floor: `src/mod/ai/http.c|h` — a bespoke
HTTP/1.1 client speaking **plain TCP only**, driven entirely by the poll
loop, with a resumable response parser covering `Content-Length`, chunked
transfer-encoding (trailers included), and end-by-close;
`src/mod/ai/backend_curl.c` — the `curl` subprocess transport for anything
needing TLS, spawned through `yew_job_spawn` with an argv **array**;
`src/mod/ai/stream.c|h` — one streaming parser (SSE and newline-delimited
JSON) shared by both transports, so an adapter never learns which pipe its
bytes came down; `src/mod/ai/backend.c|h` — the three adapters (ollama,
OpenAI-compatible, Anthropic Messages) with their request/response mapping
tables, auth schemes, model listing, and the error taxonomy;
`src/mod/ai/key.c` — API-key resolution from an environment variable or a
command, **never** a literal in `init.fl`; and `src/mod/ai/shim.c` so
`MODULES=""` still builds. The ghost-text provider is **Sprint 49**; the
opt-in flow, redaction and the presets are **Sprint 50**. Nothing here knows
what a `TextBuf` is — `http.c`, `stream.c` and `backend.c` speak bytes and
are unit-testable without an editor. AI remains **off**: every command this
sprint registers says so until `:ai enable` lands in Sprint 50.

## Deliverables

### 1. The plain-TCP law — `src/mod/ai/http.h`

```c
typedef enum { YEW_HTTP_SCHEME_HTTP = 0 } HttpScheme;   /* the whole set   */

typedef struct HttpUrl {
    const char *host;   u16 port;      /* default 80                       */
    const char *path;                  /* "/api/generate"; never empty     */
    bool        loopback;              /* host resolved to 127.0.0.0/8, ::1 */
} HttpUrl;

/* Returns false and fills err for anything this transport may not speak.  */
bool yew_http_url_parse(Arena *a, const char *url, HttpUrl *out,
                        char *err, size_t errsz);
```

**`https://` is not a parse error with a vague message; it is a routing
decision with an actionable one.** `yew_http_url_parse` on an `https` URL
fails with:

```
yew has no TLS: https backends run through curl.
set transport: "curl" on this backend, or use a http:// endpoint.
```

Why (pinned, `00-decisions.md`): a TLS stack is a dependency (invariant 7)
*and* a permanent security-maintenance obligation — a CVE in a vendored TLS
library becomes yew's emergency release. `curl` is already installed
wherever a cloud API key is, and costs us one `fork`.

**Non-loopback plain HTTP is denied by default.** `yew_http_connect` refuses
a non-loopback host unless `ai.allow_plain_remote` (default `false`), with
`refusing to send an API key in cleartext to <host>; set
ai.allow_plain_remote if this is a trusted LAN`. A `http://` endpoint on
another machine carries the body *and the `Authorization` header* in the
clear. Loopback has no wire.

| Input | Verdict |
|---|---|
| `http://127.0.0.1:11434/api/generate` | accept, `loopback = true` |
| `http://localhost:11434` | accept; path defaults to `/`; loopback per resolution, not per name |
| `http://[::1]:11434/x` | accept; bracketed IPv6 host parsed, brackets stripped |
| `http://box.lan:8080/v1` | accept only with `ai.allow_plain_remote` |
| `https://api.anthropic.com/v1/messages` | reject with the routing message above |
| `http://u:p@host/` | reject — userinfo in a URL is a credential in `init.fl` (§9) |
| `unix:/run/ollama.sock` | reject naming Sprint 49's defer list (never in 1.0) |
| port `0`, port > 65535, empty host | reject `bad url` |

### 2. Connection lifecycle — nonblocking, poll-driven

```c
typedef enum { YEW_HC_IDLE, YEW_HC_CONNECTING, YEW_HC_SENDING,
               YEW_HC_RECVING, YEW_HC_DONE, YEW_HC_DEAD } HttpConnState;

typedef struct HttpConn {
    int      fd;                  /* -1 when closed                       */
    u8       state;
    char     host[256]; u16 port; /* pool key                             */
    Bytebuf  out;  u64 sent;      /* request bytes; POLLOUT drains         */
    struct HttpRx *rx;            /* §3                                   */
    i64      t_start, t_connected, t_first_byte, t_deadline;
    bool     reusable;            /* cleared by §3 rules 6, 7 and by cancel*/
    void    *owner;               /* the AiCall; §8                       */
} HttpConn;

HttpConn *yew_http_begin (Ed *ed, const HttpUrl *u, const HttpReq *r,
                          AiErr *e);
void      yew_http_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);
void      yew_http_pump  (Ed *ed, const struct pollfd *pfd, u32 n);
void      yew_http_abort (Ed *ed, HttpConn *c);   /* §Sprint 49 cancel     */
```

Connect sequence, exactly this order:

```c
/* getaddrinfo(3) BLOCKS — POSIX has no nonblocking resolver and invariant
 * 8 forbids a thread. So: a numeric host (the loopback default) is
 * short-circuited with AI_NUMERICHOST|AI_NUMERICSERV, which consults no
 * DNS and cannot block; a NON-numeric host is resolved ONCE at backend
 * registration and cached, so a resolver stall costs one startup hiccup
 * instead of a 5-second freeze in the middle of typing.                */
getaddrinfo(host, port, {AI_NUMERICHOST|AI_NUMERICSERV | ai_family=AF_UNSPEC});
fd = socket(ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, 1);   /* §2 pitfall              */
connect(fd, ...);            /* EINPROGRESS is the expected return        */
/* poll(POLLOUT); then, and ONLY then:                                    */
getsockopt(fd, SOL_SOCKET, SO_ERROR, &err);    /* 0 = connected           */
```

Pitfalls, each with a test:

- **POLLOUT is not "connected".** A *failed* nonblocking connect also
  reports writable; the only verdict is `SO_ERROR` via `getsockopt`.
  Skipping it turns `ECONNREFUSED` into a later `EPIPE` with no useful
  message.
- **`TCP_NODELAY` is not optional here.** One small write then a read:
  Nagle plus delayed-ACK adds up to 40 ms of pure latency, a quarter of
  Sprint 49's 150 ms first-token budget.
- **`SIGPIPE`.** s19 sets `SIG_IGN` process-wide; `send()` also passes
  `MSG_NOSIGNAL`, so a plugin restoring the default handler cannot kill the
  editor on a closed socket.
- **Never `write()` the request in one blocking call.** The body can exceed
  the send buffer; the loop drains `out` under `POLLOUT` exactly as s45 §6
  pins for `RpcTx`.
- **Every fd is `SOCK_CLOEXEC` at creation.** A socket leaked into a shell
  job is a live connection to the model host with the key already sent.

Timeouts — four, separately configurable, all enforced by the loop's timer
heap rather than by socket options:

| Timeout | Option | Default | Fires when |
|---|---|---|---|
| connect | `ai.connect_timeout_ms` | 2000 | `YEW_HC_CONNECTING` exceeds it |
| first byte | `ai.first_byte_timeout_ms` | 10000 | connected, request sent, nothing read |
| idle (streaming) | `ai.stream_idle_timeout_ms` | 20000 | no byte for this long **mid-stream** |
| total | `ai.total_timeout_ms` | 120000 | wall clock since `yew_http_begin` |

Idle is distinct from total because a long generation is legitimate and a
wedged one is not, and only the gap between bytes tells them apart. SSE
comment pings (§5) count as bytes and reset the idle timer — that is what
they are for.

**Connection reuse.** A pool of at most `YEW_HTTP_POOL_MAX` (4) idle
connections keyed by `host:port`, idle deadline `ai.keepalive_ms` (30000).
A pooled connection is used only when `reusable` is set (§3).
**Pitfall — the idle-close race:** the server may close a pooled connection
at any moment and we discover it as EOF/`ECONNRESET` *after* writing. The
rule: a request that fails that way **before any response byte arrived** on
a *pooled* connection is retried exactly once on a fresh connection;
a fresh-connection failure is never retried, and a request that already
produced bytes is never retried. Anything else risks running the same
generation twice — a double charge on a metered backend, two ghosts for the
user.

### 3. The response parser — `src/mod/ai/http.c`

```c
typedef enum { YEW_HX_STATUS, YEW_HX_HDR, YEW_HX_BODY_LEN,
               YEW_HX_CHUNK_SIZE, YEW_HX_CHUNK_DATA, YEW_HX_CHUNK_CRLF,
               YEW_HX_TRAILER, YEW_HX_BODY_EOF, YEW_HX_DONE,
               YEW_HX_DEAD } HttpRxState;

typedef struct HttpRx {
    u8      state;
    bool    pending_cr;      /* a '\r' ended the last read — see pitfall 2 */
    Bytebuf line;            /* current header/chunk-size line, CRLF stripped */
    u16     nhdr;            /* cap YEW_HTTP_MAX_HDRS = 64                 */
    u16     status;          /* 200, 401, …                                */
    u8      ver_minor;       /* 0 or 1                                     */
    bool    have_len, chunked, close_delimited, no_body;
    u64     want, got;       /* Content-Length / chunk remainder           */
    u64     body_total;      /* cap YEW_HTTP_MAX_BODY = 64 MiB             */
    char    ctype[64];       /* lowercased media type, params dropped      */
    i64     retry_after_ms;  /* -1 when absent (§10 rate limit)            */
    char    err[96];
} HttpRx;

typedef void (*HttpBodyFn)(void *ctx, const u8 *p, u64 n);

void yew_http_rx_init(HttpRx *rx);
/* Feed whatever read(2) returned. NEVER blocks, NEVER loops waiting for
 * more bytes, may be called with n == 1 and with n == 0 (EOF).          */
void yew_http_rx_feed(HttpRx *rx, const u8 *b, u64 n, bool at_eof,
                      HttpBodyFn on_body, void *ctx);
```

Rules and the pitfalls that shape them:

1. **The status line can split anywhere**, including between `HTTP/1.` and
   `1`, or between two status digits. Accumulate into `rx->line`; parse only
   a completed line. Grammar: `HTTP/1.[01] SP <3 digits> [SP reason]`;
   anything else is `bad status line`.
2. **`\r\n` splits across reads.** A read ending in `\r` followed by a read
   starting with `\n` is the classic failure — testing `b[i]=='\r' &&
   b[i+1]=='\n'` walks off the buffer or treats the CR as data.
   `pending_cr` carries the state across the call. Identical in shape to s45
   §6 pitfall 2, deliberately: the same bug has now cost two protocols.
3. **1xx interim responses are skipped, not delivered.** A `100 Continue`
   or `103 Early Hints` is a complete status+headers block **followed by
   another one**. On a 1xx, reset to `YEW_HX_STATUS` and keep reading. A
   parser that takes the first status line as final reports `100` and then
   chokes on `HTTP/1.1 200 OK` appearing where a body should be.
4. **Body framing is decided once, after the blank line**, in this exact
   precedence:

   | Condition | Framing |
   |---|---|
   | status 204, 304, or 1xx | `no_body` — never wait for bytes that will not come |
   | `Transfer-Encoding` contains `chunked` | chunked; **`Content-Length` is ignored** and logged at WARN |
   | `Content-Length: N` (single, all-digits, ≤ 64 MiB) | length-delimited |
   | duplicate `Content-Length` with differing values | protocol error `conflicting Content-Length` |
   | neither | close-delimited (§rule 7) |

   Preferring chunked over a simultaneous `Content-Length` is RFC 9112
   §6.1's rule; the disagreement is the request-smuggling shape, and we log
   it because it means something between us and the model rewrites frames.
5. **The chunk-size line is the hard one.** It splits across reads more
   often than any other line here: a streaming backend emits one tiny chunk
   per token, and reads end wherever the network did.

```c
/* Parse a completed chunk-size line. Everything from the first ';' is a
 * chunk extension and is DISCARDED — 'a3;foo="bar"' is 163 bytes of data.
 * A parser that strtoull()s the whole line gets 163 for 'a3' (hex!) or 0
 * for the extension form, and then desynchronises the stream permanently:
 * every later chunk boundary is wrong and the body is silently garbage. */
static bool chunk_size_parse(const u8 *l, u32 n, u64 *out) {
    u64 v = 0; u32 i = 0;
    for (; i < n && l[i] != ';'; i++) {
        int d = hexval(l[i]);                  /* case-insensitive        */
        if (d < 0) return false;               /* 'bad chunk size'        */
        if (v > (UINT64_MAX - (u64)d) / 16) return false;  /* overflow    */
        v = v * 16 + (u64)d;
    }
    if (i == 0) return false;                  /* empty size = malformed  */
    if (v > YEW_HTTP_MAX_CHUNK) return false;  /* 16 MiB                  */
    *out = v; return true;
}
```

   After each chunk's data comes a **bare CRLF that is not part of the
   data** (`YEW_HX_CHUNK_CRLF`). Consuming it as body is the second way to
   desynchronise; hunting for the next hex digit instead of checking it is
   the third.
6. **Trailing headers after the final chunk.** A `0\r\n` chunk is followed
   by zero or more trailer lines and then a blank line. A parser that stops
   at `0\r\n` leaves those bytes in the socket; if the connection returns to
   the pool, the *next* request reads the previous response's trailers as
   its status line. Symptom: every other completion fails with `bad status
   line`, only under keep-alive. `YEW_HX_TRAILER` consumes and discards them
   (no trailer semantics in 1.0), then sets `YEW_HX_DONE`, `reusable = true`.
7. **Neither length nor chunking: end-by-close.** HTTP/1.0-shaped servers
   and some proxies stream until they close. `close_delimited` forces
   `reusable = false` (no framing left to trust) and EOF is the *success*
   signal — but **EOF is indistinguishable from truncation**, so the verdict
   is delegated to the stream layer: if `yew_ai_stream_feed` reached its
   terminator (`[DONE]`, an ndjson `done: true`, or a complete
   non-streaming body) the call succeeded; otherwise it is
   `YEW_AI_ERR_PROTOCOL`, `response ended early (connection closed after N
   bytes)`. Never report a truncated generation as a short successful one —
   the user would accept half a statement as if the model meant it.
8. **Header parsing.** Names match ASCII-case-insensitively; the value is
   everything after the first `:`, SP/HTAB-trimmed; unknown headers are
   ignored; a line beginning with SP/HTAB (obs-fold) is **rejected**
   (`obsolete header folding`) per RFC 9110 rather than unfolded — no modern
   server emits it and accepting it is attack surface. Caps:
   `YEW_HTTP_MAX_HDRLINE` 8 KiB, `YEW_HTTP_MAX_HDRS` 64, `YEW_HX_DEAD` on
   breach.
9. **The body callback receives bytes, never text.** No UTF-8 validation,
   no holding back partial sequences: §5's line splitter does the first, the
   JSON parser the second at object granularity. s45 §6 pitfall 3 restated —
   holding back three bytes of a counted body means it never completes.

### 4. Request builder

```c
typedef struct HttpHdr { const char *name, *value; } HttpHdr;

typedef struct HttpReq {
    const char *method;            /* "POST", "GET"                       */
    const char *path;
    const HttpHdr *hdrs; u32 nhdr;
    const u8 *body; u64 blen;      /* may be NULL/0                       */
    bool keepalive;
} HttpReq;

bool yew_http_req_build(Bytebuf *out, const HttpUrl *u, const HttpReq *r,
                        char *err, size_t errsz);
```

Emission order is fixed (determinism; the goldens depend on it):
request-line, `host`, `user-agent`, `accept`, `content-type`,
`content-length`, `connection`, then caller headers in array order.

- `Content-Length` is **always** present on a body-bearing request and we
  **never** send a chunked request: a request we frame ourselves has a
  length we already know, and chunked upload buys nothing but a second
  encoder to get wrong.
- `User-Agent: yew/1.0.0` — no OS, no hostname, no build id. A user
  agent is data leaving the machine (Sprint 50's docs page lists it).
- **Header injection is a build-time rejection, not an escape.**
  `yew_http_req_build` fails if any header name or value contains `\r`,
  `\n`, or a NUL, or if the name is not a valid RFC 9110 token. A model
  name, a workspace path, or a key read from a command all reach this
  function, and any of them containing a newline would otherwise let the
  caller append arbitrary headers — or an entire second request.

### 5. The shared streaming parser — `src/mod/ai/stream.c|h`

Both transports hand their body bytes to the same object, so a backend
adapter cannot tell whether it is reading a socket or curl's stdout.

```c
typedef enum { YEW_AISTREAM_SSE, YEW_AISTREAM_NDJSON,
               YEW_AISTREAM_WHOLE } AiStreamMode;   /* WHOLE = buffer all */

typedef struct AiEvent {
    const u8 *type; u32 tlen;   /* SSE "event:" name; empty for ndjson    */
    const u8 *data; u32 dlen;   /* one event's payload; NOT NUL-terminated */
    bool      terminator;       /* "[DONE]" seen, or ndjson done:true      */
} AiEvent;

typedef void (*AiEventFn)(void *ctx, const AiEvent *ev);

typedef struct AiStream {
    u8      mode;
    bool    pending_cr, saw_bom;
    Bytebuf line;               /* incomplete line across reads           */
    Bytebuf data;               /* concatenated SSE data: lines           */
    Bytebuf evt;                /* current SSE event: name                */
    u64     events, bytes;
    char    err[96];
} AiStream;

void yew_ai_stream_init(AiStream *s, AiStreamMode m);
void yew_ai_stream_feed(AiStream *s, const u8 *b, u64 n, bool at_eof,
                        AiEventFn on_event, void *ctx);
```

**Line splitting** is one function for both modes: a scanner carrying
`pending_cr`, accepting `\n`, `\r\n`, **and lone `\r`** (the SSE grammar
permits all three; at least one popular proxy normalises to CR). A line over
`YEW_AI_LINE_MAX` (1 MiB) is a protocol error, not an unbounded `Bytebuf`.

SSE framing rules, each a test row:

| Line | Action |
|---|---|
| `data: <payload>` | append `<payload>` to `data`; exactly one leading SP after the colon is stripped, and only one |
| `data:<payload>` | same — the space is optional and real servers omit it |
| second `data:` line before the blank line | append `\n` then the payload (SSE concatenation) |
| `event: <name>` | set the current event name |
| `id:` / `retry:` | parsed and **ignored** — we never reconnect, so `Last-Event-ID` has no consumer |
| `: <text>` (comment) | ignored, but it **resets the idle timer** — this is the keep-alive ping |
| blank line | dispatch `AiEvent` if `data` is non-empty, then clear `data` and `evt` |
| `data: [DONE]` | dispatch with `terminator = true` and **do not** hand `[DONE]` to the JSON parser |
| unknown field name | ignored per spec |
| UTF-8 BOM at stream start | stripped once, silently |
| EOF with a non-empty `data` and no blank line | dispatch it, set `err = "stream ended without a blank line"`, mark non-terminated |

NDJSON rules: one complete line = one event; `terminator` when the object's
`done` is `true`; empty lines skipped (ollama emits none, proxies insert
them); a partial final line at EOF is an error, not a silent drop.
`YEW_AISTREAM_WHOLE` buffers to `YEW_HTTP_MAX_BODY` and dispatches one event
at EOF — model listing and `stream: false` backends.

**Pitfall — `[DONE]` is not JSON, and neither is a ping.** Feeding either to
`yew_json_parse` yields `malformed number` / `empty document` errors that
look exactly like a broken backend, and the log noise hides the real
failure. The sentinel check lives in `stream.c`, before any adapter sees it.

### 6. Backend adapters — `src/mod/ai/backend.c|h`

```c
typedef enum { YEW_AI_OLLAMA = 0, YEW_AI_OPENAI, YEW_AI_ANTHROPIC,
               YEW_AI_NKIND } AiKind;

typedef enum { YEW_AI_TR_HTTP = 0, YEW_AI_TR_CURL } AiTransport;

typedef struct AiBackend {
    const char *name;             /* user-chosen: "local", "work", …      */
    u8   kind, transport;
    HttpUrl url;                  /* pre-parsed, pre-resolved (§2)        */
    const char *model;
    const char *key_env;          /* §9 — one of these two, never a key   */
    char *const *key_cmd;         /* argv ARRAY                           */
    i64  max_tokens;              /* Anthropic requires it; §6 table      */
    double temperature;
    bool stream;
    bool fim;                     /* backend+model support fill-in-middle */
} AiBackend;

typedef struct AiAdapter {
    const char *kind_name;
    /* Build the request body. Never sees a key: §9's accessor adds auth. */
    void (*build)(JsonW *w, const AiBackend *b, const AiPrompt *p);
    const char *(*path_gen)(const AiBackend *b);      /* completion path  */
    const char *(*path_models)(const AiBackend *b);   /* NULL = unsupported */
    AiStreamMode stream_mode;
    /* Pull the next token out of one parsed event. Returns false when the
     * event carries no text (ping, message_start, …) — not an error.     */
    bool (*delta)(const JsonValue *v, const u8 **txt, u32 *len);
    AiErrKind (*classify)(u16 status, const JsonValue *body);
} AiAdapter;

extern const AiAdapter yew_ai_adapters[YEW_AI_NKIND];
```

**ollama** — `POST /api/generate` (FIM) and `POST /api/chat`, ndjson.

| Our field | Wire (`/api/generate`) | Wire (`/api/chat`) |
|---|---|---|
| model | `model` | `model` |
| prefix text | `prompt` | last `messages[].content` (user) |
| suffix text | **`suffix`** — the FIM half | *(no equivalent; §s49 falls back)* |
| system text | `system` | `messages[0]` with `role: "system"` |
| stream | `stream: true` | `stream: true` |
| max tokens | `options.num_predict` | `options.num_predict` |
| temperature | `options.temperature` | `options.temperature` |
| stops | `options.stop` (array) | `options.stop` |
| **token text out** | `response` (string, per line) | `message.content` |
| completion flag | `done: true` | `done: true` |
| stop reason | `done_reason` (`"stop"`, `"length"`) | same |
| models | `GET /api/tags` → `models[].name` | — |
| auth | none | none |

**OpenAI-compatible** — `POST /v1/chat/completions`, SSE. Covers
llama.cpp's server, vLLM, LM Studio, OpenRouter and OpenAI itself.

| Our field | Wire |
|---|---|
| model | `model` |
| system text | `messages[0] = {role:"system", content}` |
| prompt text | `messages[1] = {role:"user", content}` |
| stream | `stream: true` |
| max tokens | `max_tokens` |
| temperature / stops | `temperature`, `stop` (array) |
| **token text out** | `choices[0].delta.content` (absent on the first and last events — not an error) |
| completion | `choices[0].finish_reason` non-null, then `data: [DONE]` |
| error (in-band) | `error.message`, `error.type`, `error.code` |
| models | `GET /v1/models` → `data[].id` |
| auth | `Authorization: Bearer <key>` |

**Anthropic Messages** — `POST /v1/messages`, SSE.

| Our field | Wire |
|---|---|
| model | `model` |
| system text | `system` (a **top-level** string, not a message) |
| prompt text | `messages[0] = {role:"user", content}` |
| max tokens | `max_tokens` — **required**; omitting it is a 400 |
| stream / temperature / stops | `stream: true`, `temperature`, `stop_sequences` |
| models | `GET /v1/models` → `data[].id` |
| auth | `x-api-key: <key>` **plus** `anthropic-version: 2023-06-01` |

Event-type table (the payload's `type` member, one row per event we can
receive):

| `type` | Carries | Our action |
|---|---|---|
| `message_start` | `message.id`, `message.model`, `message.usage.input_tokens` | record ids and input tokens; reset accumulators |
| `content_block_start` | `index`, `content_block.type` | begin a block; if `type != "text"` mark the block ignored (tool use is not a 1.0 surface) |
| `content_block_delta` | `delta.type == "text_delta"`, `delta.text` | **the only token source** — append `delta.text` |
| `content_block_stop` | `index` | end the block |
| `message_delta` | `delta.stop_reason`, `delta.stop_sequence`, `usage.output_tokens` | record the stop reason and output-token count |
| `message_stop` | — | stream complete; `terminator` |
| `ping` | — | ignore; resets the idle timer |
| `error` | `error.type`, `error.message` | run through §10 — an error can arrive **mid-stream after a 200** |

**Pitfall — key off the JSON `type`, not the SSE `event:` line.** Anthropic
sends both and they agree, but the `event:` line is a transport nicety that
proxies and gateways drop or rewrite, while the `type` member is the
message. Reading the `event:` line makes the client work against the API and
fail behind a corporate gateway — the worst place to debug it. The `event:`
name is parsed, compared, and logged at DEBUG on mismatch, never acted on.

**Pitfall — track `content_block_delta`'s `index`.** Deltas belonging to an
ignored (non-text) block must be dropped, not concatenated into the ghost,
or a tool-use block's JSON fragments appear as code.

### 7. The curl transport — `src/mod/ai/backend_curl.c`

The argv array, verbatim and complete:

```c
static char *const curl_argv[] = {
    "curl",
    "-sS",            /* silent progress, but keep errors on stderr       */
    "--no-buffer",    /* == -N; curl block-buffers when stdout is a PIPE, */
                      /* and without this an SSE stream arrives in 16 KiB */
                      /* gulps: the ghost appears all at once, 4 s late   */
    "--config", "-",  /* read EVERYTHING else from stdin; §the key rule   */
    NULL
};
```

**Nothing request-specific is ever an argv element.** Not the URL, not the
model, not a header, and above all not the key: `argv` is world-readable via
`/proc/<pid>/cmdline` and `ps aux`, so a key in argv is readable by every
process on the machine for the life of the request. It all goes down stdin
as a curl config file:

```
url = "https://api.anthropic.com/v1/messages"
request = "POST"
header = "content-type: application/json"
header = "anthropic-version: 2023-06-01"
header = "x-api-key: sk-ant-…"
data-binary = "{\"model\":\"…\",\"stream\":true,…}"
connect-timeout = 2
max-time = 120
write-out = "%{stderr}yew-http-status: %{http_code}\n"
```

Escaping into that file is exactly two substitutions, pinned because
getting them wrong is a broken request that looks like a broken model:
inside a curl config double-quoted value, `\` → `\\` and `"` → `\"`.
Nothing else needs escaping — the body is JSON, so it holds no raw control
bytes by construction (s45's writer emits `\u00XX`), and a raw `\n` would
end the directive. A debug assertion rescans the assembled config for a byte
`< 0x20` and `YEW_BUG`s: a control byte there is our bug.

`YewJobSpec` grows one field for this (the s19 extension named in
Prerequisites (c)):

```c
    const u8 *in_bytes; u64 in_len;   /* stdin source when in_buf == NULL */
```

The buffer is drained under `POLLOUT` like any other job stdin, then
`yew_memzero(bytes, len)` through a `volatile`-qualified pointer: a plain
`memset` on a buffer never read again is dead-store eliminated at `-O2` and
the key stays in the heap. `memset_s` is C11 Annex K and optional; we do not
rely on it.

Status, exit codes, and the streaming sink:

- The HTTP status arrives out of band on **stderr** as
  `yew-http-status: 401`, because stdout is the body and mixing them would
  corrupt the stream. Needs curl ≥ 7.63 for `%{stderr}`; §8 enforces it.
- stdout routes through a new sink in `src/edit/job.h` (s19's enum grows one
  row, exactly as s45 grew it):

```c
YEW_SINK_STREAM,   /* out_fd -> yew_ai_stream_feed; err_fd -> curl diags */
```

  As with `YEW_SINK_FRAMED`, `yew_job_pump` routes it **without**
  `yew_job_safe_prefix` — holding back an incomplete grapheme cluster stalls
  an SSE event forever behind a newline that never comes (s45 §6 pitfall 3,
  third occurrence; a labelled regression pins it). `YEW_JOB_READ_BUDGET`
  still applies.
- curl exit codes map into §10:

| curl exit | Meaning | Our class |
|---|---|---|
| 0 | ok (even for 4xx — we do **not** pass `-f`, because the error body is the diagnostic) | per HTTP status |
| 2 | init failed | `YEW_AI_ERR_PROTOCOL` |
| 6 | host not resolved | `YEW_AI_ERR_UNREACHABLE` |
| 7 | connect failed | `YEW_AI_ERR_UNREACHABLE` |
| 22 | HTTP error with `-f` (should never occur; asserted) | `YEW_AI_ERR_PROTOCOL` |
| 28 | operation timed out | `YEW_AI_ERR_TIMEOUT` |
| 35, 60 | TLS handshake / certificate | `YEW_AI_ERR_TLS` — message names the host and says to check the system CA store |
| 130 / SIGTERM | cancelled by us (Sprint 49) | `YEW_AI_ERR_CANCELLED`, silent |
| anything else | — | `YEW_AI_ERR_PROTOCOL`, curl's stderr quoted verbatim |

### 8. Detecting curl, and degrading when it is absent

Detection runs **at first use**, not at startup (a 20 ms budget, and most
sessions never touch a cloud backend), cached on `Ed` for the process life:

```c
typedef enum { YEW_CURL_UNKNOWN, YEW_CURL_OK, YEW_CURL_ABSENT,
               YEW_CURL_TOO_OLD } CurlState;
bool yew_ai_curl_probe(Ed *ed, char *err, size_t errsz);  /* argv: curl --version */
```

Messages, verbatim — "degrade gracefully" means "tell the user what to
type":

| State | Message |
|---|---|
| absent | `cloud AI backends need curl, which is not in $PATH. Install curl, or run a local model: :ai backend local (see :help ai-local).` |
| too old | `curl 7.61 is too old for the AI transport (need >= 7.63 for --write-out %{stderr}). Upgrade curl, or use a local http:// backend.` |
| present | nothing; `:ai backends` shows `curl 8.5.0` |

Absence is **not** an error at startup, never a warning banner, and never
disables the local backends. A `MODULES=ai` build with no curl is a fully
working local-AI editor.

### 9. API keys — the rule, and why it is a rule

> A backend definition names an **environment variable** or a **command**.
> A literal key in `init.fl` is refused at config time with a diagnostic.

```fletch
ai.backend("work", {
    kind: "anthropic",
    url: "https://api.anthropic.com",
    model: "claude-sonnet-4-6",
    transport: "curl",
    key_env: "ANTHROPIC_API_KEY",           # or:
    # key_cmd: ["pass", "show", "api/anthropic"],   # argv array, no shell
})
```

```c
/* The ONLY way to obtain key material. Writes into a caller buffer that
 * the caller zeroes; returns false and fills e when unresolvable.       */
bool yew_ai_key_get(Ed *ed, const AiBackend *b, char *out, size_t outsz,
                    AiErr *e);
```

Why literals are refused and not merely discouraged: `init.fl` is the file
users paste into bug reports (`yew --clean` exists so they *can*), the file
dotfile repos are made of, and the file `:config.edit` opens on a shared
screen. All three are a plausible Tuesday, and none is recoverable — a
leaked key is leaked. The diagnostic:

```
init.fl:14:5: error: 'key' may not hold a literal API key
   14 |     key: "sk-ant-api03-…",
      |     ^~~
      = init.fl is pasted into bug reports and committed to dotfile repos.
      = use key_env: "ANTHROPIC_API_KEY", or key_cmd: ["pass","show","…"]
```

Enforcement and handling, each with a test:

- The config validator rejects the keys `key`, `api_key`, `token`,
  `password`, `secret` on a backend map, and rejects a URL containing
  userinfo (§1). The check is on the *key name*, not on the value's shape,
  so it cannot be defeated by a key that does not look like one.
- `key_cmd` runs through `yew_job_spawn` with an argv array and
  `YEW_SINK_COLLECT`; output is trimmed of one trailing newline; a nonzero
  exit is `YEW_AI_ERR_AUTH` naming the command. It runs **once per session**
  and the result lives only in memory (`ai.key_cache` may be set to `false`
  to re-run per request for a hardware token).
- **Redaction is structural, not a filter.** The key never enters a
  `Bytebuf` any log path can see: `yew_ai_key_get` writes a stack buffer
  that goes straight into the curl config or the `Authorization` header, and
  both are assembled by functions taking a separate `secret` argument the
  logging helper does not receive. `yew_ai_log_headers()` prints
  `authorization: <redacted 51 bytes>` — the length is useful (a truncated
  key is a common paste error), the bytes are not.
- Even under Sprint 50's `YEW_AI_DEBUG=1` body logging, headers stay
  redacted: debug mode exists for prompts, not credentials.
- `yew_memzero` on every buffer that held key material, on every path
  including the error paths.

### 10. Error taxonomy — `src/mod/ai/backend.h`

```c
typedef enum { YEW_AI_OK = 0, YEW_AI_ERR_UNREACHABLE, YEW_AI_ERR_AUTH,
               YEW_AI_ERR_RATELIMIT, YEW_AI_ERR_MODEL,
               YEW_AI_ERR_PROTOCOL, YEW_AI_ERR_TIMEOUT, YEW_AI_ERR_TLS,
               YEW_AI_ERR_TOO_LARGE, YEW_AI_ERR_NO_CURL,
               YEW_AI_ERR_CANCELLED } AiErrKind;

typedef struct AiErr { u8 kind; i64 retry_ms; char msg[192]; } AiErr;
```

Every class maps to **one distinct user-facing sentence** carrying the
backend name and the next step. A single "AI request failed" is the failure
mode this table exists to prevent:

| Class | Detected by | Message (`%s` = backend name) |
|---|---|---|
| `UNREACHABLE` | `ECONNREFUSED`/`EHOSTUNREACH`/`ENETUNREACH`, resolve failure, curl 6/7 | `%s is not answering at http://127.0.0.1:11434 (connection refused) — is the server running?` |
| `AUTH` | HTTP 401, 403; Anthropic `authentication_error`; `key_cmd` nonzero exit | `%s rejected the API key (HTTP 401). Check $ANTHROPIC_API_KEY, then :ai reload.` |
| `RATELIMIT` | HTTP 429, or `overloaded_error`; `retry_ms` from `Retry-After` | `%s is rate limiting (HTTP 429); retrying is possible in 21 s.` |
| `MODEL` | HTTP 404 with a model-shaped body; ollama `model "x" not found` | `model 'qwen2.5-coder:7b' is not available on %s. :ai models lists what is.` |
| `PROTOCOL` | JSON parse failure, SSE error, framing error, truncated close-delimited body, 5xx | `%s returned a response yew could not parse (%s). :ai log has the detail.` |
| `TIMEOUT` | any §2 timeout, curl 28 | `%s did not answer within 10 s (first-byte timeout).` |
| `TLS` | curl 35/60 | `TLS handshake with %s failed (curl 60: certificate). Check the system CA store.` |
| `TOO_LARGE` | body over `YEW_HTTP_MAX_BODY`, or 413 | `%s response exceeded 64 MiB and was dropped.` |
| `NO_CURL` | §8 | §8's text |
| `CANCELLED` | §Sprint 49 | *silent* — never messaged; counted only |

`Retry-After` is parsed in both forms (delta-seconds and HTTP-date); an
unparseable value yields `retry_ms = -1`, not 0, because 0 means "retry now"
and would hammer a backend that just asked us not to. Consecutive
`RATELIMIT`/`UNREACHABLE` results put the backend in a cooldown with
exponential backoff (1 s, 2 s, 4 s … capped at `ai.backoff_max_ms`, default
60000); a request during cooldown fails immediately with the remaining time
rather than emitting traffic.

### 11. Commands and the module boundary

Registered in **both** builds, per s45 §8, so `:ai` completes and the user
learns *why* rather than *that*:

| Command | This sprint | `MODULES=""` |
|---|---|---|
| `ed.ai.backends` | lists registered backends, transport, curl version, cooldowns | `yew_mod_require` text |
| `ed.ai.models` | `GET` the model list where supported; `%s does not support model listing` otherwise | same |
| `ed.ai.ping` | one non-streaming round trip; prints class + latency | same |
| `ed.ai.log` | opens the AI log scratch buffer | same |
| `ed.ai.reload` | re-resolves backends and drops the key cache | same |
| `ed.ai.enable` / `.disable` | `YEW_CMD_DEFERRED` → **Sprint 50** | same |
| `ed.ai.stats` | `YEW_CMD_DEFERRED` → **Sprint 49** | same |

Every one of them first checks `ai.enable` (default **false**, locked in
`00-decisions.md`) and reports `AI is off; :ai enable turns it on (Sprint
50)` — the off-by-default state is a *stated* state, never a silent no-op.

### 12. Defer

- The ghost-text provider, context assembly, prompts, streaming, cancel, the
  latency gate → **Sprint 49**. `shadow_ai.c` does not exist yet and
  `yew_shadow_register(YEW_SHADOW_AI, …)` is not called; s43 §11 already
  prints `ai — (Sprint 49)` in `:shadow.stats`.
- `:ai enable`, the disclosure flow, redaction, path exclusions, the presets,
  the privacy docs page, the statusline badge → **Sprint 50**;
  `ed.ai.enable` is `YEW_CMD_DEFERRED`.
- TLS in our source → **never** (`00-decisions.md`); `https://` on the
  bespoke transport hard-errors with §1's routing message, permanently.
- HTTP/2, HTTP/3, WebSockets, unix-socket endpoints, proxies → not in 1.0.
  curl honours the environment's proxy variables on its own path; the
  bespoke transport ignores them and `http.h` says so.
- Retries beyond §2's single pooled-connection retry → never: re-running a
  metered generation is a charge the user did not authorise.
- Tool use, embeddings, image inputs, a chat UI → not in 1.0; the Anthropic
  adapter drops non-text content blocks (§6) rather than half-supporting
  them.

## Testing Strategy

- **Unit — URL** (`tests/unit/test_http_url.c`): every §1 row with the exact
  error text for the `https` and userinfo cases; IPv6 brackets; defaults.
- **Unit — response parser** (`test_http_rx.c`): a 20-response corpus
  (200 with length, 200 chunked, chunked with extensions, chunked with
  trailers, 100-then-200, 204, 304, close-delimited, 401 with a JSON body,
  429 with `Retry-After` in both forms, conflicting `Content-Length`,
  obs-fold, 8 KiB+1 header line, 65 headers, a body of exactly
  `YEW_HTTP_MAX_BODY`+1) fed at **every** split offset — the s03/s19/s45
  chunking-independence discipline — asserting an identical body byte stream
  and identical terminal state for every split, ≥ 60 000 split cases.
- **Unit — chunk sizes** (`test_http_chunk.c`): the `chunk_size_parse`
  table — lower/upper hex, `;ext=1`, `;` with a quoted value containing a
  digit, empty size, `0` final chunk, 17-digit overflow, size above the cap;
  plus a stream whose chunk-size line is split between every pair of
  characters, and a stream whose post-data CRLF is split.
- **Unit — stream** (`test_ai_stream.c`): every §5 SSE row; `\n`, `\r\n` and
  lone-`\r` variants of the same stream producing identical events; the BOM;
  two `data:` lines concatenating with one `\n`; `[DONE]` never reaching
  JSON; a comment ping producing no event; ndjson with an empty line, with a
  partial final line, with `done:true`; a 1 MiB+1 line rejected.
- **Unit — request builder** (`test_http_req.c`): byte-exact golden requests
  for all three adapters; header-injection rejection for `\r`, `\n`, NUL in
  a name and in a value; `Content-Length` correctness for a body containing
  NUL and invalid UTF-8.
- **Unit — adapters** (`test_ai_backend.c`): the §6 tables driven both ways —
  build a request and byte-compare a golden; feed a recorded response corpus
  (`tests/unit/fixtures/ai/*.txt`: real-shaped ollama ndjson, OpenAI SSE,
  Anthropic SSE) and assert the exact token string. The Anthropic corpus
  includes an `error` event **after** a 200, a `ping` between deltas, a
  non-text `content_block_start` whose deltas must be dropped, and an
  `event:`/`type` mismatch (logged, not obeyed).
- **Integration — the fake server** (`test_http_live.c` +
  `tests/helpers/fakehttp.c`, built by `make test`): binds `127.0.0.1:0`,
  prints its port, replays a scripted response with programmable write sizes
  and delays — 1 byte at a time, one giant write, close-delimited,
  mid-response close, and a keep-alive connection closed while idle. Drives
  the real poll loop and asserts body integrity, timeout classes, the single
  pooled retry, and that a fresh-connection failure is **not** retried.
- **Integration — the fake curl** (`test_ai_curl.c` +
  `tests/helpers/fakecurl.c` installed as `curl` on a test `PATH`): parses
  the stdin config, asserts the key never appeared in `argv` (re-reading
  `/proc/self/cmdline`), replays a fixture to stdout with the status line on
  stderr, and can exit with any code from the §7 table.
- **Regression — the trailer trap** (labelled): two keep-alive requests
  where the first response carries trailers; removing `YEW_HX_TRAILER` makes
  the *second* fail with `bad status line`, and the comment says so.
- **Regression — the safe-prefix trap**: as s45's, for `YEW_SINK_STREAM`.
- **Fuzz** (`fuzz_http.c`, `fuzz_ai_stream.c`): arbitrary byte streams at
  arbitrary split points. Invariants: no crash/ASan/UBSan; the parser either
  delivers a well-formed body or reaches `YEW_HX_DEAD` with a message; never
  a hang (watchdog); never more body bytes than declared; caps respected.
- **Determinism**: golden requests byte-identical across two runs and both
  compilers; header order fixed by §4.
- **Module lane**: `make MODULES="" && make MODULES="" test` builds and
  passes; `:ai backends` prints the `yew_mod_require` text verbatim.
- **Leak/fd lane**: 500 request/response cycles against `fakehttp` with fd
  accounting before and after, delta 0; valgrind clean.

## Definition of Done

1. `make` and `make test` green on gcc and clang, zero warnings under
   `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, for **both**
   `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind
   lanes clean.
2. The current closeout scans TLS implementation symbols rather than the
   error surface: a token-aware query for `SSL_new`, `SSL_connect`,
   `SSL_read`, `SSL_write`, `SSL_free`, `TLS_client_method`, `BIO_new`,
   `BIO_read`, `BIO_write`, `OPENSSL_init_ssl`, and `mbedtls_*` under
   `src/mod/ai/` is empty. The required `YEW_AI_ERR_TLS`, diagnostics, and
   curl-routing messages remain, and `https://` on the bespoke transport
   produces §1's exact routing message.
3. Chunking independence: the 20-response corpus yields an identical body
   byte stream for **every** split offset (≥ 60 000 cases) and for the
   1-byte-at-a-time and single-giant-write servers.
4. Chunked framing: extensions, trailers, and the post-data CRLF are all
   consumed; the labelled keep-alive trailer regression fails when
   `YEW_HX_TRAILER` is removed.
5. Close-delimited framing: a body truncated mid-stream reports
   `YEW_AI_ERR_PROTOCOL` with `response ended early`, and a body that
   reached its terminator before the close reports success — both asserted
   against `fakehttp`.
6. 1xx handling: a `100 Continue` preceding a 200 delivers exactly the 200's
   body, and `rx->status == 200`.
7. **No key is ever an argv element**: `fakecurl` asserts it from
   `/proc/self/cmdline`; `grep -rn 'yew_log' src/mod/ai/` shows no call
   whose format string can receive a key; `yew_ai_log_headers` prints
   `<redacted N bytes>` in a golden test.
8. A literal `key:` in a backend map is a config **error** with §9's exact
   three-line diagnostic; a URL with userinfo is rejected; both script
   tested.
9. All ten §10 error classes are reachable and each produces its own
   distinct sentence containing the backend name — a table test fails if two
   classes produce the same string.
10. curl absence and curl-too-old produce §8's exact messages, do not
    disable local backends, and are probed **once** (a counter asserts one
    `curl --version` spawn across 50 cloud requests).
11. `make fuzz FUZZ_ITERS=200000 FUZZ_SEED=1` exits 0 for `fuzz_http` and
    `fuzz_ai_stream`, plain and under ASan/UBSan; `tests/fuzz/crashes/` empty.
    `grep -rn 'system(\|popen(\|sprintf' src/mod/ai/` → empty; every
    subprocess is `yew_job_spawn` with an argv array (s19's rule).
12. Sprint 50 delivered the staged AI command surfaces. With
    `ai.enable = false`, traffic-producing `ed.ai.*` commands report the
    off-by-default state without emitting traffic; `ed.ai.enable` launches
    the shipped opt-in flow, and Sprint 49's `ed.ai.stats` reports only local
    aggregates. The stripped build retains its exact module-boundary error,
    and Sprint 50's no-deferred-command gate remains green.
13. 500 request cycles leak zero fds and zero sockets (no lingering
    connection after the pool's idle deadline); request build + parse
    baselines committed under `tests/perf/baselines/ai_http.txt`.
14. ≥ 200 assertions across the six new unit files; determinism lane green
    on `tests/unit/fixtures/ai/*`.
