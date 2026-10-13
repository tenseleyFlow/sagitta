# Sprint 49: The AI Shadow Provider — Context, Prompts, Streaming, Cancellation

## Prerequisites

- Sprint 2 — `yew_utf8_decode`, `yew_utf8_decode_prev`, `yew_gb_prev_bytes`
  /`yew_gb_next_bytes`, `yew_utf8_validate`, the U+DC80–DCFF escape policy.
  Every byte-budget boundary in §2 is snapped through these.
- Sprint 7/9/10 — `TextBuf`, `tb->gen`, `TextIter` (`yew_textiter_begin/
  chunk/advance`), `yew_textbuf_line_span`, `yew_textbuf_line_of`,
  `Cursor`/`CursorSet`, `ByteOff`, the edit choke point `yew_edit_insert`,
  `EditCtx`, `yew_undo_begin/end/abort` and `YEW_TXN_PASTE`.
- Sprint 14/15 — the poll loop's timer heap
  (`yew_timer_set`/`yew_timer_cancel`, as s43 Assumption (a) spells it),
  `yew_msg`, `yew_win_damage_lines`, the statusline segment table whose
  `badges` row names *"AI (49)"*.
- Sprint 19 — `yew_job_spawn`, `yew_job_signal` (to the process **group**),
  `yew_job_reap` through the SIGCHLD self-pipe, `YEW_JOB_TERM_GRACE_MS`,
  `YewJobState` (`YEW_JOB_SIGNALED`).
- Sprint 36 — `OptDesc`/`yew_opts[]`, `yew_opt_get/set`, the Fletch `set(map)`
  front door, `fl_data_read`/`fl_data_write` (the `:ai stats` file uses it).
- Sprint 39/40 — `b->lang` (an interner id: `"c"`, `"rust"`, …) from the
  syntax-definition registry; s46 makes the same assumption.
- Sprint 43 — **the engine this sprint plugs into**: `ShadowProvider`
  (`name`, `prov`, `debounce_ms`, `request`, `cancel`), `ShadowReq`
  (`buf_id`, `buf_gen`, `pos`, `line`, `seq`, `prov`), `ShadowSug`
  (`seq`, `prov`, `buf_id`, `buf_gen`, `pos`, `text`/`len`, `consumed`,
  `scratch`), `yew_shadow_register`, `yew_shadow_deliver` (**pump phase
  only**), the staleness law (§2: display-only ghosts; generation counters
  drop stale results; acceptance re-validates via `shadow_revalidate`), the
  debounce table (`ai` = 350 ms, `shadow.ai_debounce_ms`, **1 in flight
  globally**), the §8 interaction table, the §6 provenance styling
  (`shadow.ai` role, `dim|italic|underline`, gutter letter `a`), the overlay
  layout law, and the §11 defer note *"the AI provider → Sprint 49"* plus
  `:shadow.stats` printing `ai — (Sprint 49)`.
- Sprint 45 — `yew_json_parse`, the total accessors, `JsonW`.
- Sprint 48 — `AiBackend`, `AiAdapter`, `AiKind`, `AiTransport`, `AiErr`
  /`AiErrKind`, `HttpConn`, `yew_http_begin`/`yew_http_abort`,
  `AiStream`/`yew_ai_stream_feed`, `YEW_SINK_STREAM`, `yew_ai_key_get`,
  the per-backend mapping tables, the curl argv array and its stdin config,
  `yew_ai_curl_probe`.
- Binding: `00-decisions.md` — AI off by default with explicit opt-in;
  *"Shadow completions: three providers, one ghost-text UX"*; invariants 1
  (no data loss), 3 (no silent stubs), 4 (latency budgets are CI gates),
  5 (deterministic rendering), 8 (single-threaded core);
  `01-architecture.md` — *"AI: debounce → context assembly → stream tokens
  into ghost text; cancellation on any keystroke that invalidates the
  prefix."*

Assumption beyond earlier index entries: Sprint 50's redaction entry point
is `bool yew_ai_redact_check(Ed *ed, const AiCtx *c, RedactHit *hit)`,
called by §2 **before** the request body is built. This sprint calls it and
treats a `true` return as a hard decline; Sprint 50 supplies the patterns,
the path exclusions, and the block-vs-elide policy.

## Goals

