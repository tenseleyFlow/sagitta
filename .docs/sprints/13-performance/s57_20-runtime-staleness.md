# Sprint 57.20: Runtime Staleness Warning

## Prerequisites

- Sprint 6/7 — the Fletch runtime loader: `src/fl/module.c` resolves the
  runtime directory from `YEW_RUNTIME_DIR`, else the compiled-in
  `YEW_RUNTIME_DIR_DEFAULT` (`/usr/local/share/yew/runtime` in a normal
  build), else the in-tree `runtime/`.
- Sprint 13 — the keymap: EVERY default binding lives in
  `runtime/init.fl`, not in the binary. `src/edit/keys_default.c` is only
  the panic floor for when config fails outright.
- Sprint 36 — `flconf` precedence (builtin → user → workspace).
- Binding: invariant 3 (no silent stubs — a feature that cannot work must
  say so, not fail quietly).

## Goals

A yew binary built after a feature lands, running against a runtime
directory installed before it, silently loses every binding the feature
added. The command is registered and reachable by name, but nothing is
wired to the key, so the feature looks absent rather than broken.

This was observed in the field on 2026-09-13: a binary containing
`ed.cmdline.up` ran against an installed `init.fl` whose `<up>` was still
bound to `ed.cmdline.hist_prev`. The reported symptom was "the pager
feature was never merged". Three separate wrong diagnoses were made
before anyone compared the two files' timestamps. The editor had every
fact needed to say so and said nothing.

The same trap covers the whole campaign's keyboard surface: the numbered
tab jumps, the context menu, the command-line pager, `:!` completion, and
the mode-switch keys are ALL config, not code.

Deferred, named: auto-installing or auto-repairing the runtime; version
pinning the config schema; migrating a user's customised `init.fl`.

## Deliverables

### 1. Report the resolved runtime — `src/main.c`, `src/fl/module.c`

`yew --version` gains the runtime directory it would actually load and
where that choice came from:

```
yew 0.9.x
runtime: /usr/local/share/yew/runtime (compiled-in default)
```

Sources to distinguish, in the loader's own precedence order:
`YEW_RUNTIME_DIR` (environment), the compiled-in default, the in-tree
`runtime/`. Expose one accessor — `const char *yew_runtime_dir(const char
**origin)` — and have `--version` and the warning below share it. Two
spellings of "which runtime" would drift, and the drift is invisible.

### 2. Detect and report staleness — `src/fl/module.c` or a new `src/fl/flstale.c`

At startup, after the runtime loads, compare the loaded `init.fl` against
the binary. When the config is OLDER than the binary, emit ONE message:

```
runtime/init.fl is older than this binary; some keys may be unbound
(run `make install`, or set YEW_RUNTIME_DIR)
```

**The comparison is the deliverable's whole risk, so specify it
deliberately rather than reaching for mtime.** Evaluate at least these
and justify the choice in the header:

| Signal | Cost | Failure mode |
|---|---|---|
| mtime of `init.fl` vs mtime of `/proc/self/exe` | one stat each; portable enough | a checkout, a package manager, or `touch` resets mtimes and produces a false alarm |
| a version string in `init.fl` compared to the binary's | exact, intentional | needs the version bumped whenever bindings change, which nobody will remember |
| a hash of the binary's *expected* default bindings vs what loaded | exact, self-maintaining | needs the expected set compiled in, which is a real build-time step |

A false alarm on every launch is worse than the bug: users learn to
ignore it, and then it is noise forever. If mtime is chosen, it must
tolerate the ordinary cases (fresh clone, reinstall, same-second writes)
and warn only when the gap is unambiguous.

The message goes through `yew_msg` at `YEW_MSG_WARN`, once per session,
and must be dismissible like any other message. It must NOT block
startup, must not appear in `--batch`, and must be suppressible by an
option (`runtime.staleness_warning`, bool, default true) for users who
deliberately run a pinned config.

**Determinism:** the message must not reach a PTY golden unconditionally.
Either the test harness sets the suppressing option, or the check is
skipped when `YEW_RUNTIME_DIR` points at the in-tree runtime, which is
what every test uses. State which, and make sure the 482 existing goldens
do not all gain a warning row.

### 3. A positive check — `yew --check-runtime`

An explicit subcommand that reports, and exits non-zero when the runtime
cannot supply what the binary expects:

```
runtime: /usr/local/share/yew/runtime (compiled-in default)
init.fl: 2026-09-12 13:26  (binary: 2026-09-13 22:06)
bindings: 230 active
warning: this runtime predates the binary
```

This is what a user or a support question runs, and what the warning in
§2 should point at when it fires.

### 4. Docs

`README.md` gains a short "Runtime configuration" note: bindings live in
`runtime/init.fl`, the binary resolves it in the documented order, and a
stale installed copy silently drops new keys. One paragraph, with the
override.

## Testing Strategy

- **Unit:** the resolver reports each origin correctly with and without
  the environment override; the staleness predicate is true for an
  unambiguously older config, false for a fresh clone, false for
  same-second writes, false when the option is off, false in batch.
- **Script:** a test that builds, installs to a temp prefix, backdates the
  installed `init.fl`, and asserts the warning appears and
  `--check-runtime` exits non-zero — then reinstalls and asserts both go
  quiet. This is the actual field scenario and is the one test that
  proves the feature.
- **PTY:** exactly one new case showing the warning on the message line
  with a deliberately stale fixture runtime. Every existing golden must be
  unchanged — that is the determinism gate for §2.
- **Batch:** `--batch` stays silent; covered by the existing batch
  refusal apparatus.

## Definition of Done

1. `yew --version` reports the resolved runtime directory and its origin.
2. A runtime older than the binary produces exactly one dismissible
   warning naming the fix.
3. No false alarm on a fresh clone, a reinstall, or a same-second write.
4. `yew --check-runtime` reports the resolution and exits non-zero when
   stale.
5. All 482 existing PTY goldens are byte-identical.
6. gcc and clang warning-free; unit, PTY, script and batch lanes green;
   `MODULES=""` builds.
