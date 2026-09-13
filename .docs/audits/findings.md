# Sprint 58 findings ledger

Active baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
F01–F08 filing baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Next available ID: `YEW-F-079`

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
| YEW-F-022 | M | open | F14 PLUG | plugin trust wording gate rejects its required warning | tests/audit/yew_f_022.c | s54 section 7 / DoD 12; s58 F14 q6 |
| YEW-F-023 | M | open | F14 PLUG | plugin commands cannot enter the recorder CMDWORD space | tests/audit/yew_f_023.c | s58 F14 q8 |
| YEW-F-024 | M | open | F15 CI | cross-surface XFAIL debt table stops at F004 | tests/audit/yew_f_024.c | s58 section 3 / F15 q1 |
| YEW-F-025 | M | open | F15 CI | script tests have no XFAIL or hard-XPASS state | tests/audit/yew_f_025.c | s58 section 3 / F15 q1 |
| YEW-F-026 | M | open | F15 CI | PTY cases have no XFAIL or hard-XPASS state | tests/audit/yew_f_026.c | s58 section 3 / F15 q1 |
| YEW-F-027 | M | open | F15 CI | Fletch format ban accepts macro-forwarded nonliteral formats | tests/audit/f15_ban_misses.c | s31 DoD 5; s58 F15 q2 |
| YEW-F-028 | M | open | F15 CI | Fletch abort ban accepts macro-forwarded abort | tests/audit/f15_ban_misses.c | s32 DoD 10; s58 F15 q2 |
| YEW-F-029 | M | open | F15 CI | stable-sort ban accepts macro-forwarded qsort | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-030 | M | open | F15 CI | C11-subset ban accepts token-pasted attribute syntax | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-031 | M | open | F15 CI | explicit-registry ban accepts token-pasted constructors | tests/audit/f15_ban_misses.c | s01 sections 1/6; s58 F15 q2 |
| YEW-F-032 | M | open | F15 CI | single-thread ban accepts token-pasted pthread calls | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-033 | M | open | F15 CI | reproducibility ban omits `__TIMESTAMP__` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-034 | M | open | F15 CI | mmap ban accepts macro-forwarded calls | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-035 | M | open | F15 CI | allocator ban accepts macro-forwarded libc allocation | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-036 | M | open | F15 CI | cwd-allocation ban requires literal NULL spelling | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-037 | M | open | F15 CI | realpath-allocation ban requires literal NULL spelling | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-038 | M | open | F15 CI | locale-dependent Unicode ban omits `mbtowc` | tests/audit/f15_ban_misses.c | s19 portability law; s58 F15 q2 |
| YEW-F-039 | M | open | F15 CI | native-loader ban omits `dlvsym` | tests/audit/f15_ban_misses.c | s54 Fletch-only plugin law; s58 F15 q2 |
| YEW-F-040 | M | open | F15 CI | strerror_r ban accepts macro-forwarded calls | tests/audit/f15_ban_misses.c | s57 portability audit; s58 F15 q2 |
| YEW-F-041 | M | open | F15 CI | musl backtrace ban omits `backtrace_symbols_fd` | tests/audit/f15_ban_misses.c | s57 musl profile; s58 F15 q2 |
| YEW-F-042 | M | open | F15 CI | GNU-libc ban omits `getopt_long_only` | tests/audit/f15_ban_misses.c | s57 musl profile; s58 F15 q2 |
| YEW-F-043 | M | open | F15 CI | long-double ban misses valid continued declarations | tests/audit/f15_ban_misses.c | s57 ABI audit; s58 F15 q2 |
| YEW-F-044 | M | open | F15 CI | shim-honesty gate accepts parenthesized success | tests/audit/f15_ban_misses.c | s57 module-size profiles; s58 F15 q2 |
| YEW-F-045 | M | open | F15 CI | Unicode-width ban accepts decimal local tables | tests/audit/f15_ban_misses.c | s19 width ownership; s58 F15 q2 |
| YEW-F-046 | M | open | F15 CI | syntax-color ban accepts packed decimal colors | tests/audit/f15_ban_misses.c | s40 semantic attrs; s58 F15 q2 |
| YEW-F-047 | M | open | F15 CI | syntax-width ban accepts local width arithmetic | tests/audit/f15_ban_misses.c | s40 byte-span ownership; s58 F15 q2 |
| YEW-F-048 | M | open | F15 CI | PTY-creation ban omits direct `posix_openpt` callers | tests/audit/f15_ban_misses.c | s06 audited harness; s58 F15 q2 |
| YEW-F-049 | M | open | F15 CI | CI golden-update ban depends on contiguous spelling | tests/audit/f15_ban_misses.c | s06 golden update law; s58 F15 q2 |
| YEW-F-050 | M | open | F15 CI | piece-tree I/O ban omits `pread` | tests/audit/f15_ban_misses.c | s08 I/O ownership; s58 F15 q2 |
| YEW-F-051 | M | open | F15 CI | shadow-preview ban accepts manual destructive fill | tests/audit/f15_ban_misses.c | s44 composition law; s58 F15 q2 |
| YEW-F-052 | M | open | F15 CI | FUSS drawer ban accepts indirect pane-root replacement | tests/audit/f15_ban_misses.c | s57.7 off-canvas law; s58 F15 q2 |
| YEW-F-053 | M | open | F15 CI | deterministic-fuzz ban omits `random` | tests/audit/f15_ban_misses.c | s02 deterministic seeds; s58 F15 q2 |
| YEW-F-054 | M | open | F15 CI | clipboard shell ban omits direct shell exec | tests/audit/f15_ban_misses.c | s24 no-shell subprocess law; s58 F15 q2 |
| YEW-F-055 | M | open | F15 CI | job-interpolation ban accepts raw append into shell text | tests/audit/f15_ban_misses.c | s37 argv boundary; s58 F15 q2 |
| YEW-F-056 | M | open | F15 CI | OSC 52 query ban accepts split string literals | tests/audit/f15_ban_misses.c | s24 write-only OSC 52; s58 F15 q2 |
| YEW-F-057 | M | open | F15 CI | terminal-syscall ban omits `tcflush` | tests/audit/f15_ban_misses.c | s37 tty boundary; s58 F15 q2 |
| YEW-F-058 | M | open | F15 CI | register choke-point ban accepts allowed-file wrappers | tests/audit/f15_ban_misses.c | s36 register routing; s58 F15 q2 |
| YEW-F-059 | M | open | F15 CI | option choke-point ban accepts allowed-file wrappers | tests/audit/f15_ban_misses.c | s36 option routing; s58 F15 q2 |
| YEW-F-060 | M | open | F15 CI | package-git ban accepts allowed-file wrappers on startup | tests/audit/f15_ban_misses.c | s55 startup transport law; s58 F15 q2 |
| YEW-F-061 | M | open | F15 CI | register-width ban accepts local lookup tables | tests/audit/f15_ban_misses.c | s36 Unicode routing; s58 F15 q2 |
| YEW-F-062 | M | open | F15 CI | register-column ban depends on historical variable names | tests/audit/f15_ban_misses.c | s36 column routing; s58 F15 q2 |
| YEW-F-063 | M | open | F15 CI | register-helper presence gate accepts comments | tests/audit/f15_ban_misses.c | s36 helper routing; s58 F15 q2 |
| YEW-F-064 | M | open | F15 CI | oracle-independence ban accepts copied renamed models | tests/audit/f15_ban_misses.c | s11 independent oracle; s58 F15 q2 |
| YEW-F-065 | M | open | F15 CI | generated-table ban verifies only a retained marker | tests/audit/f15_ban_misses.c | s19 generated UCD tables; s58 F15 q2 |
| YEW-F-066 | M | open | F15 CI | termination-site ban omits `_Exit` | tests/audit/f15_ban_misses.c | s01 exit contract; s58 F15 q2 |
| YEW-F-067 | M | open | F15 CI | AI-body logging ban depends on variable names | tests/audit/f15_ban_misses.c | s50 privacy gate; s58 F15 q2 |
| YEW-F-068 | M | open | F15 CI | unit-registry ban omits static test definitions | tests/audit/f15_ban_misses.c | s01 explicit registry; s58 F15 q2 |
| YEW-F-069 | M | open | F15 CI | PTY minimum-case gate skips a missing registry | tests/audit/f15_ban_misses.c | s06 registry minimum; s58 F15 q2 |
| YEW-F-070 | M | open | F15 CI | PTY golden gate accepts computed missing names | tests/audit/f15_ban_misses.c | s06 golden completeness; s58 F15 q2 |
| YEW-F-071 | M | open | F15 CI | PTY orphan gate counts dead preprocessor rows | tests/audit/f15_ban_misses.c | s06 golden completeness; s58 F15 q2 |
| YEW-F-072 | M | open | F15 CI | designated performance evidence remains placeholder-only | tests/audit/yew_f_072.c | s56 section 4; s58 F15 q3 |
| YEW-F-073 | M | open | F15 CI | baseline history policy is not enforced | tests/audit/yew_f_073.c | s56 baseline policy; s58 F15 q4 |
| YEW-F-074 | H | open | F15 CI | Darwin shipping clean rebuilds differ by Mach-O UUID | tests/audit/yew_f_074.c | invariant 5; s58 F15 q5 |
| YEW-F-075 | C | open | F15 CI | stripped builds accept module-only config as inert state | tests/audit/yew_f_075.c | invariant 3; s58 F15 q7 |
| YEW-F-076 | M | open | F03 TEXT | accepted unsaved undo sidecars are not byte-canonical | tests/audit/yew_f_076.c | s10 section 9 / DoD 8; s58 section 6.4 |
| YEW-F-077 | M | open | F03 TEXT | rectangular yank omits required short-row padding | tests/audit/yew_f_077.c | invariant 2; s12 section 5 |
| YEW-F-078 | M | open | F03 TEXT | crash journal admits a same-metadata replacement inode | tests/audit/yew_f_078.c | invariant 1; s08 section 4; s58 section 8 |

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

