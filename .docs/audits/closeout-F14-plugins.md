# Closeout: F14 — Plugins and packages

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s54, s55

## DoD items (every numbered item from every sprint above)

- [ ] s54.1 `MODULES=` build without `plugins`: `yew plug list` exits 1 with the shared module diagnostic byte-exact (`this build has no plugins module; rebuild with 'make MODULES="… plugins"'`); full build passes gcc + clang `-Werror` and ASan/UBSan lanes.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.2 All 7 manifest-validation rows have firing unit tests; fuzzer runs 1 h on the manifest corpus with zero crashes/leaks.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.3 Precedence test: same-name plugin in data/config/workspace dirs → workspace wins when trusted, `blocked` when not, others `shadowed` in `yew plug list`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.4 Zero-residue gate green: registry-length snapshots equal across enable→disable for the hostile fixture; debug-build assert enabled.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.5 Failing `init(ctx)` → state ERROR, zero residue, trace via `ed.plug.info`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.6 Error containment: 5th error auto-disables with notification; editor survives a plugin throwing in every event in the Deliverable 5 table (host integration loop over all legal plugin events and payloads).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.7 Capability matrix at the shared host authorization seam: declared+granted / declared+denied / undeclared — 4 caps × 3 cases, all asserted. Interactive enable resolves declarations before module evaluation; sentinel tests prove top-level and init code do not run before the final answer and execute at most once. `--batch` never prompts.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.8 Trust db: `always` answers persist across restart (PTY test launches a second yew process); file rewritten sorted; schema 1 files upgrade in place.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.9 Pty goldens for prompt, picker, and toggle flows byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.10 `yew plug list` output byte-identical across two runs on the same tree; states cover all five values in the fixture set.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.11 Perf: both Deliverable-11 budgets measured in CI perf lane with committed baselines.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s54.12 Sprint 55 supersedes the original package staging guard and Sprint 59 supersedes the manual staging guard: shipped `yew pkg` commands and the assembled plugin man page are live, permanent package non-goals still hard-error, and the semantic honesty gate permits only a negative `sandbox` disclaimer while requiring the memory/resource-isolation warning.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.1 `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/ determinism lanes clean. The pkg suite passes in a `MODULES=plugins` build with `fuss` **excluded**, and `grep -rn 'src/mod/git\|yew_git_' src/mod/plug/` prints nothing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.2 `grep -rn '\bsystem(\|\bpopen(\|git pull' src/` prints nothing; every git invocation is argv-form and every user string sits behind `--end-of-options` or `--` (unit-tested over §5's table).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.3 Lockfile determinism: two clean installs of the same three plugins produce byte-identical `plugins.lock`; unknown keys survive a rewrite; a corrupt lockfile is never overwritten without `--force-relock`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.4 Tree hash: all 6 sensitivity cases and all 3 insensitivity cases assert as specified; the tree fuzzer runs 1 h with zero crashes or hangs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.5 **Drift revokes grants** — the sprint's most important test, labelled as such: grant `fs` "always" to a fixture plugin, mutate one byte of its source, restart, and assert (a) the warning appears, (b) the trust db no longer holds the grant, (c) the next capability use re-prompts.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.6 Update is fast-forward-only: force-push and moved-tag fixtures both refuse with §5's message byte-exact, exit 1, tree and lockfile untouched; the changelog delta prints before any checkout, and `--dry-run` proves it by leaving `rev` unchanged.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.7 Offline lane green: every networked subcommand exits 3 with its exact message when git is absent, when the remote is unreachable, and when the timeout fires; `list` and `doctor` hash rows still pass; a seeded `yew_pkg_git` call on the editor's startup path fails `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.8 `install` executes nothing from the repository: a fixture carrying a `Makefile`, an `install.sh`, and a `postinstall:` manifest key installs (with s54's unknown-key diagnostic) and runs none of them — proven by a sentinel file that must not exist afterwards.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.9 A local fixture with the same manifest/tree shape as the deferred examples installs by path, lands in `plugins.lock` with a tree hash, reports `ok` from `yew pkg list`/`doctor`, and enables. The actual `examples/plugins/<name>` trees, their installation, their behaviour, their zero-residue teardown, and the API-coverage gate are **`s55_5-example-plugins.md`'s DoD**, not this sprint's; §8–§9 carry only the summary and the pointer.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.10 `runtime/preset.cloud.fl` parses with zero diagnostics, passes the spec §15 style checks, imports only via the A3 row, and each of the six new options' defaults equal their C constants (s36's rule, unit-asserted). The binding Amendment A3 is present in `.docs/fletch-spec.md` §16 and s33's conformance count is updated in the same commit.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.11 Torture green with `save.strategy=inplace` and `save.backup_keep=10`: s08's in-place checker invariant holds in 100 % of iterations, exactly 10 backups are retained, and `keep = 1` reproduces s08's existing on-disk result byte-for-byte.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.12 Conflict detection: every marker detected once; the two deliberately-unmatched shapes not detected; the idle scan runs at most once per idle period (counting hook) within 2 ms at 20 open buffers.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.13 Docs: `docs/plugins-authoring.md` carries all ten `ctx` rows and quotes `plug.h`'s trust text verbatim (a test diffs the quoted block against the header comment); `docs/pkg.md` covers all five subcommands, all exit codes, and the "not a cryptographic hash" statement.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.14 Non-goal surfaces hard-error naming their reason: `yew pkg publish`/ `search` quote §7; `--recurse-submodules` quotes §5; `:PkgInstall` quotes §6's startup rule; Sprint 59's assembled plugin man page `docs/man/yew-pkg.1` passes the current man-page contract.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s55.15 ≥ 140 assertions across the four new unit files; ≥ 18 `tests/pkg/` integration cases; perf baselines committed for all three budgets.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
