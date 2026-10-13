# Sprint 54: Plugin System — Packages, Lifecycle, Capabilities

**COMPLETE 2026-08-23.** The full and stripped GCC surfaces, full Clang
ASan/UBSan fast tier, focused deterministic PTYs, manifest fuzz seam, and
both committed performance budgets are green. The one-hour sanitized
manifest campaign is a named on-demand/nightly CI gate.

## Prerequisites

- Sprint 13 — named-command registry, keymap layers (mode → user → plugin
  stacking order is already the keymap contract).
- Sprint 26 — fuzzy finder engine (the plugin picker reuses it).
- Sprint 31 — Fletch module system, sandboxed io.
- Sprint 32 — `yew fl` subcommand plumbing in the driver; runtime stack
  traces (error containment renders them).
- Sprint 34 — flapi: capability-checked bindings keyed by **script
  origin**, `edit{}` transactions, `on(...)` hooks. Plugins are new
  origins; the choke point already exists.
- Sprint 36 — init.fl loading, options model, `bind()`, workspace
  `.yew.fl` trust prompt, reload. **Assumptions** (index entries
  don't name internals): (a) s36's reload tears down the user config
  layer via an origin-tagged registration ledger — s54 generalizes that
  ledger to per-plugin origins; (b) s36 persisted trusted-dir decisions
  in `$XDG_STATE_HOME/yew/trust.fl` (Fletch data literals, atomic
  write, versioned `schema:` field) — s54 extends its schema; (c) a
  restricted **data-literal reader** (`fl_data_read`) exists from the
  s25/s36 workspace-state path — the manifest parser reuses it.
- Sprint 37 — `--batch` stdio contract (prompts are impossible there;
  see Deliverable 6).
- Binding plan docs: `00-decisions.md` (plugins row, invariants 3/9),
  `01-architecture.md` (`src/mod/plug/`), `02-fletch.md` (requirement 8:
  capabilities per script origin).

## Goals

Land the whole plugin system except distribution: package layout and the
pure-literal `plugin.fl` manifest, XDG + workspace discovery, the
load→init→enable/disable→unload lifecycle with **atomic zero-residue
teardown**, the event bus as a namespaced view of s34's hooks, capability
gating (fs/shell/net/clipboard) with first-use prompts persisted in the
trust db, honest 1.0 trust-model documentation, error containment
(auto-disable after N errors, never a crash), the 1.0 API stability
contract, and `yew plug list/enable/disable/reload` plus a picker UI.
All code under `src/mod/plug/` behind `MODULES=plugins`; a build without
it hard-errors on every plugin surface, never no-ops. `yew pkg`
(install/update/lock), the example plugins, and the cloud preset land in
Sprint 55.

## Deliverables

### 1. Module boundary — `src/mod/plug/plug.c|h`, `manifest.c`, `sandbox.c`

`src/mod/plug/hooks.c` carries the event-bus namespacing (Deliverable 5);
`pkg.c` is Sprint 55's file. Makefile: `plug` sources compile only when
`plugins` ∈ `MODULES`; core carries one weak seam (`yew_plug_boot(Ed *)`
no-op stub in core when excluded is **banned** — instead every entry
point compiled out produces the module hard-error):

```
yew: error: built without plugin support (MODULES=plugins)
```

`yew plug …` in an excluded build prints exactly that, exit 1. Pitfall:
the hard-error string is tested byte-exact; keep it in one
`static const char` in `plug.h` shared by driver + cmdline paths.

### 2. Package layout and manifest — `src/mod/plug/manifest.c`

A plugin is a directory:

```
<name>/
  plugin.fl        manifest — pure literal, REQUIRED
  src/*.fl         modules; entry names one of them
  (anything else)  ignored by the loader (docs, LICENSE, tests…)
```

`plugin.fl` is **one map literal, no code** — read with `fl_data_read`
(the workspace-state reader: literals only; any call, identifier, or
operator is a parse error). Why: the manifest must be readable and
displayable (capability prompts quote it) *before any plugin code runs*;
a manifest needing evaluation is a code-execution hole.

```fletch
# plugin.fl — manifest. Pure literal: one map, no expressions.
{
  name: "trailing-ws",          # [a-z0-9-]{1,32}; must equal the dir name
  version: "1.0.0",             # semver, informational at 1.0
  api: 1,                       # plugin API major (Deliverable 9)
  entry: "src/main.fl",         # relative; must resolve inside the dir
  capabilities: [],             # subset of: "fs" "shell" "net" "clipboard"
  events: ["buf.open"],         # every ctx.on() name must be listed here
  description: "strip it",      # optional, one line, shown in picker
}
```

```c
typedef enum {                       /* bit positions in caps masks */
    YEW_CAP_FS, YEW_CAP_SHELL, YEW_CAP_NET, YEW_CAP_CLIPBOARD, YEW_CAP__N
} YewCap;

typedef struct PlugManifest {
    u32         name;                /* interned */
    const char *version, *entry, *desc;   /* arena-owned */
    u32         api;
    u32         caps_wanted;         /* YewCap bitmask */
    u32        *events; u32 nevents; /* interned event names */
    const char *dir;                 /* absolute plugin root */
} PlugManifest;

/* false + diagnostics on any violation; never partial output */
bool yew_plug_manifest_read(Arena *a, const char *dir,
                            PlugManifest *out, DiagCtx *dc);
```

Validation table (each row is a firing unit test):

| Rule | Error names |
|---|---|
| unknown key | the key ("did you mean…?" for 1-edit typos) |
| `name` ≠ dir basename | both strings |
| `entry` escapes dir (`..`, absolute, symlink out) | resolved path |
| capability not in the 4-name set | the string |
| event not in the frozen table (Deliverable 5) | the string |
| `api` > supported | "requires plugin API N; this yew speaks 1" |
| dependencies key | "plugins have no dependencies at 1.0" (pinned) |

Pitfall: validate `entry` by `realpath` containment *after* resolving,
not by string prefix — `src/../../evil.fl` passes a prefix check.

### 3. Discovery and precedence — `yew_plug_discover`

Scan order (later shadows earlier **by name**; shadowed dirs are listed
as `shadowed` in `yew plug list`, never silently dropped):

| # | Directory | Role | Gate |
|---|---|---|---|
| 1 | `$XDG_DATA_HOME/yew/plugins/` (`~/.local/share` fallback) | installed (`yew pkg`, Sprint 55) | none |
| 2 | `$XDG_CONFIG_HOME/yew/plugins/` (`~/.config` fallback) | development / hand-managed | none |
| 3 | `<workspace>/.yew/plugins/` | project-shipped | **workspace trust** (s36 prompt); untrusted → listed as `blocked`, never loaded |

Within a dir, entries sort bytewise by name (`yew_sort_stable`) — the
plugin list, origin-id assignment, and load order are deterministic
(invariant 5 extends to plugin-driven renders). Non-dirs and dirs
lacking `plugin.fl` are skipped with one `yew_log` line each. A manifest
that fails to parse yields a `Plug` in `PLUG_ERROR` state carrying the
diagnostic — visible in the picker, not silently absent.

### 4. Lifecycle and atomic teardown — `src/mod/plug/plug.c`

```
DISCOVERED ──load──▶ LOADED ──enable──▶ ENABLED
                        ▲                  │
                        └──── disable ─────┘        any step may fail
ERROR (manifest/init/limit)      UNLOADED (VM state dropped, GC'd)
```

```c
typedef enum { PLUG_DISCOVERED, PLUG_LOADED, PLUG_ENABLED,
               PLUG_DISABLED, PLUG_ERROR } PlugState;
typedef struct Plug {
    PlugManifest mf;  PlugState st;
    u32 origin_id;                    /* flapi origin (s34) */
    u32 err_count;                    /* Deliverable 8 */
    FlValue module;                   /* entry module, GC-rooted while loaded */
} Plug;

bool yew_plug_enable (Ed *ed, Plug *p, DiagCtx *dc); /* load+init if needed */
bool yew_plug_disable(Ed *ed, Plug *p);              /* atomic teardown */
bool yew_plug_reload (Ed *ed, Plug *p, DiagCtx *dc); /* disable→rescan→enable */
```

- **load**: compile+run `entry` as an s31 module under `origin_id`. The
  module must export `fn init(ctx)`; missing export → ERROR naming it.
- **init(ctx)**: `ctx` is a host-built map of namespaced API entry
  points (`ctx.on`, `ctx.command`, `ctx.bind`, `ctx.set`, `ctx.attr`,
  `ctx.overlay`, `ctx.buf`, `ctx.win`, `ctx.ws`, `ctx.msg`,
  `ctx.name`). Everything a plugin registers goes **through ctx** — ctx
  is how registrations get origin-tagged. Frozen field list in
  Deliverable 9.
- **Registration ledger**: every ctx registration appends
  `{u32 origin_id; RegKind kind; u32 handle;}` to one `Vec(RegEntry)`
  in `Ed` (generalizing s36's user-layer ledger; the user config is
  origin 0 and keeps using it). `RegKind`:
  `REG_{BIND,HOOK,CMD,OPTION,ATTR,OVERLAY,TIMER}`.
- **disable** walks the ledger **in reverse** removing this origin's
  entries (LIFO — keymap layers must unstack in reverse), drops the
  module root, runs a GC cycle, and asserts the registries' counts
  returned to their pre-enable sizes in debug builds.
- **Pitfall — zero residue**: a disabled plugin must leave *nothing*: no
  binds, hooks, commands, options, overlay attrs, timers, and no
  closures reachable from any registry (a keymap still holding a GC'd
  closure is residue even if it "works"). The hostile host-integration
  test snapshots every registry length before enable and after disable
  and asserts equality — this is the sprint's most important test.
- **Pitfall — teardown re-entrancy**: hooks must not fire into a plugin
  mid-teardown (its own `plug.disable` subscriber would run half-dead).
  Mask the origin in the hook dispatcher *before* walking the ledger;
  fire `plug.disable` to *other* origins only.
- **Pitfall — failing init is a transaction**: if `init(ctx)` throws,
  everything it registered so far is torn down by the same ledger walk;
  state → ERROR with the trace. Partial enables are unrepresentable.

### 5. Event bus — `src/mod/plug/hooks.c` over s34 hooks

The bus **is** s34's `on(...)` machinery; this sprint namespaces it per
origin and freezes the 1.0 event table. `ctx.on(name, fn)` checks
(a) name is in the frozen table, (b) name is listed in the manifest's
`events` — violation is an init-time error, not a silent no-op. Why the
manifest list: the picker and `yew plug list --long` can show what a
plugin listens to without running it.

Frozen 1.0 events (assumption: s34 landed the buf/win/mode set; this
table freezes names + payloads and adds the `plug.*` pair):

| Event | Payload | Fires |
|---|---|---|
| `buf.open` | buffer | after load, before first render |
| `buf.change` | buffer, damaged span | coalesced per input burst |
| `buf.save` | buffer | **before** write; may edit (same transaction) |
| `buf.saved` | buffer | after successful atomic write |
| `buf.close` | buffer | before teardown |
| `win.focus` | window | focus change |
| `mode.enter` / `mode.leave` | mode name | modal transitions |
| `ws.open` / `ws.close` | workspace | session bounds |
| `plug.enable` / `plug.disable` | plugin name | lifecycle (other origins) |
| `ed.idle` | — | ≥ 500 ms without input, once per idle period |

Dispatch order per event: origin 0 (user config) first, then plugins in
discovery order — deterministic. A hook's uncaught error never stops
dispatch to later subscribers (containment, Deliverable 8).

### 6. Capability gating — `src/mod/plug/sandbox.c` + trust db

s34's flapi checks are keyed by origin; plugins arrive as origins with
`caps_granted = 0`. Before yew evaluates a plugin entry module, it resolves
every capability the plugin declared, in the frozen order `fs`, `shell`,
`net`, `clipboard`. Each undecided declaration prompts on the message line:

```
plugin "session-notes" requests fs (read/write files outside open
buffers) — [a]llow once  [A]llow always  [d]eny once  [D]eny always
```

- No plugin bytecode, including entry-module top-level code, runs until
  every declaration has an allow or deny decision. Denial does not block
  loading; a later guarded call raises a catchable Fletch error.
- A capability **not declared** in the manifest never prompts: the call
  errors immediately (`plugin "x" did not declare net`) — declaring is
  the contract, prompting is consent.
- `always` answers persist to the trust db; `once` answers last the
  session.
- **`--batch` / headless**: never prompt (invariant: stdio contract,
  s37). Undecided capability → deny + error. `--grant name:cap`
  (repeatable) pre-grants for scripts; document with the batch docs.
- **Pitfall**: the prompt runs inside the event loop like any cmdline
  prompt (s18 machinery) — never a nested read loop mid-render. It defers
  the enable or reload request; no Fletch frame is suspended or callback
  replayed. Escape cancels the deferred enable without executing plugin
  code. This pre-execution contract is required because Fletch capability
  errors remain catchable and the VM has no heap-backed continuation ABI.

Trust db — `$XDG_STATE_HOME/yew/trust.fl`, s36 schema extended
(atomic write, hand-editable, pure literal):

```fletch
{
  schema: 2,                       # 1 = s36 (dirs only)
  dirs: { "/home/u/proj": "trusted" },
  plugins: {                       # s54: per plugin+capability
    "session-notes": { fs: "allow" },
    "webhookd":      { net: "deny" },
  },
}
```

Keys sorted bytewise on write (determinism); unknown keys preserved on
rewrite (forward compatibility with schema 3).

### 7. Trust model documentation — honest, in-tree

One section, written once, quoted by the picker help and the Sprint 55
author guide (`plug.h` header comment is the source of truth):

> Plugins are Fletch running in the same VM, same process, same address
> space as your editor. There is no memory isolation and no resource
> isolation: an enabled plugin can read any open buffer, burn CPU, or
> allocate until the GC sweats. Capability gates cover exactly the flapi
> I/O surface — fs, shell, net, clipboard — nothing else. Enabling a
> plugin is an act of trust in its author; capabilities limit blast
> radius, they do not create a sandbox. Read the source: it's Fletch,
> and it's short.

No "sandbox" wording anywhere user-facing (the file `sandbox.c` keeps
its architecture-doc name but its header comment states the above).
Security theater is a review rejection.

### 8. Error containment

Every plugin entry (hook dispatch, command invocation, key-bound
closure) runs in a protected frame: uncaught Fletch error → transaction
rollback (s34), stack trace to `yew_log`, notification, `err_count++`.

- Notification: `plugin "x" error (2/5): <first line> — :PlugInfo x for
  trace`.
- At `plug.error_limit` (option, default 5, per session) the plugin is
  auto-disabled via the normal teardown path plus notification
  `plugin "x" disabled after 5 errors`. Manual re-enable resets the
  count.
- The editor **never** crashes, never exits, never loses the current
  transaction's rollback on any plugin error path (invariant 1 applies:
  a plugin editing during `buf.save` that throws must roll back cleanly
  and the save proceeds with the pre-hook content).