`YEW-F-022` is Medium because Sprint 54 requires its author guide to quote the
honest `plug.h` trust warning verbatim while also banning the word `sandbox`
from every user-facing string. The required warning itself ends by saying that
capability gates do not create a sandbox, so the literal release gate fails on
the one sentence that most directly prevents a misleading isolation claim.
The product text is honest; the defect is a self-contradictory release control.
It remains open for Sprint 59; no product source changed during the audit.

`YEW-F-023` is Medium because plugin commands execute normally but can never
be represented by the recorder. The plugin registration path excludes
`YEW_CMD_RECORDABLE`, always supplies a NULL CMDWORD, and the author guide
calls `recordable` host-only. With no word to enter in the global map, a plugin
command named `up` is accepted beside core's `up` instead of reaching the
required collision check. This is visible as a macro that omits the plugin
action rather than a byte-loss path, so it is Medium. It remains open for
Sprint 59; no product source changed during the audit.

`YEW-F-024` is Medium because Sprint 58 calls
`.docs/audits/xfail-debt.md` the authoritative cross-surface debt table and
requires every live finding to remain there until closure, but it lists only
`YEW-F-001` through `YEW-F-004` while the finding ledger and enforced audit
registry run through `YEW-F-023`. The expected failures still execute, so
this is tracking/control drift rather than a silently green product failure.
It remains open for Sprint 59; no product source changed during the audit.