Register the AI provider on Sprint 43's ghost-text surface and make it fast
enough to be worth having: `src/mod/ai/shadow_ai.c` (the `ShadowProvider`,
the in-flight state machine, per-frame delivery batching, cancellation on
both transports, the acceptance counter), `src/mod/ai/context.c|h` (the
byte-budgeted window around the cursor, path and language, the Sprint 50
redaction gate), `src/mod/ai/prompt.c|h` (the fill-in-the-middle and
chat-instruct templates, written out in full, plus stop-sequence and
response-trimming policy), and `tests/helpers/mockai.c` — a deterministic
**mock backend binary** that replays fixed responses with programmable
timing, against which the 150 ms first-token budget becomes a CI gate. No
real model ever runs in CI. The opt-in flow, the redaction pattern set, the
shipped presets, the privacy docs page and the statusline badge are **Sprint
50**; until it lands, `ai.enable` is `false`, the provider declines silently
(s43's `request` returning `false` is normal and not an error), and
`:ai enable` hard-errors naming Sprint 50.

## Deliverables

### 1. The provider — `src/mod/ai/shadow_ai.c`

```c
static bool ai_shadow_request(Ed *ed, const ShadowReq *r);
static void ai_shadow_cancel (Ed *ed, u32 buf_id, u32 up_to);

static const ShadowProvider yew_shadow_ai = {
    .name = "ai", .prov = YEW_SHADOW_AI,
    .debounce_ms = 350,                     /* s43 §4; option below       */
    .request = ai_shadow_request, .cancel = ai_shadow_cancel,
};
void yew_ai_shadow_init(Ed *ed);   /* calls yew_shadow_register once      */
```

`ai_shadow_request` declines — returns `false`, silently, no message, no log
above DEBUG — when any of these hold. Declining is the normal case and the
list is exhaustive so nobody adds a sixth branch that messages the user
every 350 ms:

| Condition | Why |
|---|---|
| `ai.enable == false` | off by default (locked) |
| no backend selected, or the selected backend is in §s48 cooldown | there is nothing to ask |
| the workspace is denied in the trust db | Sprint 50's per-workspace gate |
| `yew_ai_redact_check` hit on a cloud backend | Sprint 50's blocking rule (§2) |
| the buffer's path matches an excluded glob | Sprint 50's path list |
| a request is already in flight and `r->seq` is not newer | §4's global limit |
| `yew_ai_curl_probe` failed and the backend needs curl | s48 §8 — messaged **once per session**, then silent |

One request is in flight **globally**, not per buffer (s43 §4's table says
so). `AiCall` is a single slot on `Ed`:

```c
typedef struct AiCall {
    bool      live;
    u32       seq, buf_id;      /* echo back into ShadowSug              */
    u64       buf_gen;
    ByteOff   pos;
    u8        transport;        /* YEW_AI_TR_HTTP | YEW_AI_TR_CURL       */
    HttpConn *conn;             /* HTTP transport                        */
    u32       job;              /* curl transport; 0 = none              */
    AiStream  stream;
    Arena     arena;            /* reset per call                        */
    Bytebuf   text;             /* accumulated completion bytes          */
    u32       delivered;        /* bytes already handed to s43           */
    bool      dirty;            /* new bytes since the last frame (§4)   */
    i64       t_armed, t_sent, t_first_token, t_done;
    u32       ntokens;
} AiCall;
```

### 2. Context assembly — `src/mod/ai/context.c|h`

```c
typedef struct AiCtx {
    const u8 *prefix; u32 plen;   /* bytes before the cursor             */
    const u8 *suffix; u32 slen;   /* bytes after the cursor              */
    const char *path;             /* workspace-relative, '/'-separated   */
    const char *lang;             /* b->lang string, or "" when unknown  */
    u32   line_1based;            /* for the chat template's framing     */
    bool  truncated_head, truncated_tail;
} AiCtx;

bool yew_ai_context_build(Ed *ed, Win *w, const ShadowReq *r, Arena *a,
                          AiCtx *out, AiErr *e);
```

Budget, defaults, and the snapping rules:

| Knob | Option | Default | Note |
|---|---|---|---|
| total context | `ai.context_bytes` | 4096 | prefix + suffix, before any template text |
| prefix share | `ai.context_prefix_pct` | 75 | what precedes the cursor matters most |
| max suffix | derived | 1024 | the remainder |
| max ghost lines requested | `ai.max_lines` | 8 | matches `YEW_SHADOW_MAX_LINES` (s43 §5) |

1. Walk **backwards** from `r->pos` up to the prefix budget, then snap
   forward to the next **line start** if one lies within the last 25 % of
   the budget, else to the next grapheme boundary via `yew_gb_prev_bytes`.
   Snapping to a line is worth up to a quarter of the budget because a model
   handed half a token sequence produces half-token output.
2. Walk **forwards** from `r->pos` up to the suffix budget, snapping back to
   the previous line end under the same rule.
3. **Never split a UTF-8 sequence or a grapheme cluster.** A truncated
   sequence is not merely invalid JSON input — `yew_jsonw_str` would emit it
   as a `\uDCxx` raw-byte escape (s45 §5), which the model then sees as
   noise and echoes back into the ghost. Assertion in debug builds:
   `yew_utf8_validate(prefix, plen) == plen` for both halves.
4. Bytes are copied into the call arena, not aliased into the piece tree:
   the buffer may be edited (and pieces freed) while the request is in
   flight, and a suggestion computed against freed bytes is exactly the
   corruption s43's staleness law exists to prevent. `truncated_head` /
   `truncated_tail` record whether we clipped, so §3's template can say so.
5. `path` is workspace-relative (`yew_ws_root()`), `/`-separated, and
   **never absolute** — an absolute path carries the user's home directory
   and often their employer's project name off the machine for no modelling
   benefit. Sprint 50's docs page states this as a guarantee.
6. **Redaction runs here, before a single byte of the request body is
   built:**

```c
    /* Sprint 50 owns the patterns and the block/elide policy. The call
     * site is HERE and only here: any code path that assembles a request
     * without going through yew_ai_context_build would bypass redaction,
     * so context assembly and the redaction gate are one function by
     * construction. DoD 7 greps for a second body-building site.       */
    RedactHit hit;
    if (yew_ai_redact_check(ed, out, &hit)) { /* Sprint 50 decides      */ }
```

**Pitfall — the cursor is not always inside the budget window.** After a
jump to the end of a 100 MiB file the backwards walk starts at byte
104 857 600; it must be O(budget) via `TextIter`, never a scan from byte 0.
A perf assertion pins `yew_ai_context_build` at ≤ 200 µs on a 100 MiB
buffer.

### 3. Prompt templates — `src/mod/ai/prompt.c`

Two templates, chosen per backend by `AiBackend.fim`:

```c
typedef enum { YEW_AI_TPL_FIM, YEW_AI_TPL_CHAT } AiTemplate;
AiTemplate yew_ai_template_of(const AiBackend *b);
void yew_ai_prompt_build(JsonW *w, const AiBackend *b, const AiCtx *c);
```

**(a) Fill-in-the-middle** — ollama `/api/generate` only, because it is the
only endpoint in s48's tables with a `suffix` parameter:

```json
{ "model": "qwen2.5-coder:7b",
  "prompt": "<the AiCtx prefix, verbatim>",
  "suffix": "<the AiCtx suffix, verbatim>",
  "stream": true,
  "raw":    false,
  "options": { "num_predict": 128, "temperature": 0.1,
               "stop": ["\n\n", "\n}", "```"] } }
```

No instructions, no system prompt, no path, no language: a FIM model is not
being asked a question, it is being asked to continue a document, and every
extra byte is a byte the model must decide is not code.

**Pitfall — never hand-write FIM sentinels.** `<|fim_prefix|>`,
`<PRE>`/`<SUF>`/`<MID>`, `<｜fim▁begin｜>` and friends differ per model
family, and ollama's Modelfile template already inserts the right ones
around `prompt`/`suffix`. Writing them ourselves produces a prompt with two
sets of sentinels, and the model emits its own sentinel back as text — the
ghost shows `<|fim_middle|>` and looks broken while the request "succeeded".
`prompt.c` carries that sentence above the builder.

FIM support, by model family (`ai.fim` overrides per backend; the table is
the default when the model name matches a prefix, case-insensitively):

| Model prefix | FIM | Notes |
|---|---|---|
| `qwen2.5-coder`, `qwen3-coder` | yes | the local default recommended in Sprint 50's preset |
| `codellama`, `codegemma`, `starcoder2`, `deepseek-coder` | yes | |
| `codestral` | yes | |
| `llama3*`, `mistral`, `gemma*`, `phi*` (non-coder) | **no** | chat template |
| anything else on `kind: "ollama"` | no | conservative default; `:ai models` shows the verdict |
| `kind: "openai"` / `"anthropic"` | no | no `suffix` field exists (s48 §6) |

**(b) Chat-instruct fallback** — every other backend. The system prompt,
verbatim and complete, is the deliverable; it is one string in `prompt.c`
and a golden test byte-compares it:

```
You are a code completion engine embedded in a text editor.
You are given the text before the cursor and the text after it.
Reply with ONLY the code that belongs at the cursor.

Rules:
- Output raw code. No prose, no explanation, no apology.
- No Markdown. No ``` fences. No language tag.
- Do not repeat any of the text before the cursor.
- Do not repeat any of the text after the cursor.
- Continue the file's existing indentation, brace style, and naming.
- If nothing sensible belongs at the cursor, reply with an empty message.
- Reply with at most 8 lines.
```

The user message:

```
File: src/edit/motion.c
Language: c

<|before|>
<the AiCtx prefix, verbatim>
<|cursor|>
<the AiCtx suffix, verbatim>
<|after|>
```

- The three angle-bracket markers are *our* framing, not a model's FIM
  sentinels, and they are chosen to be strings no source file contains.
  They exist because a chat model given two blobs with no delimiter
  reliably continues the *suffix* instead of the gap.
- `File:` and `Language:` lines are omitted entirely when the path is
  unknown or `lang` is empty — an empty `Language:` line teaches the model
  that empty answers are acceptable formatting.
- When `truncated_head` is set, the prefix is preceded by one line
  `[… earlier in the file elided …]`, so the model does not treat the window
  edge as the file's start and emit an include block or a shebang.
- `max_tokens` is always sent (s48 §6: Anthropic **requires** it);
  `ai.max_tokens` default 256.
- Stop sequences: `["```", "<|before|>", "<|after|>"]` — the fence is there
  because instruct models emit one perhaps 5 % of the time regardless of the
  rule above, and it is cheaper to stop on it than to strip it.

**Response trimming**, applied to the accumulated bytes before every
delivery, in this order, and pinned because each rule was earned:

1. Strip a leading ```` ```lang\n ```` fence and a trailing ```` ``` ````,
   if the whole response is fenced. (Rule 2 of the system prompt is not
   always obeyed.)
2. Strip a leading newline: chat models frequently begin with one, and it
   would push the ghost onto the next row.
3. If the response begins with a copy of the prefix's last line, drop that
   copy — the "do not repeat the text before the cursor" instruction is the
   one models break most often, and the visible symptom is the ghost
   duplicating what the user just typed.
4. Truncate at `ai.max_lines` lines.
5. Trim trailing whitespace **on the last line only**; interior trailing
   whitespace is the model's indentation and removing it breaks continued
   lines.

Trimming is idempotent and is re-run on each partial delivery, so a fence
that has not finished arriving does not flicker in and out of the ghost.

### 4. Streaming into the ghost, batched per frame

Tokens arrive from `yew_ai_stream_feed` (s48 §5) through the adapter's
`delta` hook. The delivery discipline:

```c
/* Called from the poll loop's pump phase, once per iteration, AFTER all
 * ready fds have been drained. Never called from the byte callback.     */
void yew_ai_shadow_frame(Ed *ed) {
    AiCall *c = &ed->ai.call;
    if (!c->live || !c->dirty) return;
    if (yew_now_ms() - ed->ai.last_deliver_ms < ed->opt.ai_frame_ms) return;
    ShadowSug s = { .seq = c->seq, .prov = YEW_SHADOW_AI,
                    .buf_id = c->buf_id, .buf_gen = c->buf_gen,
                    .pos = c->pos,
                    .text = c->text.data, .len = c->text.len };
    yew_shadow_deliver(ed, &s);       /* s43 re-checks seq_min and buf_id */
    ed->ai.last_deliver_ms = yew_now_ms();
    c->dirty = false; c->delivered = c->text.len;
}
```

- **One delivery per frame, at most.** `ai.frame_ms` (default 33, i.e.
  ~30 Hz) is the floor between deliveries. Why this exists: a local 7B model
  on a warm GPU emits 60–120 tokens/s, and a naive
  deliver-per-token re-lays-out and re-draws an 8-line overlay 120 times a
  second. s43 §Perf budgets a live 8-line ghost at 250 µs of frame time, so
  120 Hz of it is 3 % of the CPU doing nothing but redrawing text nobody has
  read yet — and it lands on the same thread as keystrokes (invariant 8),
  so it comes straight out of the 5 ms p99 gate.
- The **first** token is exempt from the frame floor and delivered
  immediately: it is the number §8 gates on, and 33 ms of deliberate delay
  in a 150 ms budget is 22 % of it.
- `yew_shadow_deliver` is called from the pump phase only, which is s43's
  contract; the stream callback only appends to `c->text` and sets `dirty`.
  A callback that delivered directly could re-enter layout from inside a
  `read()` drain loop.
- The suggestion's `text` pointer is stable for the call's lifetime
  (`Bytebuf` growth reallocates, so `yew_shadow_deliver` copies into the
  shadow arena — s43 §1 already says `ShadowSug.text` is *"arena-owned by
  the shadow arena"*). `sug.scratch` (s43 §7's accept-word buffer) is
  invalidated on every delivery, because the text grew.
- Growth-only rule: a delivery never shortens the ghost. If trimming (§3)
  would shorten the text — a fence closing, say — the delivery is skipped
  and the shortened form lands on the next growth or at completion. A ghost
  that shrinks under the reader's eyes is the pushing-layout complaint s43
  §5 already refused, at token granularity.

### 5. Cancellation — two transports, two mechanisms, one guard behind both

```c
static void ai_shadow_cancel(Ed *ed, u32 buf_id, u32 up_to);
void yew_ai_call_abort(Ed *ed, AiCall *c, AiErrKind why);
```

s43 raises `seq_min[YEW_SHADOW_AI]` on every edit and then calls the
provider's `cancel`. Both halves are load-bearing and neither is sufficient:

**Path A — bespoke HTTP transport.** `yew_http_abort(ed, c->conn)`:

1. Remove the fd from the poll set **before** touching it, so a pump already
   in progress cannot dispatch on it.
2. `shutdown(fd, SHUT_RDWR)` then `close(fd)`. Shutdown first because a
   plain `close` on a socket with unread data sends RST on some paths and
   FIN on others; shutdown makes the server's read fail promptly either way,
   which is what stops the generation.
3. **Mark the connection non-reusable and never return it to the pool.**
   This is the pitfall: a cancelled connection has an unknown number of
   unread body bytes still in flight, and a pooled connection in that state
   makes the *next* request read the previous response's tokens as its own
   — a ghost from the wrong buffer, with a valid `seq`. Because the bytes
   would arrive framed correctly, no other check catches it.
4. Free the call arena; `c->live = false`.

**Path B — curl subprocess.**

1. `yew_job_signal(ed, c->job, SIGTERM)` — to the process **group**, per
   s19's pinned rule, because `kill(pid)` leaves curl's own children (and,
   under a wrapper, the wrapper's) running.
2. Do **not** `waitpid` inline. The child is reaped by `yew_job_reap` on the
   SIGCHLD self-pipe byte, on the next loop iteration. A blocking wait here
   is a keystroke-latency stall on a process we do not control (invariant 4).
3. Escalate to `SIGKILL` after `YEW_JOB_TERM_GRACE_MS` (200 ms) via a timer,
   exactly as s19's filter cancel does.
4. Close our end of the pipes only **after** signalling. Closing first
   leaves curl alive writing into a broken pipe: it dies eventually on
   `EPIPE`/`SIGPIPE`, but "eventually" is one more token, one more cent, and
   one more second of the model's time — and on a backend that bills by
   output token, cancellation that does not actually cancel is a bug the
   user pays for.
5. A job that exits with `YEW_JOB_SIGNALED`/`SIGTERM` while `c->live` is
   false is classified `YEW_AI_ERR_CANCELLED` and is **silent** (s48 §10) —
   never a message, never an error badge. The user pressed a key; that is
   not a failure.

**The generation counter is the second, independent guard.** Cancellation is
best-effort by construction: bytes can already be in the socket buffer, the
curl child can already have written its final event, and `yew_job_pump` can
already be holding them. So every delivery still passes s43's
`seq < seq_min[prov]` and `buf_id` checks, and every *acceptance* still runs
`shadow_revalidate` against the live buffer. Neither mechanism is allowed to
be removed on the grounds that the other exists — DoD 5 disables each in
turn and proves the other catches the stale suggestion.

Cancellation triggers, all routed through s43's §8 interaction table rather
than re-decided here: any non-matching insert, any delete, any cursor
motion, mode change, undo/redo, paste, macro replay, save, buffer switch,
focus loss, and the completion menu opening. Typing the ghost's own prefix
does **not** cancel (s43 §3's pinned "must not dismiss unconditionally"),
and it does not restart the request either — the in-flight generation is
still valid for a strictly longer prefix.

### 6. The acceptance counter — local, and only local

```c
typedef struct AiStats {
    u64 requests, declined, delivered, cancelled, errors;
    u64 accepted_word, accepted_line, accepted_all, dismissed;
    u64 bytes_suggested, bytes_accepted;
    u64 first_token_ms_sum, first_token_ms_max, n_first_token;
    u64 tokens_in, tokens_out;         /* from the backends' usage fields */
} AiStats;                             /* per backend name + a total      */
```

Persisted to `$XDG_STATE_HOME/yew/ai_stats.fl` through `fl_data_write`
(s36 §7) on clean exit and every 5 minutes; loaded at `:ai stats`. `:ai
stats` prints:

```
backend  requests  delivered  accepted  accept%  p50 first token  tokens out
local         412        377       118    31.3%           104 ms      18 402
work           37         35        19    54.3%           612 ms       2 118
```

Pinned properties, each with a test:

- **It never leaves the machine.** No counter, no aggregate, no ratio is
  ever placed in a request body, a header, or a query string. DoD 8 is a
  grep proving `AiStats` is referenced only by `shadow_ai.c`,
  `stats.c` and the `:ai stats` command — never by `prompt.c`,
  `backend.c` or either transport.
- It is a plain Fletch data literal the user can read, edit, or delete;
  deleting it is supported and resets the counters (a missing file is not
  an error).
- `accept%` is the number the user gets to judge the feature by. If it is
  low, the honest response is to turn AI off, and `:ai stats` exists to make
  that decision available rather than felt.
- Latency percentiles come from a 256-entry ring of first-token times, not
  from a sum — a mean hides the tail that invariant 4 cares about.

### 7. Options — `src/edit/option.c` rows (s36 §2 table extension)

| Option | Type | Scope | Default | Effect |
|---|---|---|---|---|
| `ai.enable` | bool | global | **false** | master switch; locked off by default |
| `ai.backend` | str | global | `""` | selected backend name |
| `ai.context_bytes` | int 256–65536 | global | 4096 | §2 |
| `ai.context_prefix_pct` | int 10–95 | global | 75 | §2 |
| `ai.max_tokens` | int 16–4096 | global | 256 | §3 |
| `ai.max_lines` | int 1–8 | global | 8 | §3 rule 4 |
| `ai.temperature` | int 0–100 | global | 10 | tenths of a percent ×10; int because s36 has no float option type |
| `ai.frame_ms` | int 0–200 | global | 33 | §4 |
| `shadow.ai_debounce_ms` | int 0–5000 | global | 350 | **s43 owns this row**; listed for completeness, not redeclared |
| `ai.fim` | enum `auto\|on\|off` | global | `auto` | §3's family table |
| `ai.key_cache` | bool | global | true | s48 §9 |

`ai.temperature` as an integer deserves the note: s36's `OptType` has no
float, adding one for a single knob is a worse trade than dividing by 1000
at the one call site in `prompt.c`, and the option's `help` string says so.

### 8. The latency budget and the mock backend — `tests/helpers/mockai.c`

> **Gate: the first ghost token is visible ≤ 150 ms after the debounce timer
> fires**, against a local backend, measured from `t_armed` (timer callback
> entry) to the frame in which `yew_shadow_deliver` first returns having
> accepted the suggestion.

Budget breakdown, each stage separately asserted so a regression names
itself:

| Stage | Budget | Measured between |
|---|---|---|
| context assembly + redaction scan | 3 ms | `t_armed` → context built |
| prompt build + JSON serialise | 1 ms | → body ready |
| connect (pooled) / connect (cold) | 0 / 5 ms | → `t_connected` |
| request write | 1 ms | → `t_sent` |
| **model first byte (mock)** | 100 ms | → `t_first_token` |
| stream parse + adapter delta | 1 ms | → text appended |
| deliver + layout + draw | 5 ms | → frame painted |
| slack | 34 ms | |

**The mock**, and it is a deliverable, not a test detail:

```
mockai --port 0 --script tests/fixtures/ai/<name>.script [--seed N]
```

- A single-file C program, no editor headers, built by `make test`. It binds
  `127.0.0.1:0`, prints `port <n>\n` on stdout, and serves exactly one
  scripted response per connection.
- The script format is line-oriented and deterministic:

| Directive | Meaning |
|---|---|
| `status 200` | status line for the next response |
| `header content-type: text/event-stream` | one response header |
| `framing sse\|ndjson\|length\|close\|chunked` | how the body is delimited |
| `delay 100` | sleep N ms before the next emission |
| `emit <literal bytes>` | write exactly these bytes (with `\n`, `\r`, `\xNN` escapes) |
| `chunk <literal bytes>` | wrap in a chunk header/trailer |
| `split 1` | write the pending bytes one byte per `write()` |
| `close` | close the connection without a terminator |
| `exit 7` | (mockcurl only) exit with this code |

- **Determinism is the point.** No randomness unless `--seed` is given, no
  wall-clock content, no model. Two runs of a script produce byte-identical
  output, which is what lets the pty goldens in §Testing exist at all.
- `mockcurl.c` is the same program with a curl-compatible front end: it
  parses the `--config -` stdin file (s48 §7), asserts the required headers
  are present, and replays a script — so the *curl* path is gated on the
  same 150 ms budget without curl or a network.
- **No real model, no real network, ever, in CI.** The perf lane sets
  `YEW_AI_MOCK=1`, which makes `yew_http_url_parse` refuse any host that is
  not the mock's, so a test that accidentally points at a real endpoint
  fails loudly instead of silently costing money and leaking a fixture.

### 9. Wiring and instrumentation

- `yew_ai_shadow_init` is called from the editor's startup after
  `yew_shadow_register` exists, guarded by `YEW_WITH_AI`; the shim build
  compiles an empty body (s45 §8's pattern).
- `:shadow.stats` (s43 §10) now shows `ai` as registered, with
  `requests / delivered / dropped_stale / dropped_gen` from s43's counters
  next to §6's AI-specific ones. s43 named `dropped_stale` climbing while
  `delivered` stays flat *"the signature of a mis-set debounce, and it is
  the number Sprint 49 tunes against"* — the tuning record (the debounce
  sweep and its chosen value) lands in `tests/perf/baselines/ai_shadow.txt`.
- `ed.ai.stats` is implemented here (s48 registered it `YEW_CMD_DEFERRED`).
- The AI log (`ed.ai.log`, s48 §11) gains request-lifecycle lines at INFO:
  backend, model, template kind, context bytes, transport, first-token ms,
  total ms, token counts, terminal class. **No prompt text and no completion
  text** — Sprint 50 owns the opt-in debug mode that adds them, and DoD 9
  greps for it.

### 10. Defer

- The opt-in flow (`:ai enable`), the disclosure text, per-workspace trust,
  the redaction pattern set and path exclusions, the block-vs-elide policy,
  the shipped presets, the privacy docs page, and the statusline `[AI]`
  badge → **Sprint 50**. This sprint calls `yew_ai_redact_check` and
  hard-errors on `ed.ai.enable` naming 50.
- Multi-cursor ghosts → **never for 1.0** (s43 §11, unchanged).
- Multi-file context (imports, related files, a repo map) → not in 1.0. The
  budget is one buffer's window; a cross-file assembler is a retrieval
  system with its own staleness and privacy story, and half of one is worse
  than none. `context.h` states it as a non-goal with this reason.
- Chat, refactor-by-instruction, commit-message generation, an
  explain-this-code panel → not in 1.0. The AI surface is exactly the ghost.
- Caching completions by prefix hash → not in 1.0: a cache hit after an edit
  is a staleness problem s43 solved once, and re-solving it in a second
  place is how the two disagree.
- Speculative prefetch during typing pauses shorter than the debounce →
  never: it is a request the user did not pause for, on a metered backend.

## Testing Strategy

- **Unit — context** (`tests/unit/test_ai_context.c`): budget snapping over
  a fixture set including CJK, a ZWJ family straddling the budget edge, an
  invalid `0xFF` byte at the boundary, a 4-byte sequence split by the raw
  budget, a cursor at byte 0, a cursor at EOF, a file shorter than the
  budget, a 100 MiB file with the cursor at the end (O(budget) assertion);
  `yew_utf8_validate` clean for both halves in **every** case; the
  workspace-relative path for a nested file, an unnamed buffer, and a file
  outside the workspace root.
- **Unit — prompts** (`test_ai_prompt.c`): byte-exact goldens for all three
  backends × both templates; the system prompt compared against a committed
  golden so an edit to it is a visible diff; the FIM family table, one row
  each; `truncated_head` adding its elision line; `max_tokens` present for
  Anthropic in every case (a missing-`max_tokens` request is a build-time
  assertion).
- **Unit — trimming** (`test_ai_trim.c`): each of §3's five rules, plus
  idempotence (trim(trim(x)) == trim(x)) over a 200-case corpus, plus a
  partially-arrived fence at every prefix length proving no flicker.
- **Unit — batching** (`test_ai_frame.c`): 500 tokens delivered in one pump
  phase produce exactly **one** `yew_shadow_deliver`; a 4-second stream at
  120 tokens/s produces ≤ `4000/33 + 1` deliveries; the first token is
  delivered without waiting for the frame floor; a trimming-induced
  shortening is skipped rather than delivered.
- **Unit — cancellation** (`test_ai_cancel.c`): the two paths driven
  separately against `mockai`/`mockcurl`; the cancelled HTTP connection is
  **not** in the pool afterwards (asserted by making the next request and
  checking it opened a new fd); the curl child is signalled before its pipes
  are closed (ordering asserted by the helper, which records what it
  received); `SIGKILL` escalation after the grace period; a
  cancelled-then-reaped job never produces a message.
- **Regression — the two guards** (labelled): (a) stub `ai_shadow_cancel` to
  a no-op → a stale suggestion still fails to reach the ghost, proving s43's
  `seq_min` catches it; (b) stub `seq_min` bumping → the cancel path stops
  the bytes. Both must pass; removing either mechanism fails one of them,
  and the test comments say which invariant each defends.
- **Integration** (`test_ai_shadow_live.c`): full loop against `mockai` for
  all three adapters — debounce fires, request goes out, tokens stream in,
  ghost grows, accept-word inserts through s43's `shadow_accept_n`, undo
  takes the accept back whole; an `error` event mid-stream after a 200
  surfaces the right class and dismisses the ghost; a `close` without a
  terminator reports `response ended early` and inserts nothing.
- **Pty goldens** (`tests/pty/ai_*.golden`): the ghost appearing with the
  `dim|italic|underline` provenance attrs and the gutter `a` (s43 §6) at all
  three colour tiers; a streaming ghost's intermediate frames byte-stable
  across two runs with the same script (determinism lane); Esc dismissing
  mid-stream without leaving I mode.
- **Perf / CI gate** (`tests/perf/perf_ai_shadow.c`, baselines committed):
  §8's stage table, each asserted separately, over 100 runs against the
  mock, on the HTTP transport and the curl transport; p99 keypress latency
  stays ≤ 5 ms while a 120 tokens/s stream runs.
- **Fuzz** (`tests/fuzz/fuzz_ai_shadow.c`): random interleavings of edits,
  motions, mock responses (including garbage, partial UTF-8, 1 MiB single
  tokens, and immediate close) and accepts. Invariants: the buffer changes
  only on an accept; every accepted byte range is a prefix of the ghost
  after `consumed`; no accept runs with `shadow_revalidate < 0`; exactly one
  `AiCall` is live at any time; every call ends in exactly one terminal
  state; no fd or child leaks over 20 000 iterations.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror` for `MODULES="lsp ai
   fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind/determinism lanes
   clean.
2. **The 150 ms gate is a CI gate**: against `mockai` with a 100 ms
   first-byte delay, first-ghost-token p95 ≤ 150 ms over 100 runs on both
   transports, with each §8 stage inside its own budget; baseline committed
   under `tests/perf/baselines/ai_shadow.txt`. No test in the tree contacts a
   non-loopback host — `YEW_AI_MOCK=1` in the CI lanes makes it impossible,
   and a deliberate attempt fails the suite.
3. Batching: 500 tokens in one pump phase → exactly 1 delivery; a
   120 tokens/s stream produces ≤ 31 deliveries/s; keypress p99 ≤ 5 ms
   during a live stream.
4. Context: `yew_utf8_validate` returns `len` for prefix and suffix in every
   fixture including the CJK/ZWJ/invalid-byte boundary cases; the 100 MiB
   end-of-file assembly is ≤ 200 µs.
5. **Both cancellation guards proven independently**: the two labelled
   regressions pass, and each fails when its own mechanism is stubbed out.
   A cancelled HTTP connection never re-enters the pool (asserted by fd
   identity); a cancelled curl child is signalled **before** its pipes close
   and is reaped with zero zombies over 500 cancel cycles.
6. Prompts: the system prompt and all six request goldens are byte-identical
   across two runs and both compilers; the FIM sentinel ban holds —
   `grep -rn 'fim_prefix\|<PRE>\|fim▁' src/mod/ai/` → empty.
7. Redaction is unbypassable: `grep -rn 'yew_ai_prompt_build' src/` shows
   exactly one call site, and it is downstream of `yew_ai_context_build`;
   a seeded second body-building path fails `make test`.
8. Stats are local: `grep -rn 'AiStats\|ai_stats' src/mod/ai/` matches only
   `shadow_ai.c`, `stats.c`, and the command handler — never `prompt.c`,
   `backend.c`, `http.c` or `backend_curl.c`; `:ai stats` renders the §6
   table from a fixture file, and a missing file is not an error.
9. No prompt or completion text reaches any log path in this sprint:
   `grep -rn 'yew_log' src/mod/ai/shadow_ai.c src/mod/ai/prompt.c` shows no
   call whose arguments include `ctx->prefix`, `ctx->suffix`, or
   `call->text` (Sprint 50 extends this into a repo-wide gate).
10. s43 integration: `:shadow.stats` lists `ai` as registered (no longer
    `— (Sprint 49)`); the provider declines silently in all seven §1 rows;
    accept-word/line/all on an AI ghost route through `shadow_accept_n` and
    undo takes each accept back as one step.
11. `ed.ai.enable` still hard-errors naming **Sprint 50**; `ai.enable`
    defaults to `false` and a fresh profile makes zero network syscalls
    (asserted by an `strace`-free counter on `socket(2)` in the test build).
12. `make fuzz FUZZ_ITERS=20000` green for `fuzz_ai_shadow`, plain and under
    ASan/UBSan; zero fd or child leaks.
13. `mockai` and `mockcurl` are built by `make test`, documented in
    `tests/helpers/README` with the §8 script grammar, and are the only
    backends any test uses.
14. ≥ 160 assertions across the six new unit files; pty goldens byte-stable
    across the determinism lane's double run.
