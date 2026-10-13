# Sprint 50: Off by Default — Opt-In, Redaction, Presets, and the Honest Docs Page

## Prerequisites

- Sprint 2 — `yew_utf8_decode`, `yew_cluster_width`, `yew_str_width`,
  `yew_str_clip`; the EAW-ambiguous rule that `ambiguous_wide` flips
  (§8 depends on it).
- Sprint 8 — `FileMeta`, canonical paths, the atomic-write primitive
  (tmp + `fsync` + `rename` + dir `fsync`).
- Sprint 15/18 — `yew_msg`, the statusline segment/priority table whose
  `badges` row names *"AI (49)"* and its truncation order; the E-mode
  cmdline prompt machinery (**no nested read loops** — s36 §4 pins it for
  the trust prompt and this sprint obeys the same rule).
- Sprint 20 — `yew_re_compile`/`yew_re_search`, `YewReErr`, the complete
  1.0 syntax table (classes, `\b`, `{m,n}`, `(?i)`, `(?i:…)`, `\A`/`\z`,
  line anchors `^`/`$`), the no-backtracking guarantee, and the pinned note
  that `\w`/`\d`/`\s` are **Unicode-aware** (§4 leans on that being true and
  avoids them where it matters).
- Sprint 36 — `OptDesc`/`yew_opts[]`, `yew_opt_get/set`, the Fletch
  `set(map)` front door, origins and the registration ledger,
  `fl_data_read`/`fl_data_write` (the canonical pure-literal path), and
  **the trust db**: `$XDG_STATE_HOME/yew/trust.fl`, pure literal, atomic
  write, versioned `schema:`, keys sorted bytewise on write, *"unknown keys
  preserved on rewrite (forward compatibility)"*, the `dirs` map keyed by
  realpath with `{state, hash, dev, ino, at}`, the five invalidation rows,
  the never-prompt-without-a-tty rule, and `--trust-workspace`.
- Sprint 43 — the ghost surface, `shadow.ai` provenance role, and s43 §10's
  counters.
- Sprint 48 — `AiBackend`, `AiKind`, `AiTransport`, the three adapters,
  `yew_ai_key_get` and the **no-literal-keys** rule, `yew_ai_curl_probe`,
  the §10 error taxonomy, and `ed.ai.enable` registered
  `YEW_CMD_DEFERRED` naming this sprint.
- Sprint 49 — `AiCtx`, `yew_ai_context_build` and its single redaction call
  site, `AiCall`, `AiStats`, `ai.*` options, and Assumption *"Sprint 50's
  redaction entry point is `yew_ai_redact_check`"*. This sprint supplies it.
- Binding: `00-decisions.md` — *"AI: … Off by default, explicit opt-in,
  documented redaction/privacy defaults"*, *"Ships with a good default
  config"*; invariants 1, 3 (no silent stubs), 5, 9 (keyboard-reachable).