`YEW-F-025` is Medium because the script runner has no syntax or state for an
expected failure. A seeded `# XFAIL: YEW-F-NNN` is only a Fletch comment;
failure remains an ordinary `FAIL`, and success remains `PASS`, so hard XPASS
cannot be represented on this required surface. The unit audit runner and
Fletch conformance runner do implement hard XPASS. It remains open for Sprint
59; no product source changed during the audit.

`YEW-F-026` is Medium because Sprint 58 explicitly requires
`PtyCase.xfail_id`, but the structure has only name/profile/geometry/function
fields and the runner has no expected-failure classification. A seeded golden
mismatch remains an ordinary failure and a later matching golden cannot be
reported as XPASS. It remains open for Sprint 59; no product source changed
during the audit.

`YEW-F-027` is Medium because the Fletch format scanner recognizes direct
printf-family call tokens but accepts a macro-forwarded call carrying a
nonliteral user-controlled format. The isolated fixture runs the actual gate
and exits green. This weakens a documented control without proving a product
violation, so the finding is Medium and remains open for Sprint 59.

`YEW-F-028` is Medium because the VM abort scanner accepts `abort()` reached
through a plainly named macro. The compiler still emits the forbidden abort
path while the gate reports green. This is a release-control gap, not a
confirmed product crash, and remains open for Sprint 59.