- Pitfall: containment must catch errors *inside teardown too* — a
  plugin whose registered closure errors while being unregistered must
  not abort the ledger walk.

### 9. API stability contract — frozen at 1.0

| Surface | Frozen means |
|---|---|
| manifest keys + semantics (Deliverable 2) | additions only, keyed by `api` bump |
| event names + payload shapes (Deliverable 5) | never renamed; additions allowed |
| capability names (4) | closed set until api 2 |
| `ctx` fields: `on command bind set attr overlay buf win ws msg name` | signatures frozen |
| trust db + lockfile schemas | `schema:` versioned, unknown keys preserved |

Deprecation policy: a deprecated surface logs one warning per session
per plugin, survives every 1.x, and is removed only at an `api` major
bump (which the loader rejects with a migration-doc pointer). No
undocumented flapi calls: anything not in the author guide's reference
table may change without notice — the guide (Sprint 55) is the contract
boundary.

### 10. `yew plug` CLI, editor commands, picker

CLI (exit 0 ok / 1 error / 3 I/O per the ledger):

```
yew plug list [--long]     # porcelain: NAME\tSTATE\tVERSION\tSOURCE
yew plug enable  <name>    # persists desired-state in trust.fl plugins map
yew plug disable <name>    #   (key: enabled: true|false, default true)
yew plug reload  <name>    # in a running editor: via next focus; CLI: no-op+hint
```

