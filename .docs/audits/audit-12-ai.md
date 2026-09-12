# F12 AI — HTTP, backends, shadow, and privacy

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/mod/ai/`
Owners read: Sprints 48, 49, 50

The product-code baseline remained immutable. This front strengthens unit
controls, the fake HTTP fixture, and the structured HTTP fuzz corpus; no
`src/` file changed.

## Q1 — off-by-default transport boundary

probed, nothing found

- A fresh-profile session sends 500 ordinary editing keystrokes while a
  deliberately loud fake `curl` is first on `PATH`. The AI job count, curl
  probe state, socket count, shim counter, and transport-start counters remain
  zero throughout.
- `ai.enable` remains false by default, and shadow-provider eligibility is
  decided before either native HTTP or curl transport can start.

## Q2 — pre-transport redaction and privacy claims

probed, nothing found

- One cloud buffer combines an API key, password assignment, PEM private-key
  header, content sentinel, workspace root under `$HOME`, and an absolute home
  path. Both native-HTTP and curl probes reject it with zero transport starts,
  zero captured bytes, and an empty request body.
- The loopback control captures the exact transmitted request body. It contains
  only the relative workspace path and the expected elisions; the home prefix
  and every secret sentinel are absent.
- The privacy-document inventory tests pin the exact JSON fields and request
  headers for Ollama, OpenAI, and Anthropic. The implementation admits context
  through a whitelist, applies path exclusions before reading content, runs
  redaction at the single context-to-prompt boundary, requires both environment
  and editor-option gates for debug bodies, redacts credentials even there, and
  persists only aggregate counters and latency data.
- The complete AI filter passed 106 tests and 6,245 assertions.

## Q3 — loopback-only native HTTP

probed, nothing found

- The live matrix accepts `127.0.0.1`, `localhost`, `::1`, hexadecimal
  `0x7f000001`, and a synthetic DNS name whose only answer is loopback.
- A synthetic DNS name resolving to `192.0.2.1` is refused before any socket is
  opened. A loopback server's `302` response naming that remote address is
  returned to the caller and is never followed.
- The complete HTTP filter passed 17 tests and 467,475 assertions.

## Q4 — hostile chunked-transfer input

probed, nothing found

- The structured HTTP corpus now includes valid trailers, truncated size and
  data records, missing data CRLF, oversized, negative, `0x`-prefixed and
  overflowing lengths, a truncated trailer, and conflicting framing. All 15
  corpus seeds complete without a crash, over-read, or accepted invalid frame.
- Four independent 50,000-iteration parser streams completed. Seeds 1, 2, 3,
  and 4 produced final hashes `e97620d437df90ae`, `44019d502d21b719`,
  `e74bfb74c5b84852`, and `8648c91d87b16c86`, respectively.

## Q5 — edit-triggered cancellation and process accounting

probed, nothing found

- An isolated real-key control starts a live `sleep 30` AI job, publishes a
  ghost, and sends the actual insert-mode `x` keystroke through the editor.
  The keystroke dismisses the ghost, cancels the call, inserts exactly one user
  byte, retires then reaps the process, and leaves both job arrays empty.
- The stress control performs 500 real arm/cancel/reap cycles, exceeding the
  required 200. The exact open-fd count is unchanged and
  `waitpid(-1, WNOHANG)` reports `ECHILD`; there are no zombies or leaked file
  descriptors.

## Q6 — global shadow staleness law

probed, nothing found

- Editor state owns one `AiCall` slot, and a non-newer request cannot replace
  it. Sequence floors and buffer generations are checked before delivery.
- A reply delivered 400 logical milliseconds after an intervening edit is
  rejected as stale and inserts nothing. AI-provenance accept then revalidates
  to `-1`; the buffer remains byte-identical apart from the user's exact edit.

## Q7 — stripped-module command surface

probed, nothing found

- The registry-derived control covers all 13 current `ed.ai.*` commands:
  `backends`, `models`, `ping`, `log`, `reload`, `enable`, `disable`, `forget`,
  `privacy`, `preset`, `status`, `stats`, and `open`. Enumeration prevents a
  newly registered surface from escaping the table.
- Fresh `MODULES=""` and `MODULES="lsp fuss plugins"` builds each pass 106
  exact canonical-shim assertions. The full build passes the live 83-assertion
  command surface.

## Unverified observations

None.

## Count

Raw 0 · deduped 0 · critical 0 · high 0 · medium 0 · low 0 · unverified 0.