`YEW-F-029` is Medium because the stable-sort ban accepts `qsort` behind a
macro even though the resulting call retains the unstable cross-libc ordering
the rule forbids. The isolated actual-gate probe is green and remains an open
Sprint 59 control finding.

`YEW-F-030` is Medium because token pasting produces the forbidden
`__attribute__` spelling only after preprocessing. The source grep reports
green although the compiler sees syntax outside the locked C11 subset. No
such source is present in yew; the gate finding remains open for Sprint 59.

`YEW-F-031` is Medium because the constructor check can be bypassed by token
pasting both the attribute and `constructor` name. That reintroduces implicit
registration while the explicit-registry gate stays green. The seeded control
finding remains open for Sprint 59.

`YEW-F-032` is Medium because token-pasted `pthread_create` reaches the
forbidden threading API without leaving the contiguous `pthread` text the
gate searches for. The product tree is not shown to spawn a thread; the
single-thread release control is incomplete and remains open for Sprint 59.

`YEW-F-033` is Medium because `__TIMESTAMP__` embeds filesystem-dependent
build time just as surely as the two macros currently banned, yet is omitted
from the reproducibility scan. The actual gate accepts the isolated seed. It
remains open for Sprint 59.

`YEW-F-034` is Medium because a macro-forwarded `mmap` call survives the
source ban while preserving the truncate/SIGBUS hazard the rule exists to
exclude. This is a gate finding only and remains open for Sprint 59.

`YEW-F-035` is Medium because the audited-allocation scan keys on a direct
libc function token followed by `(` and accepts a macro-forwarded `malloc`.
The isolated seed does not establish an allocation in the product tree. The
control gap remains open for Sprint 59.

`YEW-F-036` and `YEW-F-037` are Medium because the two libc-owned allocation
checks require `NULL` to appear literally at the call site. Passing a pointer
variable initialized to NULL retains `getcwd`/`realpath` ownership semantics
but passes both actual gates. They remain separate rule findings for Sprint
59; no product source changed.

`YEW-F-038` is Medium because the locale-dependent Unicode list includes
`mbrtowc` but omits its older stateful sibling `mbtowc`. The latter has the
same forbidden locale dependence and passes the actual gate. This control
finding remains open for Sprint 59.

`YEW-F-039` is Medium because the native-loader list covers `dlsym` but omits
the GNU versioned lookup `dlvsym`. A Fletch-only plugin policy cannot be
established by that list while a native symbol resolver passes. It remains
open for Sprint 59.

`YEW-F-040` is Medium because a macro-forwarded `strerror_r` call preserves
the incompatible ABI surface while evading the direct-call regex. The product
tree has no demonstrated violation; the portability control remains open for
Sprint 59.

`YEW-F-041` is Medium because `backtrace_symbols_fd` is part of the same
glibc/execinfo family but is absent from the musl-compatibility pattern. The
actual gate accepts a direct call, so the release claim is incomplete and
remains open for Sprint 59.

`YEW-F-042` is Medium because `getopt_long_only` is a GNU extension adjacent
to the listed `getopt_long`, but the word-boundary shape lets the longer name
pass. The musl portability control remains open for Sprint 59.

`YEW-F-043` is Medium because C line continuation permits `long double` to
span physical source lines before preprocessing while grep evaluates each
line separately. The ABI-divergent type passes the actual gate and the
control finding remains open for Sprint 59.

`YEW-F-044` is Medium because the shim honesty parser recognizes only a few
literal return expressions. A disabled action returning `(YEW_CMD_OK)` has
identical success semantics but passes `check-module-shims.sh`. No production
shim was changed; the control finding remains open for Sprint 59.

`YEW-F-045` is Medium because the non-Unicode width gate searches four
symbolic/hex spellings, while the same code points in decimal form a local
width table that passes. The source-independent width ownership claim is
therefore not established and remains open for Sprint 59.

`YEW-F-046` is Medium because a syntax definition can emit a packed decimal
foreground color without matching hex, RGB, or terminal escape spellings.
The semantic-attrs-only control accepts the seed and remains open for Sprint
59.

`YEW-F-047` is Medium because syntax-local cell width arithmetic needs none of
the two helper names the gate scans. A simple wide-threshold calculation
passes even though syntax is required to own byte spans only. It remains open
for Sprint 59.