`list` states: `enabled disabled error blocked shadowed` — deterministic
order (discovery order), byte-stable output for scripts. Editor
commands in the one registry: `ed.plug.list` (opens picker),
`ed.plug.enable/disable/reload/info` (E-mode completes plugin names).

Picker (s26 finder skin): one row per plugin — state glyph `●` enabled
/ `○` disabled / `✗` error / `⊘` blocked, name, version, description;
`Enter` toggles enable, `r` reloads, `i` opens info (manifest + granted
caps + last error trace). Region-registered for mouse like every list.

### 11. Defer

- `yew pkg` (install/update/lock/verify), example plugins, cloud preset
  → **Sprint 55**. `yew pkg …` this sprint: hard error naming Sprint 55.
- Plugin-author guide + man material → Sprint 55; final man assembly →
  Sprint 59.
- Plugin dependencies, native/FFI plugins, VM-level resource limits,
  central registry → post-1.0 (pinned; manifest rejects `dependencies`).

## Testing Strategy

- **Host integration** (`tests/unit/test_plug_{manifest,discovery,lifecycle,
  sandbox,trust,ctx,options,overlay,completion}.c`): every manifest row;
  real temp-XDG discovery; trusted data/config/workspace precedence;
  enable/disable/re-enable and failing-init transactions; reverse ledger
  teardown; all-event containment; four-capability/three-decision matrix;
  schema migration and unknown-key preservation. The authorization matrix
  lives at `fl_cap_check`, the shared host seam, because shell, net, and
  clipboard intentionally have no invented Fletch stdlib calls in 1.0.
