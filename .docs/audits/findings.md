# Sprint 58 findings ledger

Active baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
F01–F08 filing baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Next available ID: `YEW-F-022`

IDs are assigned only after a reproducer fails at the fixed baseline. They
are never reused, renumbered, or deleted. Resolution changes status and keeps
the historical row and body.

All findings remain reproducible as hard XFAILs at the applicable baseline
recorded in `audit-00.md`.

| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-001 | M | open | F01 UNI | ambiguous-wide doubles fixed-cell chrome glyphs | tests/audit/yew_f_001.c | s27 §7 |
| YEW-F-002 | M | open | F01 UNI | long RI output delays a completed flag cluster | tests/audit/yew_f_002.c | s19 §3 |
| YEW-F-003 | H | open | F01 UNI | ASCII-base keycap leaves inconsistent grid width | tests/audit/yew_f_003.c | s05 §3 |
| YEW-F-004 | M | open | F04 MODAL | full Fletch parser rejects bare dotted map keys | tests/audit/yew_f_004.c | spec §2 `entry` |
| YEW-F-005 | H | open | F06 RE | multi-cursor replacement exits inside a Fletch edit transaction | tests/audit/yew_f_005.c | s21 §4 / DoD 6 |
| YEW-F-006 | C | open | F07 UI | workspace re-emission drops unknown root and workspace keys | tests/audit/yew_f_006.c | s25 §4 / §6; s58 F07 q5 |
| YEW-F-007 | C | open | F07 UI | workspace restore reorders group members from tab-array order | tests/audit/yew_f_007.c | s25 §3 / §6 step 4 / DoD 4; s58 F07 q2 |
| YEW-F-008 | H | open | F08 FL | unprivileged plugin macro replay inherits config authority | tests/audit/yew_f_008.c | spec §13 / s34 DoD 10; s58 F08 q6 |
| YEW-F-009 | M | open | F09 REC | recorder folding self-test no longer reaches its injected fault | tests/audit/yew_f_009.c | s35 DoD 3; s58 F09 q3 |
| YEW-F-010 | M | open | F09 REC | macro store accepts source that fails on first replay | tests/audit/yew_f_010.c | s38 §4 / DoD 5; s58 F09 q7 |
| YEW-F-011 | M | open | F10 SYN | matching source metadata can retain stale syntax tables | tests/audit/yew_f_011.c | s40 §6; s58 F10 q4 |
| YEW-F-012 | M | open | F10 SYN | pending embeds occupy a canonical state tail slot | tests/audit/yew_f_012.c | s41.5 §1 / DoD 5; s58 F10 q2 |
| YEW-F-013 | M | open | F10 SYN | JS/TS known-wrong golden rows lack the heuristic comment | tests/audit/yew_f_013.c | s42 §9 / testing strategy; s58 F10 q9 |
| YEW-F-014 | M | open | F11 LSP | stripped LSP completion bypasses the module hard error | tests/audit/yew_f_014.c | s45 DoD 13; s47 §7; s58 F11 q8 |
| YEW-F-015 | M | open | F11 LSP | snippet-policy grep gate matches unrelated core code | tests/audit/yew_f_015.c | s47 DoD 4; s58 F11 q7 |
| YEW-F-016 | M | open | F11 LSP | required 1-based display edges violate the LSP +/-1 gate | tests/audit/yew_f_016.c | s46 DoD 4; s47 §5; s58 F11 q2 |
| YEW-F-017 | M | open | F13 GIT | interactive rebase bypasses the Git verb and environment boundary | tests/audit/yew_f_017.c | s51 §2/§3; s52 §11; s58 F13 q1/q7 |
| YEW-F-018 | M | open | F13 GIT | FUSS picker detail bypasses the module clock discipline | tests/audit/yew_f_018.c | s51 §10/DoD 2; s58 F13 q4 |
| YEW-F-019 | M | open | F13 GIT | porcelain rename test survives the required one-NUL mutation | tests/audit/yew_f_019.c | s51 DoD 4; s58 F13 q2 |
| YEW-F-020 | M | open | F13 GIT | Git formatting gate rejects legitimate display formatting | tests/audit/yew_f_020.c | s51 DoD 2; s58 F13 q1 |
| YEW-F-021 | M | open | F14 PLUG | plugin teardown retains raw hook and ledger lengths | tests/audit/yew_f_021.c | s54 section 4 / DoD 4; s58 F14 q3 |

The width mismatch is visible chrome corruption but the underlying document
bytes remain intact and the user can disable `ambiguous_wide`; that is Medium
under the wrong-but-recoverable rubric. Root-cause hypothesis: the document
width option feeds the global Unicode width table used by chrome, while several
layout slots remain one cell by contract. The reproducer fails at the fixed
baseline and was confirmed by hosted audit-control run `33815573832` across
GCC, Clang, ASan/UBSan, Linux arm64, macOS arm64, musl, and `MODULES=""`.