`YEW-F-048` is Medium because the PTY rule says creation must use the audited
`posix_openpt` harness but does not scan for `posix_openpt` itself. A direct
caller in another PTY test passes the gate. The harness control remains open
for Sprint 59.

`YEW-F-049` is Medium because shell token concatenation constructs the golden
update environment name at execution time while preventing its contiguous
appearance in workflow source. CI could therefore enable updates while the
gate stays green. It remains open for Sprint 59.

`YEW-F-050` is Medium because the piece-tree I/O ban lists `open`, `fopen`, and
`read`, but direct `pread` retains the forbidden file-I/O ownership and passes.
No product I/O was found; the release control remains open for Sprint 59.

`YEW-F-051` is Medium because the insertion-preview rule bans one fill helper,
not destructive fill behavior. A loop assigning every grid cell directly
passes while violating the same compositional rendering contract. It remains
open for Sprint 59.

`YEW-F-052` is Medium because taking the address of `pane_root` and assigning
through that pointer replaces the live root without matching the direct
assignment pattern. The FUSS drawer control remains open for Sprint 59.

`YEW-F-053` is Medium because deterministic fuzzing bans `rand`, `srand`, and
one `time` spelling but omits the libc `random()` generator. A direct call
passes the actual gate. It remains open for Sprint 59.

`YEW-F-054` is Medium because the clipboard no-shell rule scans only `popen`
and `system`; executing `/bin/sh -c` directly with `execl` has the same
forbidden behavior and passes. No such product path was established, and the
control remains open for Sprint 59.

`YEW-F-055` is Medium because the job-data rule recognizes two formatting
shapes while a raw byte append into a buffer named `shell` performs equivalent
interpolation and passes. The argv-boundary control remains open for Sprint
59.

`YEW-F-056` is Medium because adjacent C literals construct an OSC 52 query
whose semicolon and question mark are separated only in source. The terminal
receives the forbidden query while grep reports green. It remains open for
Sprint 59.

`YEW-F-057` is Medium because the terminal boundary lists four calls but omits
`tcflush`, another direct terminal-control syscall. An out-of-boundary call
passes the actual gate. It remains open for Sprint 59.

`YEW-F-058`, `YEW-F-059`, and `YEW-F-060` are separate Medium boundary
findings. Each allow-list exempts an implementation file, and each can be
defeated by adding a raw wrapper there and calling that wrapper from forbidden
code. The register write, option write, and package-git startup policies all
pass their isolated seeds and remain open for Sprint 59.

`YEW-F-061` is Medium because register-local Unicode width calculation can use
a decimal lookup table without importing or naming a yew width helper. The
actual gate accepts the duplicated ownership and remains open for Sprint 59.

`YEW-F-062` is Medium because the register column-arithmetic rule searches
three historical variable names followed by `.v`. Equivalent `CellCol`
addition under renamed locals passes. It remains open for Sprint 59.

`YEW-F-063` is Medium because the four required-helper checks accept names in
comments as proof of routing. A register implementation containing only those
comments and a local calculation passes. It remains open for Sprint 59.

`YEW-F-064` is Medium because oracle independence cannot be established by
banning two implementation names. A copied, renamed piece model passes while
retaining the correlated implementation structure the rule forbids. It
remains open for Sprint 59.

`YEW-F-065` is Medium because retaining the generated-file marker after a
manual table edit satisfies the entire generated-UCD check. The gate neither
regenerates nor compares the table, so provenance is unproved. It remains
open for Sprint 59.

`YEW-F-066` is Medium because `_Exit` terminates the process outside
`yew_bug` without matching the lowercase `exit()` scanner. This is a control
gap rather than a product termination path and remains open for Sprint 59.

`YEW-F-067` is Medium because the AI privacy scanner infers body data from a
small list of identifier substrings. Renaming the bytes and logging them
directly passes the gate. No user payload was logged by the audit; the control
remains open for Sprint 59.

`YEW-F-068` is Medium because the explicit unit registry inventory recognizes
only definitions beginning exactly with `void`. Adding ordinary `static`
linkage hides a test definition from the inventory and lets it remain
unregistered. It remains open for Sprint 59.

`YEW-F-069` is Medium because the PTY minimum-count check is conditional on
the registry file existing. Deleting the registry skips the check entirely
and the actual ban suite exits green. It remains open for Sprint 59.

