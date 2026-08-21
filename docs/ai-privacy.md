# AI and your privacy

AI is off by default. When you turn it on, yew sends the text around your
cursor and the file's workspace-relative path to the backend you selected,
and nowhere else. A local backend uses a loopback socket on this computer; a
cloud backend sends those bytes over the internet to the configured URL.

## What is sent, exactly

| Field | Value | Local | Cloud |
|---|---|---:|---:|
| prefix | At most 75% of `ai.context_bytes` immediately before the cursor | yes | yes |
| suffix | The remaining context budget immediately after the cursor | yes | yes |
| path | Workspace-relative, `/`-separated path | yes | yes |
| language | Buffer language name, such as `c` or `rust` | yes | yes |
| generation settings | Model, maximum tokens, temperature, and stop sequences | yes | yes |
| User-Agent | `yew/1.0.0` | yes | yes |
| API key | Read from the configured environment variable or command, in an HTTP header | n/a | yes |
| anything else | — | no | no |

## What is never sent

yew does not send the file's absolute path, your username, hostname, operating
system, editor version beyond the User-Agent above, other open buffers, the
workspace file list, git remotes or branch names, `:ai stats` counters, editor
option values, telemetry, usage analytics, or crash reports.

## When

The default trigger is 350 ms after you stop typing in insert mode at a
qualifying position. `shadow.ai_debounce_ms` changes that delay. A request is
not made while a selection or multiple cursors are active, during macro
replay, or in `--batch`.

## Where

The configured backend URL is the destination. `127.0.0.1`, `::1`, and
`localhost` stay on this machine. Other hosts are remote and are displayed in
the statusline as `[AI->host]` while enabled.

## Credentials

Configure `key_env` or `key_cmd`; never put a literal credential in `init.fl`.
Keys are read at request time and are not stored in the backend registry,
statistics, or ordinary logs. Authentication header values remain redacted
even when debug-body logging is explicitly enabled.

## Secret deny rules

Only the prefix and suffix that would be transmitted are scanned. The shipped
rules are:

| Rule | Regex | Detects |
|---|---|---|
| `aws-access-key` | `\b(A3T[A-Z0-9]\|AKIA\|ASIA\|ABIA\|ACCA\|AGPA\|AIDA\|AIPA\|ANPA\|ANVA\|AROA)[A-Z0-9]{16}\b` | documented AWS access-key identifiers |
| `aws-secret` | `(?i:aws_?secret_?access_?key)[^\n]{0,20}[:=][^\n]{0,4}[A-Za-z0-9/+=]{40}` | a labelled 40-character AWS secret |
| `pem-private-key` | `-----BEGIN [A-Z0-9 ]{0,32}PRIVATE KEY-----` | PEM private-key headers |
| `ssh-private-key` | `\bssh-rsa AAAA[A-Za-z0-9+/]{100,}` | pasted `ssh-rsa` private-key blobs |
| `bearer-token` | `(?i:bearer)[ \t]+[A-Za-z0-9._~+/-]{16,}={0,2}` | bearer authorization tokens |
| `authorization-header` | `(?i:authorization)[ \t]*[:=][ \t]*["']?[A-Za-z0-9._~+/-]{16,}` | credential-bearing authorization assignments |
| `jwt` | `\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b` | three-part JSON web tokens |
| `env-assignment` | `(?i:^[ \t]*(export[ \t]+)?[A-Za-z_][A-Za-z0-9_]*(SECRET\|TOKEN\|PASSWORD\|PASSWD\|PRIVATE_KEY\|API_?KEY\|CREDENTIAL)[A-Za-z0-9_]*[ \t]*=[ \t]*["']?[^\n"' \t]{8,})` | secret-named `.env` assignments |
| `conn-string-creds` | `\b[a-z][a-z0-9+.-]{1,31}://[^\n/:@ ]{1,64}:[^\n/@ ]{1,128}@` | URLs containing `user:password@host` |
| `github-token` | `\bgh[pousr]_[A-Za-z0-9]{36,255}\b` | GitHub token families |
| `slack-token` | `\bxox[baprs]-[A-Za-z0-9-]{10,}\b` | Slack token families |
| `google-api-key` | `\bAIza[A-Za-z0-9_-]{35}\b` | Google API keys |
| `openai-key` | `\bsk-[A-Za-z0-9_-]{20,}\b` | OpenAI-compatible keys |
| `anthropic-key` | `\bsk-ant-[A-Za-z0-9_-]{20,}\b` | Anthropic keys |
| `private-key-var` | `(?i:private_?key)[ \t]*[:=][ \t]*["'][^\n"']{32,}` | long private-key string literals |
| `htpasswd-bcrypt` | `\$2[aby]?\$[0-9]{2}\$[A-Za-z0-9./]{53}` | bcrypt credential hashes |

On a cloud backend, a match blocks the whole request before a request body is
built or a transport starts. yew reports the rule and line; it does not scrub
one recognized value and transmit the surrounding context. On a loopback
backend, the matched span becomes `<redacted:rule>` and the remaining context
may be sent locally. `ai.on_redact = "off"` is available only as a global
user decision and warns when it disables this protection; workspace config
cannot weaken it.

Blocking is deliberate. A scanner can miss an unfamiliar credential, and
scrubbing a recognized value would make such failures silent and permanent.
The surrounding context may itself contain internal hosts, account names, or
a second secret. Blocking makes the match visible before any remote bytes are
sent.

## Excluded paths

Excluded buffers are rejected before their content is read. Matching is
case-insensitive; directory patterns match any path component.

| Patterns | Typical files |
|---|---|
| `.env*` | `.env`, `.env.local`, `.env.production` |
| `*secret*`, `*credential*` | secret- or credential-named files/components |
| `.ssh/`, `.aws/`, `.gnupg/` | conventional credential directories |
| `.docker/config.json` | Docker registry credentials |
| `*.pem`, `*.key` | certificates and keys |
| `*.p12`, `*.pfx`, `*.jks`, `*.keystore` | key stores |
| `id_rsa*`, `id_ecdsa*`, `id_ed25519*` | conventional SSH private-key names |
| `.netrc`, `_netrc`, `.npmrc`, `.pypirc` | conventional credential files |
| `*.kdbx`, `*.gpg`, `*.asc` | password databases and encrypted/armored files |

Add project-independent exclusions in `init.fl` with a Fletch string list;
they append to the table above:

```fletch
set({"ai.exclude_paths": ["*.wolf-key", "generated/private/?"]})
```

`*` and `?` do not cross `/`, and `**` is rejected. Setting
`ai.exclude_replace = true` deliberately drops the shipped rows and writes a
warning naming them.

## What yew stores

- `ai_stats.fl` contains counters and latency summaries, never bodies.
- The AI log contains backend names, timings, byte counts, and error classes.
- `trust.fl` contains the workspace path and its independent allow/deny grant.

These files are deletable. yew has no telemetry, crash upload, or usage
analytics, and adding them is a permanent non-goal.

## Debug-body logging

Full prompt and completion bodies are logged only when both
`YEW_AI_DEBUG=1` and `ai.debug_bodies = true` are set. Enabling both prints a
warning once per editor session. This mode is for deliberate local debugging;
logs may then contain source code. Credentials in authorization headers are
still replaced with a byte-counted redaction marker.

## What yew cannot promise

Once bytes reach a remote provider, yew cannot recall them, delete them, know
who can read them, or verify how long they are kept or whether they are used
for training. Those policies belong to the provider and can change; yew does
not copy them into this manual.