- **Script/batch**: the existing interactive-refusal fixture asserts that
  `ed.plug.list` cannot open a picker in batch mode; smoke covers the
  stripped-module error, five porcelain states, and byte-identical list
  output. Plugin transactions themselves stay at the host integration seam
  so tests can inspect raw registry counts and every frozen event payload.
- **Pty**: capability prompt plus allow/deny once/always, real process
  restart persistence, picker, and toggle flows. The runner's determinism
  pass executes each case twice.
- **Fuzz**: `fuzz_fl_parse` frames raw bytes as `plugin.fl` maps and calls
  the real `fl_data_read` path; the one-hour ASan/UBSan target is
  `make fuzz-plug-manifest` and runs in the on-demand/nightly workflow.
- **Perf**: discovery + manifest parse of 20 fixture plugins ≤ 2 ms;
  10 enabled no-op plugins add ≤ 5 ms to cold start (budget guards the
  Sprint 56 gate).
- **Regression fixture**: the hostile plugin registers 100 commands, 123
  bindings, and every legal frozen plugin event; each event is fired and
  disable leaves zero residue.

## Definition of Done

1. `MODULES=` build without `plugins`: `yew plug list` prints the
   Deliverable 1 error byte-exact, exit 1; full build passes gcc + clang
   `-Werror` and ASan/UBSan lanes.