`YEW-F-070` is Medium because a snapshot name held in a variable is invisible
to the missing-golden extractor. The runtime names a nonexistent golden while
the static gate passes. It remains open for Sprint 59.

`YEW-F-071` is Medium because the PTY case extractor does not honor the C
preprocessor. A `C(orphan)` row under `#if 0` persuades the gate that an orphan
golden is live even though the compiler removes the row. It remains open for
Sprint 59.

`YEW-F-072` is Medium because the two designated lanes cannot currently
produce a performance verdict: both committed calibration references and the
arm64 baseline are absent, while the x86_64 baseline still carries an all-zero
template calibration vector. Hosted lanes continue to execute the harnesses
and hard sanity checks, but they are advisory by contract. With no designated
measurements, F15 cannot recompute a 30-run noise floor or compare a threshold
to it. This is an invariant-4 control mismatch, not evidence that a user-facing
budget is exceeded, and remains open for Sprint 59.

`YEW-F-073` is Medium because `perf-baseline-guard.sh` reads only the changed
path list. It rejects a source-and-baseline commit but never reads the commit
message or numerical diff, so an isolated baseline-only commit titled
`Refresh numbers` can double every value and still pass. Historical review
found many modified baseline commits without the required old-to-new record;
the exact table is retained in `audit-15-ci.md`. The control finding remains
open for Sprint 59.

`YEW-F-074` is High because invariant 5 requires byte-identical builds and
all four single-module profiles (`lsp`, `ai`, `fuss`, `plugins`) produced
different SHA-256 hashes across consecutive clean builds of the fixed product
baseline on arm64 macOS. The unstripped binary embeds changing object
timestamps; after `strip -S`, the remaining delta is `LC_UUID` plus the
ad-hoc signature derived from it. Rebuilding and stripping twice with
`-Wl,-no_uuid` produced the same hash
`99d3e903f772158f0c7903b9cdb98d52a8d762703be492c2e3febc69d14bd031`.
This is nondeterministic release output and therefore High under the rubric.
It remains open for Sprint 59/60; no product or build fix landed in the audit.

`YEW-F-075` is Critical because the `MODULES=""` build accepts writes to
`ai.enable`, `lsp.open_in`, and `git.ascii_glyphs` even though their modules
are absent; their stored values cannot activate the excluded behavior.
Plugin-only options take the other inconsistent path and report generic
`unknown option` rather than the canonical module refusal. F15 q7 explicitly
requires canonical hard errors on the config-key surface, and the severity
rubric classifies a user-reachable silent stub as Critical. One option table
without module ownership is the shared root cause. The finding remains open
for Sprint 59; no product source changed during the audit.

`YEW-F-076` is Medium because a corrupt but unused anchor-hash field in an
unsaved undo sidecar is accepted as current and then silently canonicalized
on its next write. Document bytes and the undo tree remain recoverable, but
the accepted-file byte round-trip required by Sprint 58 section 6.4 is false.
Coverage-guided mutation flipped one bit at header offset 40; the standalone
reproducer deterministically rebuilds that input and remains open for Sprint
59. No product source changed during the audit.

`YEW-F-077` is Medium because an ordinary rectangular yank clips short rows
to their natural byte length but labels the resulting register non-ragged.
Sprint 12 requires those rows to be right-padded to the rectangle's `CCol`
width at store time, so a later paste reproduces different geometry from the
selection the user yanked. The hard-XFAIL selects columns `[0, 2)` across
`a` and `bb`: the required rows are `a ` and `bb`, while the baseline stores
`a` and `bb`. The original buffer remains intact and the result is
recoverable, so this is Medium rather than data loss. It remains open for
Sprint 59; no product source changed during the audit.

`YEW-F-078` is Medium because a durable journal authenticates its base with
the canonical path, byte size, and nanosecond mtime, but not file identity or
content. Another process can install different same-size bytes at that path
and restore the recorded mtime; probe and replay then splice the old file's
edit into the replacement instead of recovering the exact intended buffer.
The reproducer retains two hardlinks to the original base, so both the
mismatch and the correct recovery source remain observable and recoverable.
No disk file is silently overwritten by replay, making this Medium rather
than Critical. It remains open for Sprint 59; no product source changed.

## Unverified observations

Observations live in their front files. They have no IDs and are excluded
from every total.