`YEW-F-002` is visible but recoverable: all bytes eventually arrive, yet a
completed four-byte flag cluster remains absent from a live job buffer until
the child writes again or exits. Root-cause hypothesis: `yew_job_safe_prefix`
uses the documented bounded `yew_gb_prev_bytes` approximation as though its
answer were the exact final-cluster boundary. The reproducer fails at the
fixed baseline and was confirmed by hosted audit-control run `33815573832`
across the same cross-compiler, cross-architecture matrix.

`YEW-F-003` is High because valid keycap text reaches a `YEW_BUG` in the
renderer, terminating yew with exit 4. Root-cause hypothesis: the printable
ASCII run in `yew_grid_puts` commits the base before segmentation can see its
VS16/keycap suffix; `append_zero_width` joins the bytes but retains the base's
one-cell width. The reproducer stops just before the fatal renderer call so
the XFAIL runner can retain the other findings. It fails at the fixed baseline
and was confirmed by hosted audit-control run `33815573832` across the same
cross-compiler, cross-architecture matrix.

`YEW-F-004` is Medium because a documented configuration shape fails loudly
at startup but does not corrupt document bytes; quoting the option name is a
working recovery. Root-cause hypothesis: the full expression parser treats an
identifier map key as a single token and requires `:` immediately, while the
pure-literal parser's entry path explicitly accepts dotted keys. The shipped
`runtime/init.fl` quotes its dotted option names, masking the mismatch on the
default startup path.

`YEW-F-005` is High because a valid Fletch `edit {}` block containing a
buffer-range replacement with two live cursors reaches `yew_bug()` and exits
4. The reproducer opens the same outer `YEW_TXN_MACRO` boundary as Fletch,
then invokes the real replacement command with the live cursor set. The plan
does not own a replacement transaction at nonzero depth, so its first edit
encounters the multi-cursor requirement while the pending reason is MACRO.
Correct behavior is a normal, one-undo replacement that restores exact text
and both cursor positions on undo. This remains open for Sprint 59; no product
source changes during Sprint 58.

`YEW-F-006` is Critical: unknown workspace data belongs to the user and a
normal parse followed by save silently deletes it. The hard-XPASS reproducer
shows that an unknown option survives, while equivalent unknown root and
workspace keys do not. Root-cause hypothesis: state_parse.c retains only the
options subtree and state_emit.c reconstructs root and workspace maps from
known fields. This violates Sprint 25's forward-compatibility retention
contract and remains open for Sprint 59; no product source changed.

`YEW-F-007` is Critical: a normal save and restore silently reorders the
user's group-member sequence. The hard-XPASS reproducer writes a group whose
tab records occur in ordinal order 3, 2, 1 while the group's intended order
is f0, f1, f2; restore returns f0, f2, f1. The writer records the correct
ordinals, but state_parse.c applies each one immediately: an early ordinal 3
clamps against a partial group before lower ordinals arrive, destroying the
saved ordering. This violates the frozen workspace restore contract and
remains open for Sprint 59; no product source changed.

`YEW-F-008` is High because a plugin declaring `capabilities: []` can write
an arbitrary file by storing Fletch source in a macro register through
`ed.run("ed.reg.set", ...)` and replaying it. The hard-XPASS reproducer
creates only an isolated temporary file; it does not run a shell command or
touch user data. During replay, the plugin-supplied source is compiled through
the config-origin `fl_compile_str` path and consequently receives config's
`FL_CAP_ALL` authority. This violates spec §13's defining-module rule. It
remains open for Sprint 59; no product source changed during the audit.

`YEW-F-009` is Medium because the recorder's mandatory shrinker self-test no
longer exercises its injected divergence, leaving a release-control claim
unproved without changing user bytes or product behavior. Generator-pool
growth changed seed 20764's prefix from the pinned pair of buffer-end motions
and insert into an unrelated unit motion. `YEW_RT_SELFTEST=1` consequently
exits 2 before fault injection instead of reporting and shrinking the planted
failure. The reproducer asserts the documented prefix and fails at the
replacement baseline. It remains open for Sprint 59; no product source
changed during the audit.

`YEW-F-010` is Medium because storing an invalid macro reports success, but
its first replay fails. The failure is recoverable: the VM transaction rolls
back the partial edit and preserves document bytes. `yew_macro_store`
performs compile-only validation, so a syntactically valid program containing
an unresolved global is accepted even though it cannot execute. The
reproducer stores a macro that inserts text and then calls a missing function;
store succeeds, replay returns `YEW_CMD_ERR_STATE`, and the buffer remains
unchanged. It remains open for Sprint 59; no product source changed during
the audit.

`YEW-F-011` is Medium because an equal-size syntax source replacement whose
nanosecond mtime is restored can retain the old compiled table. Highlighting
is stale but recoverable, and document bytes remain intact. The cache header
records the source hash as the authority, but `yew_syn_def_load` accepts an
in-memory entry on matching size and mtime before hashing the source. The
reproducer installs an isolated builtin-shaped `runtime/syntax/ini.fl`, loads
an `x` rule, replaces it with an equal-size `y` rule, restores the exact
timestamp, and observes zero recompiles plus the stale `x` rule. It remains
open for Sprint 59; no product source changed during the audit.

