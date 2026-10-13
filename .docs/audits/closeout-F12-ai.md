# Closeout: F12 — AI boundaries

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s48, s49, s50

## DoD items (every numbered item from every sprint above)

- [ ] s48.1 `make` and `make test` green on gcc and clang, zero warnings under `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, for **both** `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.2 `grep -rn 'SSL\|TLS\|openssl\|mbedtls\|BIO_' src/mod/ai/` matches only comments and the §10 `YEW_AI_ERR_TLS` identifier — there is no TLS code in the tree, and `https://` on the bespoke transport produces §1's exact routing message.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.3 Chunking independence: the 20-response corpus yields an identical body byte stream for **every** split offset (≥ 60 000 cases) and for the 1-byte-at-a-time and single-giant-write servers.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.4 Chunked framing: extensions, trailers, and the post-data CRLF are all consumed; the labelled keep-alive trailer regression fails when `YEW_HX_TRAILER` is removed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.5 Close-delimited framing: a body truncated mid-stream reports `YEW_AI_ERR_PROTOCOL` with `response ended early`, and a body that reached its terminator before the close reports success — both asserted against `fakehttp`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.6 1xx handling: a `100 Continue` preceding a 200 delivers exactly the 200's body, and `rx->status == 200`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.7 **No key is ever an argv element**: `fakecurl` asserts it from `/proc/self/cmdline`; `grep -rn 'yew_log' src/mod/ai/` shows no call whose format string can receive a key; `yew_ai_log_headers` prints `<redacted N bytes>` in a golden test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.8 A literal `key:` in a backend map is a config **error** with §9's exact three-line diagnostic; a URL with userinfo is rejected; both script tested.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.9 All ten §10 error classes are reachable and each produces its own distinct sentence containing the backend name — a table test fails if two classes produce the same string.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.10 curl absence and curl-too-old produce §8's exact messages, do not disable local backends, and are probed **once** (a counter asserts one `curl --version` spawn across 50 cloud requests).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.11 `make fuzz FUZZ_ITERS=200000 FUZZ_SEED=1` exits 0 for `fuzz_http` and `fuzz_ai_stream`, plain and under ASan/UBSan; `tests/fuzz/crashes/` empty. `grep -rn 'system(\|popen(\|sprintf' src/mod/ai/` → empty; every subprocess is `yew_job_spawn` with an argv array (s19's rule).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.12 Off-by-default is visible: with `ai.enable = false`, every `ed.ai.*` command prints `AI is off; :ai enable turns it on (Sprint 50)`; `ed.ai.enable` and `ed.ai.stats` hard-error naming Sprints 50 and 49.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.13 500 request cycles leak zero fds and zero sockets (no lingering connection after the pool's idle deadline); request build + parse baselines committed under `tests/perf/baselines/ai_http.txt`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s48.14 ≥ 200 assertions across the six new unit files; determinism lane green on `tests/unit/fixtures/ai/*`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.1 `make test` green on gcc and clang at `-Werror` for `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind/determinism lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.2 **The 150 ms gate is a CI gate**: against `mockai` with a 100 ms first-byte delay, first-ghost-token p95 ≤ 150 ms over 100 runs on both transports, with each §8 stage inside its own budget; baseline committed under `tests/perf/baselines/ai_shadow.txt`. No test in the tree contacts a non-loopback host — `YEW_AI_MOCK=1` in the CI lanes makes it impossible, and a deliberate attempt fails the suite.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.3 Batching: 500 tokens in one pump phase → exactly 1 delivery; a 120 tokens/s stream produces ≤ 31 deliveries/s; keypress p99 ≤ 5 ms during a live stream.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.4 Context: `yew_utf8_validate` returns `len` for prefix and suffix in every fixture including the CJK/ZWJ/invalid-byte boundary cases; the 100 MiB end-of-file assembly is ≤ 200 µs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.5 **Both cancellation guards proven independently**: the two labelled regressions pass, and each fails when its own mechanism is stubbed out. A cancelled HTTP connection never re-enters the pool (asserted by fd identity); a cancelled curl child is signalled **before** its pipes close and is reaped with zero zombies over 500 cancel cycles.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.6 Prompts: the system prompt and all six request goldens are byte-identical across two runs and both compilers; the FIM sentinel ban holds — `grep -rn 'fim_prefix\|<PRE>\|fim▁' src/mod/ai/` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.7 Redaction is unbypassable: `grep -rn 'yew_ai_prompt_build' src/` shows exactly one call site, and it is downstream of `yew_ai_context_build`; a seeded second body-building path fails `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.8 Stats are local: `grep -rn 'AiStats\|ai_stats' src/mod/ai/` matches only `shadow_ai.c`, `stats.c`, and the command handler — never `prompt.c`, `backend.c`, `http.c` or `backend_curl.c`; `:ai stats` renders the §6 table from a fixture file, and a missing file is not an error.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.9 No prompt or completion text reaches any log path in this sprint: `grep -rn 'yew_log' src/mod/ai/shadow_ai.c src/mod/ai/prompt.c` shows no call whose arguments include `ctx->prefix`, `ctx->suffix`, or `call->text` (Sprint 50 extends this into a repo-wide gate).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.10 s43 integration: `:shadow.stats` lists `ai` as registered (no longer `— (Sprint 49)`); the provider declines silently in all seven §1 rows; accept-word/line/all on an AI ghost route through `shadow_accept_n` and undo takes each accept back as one step.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.11 `ed.ai.enable` still hard-errors naming **Sprint 50**; `ai.enable` defaults to `false` and a fresh profile makes zero network syscalls (asserted by an `strace`-free counter on `socket(2)` in the test build).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.12 `make fuzz FUZZ_ITERS=20000` green for `fuzz_ai_shadow`, plain and under ASan/UBSan; zero fd or child leaks.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.13 `mockai` and `mockcurl` are built by `make test`, documented in `tests/helpers/README` with the §8 script grammar, and are the only backends any test uses.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s49.14 ≥ 160 assertions across the six new unit files; pty goldens byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.1 `make test` green on gcc and clang at `-Werror` for `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind/determinism lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.2 **Off by default is proven, not asserted**: a fresh profile with `MODULES=ai` compiled in makes **zero** `socket(2)` calls across a scripted 500-keystroke editing session; all three §1 gates have a firing test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.3 **The CI grep gate on bodies** — in `scripts/bans.sh`, and it fails the build on a hit:
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.4 Every §4 pattern row has ≥ 3 positive and ≥ 3 negative fixtures, all green; no row uses `\w`, `\d` or `\s` (`grep -n '\\\\[wds]' runtime/ai-deny.fl` → empty); the whole set runs in ≤ 2 ms over 4096 bytes.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.5 The blocking rule holds: a cloud request with a hit transmits **zero bytes** (counted at the transport, both HTTP and curl), shows §6's message with the rule name and line number, and offers the three keys; the loopback path elides and sends, with the request-body golden proving the elision text.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.6 `ai.on_redact` cannot be set from a workspace config: a `.yew.fl` attempting it fails with a diagnostic naming `FL_ORIGIN_WORKSPACE`, and the global value is unchanged (script-tested).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.7 Every §5 glob row fires; the matcher rejects `**`; case-insensitivity is asserted on all four target platforms' path shapes; an excluded buffer builds **no context at all** (asserted by a counter inside `yew_ai_context_build`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.8 Trust db schema 3: allow/deny/unset all resolve; a schema-2 db round-trips with `plugins:` byte-identical; `dev`/`ino` mismatch clears the `ai` grant; the db is written sorted and atomically.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.9 The opt-in flow: all four steps pty-golden for both branches; `<esc>` at every step leaves config and trust db byte-identical; the cloud branch accepts only the literal `send`; no prompt is ever issued without a tty; step 4's file list matches what was actually written (asserted by diffing the files).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.10 Both presets parse with zero diagnostics, set only real in-range options, and their comment-claimed defaults match `yew_opts[]`; `:ai preset local` followed by `:ai preset cloud` leaves exactly one selected backend and no residue (s36's ledger teardown reused).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.11 The badge: four states × local/remote pty goldens at three widths; the remote badge survives 40-column truncation while lower-priority segments are dropped; the marker is ASCII `->` (`grep -n '→' src/ui/statusline.c` → empty); `ai.badge = "off"` with a remote backend configured is a validation error.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.12 `docs/ai-privacy.md` exists, is opened by `:ai privacy`, and its field table is **verified against the wire**: a test builds a request for each adapter and asserts the set of distinct data fields in the body and headers equals the table's "sent" rows exactly — a new field added to a prompt template without a docs row fails the build.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.13 `make fuzz FUZZ_ITERS=200000` green for `fuzz_ai_redact`; no hang (s20's guarantee), `tests/fuzz/crashes/` empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.14 Campaign 10 closes with nothing deferred: `grep -rn 'Sprint 4[89]\|Sprint 50' src/mod/ai/` finds no `YEW_CMD_DEFERRED` and no hard-error stub; `:shadow.stats` lists `ai` as registered; `:ai enable` works end to end.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s50.15 ≥ 150 assertions across the four new unit files; ≥ 12 script tests; pty goldens byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

- `YEW-F-118` (Medium): Sprint 48/49 closeout still requires command staging
  guards superseded by Sprint 50's shipped AI surfaces.
- `YEW-F-119` (Medium): raw TLS/stats ownership scans reject required error,
  routing, API, editor-state, and lifecycle owners.

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