Assumption beyond earlier index entries: `runtime/` presets are loadable by
name through `:ai preset <name>` and by `import "preset.ai-local"` from
`init.fl`, resolving under `$YEW_RUNTIME_DIR` (s36 §1 row 1's directory).
If s36's import resolver spells the runtime root differently, `preset.c`
adapts at one call site.

## Goals

Make the AI module safe to ship: `src/mod/ai/optin.c` (the `:ai enable`
walk-through, with the disclosure text as a deliverable rather than a
gesture), the per-workspace allow/deny grant persisted in Sprint 36's trust
db as **schema 3**, `src/mod/ai/redact.c|h` (the deny-pattern file, the
shipped default pattern set with its actual regexes, the path-exclusion
globs, and the pinned rule that a hit on a cloud backend **blocks** the
request), the two shipped presets `runtime/preset.ai-local.fl` and
`runtime/preset.ai-cloud.fl` written out in full and annotated,
`docs/ai-privacy.md` stating completely and honestly what leaves the machine
and when, and the statusline `[AI]` badge with its four states and
facsimile's `[AI->host]` remote marker. This sprint closes Campaign 10:
after it, `ed.ai.enable` works, nothing about AI is deferred, and every
default is the conservative one.

## Deliverables

### 1. Off by default — where it is enforced, and how it stays enforced

`ai.enable` defaults to `false` (s49 §7, locked in `00-decisions.md`). Three
independent gates, deliberately redundant, because a single flag that one
early-return forgets is how features ship on by accident:

| Gate | Checked in | Failure mode it catches |
|---|---|---|
| `ai.enable` | `ai_shadow_request` (s49 §1) and every `ed.ai.*` handler | a new command forgetting the check |
| `ed->ai.backend != NULL` | `yew_ai_context_build` | a config that enabled AI without choosing a backend |
| workspace grant (§3) | `yew_ai_workspace_allowed(ed)`, called by `ai_shadow_request` | a global opt-in leaking into a workspace the user never approved |

A fresh profile (`$XDG_CONFIG_HOME` empty) makes **zero** network syscalls
for the life of the process. DoD 2 counts `socket(2)` calls in the test
build and asserts zero across a scripted editing session with `MODULES=ai`
compiled in.

### 2. `:ai enable` — the opt-in walk-through — `src/mod/ai/optin.c`

Four steps, each an ordinary cmdline prompt inside the event loop (never a
nested read loop — s36 §4). `<esc>` at any step aborts and changes nothing.

**Step 1 — where the model runs.**

```
Turn on AI completions?

  [1] Local model (ollama on this machine)
      Your code is sent to a program running on this computer.
      Nothing leaves the machine. No account, no API key, no network.

  [2] Cloud model (Anthropic, OpenAI, or an OpenAI-compatible service)
      Parts of the file you are editing are sent over the internet to a
      company's servers, where they are processed and may be retained
      under that company's policy — not yew's.

  [3] Read the full privacy page first  (:ai privacy)

  [n] Not now

Choice [1/2/3/n]:
```

**Step 2 — the disclosure, and it differs by choice.** For `[1]` (local):

```
Local model — what leaves this editor:

  * Up to 4096 bytes of the file you are editing: the text immediately
    before your cursor and immediately after it.
  * The file's path, relative to this workspace (never an absolute path,
    so never your home directory or your username).
  * The file's language name ("c", "rust", ...).

  Where it goes:  http://127.0.0.1:11434 — a process on this machine.
  Over the network: nothing. The socket is loopback; there is no wire.
  Retained by yew: nothing but counters you can read with :ai stats
  and delete at any time.

  When: 350 ms after you stop typing, in insert mode, at a line end.
  Never while a selection is active, never during a macro, never in
  --batch, and never for a file matching the excluded paths (:ai privacy).

Enable local AI completions? [y/N]:
```

For `[2]` (cloud), after the backend name is chosen, the same list plus the
paragraph that is the reason this prompt exists:

```
Cloud model — the same file text and path leave this machine.

  Destination:  https://api.anthropic.com  (backend "work")
  Credential:   read from $ANTHROPIC_API_KEY at request time
  Transport:    the curl program on this system

  Once the text is sent, yew cannot recall it, cannot delete it, and
  does not know how long it is kept or who can read it. That is between
  you and Anthropic, under their terms — which yew neither controls
  nor recites here, because they change and this text does not.

  yew scans every request for secrets first (API keys, private keys,
  bearer tokens, .env assignments, credentialed URLs). If it finds one,
  the request is BLOCKED and you are told which line matched. It does not
  scrub and send: a scrubber that is 99% right leaks on the hundredth
  file, and you would never learn which one.

Type the word 'send' to confirm you want file contents sent to
api.anthropic.com, or press Esc to cancel:
```

The typed-word confirmation is only on the cloud branch, and it is
deliberate: `y` is muscle memory, and the whole point of the step is to
interrupt muscle memory once. Local costs a `y`.

**Step 3 — scope.**

```
Enable AI for:
  [w] this workspace only  (/home/u/proj)         <- recommended
  [a] every workspace
  [o] this session only
Choice [w/a/o]:
```

**Step 4 — what was written.** The flow prints exactly which files changed
and how to undo it, because a feature that edits your config without saying
so is one the user cannot trust:

```
AI enabled.
  wrote  ~/.config/yew/init.fl        (ai.backend = "local", ai.enable)
  wrote  ~/.local/state/yew/trust.fl  (ai: allow for /home/u/proj)
  undo:  :ai disable        (turns it off, keeps the backend definition)
         :ai forget         (removes the workspace grant)
```

Pinned properties:

- **Never prompts without a tty** (s36 §4's rule, inherited): under
  `--batch` or a non-terminal stdin, `:ai enable` fails with `AI cannot be
  enabled non-interactively; set ai.enable in init.fl if you have read
  :ai privacy` and exit code 1. There is no `--yes`.
- The config write is a targeted edit through s36's option path plus an
  appended, commented block in `init.fl` — never a rewrite of the user's
  file. If `init.fl` is unwritable, the flow says so and offers session-only.
- `:ai disable` and `:ai forget` are registered here and are the documented
  inverses. `:ai privacy` opens §7's page in a scratch buffer.

### 3. Per-workspace grant — trust db **schema 3**

s36's db gains one member on each `dirs` entry:

```fletch
# yew trust database — hand-editable; delete a line to be asked again.
{
  schema: 3,
  dirs: {
    "/home/u/proj":  { state: "trusted", ai: "allow",
                       hash: "9f2ac1d4e07b5533", dev: 66306, ino: 4213771,
                       at: 1785600000 },
    "/home/u/client": { state: "trusted", ai: "deny" },
    "/home/u/junk":   { state: "denied" },
  },
}
```

```c
typedef enum { YEW_AI_WS_UNSET = 0, YEW_AI_WS_ALLOW, YEW_AI_WS_DENY }
    AiWsGrant;
AiWsGrant yew_ai_workspace_grant(Ed *ed);          /* reads the trust db  */
bool      yew_ai_workspace_allowed(Ed *ed);        /* the §1 gate         */
void      yew_ai_workspace_set(Ed *ed, AiWsGrant g);
```

- `ai` is **independent of `state`**: a directory may be trusted for
  `.yew.fl` execution and denied for AI, and vice versa. They answer
  different questions ("may this run code on my machine" vs "may this send
  my code off it") and collapsing them would mean trusting a repo's config
  silently grants it your source.
- `UNSET` resolves to `ai.default_workspace` (enum `ask|allow|deny`, default
  **`ask`**): the first request in an ungranted workspace suppresses itself
  and posts one message — `AI is off in this workspace; :ai enable grants
  it` — once per session, not per keystroke.
- **Version reconciliation.** s36 pinned schema 1 (dirs) and named s54's
  plugins as schema 2; this sprint is schema 3. Because s36 also pinned
  *"unknown keys preserved on rewrite"*, a db written by any of the three
  loads under all of them, and the writer emits `schema: max(known, read)`.
  Neither sprint may drop a key it did not write. A unit test round-trips a
  schema-2 db through this sprint's writer and asserts the `plugins:` block
  survives byte-identically.
- s36's five invalidation rows apply unchanged, and a `dev`/`ino` mismatch
  clears the `ai` grant along with `state`: a directory replaced at the same
  path is a different project, and its source has not been approved for
  transmission.

### 4. Redaction — the deny patterns

```c
typedef struct RedactHit {
    const char *rule;     /* the pattern's name, for the message         */
    u32   line_1based;    /* in the buffer, for the jump                 */
    ByteOff off;  u32 len;
    bool  in_prefix;      /* which half of the AiCtx matched             */
} RedactHit;

/* Returns true when the context must not be sent as-is. Called by
 * yew_ai_context_build (s49 §2) and by nothing else.                    */
bool yew_ai_redact_check(Ed *ed, const AiCtx *c, RedactHit *hit);
```

Patterns live in `runtime/ai-deny.fl` (shipped) plus optional
`$XDG_CONFIG_HOME/yew/ai-deny.fl` (merged, user rows appended so they
cannot silently remove a shipped one; `ai.deny_replace = true` opts into
replacement). Each row is `{ name, re, flags, note }`, compiled once through
`yew_re_compile` at first use and cached.

The shipped set — these are the regexes, in s20's syntax:

| Name | Regex | Catches |
|---|---|---|
| `aws-access-key` | `\b(A3T[A-Z0-9]\|AKIA\|ASIA\|ABIA\|ACCA\|AGPA\|AIDA\|AIPA\|ANPA\|ANVA\|AROA)[A-Z0-9]{16}\b` | AWS access key ids, all documented prefixes |
| `aws-secret` | `(?i:aws_?secret_?access_?key)[^\n]{0,20}[:=][^\n]{0,4}[A-Za-z0-9/+=]{40}` | the 40-char secret, anchored to its label so a random base64 blob is not a false positive |
| `pem-private-key` | `-----BEGIN [A-Z0-9 ]{0,32}PRIVATE KEY-----` | RSA/EC/OPENSSH/PGP private keys, and the generic PKCS#8 header |
| `ssh-private-key` | `\bssh-rsa AAAA[A-Za-z0-9+/]{100,}` | a pasted key blob without its header |
| `bearer-token` | `(?i:bearer)[ \t]+[A-Za-z0-9._~+/-]{16,}={0,2}` | `Authorization: Bearer …` in code, tests, and curl snippets |
| `authorization-header` | `(?i:authorization)[ \t]*[:=][ \t]*["']?[A-Za-z0-9._~+/-]{16,}` | the header assembled in source |
| `jwt` | `\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b` | any JWT (the `eyJ` prefix is `{"` base64url) |
| `env-assignment` | `(?i:^[ \t]*(export[ \t]+)?[A-Za-z_][A-Za-z0-9_]*(SECRET\|TOKEN\|PASSWORD\|PASSWD\|PRIVATE_KEY\|API_?KEY\|CREDENTIAL)[A-Za-z0-9_]*[ \t]*=[ \t]*["']?[^\n"' \t]{8,})` | `.env`-style `KEY=VALUE`, only when the *name* claims to be a secret |
| `conn-string-creds` | `\b[a-z][a-z0-9+.-]{1,31}://[^\n/:@ ]{1,64}:[^\n/@ ]{1,128}@` | `postgres://user:pw@host`, `amqp://`, `mongodb://`, `redis://`, `https://u:p@` |
| `github-token` | `\bgh[pousr]_[A-Za-z0-9]{36,255}\b` | GitHub PATs, OAuth, app, refresh tokens |
| `slack-token` | `\bxox[baprs]-[A-Za-z0-9-]{10,}\b` | Slack bot/user/app tokens |
| `google-api-key` | `\bAIza[A-Za-z0-9_-]{35}\b` | Google API keys |
| `openai-key` | `\bsk-[A-Za-z0-9_-]{20,}\b` | OpenAI and OpenAI-compatible keys |
| `anthropic-key` | `\bsk-ant-[A-Za-z0-9_-]{20,}\b` | Anthropic keys (also matched by the row above; both fire, first name wins) |
| `private-key-var` | `(?i:private_?key)[ \t]*[:=][ \t]*["'][^\n"']{32,}` | a key inlined into a config literal |
| `htpasswd-bcrypt` | `\$2[aby]?\$[0-9]{2}\$[A-Za-z0-9./]{53}` | bcrypt hashes (a credential, even hashed) |

**Pitfall — do not write these with `\w`, `\d` or `\s`.** s20's perl classes
are Unicode-aware by design, so `\w{16}` happily matches sixteen Cyrillic
letters and `\d` matches Devanagari digits. A secret shape is an ASCII
shape; every row above spells its class out. A row that uses `\w` will pass
its own test and mis-fire on a CJK comment, which is the worst kind of
false positive because it blocks a request for a file with no secret in it.

**Pitfall — anchor the label, not the value.** `aws-secret` and
`env-assignment` match a *labelled* value rather than a value shape. A bare
40-character base64 string is a hash, a checksum, a git object id, or a
base64-encoded test fixture far more often than it is an AWS secret, and a
rule that blocks all of them makes the feature unusable in any repo with a
lockfile — after which the user turns redaction off, which is strictly
worse than a slightly narrower rule.

Scanning happens over `AiCtx.prefix` and `AiCtx.suffix` (s49 §2), i.e. only
the bytes that would actually be sent, using `yew_re_search` with the
literal prefilter. Budget: ≤ 2 ms for the whole shipped set over a 4096-byte
context (part of s49 §8's 3 ms context stage), asserted in the perf lane.

### 5. Path exclusions

Checked against the buffer's workspace-relative path **and** its basename
before any content is read, so an excluded file never even builds a context:

| Glob | Matches |
|---|---|
| `.env*` | `.env`, `.env.local`, `.env.production` |
| `*secret*` | any path component containing "secret" |
| `*credential*` | `credentials`, `aws-credentials.json` |
| `.ssh/` | anything under a `.ssh` directory component |
| `*.pem` | certificates and keys |
| `*.key` | keys |
| `*.p12`, `*.pfx`, `*.jks`, `*.keystore` | key stores |
| `id_rsa*`, `id_ecdsa*`, `id_ed25519*` | ssh private keys by conventional name |
| `.netrc`, `_netrc`, `.npmrc`, `.pypirc` | credential files by convention |
| `.aws/`, `.gnupg/`, `.docker/config.json` | credential directories |
| `*.kdbx`, `*.gpg`, `*.asc` | password databases and armored keys |

Matching rules, pinned:

- A pattern ending in `/` matches any **path component**; every other
  pattern is matched against the basename and against the full relative
  path, so `.ssh/` catches `vendor/.ssh/id_rsa` and `*.pem` catches
  `certs/server.pem`.
- Matching is **case-insensitive**, because `SECRET.txt`, `Credentials.cs`
  and `.PEM` are all real, and because two of our four target platforms have
  case-insensitive filesystems.
- `*` does not cross a `/` except in the trailing-slash form; `?` matches
  one byte; there is no `**`. The matcher is 40 lines and has a fuzzer;
  borrowing a full glob engine here would be the third one in the tree.
- User rows append via `ai.exclude_paths` (a Fletch list); removing a
  shipped row requires `ai.exclude_replace = true`, and setting it logs one
  WARN naming the rows that were dropped.
- An excluded buffer is silent — no message per keystroke — but
  `:ai status` says `AI: excluded (path matches '.env*')`, so a user
  wondering why the ghost stopped gets an answer on request.

### 6. The blocking rule, and why it is blocking

> A redaction hit on a **cloud** backend **blocks the request** and tells
> the user which rule matched and where. It is never scrubbed and sent.
> A hit on a **loopback** backend elides the matched span (replaced by
> `<redacted:rule>`) and sends the rest, with one INFO message per session.

```
AI request blocked: line 87 matches 'aws-access-key'.
The text near your cursor looks like a credential, so nothing was sent to
api.anthropic.com.  [g] go to line 87   [i] ignore this file for the
session   [p] :ai privacy
```

Three reasons this is the right choice, written out because "just scrub it"
is the obvious suggestion and it is wrong:

1. **A scrubber's failures are invisible and permanent.** If the pattern set
   catches 95 % of secret shapes, then scrubbing means 5 % of secrets are
   transmitted and the user is *told nothing* — the request succeeded, the
   ghost appeared, the day continued. Blocking makes the same 95 % a visible
   event and the remaining 5 % no worse than it already was. The failure
   mode of blocking is an annoying prompt; the failure mode of scrubbing is
   a leaked production key nobody knows to rotate.
2. **The context around a secret is often as sensitive as the secret.** The
   4096 bytes surrounding an AWS key contain the bucket names, the account
   structure, the internal hostnames, and frequently a second credential the
   pattern set does not know. Removing 20 bytes from that window and calling
   it clean is theatre.
3. **A hit is information the user needs.** "There is a credential in this
   file" is worth an interruption regardless of AI: it means the secret is
   probably also in the git history, in the editor's undo journal, and in
   whatever backup ran last night. The block is the only moment yew ever
   has to say so.

The loopback asymmetry is justified by the thing that actually differs:
nothing leaves the machine, so there is no disclosure to prevent — only the
model's own confusion to avoid, which eliding solves. `redact.h` carries
both halves of this reasoning in its header comment.

`ai.on_redact` (enum `block|elide|off`, default **`block`**) exists for
users who genuinely need it; setting it to `off` logs one WARN per session
naming the rules that will no longer fire, so the choice is not silent.
`ai.on_redact` may **not** be set from a workspace `.yew.fl` — a repo
must not be able to weaken its author's privacy settings. The option
declares `YEW_OPT_GLOBAL` and `optin.c` refuses a `FL_ORIGIN_WORKSPACE`
write, naming the origin in the error.

### 7. The shipped presets

Loaded with `:ai preset local` / `:ai preset cloud`, or imported from
`init.fl`. Both are ordinary Fletch, readable, and are the answer to "what
should I set?".

**`runtime/preset.ai-local.fl`**

```fletch
# yew AI preset — LOCAL models (ollama).
#
# Nothing in this file causes anything to leave your machine: every
# request goes to a loopback socket.  Load it with:
#     :ai preset local
# or, in init.fl:
#     import "preset.ai-local"
#
# Requires ollama running:  ollama serve
# Then pull a completion model:
#     ollama pull qwen2.5-coder:7b      # the recommendation below

ai.backend("local", {
    kind:  "ollama",
    url:   "http://127.0.0.1:11434",   # loopback only; no TLS, no wire
    model: "qwen2.5-coder:7b",
    # fim: "auto" picks fill-in-the-middle when the model family supports
    # it (Sprint 49 §3's table).  FIM models see the code AFTER your cursor
    # too, which is most of why local completion feels good.
    fim:    "auto",
    stream: true,
})

# Model sizing, honestly:
#   1.5B  ~0.6 GB  instant, and about as smart as it sounds
#   7B    ~4.5 GB  the sweet spot: ~60-120 tok/s on a modern GPU,
#                  ~10-20 tok/s CPU-only, which is usable but not brisk
#   14B+  ~9 GB+   noticeably better completions, first token past 300 ms
#                  on most hardware -- past the point where a ghost feels
#                  like it appeared rather than arrived
# If :ai stats shows a p50 first token above ~250 ms, drop a size class
# before you touch any other setting.

set({
    ai.enable:        true,
    ai.backend:       "local",

    # A local model costs watts, not dollars, and the round trip is short,
    # so we can afford to ask sooner than the 350 ms default.
    shadow.ai_debounce_ms: 250,

    # Local means the whole 4 KiB context is free.  Cloud does not.
    ai.context_bytes: 4096,
    ai.max_tokens:    256,
    ai.max_lines:     8,

    # Low temperature: this is code completion, not brainstorming.  At 0.0
    # some models loop; 0.10 is the lowest value that reliably does not.
    ai.temperature:   10,          # tenths of a percent x10 -> 0.10
})

# Redaction still runs.  On a loopback backend a hit elides the span
# instead of blocking, because nothing is being disclosed -- see
# :ai privacy.  Leave this alone unless you have read that page.
# set({ ai.on_redact: "elide" })
```

**`runtime/preset.ai-cloud.fl`**

```fletch
# yew AI preset — CLOUD models (Anthropic / OpenAI-compatible).
#
# READ :ai privacy FIRST.  Loading this file arranges for parts of the
# files you edit to be sent over the internet to a company's servers.
#
# Requires curl (any TLS work happens there, not in yew):
#     curl --version      # need >= 7.63
#
# Your API key is NEVER written in this file or in init.fl.  init.fl gets
# pasted into bug reports and committed to dotfile repos; a key in it is a
# key you will rotate in a hurry someday.  Name an environment variable,
# or a command that prints the key.

ai.backend("work", {
    kind:      "anthropic",
    url:       "https://api.anthropic.com",
    model:     "claude-sonnet-4-6",
    transport: "curl",                     # https always means curl

    key_env:   "ANTHROPIC_API_KEY",        # export it from your shell rc,
                                           # or:
    # key_cmd: ["pass", "show", "api/anthropic"],   # argv array, no shell
    # key_cmd: ["security", "find-generic-password", "-w", "-s", "anthropic"],

    max_tokens: 256,                       # the Messages API requires it
    stream:     true,
})

# An OpenAI-compatible endpoint (OpenAI, OpenRouter, vLLM, LM Studio):
# ai.backend("oai", {
#     kind: "openai", url: "https://api.openai.com",
#     model: "gpt-4.1-mini", transport: "curl",
#     key_env: "OPENAI_API_KEY",
# })

set({
    ai.enable:  true,
    ai.backend: "work",

    # Deliberately conservative, and each number is a decision:

    # Half the local context.  Every byte here is a byte that leaves the
    # machine and that you are billed for on the way in.
    ai.context_bytes: 2048,

    # A network round trip is not a keystroke.  600 ms means you have
    # genuinely paused, not drawn breath between two words.
    shadow.ai_debounce_ms: 600,

    # Shorter answers: an 8-line cloud completion you reject cost real
    # money, and the accept rate on long completions is poor (:ai stats).
    ai.max_tokens: 192,
    ai.max_lines:  4,

    ai.temperature: 10,

    # BLOCK, not elide.  A secret must never leave this machine, and a
    # scrubber that is 99% right leaks on the hundredth file.
    ai.on_redact: "block",

    # Ask per workspace.  A cloud grant for your own scratch repo is not a
    # grant for your employer's.
    ai.default_workspace: "ask",
})
```

Both files are mechanically checked: they parse with zero diagnostics, they
set only options that exist in `yew_opts[]`, every value is inside its
declared range, and the comments' claimed defaults match §s49 §7's table
(a test greps the comment lines for `default` claims and compares).

### 8. The statusline `[AI]` badge — `src/ui/statusline.c`

s15 reserved the `badges` segment with *"AI (49) — width 0 now"*. It gets a
width here:

| State | Badge | Theme role | When |
|---|---|---|---|
| disabled | *(absent, width 0)* | — | `ai.enable == false`, no backend, workspace denied, or `MODULES` lacks `ai` |
| idle | `[AI]` | `badge.ai` | enabled, nothing in flight |
| streaming | `[AI~]` | `badge.ai.active` | an `AiCall` is live |
| error | `[AI!]` | `badge.ai.error` | last call ended in any §s48 §10 class except `CANCELLED`; cleared by the next success or by `:ai log` |

The **remote marker** follows facsimile's convention verbatim: a backend
whose URL is not loopback renders the host between the name and the state
suffix.

| Situation | Rendering |
|---|---|
| local idle | `[AI]` |
| local streaming | `[AI~]` |
| remote idle | `[AI->api.anthropic.com]` |
| remote streaming | `[AI->api.anthropic.com~]` |
| remote error | `[AI->api.anthropic.com!]` |
| host longer than `ai.badge_host_max` (20 cells) | elided from the **left**: `[AI->…c.anthropic.com]` — the registrable part is the informative end |

- **ASCII `->`, not `→`.** The arrow is EAW-Ambiguous: it renders 1 cell
  under `ambiguous_wide = false` and 2 under `true` (s36's option), so a
  glyph arrow makes the badge's width depend on a setting the statusline's
  truncation math already resolved. Same class of bug as s43 §6's
  one-cell-glyph gutter assert, and the same answer: use ASCII where width
  must be certain. `->` is two guaranteed cells forever.
- The remote marker is not decoration. It is the one always-visible
  reminder that this session's completions are leaving the machine, which is
  precisely the fact a user forgets after the opt-in prompt scrolls away.
  It is therefore **not** suppressible by an option; `ai.badge` may be set
  to `off` only together with disabling remote backends, and the option's
  validator enforces it.
- Priority 5 in s15's drop table (dropped with `percent` when the line is
  narrow) — except that a **remote** badge is priority 2, ahead of the
  encoding and eol segments. A privacy indicator that vanishes on a narrow
  terminal is not an indicator.
- `blocked` (§6) is an `error`-state badge with its own message; it does not
  get a fifth glyph, because a fifth state the user must learn to read is
  worse than a message they can read directly.

### 9. `docs/ai-privacy.md` — the honest page

Rendered by `:ai privacy` into a scratch buffer and included verbatim in
Sprint 59's manual. Structure, and every row is a claim we can defend:

| Section | Contents |
|---|---|
| The one-paragraph answer | AI is off. When on, the text around your cursor and the file's relative path go to the backend you chose, and nowhere else. |
| **What is sent**, exactly | the field table below |
| **What is never sent** | the file's absolute path, your username, hostname, OS, editor version beyond `User-Agent: yew/1.0.0`, other open buffers, the workspace file list, git remotes or branch names, `:ai stats` counters, any option value, any telemetry — because there is none |
| **When** | 350 ms (or your `shadow.ai_debounce_ms`) after you stop typing, in insert mode, at a qualifying position; never with a selection, multi-cursor, during macro replay, or in `--batch` |
| **Where** | the configured URL, printed literally; loopback vs internet stated per backend |
| **Credentials** | env var or command; never in `init.fl`; redacted from every log path |
| **Redaction** | the §4 table reproduced, and the §6 blocking rule with its reasoning |
| **Excluded paths** | the §5 table |
| **What yew stores** | `ai_stats.fl` (counters only, deletable), the log (no bodies unless you opt in), the trust db grant |
| **The debug mode** | `YEW_AI_DEBUG=1` + `ai.debug_bodies = true` logs full prompts and completions to the state log; both are required, it is off by default, it prints a WARN banner on every session that enables it, and keys stay redacted even then |
| **What we cannot promise** | what the remote provider does with the bytes after they arrive |

The field table:

| Field | Value | Sent to local | Sent to cloud |
|---|---|---|---|
| prefix | ≤ `ai.context_bytes × 75 %` bytes before the cursor | yes | yes |
| suffix | the remainder after the cursor | yes | yes |
| path | workspace-**relative**, `/`-separated | yes | yes |
| language | `"c"`, `"rust"`, … | yes | yes |
| model, max tokens, temperature, stop sequences | your settings | yes | yes |
| `User-Agent` | `yew/1.0.0` | yes | yes |
| API key | from env or command | n/a | yes, in a header |
| anything else | — | no | no |

### 10. Defer

- Provider-specific data-retention or training-opt-out claims → **never**
  written into yew's docs. They change, we cannot verify them, and a
  stale promise in our manual is worse than the honest sentence in §9's last
  row.
- Telemetry, crash-report upload, usage analytics → **never in any version**.
  Stated as a permanent non-goal in `docs/ai-privacy.md` so nobody proposes
  an opt-in version of it later.
- Encrypting `ai_stats.fl` or the trust db → no: they contain counters and
  directory paths, and encrypting them would require a key we would have to
  store next to them.
- A secret **scanner** as a general editor feature (scan on open, warn on
  save) → post-1.0. The pattern set here is scoped to what would be
  transmitted, and `redact.h` says so, so the scope creep is a decision
  rather than an accident.
- Per-language or per-directory AI settings → express them today with
  `on("buf.open", …)`; the declarative form is post-1.0 (s36 §8 pinned the
  same answer for options).
- Multi-file context, chat, and everything else the AI surface is not →
  s49 §10, unchanged.

## Testing Strategy

- **Unit — redaction** (`tests/unit/test_ai_redact.c`): every §4 row driven
  positive with ≥ 3 realistic samples and negative with ≥ 3 near-misses
  (a 40-char git object id must **not** trip `aws-secret`; `AKIA` followed
  by 15 characters must not trip `aws-access-key`; a CJK comment of 40
  characters must not trip anything — the `\w` pitfall, asserted); the
  merge/replace semantics of the user file; a user pattern that fails to
  compile reports `YewReErr` with file:line and does not disable the shipped
  set.
- **Unit — globs** (`test_ai_paths.c`): every §5 row plus case variants,
  nested components, a path with a `/` inside a component name (impossible,
  asserted as such), a basename-only match, and `**` rejected;
  fuzzed against random paths for crash-freedom.
- **Unit — trust schema** (`test_ai_trust3.c`): allow/deny/unset resolution;
  `ai` independent of `state`; a `dev`/`ino` mismatch clearing the grant; a
  **schema-2 db round-tripping with its `plugins:` block byte-identical**;
  `schema:` emitted as the max; sorted keys preserved.
- **Script** (`tests/script/ai_optin_*.fl`): the four §2 steps for both
  branches; `<esc>` at each step changing nothing (config and trust db
  byte-identical before and after); the cloud branch refusing anything but
  the literal word `send`; the no-tty branch failing with the exact message
  and exit 1; `:ai disable` and `:ai forget` as exact inverses.
- **Script — blocking** (`ai_redact_block.fl`): a fixture file with an AWS
  key near the cursor, a cloud backend selected → **zero bytes written to
  the transport** (asserted by a counting mock, not by an absent response),
  the exact §6 message, and `[g]` jumping to the right line; the same
  fixture on a loopback backend → the span replaced by `<redacted:…>` in the
  request body golden and the rest sent.
- **Golden — presets** (`test_ai_presets.c`): both files parse with zero
  diagnostics; every option they set exists and is in range; every `default`
  claim in a comment matches `yew_opts[]`; loading `preset.ai-cloud.fl`
  without `$ANTHROPIC_API_KEY` set produces the `YEW_AI_ERR_AUTH` message
  naming the variable, not a crash and not a silent disable.
- **Pty goldens** (`tests/pty/ai_badge_*.golden`): all four badge states,
  local and remote, at 200, 80 and 40 columns; the remote badge surviving
  the 40-column truncation while `percent` and `encoding` are dropped; the
  left-elided long host; the badge absent entirely in a `MODULES=""` build
  and with `ai.enable = false`; the whole opt-in flow, all four steps, both
  branches.
- **Determinism**: the trust db and `ai_stats.fl` emit byte-identically
  across two runs; the badge goldens are byte-stable in the double-run lane.
- **Perf**: the shipped pattern set over a 4096-byte context ≤ 2 ms
  (baseline committed); the glob set over a path ≤ 20 µs; opt-in adds
  nothing to the startup path (config load still < 1 ms, s36's gate).
- **Fuzz** (`tests/fuzz/fuzz_ai_redact.c`): random buffers (including
  invalid UTF-8, 1 MiB single lines, and every shipped pattern's near-miss
  neighbourhood) through the full pattern set — no crash, no hang (s20's
  no-backtracking guarantee makes this a real assertion), and the hit's
  `off`/`len` always inside the context.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror` for `MODULES="lsp ai
   fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind/determinism lanes
   clean.
2. **Off by default is proven, not asserted**: a fresh profile with
   `MODULES=ai` compiled in makes **zero** `socket(2)` calls across a
   scripted 500-keystroke editing session; all three §1 gates have a firing
   test.
3. **The CI grep gate on bodies** — in `scripts/bans.sh`, and it fails the
   build on a hit:

   ```sh
   # No source path may log prompt or completion text outside the
   # explicitly opt-in debug mode.
   grep -rnE 'yew_log[^;]*(ctx->prefix|ctx->suffix|->text\b|prompt|completion|body)' \
        src/ --include='*.c' | grep -v 'yew_ai_debug_body' && exit 1
   grep -rn 'yew_ai_debug_body' src/ | wc -l   # must be <= 4 call sites
   ```

   plus a positive test: with `YEW_AI_DEBUG` unset **or** `ai.debug_bodies`
   false, a full request/response cycle writes a log containing the model
   name, timings and byte counts and containing **no substring of the
   prompt or the completion** (asserted against a fixture with a unique
   marker string in it). With both set, the bodies appear, the WARN banner
   appears once, and the `authorization`/`x-api-key` values are still
   `<redacted N bytes>`.
4. Every §4 pattern row has ≥ 3 positive and ≥ 3 negative fixtures, all
   green; no row uses `\w`, `\d` or `\s` (`grep -n '\\\\[wds]'
   runtime/ai-deny.fl` → empty); the whole set runs in ≤ 2 ms over 4096
   bytes.
5. The blocking rule holds: a cloud request with a hit transmits **zero
   bytes** (counted at the transport, both HTTP and curl), shows §6's
   message with the rule name and line number, and offers the three keys;
   the loopback path elides and sends, with the request-body golden proving
   the elision text.
6. `ai.on_redact` cannot be set from a workspace config: a `.yew.fl`
   attempting it fails with a diagnostic naming `FL_ORIGIN_WORKSPACE`, and
   the global value is unchanged (script-tested).
7. Every §5 glob row fires; the matcher rejects `**`; case-insensitivity is
   asserted on all four target platforms' path shapes; an excluded buffer
   builds **no context at all** (asserted by a counter inside
   `yew_ai_context_build`).
8. Trust db schema 3: allow/deny/unset all resolve; a schema-2 db
   round-trips with `plugins:` byte-identical; `dev`/`ino` mismatch clears
   the `ai` grant; the db is written sorted and atomically.
9. The opt-in flow: all four steps pty-golden for both branches; `<esc>` at
   every step leaves config and trust db byte-identical; the cloud branch
   accepts only the literal `send`; no prompt is ever issued without a tty;
   step 4's file list matches what was actually written (asserted by
   diffing the files).
10. Both presets parse with zero diagnostics, set only real in-range
    options, and their comment-claimed defaults match `yew_opts[]`;
    `:ai preset local` followed by `:ai preset cloud` leaves exactly one
    selected backend and no residue (s36's ledger teardown reused).
11. The badge: four states × local/remote pty goldens at three widths; the
    remote badge survives 40-column truncation while lower-priority segments
    are dropped; the marker is ASCII `->` (`grep -n '→' src/ui/statusline.c`
    → empty); `ai.badge = "off"` with a remote backend configured is a
    validation error.
12. `docs/ai-privacy.md` exists, is opened by `:ai privacy`, and its field
    table is **verified against the wire**: a test builds a request for each
    adapter and asserts the set of distinct data fields in the body and
    headers equals the table's "sent" rows exactly — a new field added to a
    prompt template without a docs row fails the build.
13. `make fuzz FUZZ_ITERS=200000` green for `fuzz_ai_redact`; no hang
    (s20's guarantee), `tests/fuzz/crashes/` empty.
14. Campaign 10 closes with nothing deferred: `grep -rn 'Sprint 4[89]\|Sprint
    50' src/mod/ai/` finds no `YEW_CMD_DEFERRED` and no hard-error stub;
    `:shadow.stats` lists `ai` as registered; `:ai enable` works end to end.
15. ≥ 150 assertions across the four new unit files; ≥ 12 script tests; pty
    goldens byte-stable across the determinism lane's double run.