2. All 7 manifest-validation rows have firing unit tests; fuzzer runs
   1 h on the manifest corpus with zero crashes/leaks.
3. Precedence test: same-name plugin in data/config/workspace dirs →
   workspace wins when trusted, `blocked` when not, others `shadowed`
   in `yew plug list`.
4. Zero-residue gate green: registry-length snapshots equal across
   enable→disable for the hostile fixture; debug-build assert enabled.
5. Failing `init(ctx)` → state ERROR, zero residue, trace via
   `ed.plug.info`.
6. Error containment: 5th error auto-disables with notification; editor
   survives a plugin throwing in every event in the Deliverable 5 table
   (host integration loop over all legal plugin events and payloads).
7. Capability matrix at the shared host authorization seam:
   declared+granted / declared+denied / undeclared — 4 caps × 3 cases,
   all asserted. Interactive enable resolves
   declarations before module evaluation; sentinel tests prove top-level and
   init code do not run before the final answer and execute at most once.
   `--batch` never prompts.
8. Trust db: `always` answers persist across restart (PTY test launches a
   second yew process); file rewritten sorted; schema 1 files upgrade in
   place.
9. Pty goldens for prompt, picker, and toggle flows byte-stable across
   the determinism lane's double run.
10. `yew plug list` output byte-identical across two runs on the same
    tree; states cover all five values in the fixture set.
11. Perf: both Deliverable-11 budgets measured in CI perf lane with
    committed baselines.
12. `yew pkg anything` errors naming Sprint 55; `grep -rn 'sandbox' `
    user-facing strings returns nothing (docs honesty check).