`YEW-F-012` is Medium because the documented canonical state law and the
pending-embed mechanism disagree, weakening the promised equality and cache
invariants and potentially retaining an otherwise unreachable definition.
Before JavaScript is resident, an HTML `<script>` opener leaves `ndef == 1`
while storing the pending definition in `aux[1]`; `syn_state_canon`
deliberately preserves that future slot even though Sprint 41.5 requires every
tail slot from `ndef` onward to be zero. The isolated reproducer records the
exact state without changing product behavior. It remains open for Sprint 59.

`YEW-F-013` is Medium because the release-control fixture required by Sprint
42 is absent: the JS and TypeScript known-wrong golden rows use an identifier
named `knownWrong`, but neither fixture contains the required adjacent comment
naming the value-flag heuristic. Descriptions elsewhere do not satisfy the
fixture-local documentation contract, so future reviewers cannot distinguish
intentional heuristic debt from a regression at the point of evidence. It
remains open for Sprint 59; no product source changed during the audit.

`YEW-F-014` is Medium because a stripped build gives one `ed.lsp.*` command
different module-boundary semantics from every other command in its domain.
`yew_lsp_complete` reports an informational message and opens core index
completion instead of returning the exact `yew_mod_require` error required by
Sprints 45, 47, and F11; the minimal-module unit control explicitly excludes
that command. The fallback is useful and recoverable, but it contradicts the
locked surface and makes the advertised module boundary inaccurate. It
remains open for Sprint 59; no product source changed during the audit.

`YEW-F-015` is Medium because Sprint 47's mandatory repository-wide
`tabstop|placeholder` scan cannot establish its claimed absence condition.
The LSP policy paragraph is one match, but ordinary core implementation names
and comments contribute ten more matching lines at the baseline. The product
still downgrades the choice snippet deterministically; the defect is in a
release gate that promises a specific result it cannot produce. It remains
open for Sprint 59; no product source changed during the audit.

`YEW-F-016` is Medium because two locked LSP contracts cannot both satisfy
their literal release controls. Sprint 46 requires a repository scan for
`line + 1`, `line - 1`, and `.v + 1` under `src/mod/lsp/` to be empty, while
Sprint 47 requires 1-based picker display at that layer. The baseline has
three matches, all at user-facing display/error edges; protocol positions
remain zero-based. The visible behavior is correct, but the frozen gate
rejects its required implementation and cannot support a release claim. It
remains open for Sprint 59; no product source changed during the audit.

`YEW-F-017` is Medium because interactive rebase is the one Git execution
path outside the static verb inventory and the shared environment builder.
Its direct synchronous job safely uses an argv array and terminal handover,
but it omits `GIT_TERMINAL_PROMPT=0`, `GIT_FLUSH=1`, and the trace-variable
removals required on every verb; only the two editor variables were permitted
to differ. The operation fails visibly rather than corrupting document bytes.
The source-backed reproducer records both halves of the bypass at the fixed
replacement baseline. It remains open for Sprint 59; no product source
changed during the audit.

`YEW-F-018` is Medium because the Git module's mandatory clock-source gate is
not empty and FUSS picker detail reads the process wall clock through
`time(NULL)` instead of an injected yew clock. That makes exact picker output
uncontrollable under clock steps and contradicts the subsystem's deterministic
clock discipline, while leaving document bytes safe. The literal source gate
also matches four permitted helper declarations/definitions whose names end
in `clock`, so its promised empty result is independently unattainable. The
source-backed reproducer records all five matches at the fixed replacement
baseline. It remains open for Sprint 59; no product source changed during the
audit.

`YEW-F-019` is Medium because the mandatory porcelain-v2 mutation control
does not detect the exact desynchronisation it claims to pin. With both
rename advances changed from two NULs to one, the source pathname becomes an
unknown record and is silently skipped; the parser still returns the seven
entries and original-path bytes asserted by the existing test. The manual
mutant passed 16 assertions. The source-independent reproducer models that
stream advance and records the indistinguishable entry count. It remains
open for Sprint 59; no product source changed during the audit.

`YEW-F-020` is Medium because Sprint 51's formatting grep cannot establish
the narrower argv-safety rule it is meant to enforce. Seven
`bytebuf_printf` calls outside `porcelain.c` format owned status, patch, and
picker-detail text; none constructs a Git argv element, but every one fails
the literal mandatory gate. The argv hook and hostile-filename matrix still
prove the product behavior. The source-backed reproducer pins the gate's
seven-match baseline for Sprint 59; no product source changed during the
audit.

`YEW-F-021` is Medium because plugin teardown clears every active hook and
registration and the collector reclaims their closures, but the raw hook and
ledger lengths remain at their first-cycle high-water marks instead of
returning to the pre-enable values required by Sprint 54 and F14. The retained
rows are inert and reused: the 20-plugin by 20-cycle control plateaus after the
first cycle, so this is bounded bookkeeping residue rather than executable
callback leakage. It remains open for Sprint 59; no product source changed
during the audit.

## Unverified observations

Observations live in their front files. They have no IDs and are excluded
from every total.
