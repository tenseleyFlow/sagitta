# Sprint 58 findings ledger

Active baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
F01–F08 filing baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Next available ID: `YEW-F-080`

IDs are assigned only after a reproducer fails at the fixed baseline. They
are never reused, renumbered, or deleted. Resolution changes status and keeps
the historical row and body.

All findings remain reproducible as hard XFAILs at the applicable baseline
recorded in `audit-00.md`.

| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-001 | M | fixed | F01 UNI | ~~ambiguous-wide doubles fixed-cell chrome glyphs~~ — fixed 2026-09-13 in `c5a11c96` | tests/audit/yew_f_001.c | s27 §7 |
| YEW-F-002 | M | fixed | F01 UNI | ~~long RI output delays a completed flag cluster~~ — fixed 2026-09-13 in `566b07b6` | tests/audit/yew_f_002.c | s19 §3 |
| YEW-F-003 | H | fixed | F01 UNI | ~~ASCII-base keycap leaves inconsistent grid width~~ — fixed 2026-09-13 in `2fd132e1` | tests/audit/yew_f_003.c | s05 §3 |
| YEW-F-004 | M | fixed | F04 MODAL | ~~full Fletch parser rejects bare dotted map keys~~ — fixed 2026-09-13 in `a030d621` | tests/audit/yew_f_004.c | spec §2 `entry` |
| YEW-F-005 | H | fixed | F06 RE | ~~multi-cursor replacement exits inside a Fletch edit transaction~~ — fixed 2026-09-13 in `db0759ed` | tests/audit/yew_f_005.c | s21 §4 / DoD 6 |
| YEW-F-006 | C | fixed | F07 UI | ~~workspace re-emission drops unknown root and workspace keys~~ — fixed 2026-09-13 in `9e829cd9` | tests/audit/yew_f_006.c | s25 §4 / §6; s58 F07 q5 |
| YEW-F-007 | C | fixed | F07 UI | ~~workspace restore reorders group members from tab-array order~~ — fixed 2026-09-13 in `bea3990b` | tests/audit/yew_f_007.c | s25 §3 / §6 step 4 / DoD 4; s58 F07 q2 |
| YEW-F-008 | H | fixed | F08 FL | ~~unprivileged plugin macro replay inherits config authority~~ — fixed 2026-09-13 in `8995bb1f` | tests/audit/yew_f_008.c | spec §13 / s34 DoD 10; s58 F08 q6 |
| YEW-F-009 | M | fixed | F09 REC | ~~recorder folding self-test no longer reaches its injected fault~~ — fixed 2026-09-13 in `1c11ebee` | tests/audit/yew_f_009.c | s35 DoD 3; s58 F09 q3 |
| YEW-F-010 | M | fixed | F09 REC | ~~macro store accepts source that fails on first replay~~ — fixed 2026-09-13 in `c362cefd` | tests/audit/yew_f_010.c | s38 §4 / DoD 5; s58 F09 q7 |
| YEW-F-011 | M | fixed | F10 SYN | ~~matching source metadata can retain stale syntax tables~~ — fixed 2026-09-13 in `2c9c7431` | tests/audit/yew_f_011.c | s40 §6; s58 F10 q4 |
| YEW-F-012 | M | fixed | F10 SYN | ~~pending embeds occupy a canonical state tail slot~~ — fixed 2026-09-13 in `43a82533` | tests/audit/yew_f_012.c | s41.5 §1 / DoD 5; s58 F10 q2 |
| YEW-F-013 | M | fixed | F10 SYN | ~~JS/TS known-wrong golden rows lack the heuristic comment~~ — fixed 2026-09-13 in `838fd7e1` | tests/audit/yew_f_013.c | s42 §9 / testing strategy; s58 F10 q9 |
| YEW-F-014 | M | fixed | F11 LSP | ~~stripped LSP completion bypasses the module hard error~~ — fixed 2026-09-13 in `114f99fb` | tests/audit/yew_f_014.c | s45 DoD 13; s47 §7; s58 F11 q8 |
| YEW-F-015 | M | fixed | F11 LSP | ~~snippet-policy grep gate matches unrelated core code~~ — fixed 2026-09-13 in `59d318cf` | tests/audit/yew_f_015.c | s47 DoD 4; s58 F11 q7 |
| YEW-F-016 | M | fixed | F11 LSP | ~~required 1-based display edges violate the LSP +/-1 gate~~ — fixed 2026-09-13 in `07ee554b` | tests/audit/yew_f_016.c | s46 DoD 4; s47 §5; s58 F11 q2 |
| YEW-F-017 | M | fixed | F13 GIT | ~~interactive rebase bypasses the Git verb and environment boundary~~ — fixed 2026-09-13 in `c96b4f91` | tests/audit/yew_f_017.c | s51 §2/§3; s52 §11; s58 F13 q1/q7 |
| YEW-F-018 | M | fixed | F13 GIT | ~~FUSS picker detail bypasses the module clock discipline~~ — fixed 2026-09-13 in `b61d329b` | tests/audit/yew_f_018.c | s51 §10/DoD 2; s58 F13 q4 |
| YEW-F-019 | M | fixed | F13 GIT | ~~porcelain rename test survives the required one-NUL mutation~~ — fixed 2026-09-13 in `b464953d` | tests/audit/yew_f_019.c | s51 DoD 4; s58 F13 q2 |
| YEW-F-020 | M | fixed | F13 GIT | ~~Git formatting gate rejects legitimate display formatting~~ — fixed 2026-09-13 in `0195ed1c` | tests/audit/yew_f_020.c | s51 DoD 2; s58 F13 q1 |
| YEW-F-021 | M | fixed | F14 PLUG | ~~plugin teardown retains raw hook and ledger lengths~~ — fixed 2026-09-14 in `073489ff` | tests/audit/yew_f_021.c | s54 section 4 / DoD 4; s58 F14 q3 |
| YEW-F-022 | M | fixed | F14 PLUG | ~~plugin trust wording gate rejects its required warning~~ — fixed 2026-09-14 in `4858a39c` | tests/audit/yew_f_022.c | s54 section 7 / DoD 12; s58 F14 q6 |
| YEW-F-023 | M | fixed | F14 PLUG | ~~plugin commands cannot enter the recorder CMDWORD space~~ — fixed 2026-09-14 in `d2dc4ddf` | tests/audit/yew_f_023.c | s58 F14 q8 |
| YEW-F-024 | M | fixed | F15 CI | ~~cross-surface XFAIL debt table stops at F004~~ — fixed 2026-09-14 in `56ce3f24` | tests/audit/yew_f_024.c | s58 section 3 / F15 q1 |
| YEW-F-025 | M | fixed | F15 CI | ~~script tests have no XFAIL or hard-XPASS state~~ — fixed 2026-09-14 in `da1f9cd0` | tests/audit/yew_f_025.c | s58 section 3 / F15 q1 |
| YEW-F-026 | M | fixed | F15 CI | ~~PTY cases have no XFAIL or hard-XPASS state~~ — fixed 2026-09-14 in `86ccb661` | tests/audit/yew_f_026.c | s58 section 3 / F15 q1 |
| YEW-F-027 | M | fixed | F15 CI | ~~Fletch format ban accepts macro-forwarded nonliteral formats~~ — fixed 2026-09-14 in `c371b5a4` | tests/audit/f15_ban_misses.c | s31 DoD 5; s58 F15 q2 |
| YEW-F-028 | M | fixed | F15 CI | ~~Fletch abort ban accepts macro-forwarded abort~~ — fixed 2026-09-14 in `f6c8075d` | tests/audit/f15_ban_misses.c | s32 DoD 10; s58 F15 q2 |
| YEW-F-029 | M | fixed | F15 CI | ~~stable-sort ban accepts macro-forwarded qsort~~ — fixed 2026-09-14 in `0e2ab552` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-030 | M | fixed | F15 CI | ~~C11-subset ban accepts token-pasted attribute syntax~~ — fixed 2026-09-14 in `ccfc924c` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-031 | M | fixed | F15 CI | ~~explicit-registry ban accepts token-pasted constructors~~ — fixed 2026-09-14 in `ccfc924c` | tests/audit/f15_ban_misses.c | s01 sections 1/6; s58 F15 q2 |
| YEW-F-032 | M | fixed | F15 CI | ~~single-thread ban accepts token-pasted pthread calls~~ — fixed 2026-09-14 in `a294794c` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-033 | M | fixed | F15 CI | ~~reproducibility ban omits `__TIMESTAMP__`~~ — fixed 2026-09-14 in `381bddf0` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-034 | M | fixed | F15 CI | ~~mmap ban accepts macro-forwarded calls~~ — fixed 2026-09-14 in `47046a40` | tests/audit/f15_ban_misses.c | s01 section 6; s58 F15 q2 |
| YEW-F-035 | M | fixed | F15 CI | ~~allocator ban accepts macro-forwarded libc allocation~~ — fixed 2026-09-14 in `066041c3` | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-036 | M | fixed | F15 CI | ~~cwd-allocation ban requires literal NULL spelling~~ — fixed 2026-09-14 in `16441f04` | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-037 | M | fixed | F15 CI | ~~realpath-allocation ban requires literal NULL spelling~~ — fixed 2026-09-14 in `16441f04` | tests/audit/f15_ban_misses.c | s57 section 3; s58 F15 q2 |
| YEW-F-038 | M | fixed | F15 CI | ~~locale-dependent Unicode ban omits `mbtowc`~~ — fixed 2026-09-14 in `af94a8c1` | tests/audit/f15_ban_misses.c | s19 portability law; s58 F15 q2 |
| YEW-F-039 | M | fixed | F15 CI | ~~native-loader ban omits `dlvsym`~~ — fixed 2026-09-14 in `f09c70e0` | tests/audit/f15_ban_misses.c | s54 Fletch-only plugin law; s58 F15 q2 |
| YEW-F-040 | M | fixed | F15 CI | ~~strerror_r ban accepts macro-forwarded calls~~ — fixed 2026-09-14 in `1cac4adf` | tests/audit/f15_ban_misses.c | s57 portability audit; s58 F15 q2 |
| YEW-F-041 | M | fixed | F15 CI | ~~musl backtrace ban omits `backtrace_symbols_fd`~~ — fixed 2026-09-14 in `aec0fe6b` | tests/audit/f15_ban_misses.c | s57 musl profile; s58 F15 q2 |
| YEW-F-042 | M | fixed | F15 CI | ~~GNU-libc ban omits `getopt_long_only`~~ — fixed 2026-09-14 in `aec0fe6b` | tests/audit/f15_ban_misses.c | s57 musl profile; s58 F15 q2 |
| YEW-F-043 | M | fixed | F15 CI | ~~long-double ban misses valid continued declarations~~ — fixed 2026-09-14 in `4b2b0f02` | tests/audit/f15_ban_misses.c | s57 ABI audit; s58 F15 q2 |
| YEW-F-044 | M | fixed | F15 CI | ~~shim-honesty gate accepts parenthesized success~~ — fixed 2026-09-14 in `75bfedf5` | tests/audit/f15_ban_misses.c | s57 module-size profiles; s58 F15 q2 |
| YEW-F-045 | M | fixed | F15 CI | ~~Unicode-width ban accepts decimal local tables~~ — fixed 2026-09-14 in `31497135` | tests/audit/f15_ban_misses.c | s19 width ownership; s58 F15 q2 |
| YEW-F-046 | M | fixed | F15 CI | ~~syntax-color ban accepts packed decimal colors~~ — fixed 2026-09-14 in `76f2fed1` | tests/audit/f15_ban_misses.c | s40 semantic attrs; s58 F15 q2 |
| YEW-F-047 | M | fixed | F15 CI | ~~syntax-width ban accepts local width arithmetic~~ — fixed 2026-09-14 in `41512e2c` | tests/audit/f15_ban_misses.c | s40 byte-span ownership; s58 F15 q2 |
| YEW-F-048 | M | fixed | F15 CI | ~~PTY-creation ban omits direct `posix_openpt` callers~~ — fixed 2026-09-14 in `674574eb` | tests/audit/f15_ban_misses.c | s06 audited harness; s58 F15 q2 |
| YEW-F-049 | M | fixed | F15 CI | ~~CI golden-update ban depends on contiguous spelling~~ — fixed 2026-09-14 in `58684ebb` | tests/audit/f15_ban_misses.c | s06 golden update law; s58 F15 q2 |
| YEW-F-050 | M | fixed | F15 CI | ~~piece-tree I/O ban omits `pread`~~ — fixed 2026-09-14 in `6cf5e51c` | tests/audit/f15_ban_misses.c | s08 I/O ownership; s58 F15 q2 |
| YEW-F-051 | M | fixed | F15 CI | ~~shadow-preview ban accepts manual destructive fill~~ — fixed 2026-09-14 in `910ea6be` | tests/audit/f15_ban_misses.c | s44 composition law; s58 F15 q2 |
| YEW-F-052 | M | fixed | F15 CI | ~~FUSS drawer ban accepts indirect pane-root replacement~~ — fixed 2026-09-14 in `6db23a13` | tests/audit/f15_ban_misses.c | s57.7 off-canvas law; s58 F15 q2 |
| YEW-F-053 | M | fixed | F15 CI | ~~deterministic-fuzz ban omits `random`~~ — fixed 2026-09-14 in `fff0404d` | tests/audit/f15_ban_misses.c | s02 deterministic seeds; s58 F15 q2 |
| YEW-F-054 | M | fixed | F15 CI | ~~clipboard shell ban omits direct shell exec~~ — fixed 2026-09-14 in `702b5e92` | tests/audit/f15_ban_misses.c | s24 no-shell subprocess law; s58 F15 q2 |
| YEW-F-055 | M | fixed | F15 CI | ~~job-interpolation ban accepts raw append into shell text~~ — fixed 2026-09-14 in `7c3f0347` | tests/audit/f15_ban_misses.c | s37 argv boundary; s58 F15 q2 |
| YEW-F-056 | M | fixed | F15 CI | ~~OSC 52 query ban accepts split string literals~~ — fixed 2026-09-14 in `7808dea7` | tests/audit/f15_ban_misses.c | s24 write-only OSC 52; s58 F15 q2 |
| YEW-F-057 | M | fixed | F15 CI | ~~terminal-syscall ban omits `tcflush`~~ — fixed 2026-09-14 in `45f6137b` | tests/audit/f15_ban_misses.c | s37 tty boundary; s58 F15 q2 |
| YEW-F-058 | M | fixed | F15 CI | ~~register choke-point ban accepts allowed-file wrappers~~ — fixed 2026-09-14 in `1519b8d8` | tests/audit/f15_ban_misses.c | s36 register routing; s58 F15 q2 |
| YEW-F-059 | M | fixed | F15 CI | ~~option choke-point ban accepts allowed-file wrappers~~ — fixed 2026-09-14 in `02949a63` | tests/audit/f15_ban_misses.c | s36 option routing; s58 F15 q2 |
| YEW-F-060 | M | fixed | F15 CI | ~~package-git ban accepts allowed-file wrappers on startup~~ — fixed 2026-09-14 in `953b1e3c` | tests/audit/f15_ban_misses.c | s55 startup transport law; s58 F15 q2 |
| YEW-F-061 | M | fixed | F15 CI | ~~register-width ban accepts local lookup tables~~ — fixed 2026-09-14 in `31497135` | tests/audit/f15_ban_misses.c | s36 Unicode routing; s58 F15 q2 |
| YEW-F-062 | M | fixed | F15 CI | ~~register-column ban depends on historical variable names~~ — fixed 2026-09-14 in `d87c57f6` | tests/audit/f15_ban_misses.c | s36 column routing; s58 F15 q2 |
| YEW-F-063 | M | fixed | F15 CI | ~~register-helper presence gate accepts comments~~ — fixed 2026-09-14 in `f36b1e1e` | tests/audit/f15_ban_misses.c | s36 helper routing; s58 F15 q2 |
| YEW-F-064 | M | fixed | F15 CI | ~~oracle-independence ban accepts copied renamed models~~ — fixed 2026-09-14 in `e79f2414` | tests/audit/f15_ban_misses.c | s11 independent oracle; s58 F15 q2 |
| YEW-F-065 | M | fixed | F15 CI | ~~generated-table ban verifies only a retained marker~~ — fixed 2026-09-14 in `471a5064` | tests/audit/f15_ban_misses.c | s19 generated UCD tables; s58 F15 q2 |
| YEW-F-066 | M | fixed | F15 CI | ~~termination-site ban omits `_Exit`~~ — fixed 2026-09-14 in `4dc257b5` | tests/audit/f15_ban_misses.c | s01 exit contract; s58 F15 q2 |
| YEW-F-067 | M | fixed | F15 CI | ~~AI-body logging ban depends on variable names~~ — fixed 2026-09-14 in `13eca642` | tests/audit/f15_ban_misses.c | s50 privacy gate; s58 F15 q2 |
| YEW-F-068 | M | fixed | F15 CI | ~~unit-registry ban omits static test definitions~~ — fixed 2026-09-14 in `b159ff7d` | tests/audit/f15_ban_misses.c | s01 explicit registry; s58 F15 q2 |
| YEW-F-069 | M | fixed | F15 CI | ~~PTY minimum-case gate skips a missing registry~~ — fixed 2026-09-14 in `95695028` | tests/audit/f15_ban_misses.c | s06 registry minimum; s58 F15 q2 |
| YEW-F-070 | M | fixed | F15 CI | ~~PTY golden gate accepts computed missing names~~ — fixed 2026-09-14 in `613e23aa` | tests/audit/f15_ban_misses.c | s06 golden completeness; s58 F15 q2 |
| YEW-F-071 | M | fixed | F15 CI | ~~PTY orphan gate counts dead preprocessor rows~~ — fixed 2026-09-14 in `e5cec738` | tests/audit/f15_ban_misses.c | s06 golden completeness; s58 F15 q2 |
| YEW-F-072 | M | open | F15 CI | designated performance evidence remains placeholder-only | tests/audit/yew_f_072.c | s56 section 4; s58 F15 q3 |
| YEW-F-073 | M | fixed | F15 CI | ~~baseline history policy is not enforced~~ — fixed 2026-09-14 in `aa77a9e6` | tests/audit/yew_f_073.c | s56 baseline policy; s58 F15 q4 |
| YEW-F-074 | H | fixed | F15 CI | ~~Darwin shipping clean rebuilds differ by Mach-O UUID~~ — fixed 2026-09-13 in `16761aba` | tests/audit/yew_f_074.c | invariant 5; s58 F15 q5 |
| YEW-F-075 | C | fixed | F15 CI | ~~stripped builds accept module-only config as inert state~~ — fixed 2026-09-13 in `ee6f9894` | tests/audit/yew_f_075.c | invariant 3; s58 F15 q7 |
| YEW-F-076 | M | fixed | F03 TEXT | ~~accepted unsaved undo sidecars are not byte-canonical~~ — fixed 2026-09-14 in `4c887676` | tests/audit/yew_f_076.c | s10 section 9 / DoD 8; s58 section 6.4 |
| YEW-F-077 | M | fixed | F03 TEXT | ~~rectangular yank omits required short-row padding~~ — fixed 2026-09-14 in `792e32d8` | tests/audit/yew_f_077.c | invariant 2; s12 section 5 |
| YEW-F-078 | M | fixed | F03 TEXT | ~~crash journal admits a same-metadata replacement inode~~ — fixed 2026-09-14 in `f65eea0d` | tests/audit/yew_f_078.c | invariant 1; s08 section 4; s58 section 8 |
| YEW-F-079 | C | fixed | F07 UI | ~~workspace re-emission drops unknown entity-record fields~~ — fixed 2026-09-13 in `9222b491` | tests/audit/yew_f_079.c | invariant 1; s25 §4 / §6; s59 §1.2 |

`YEW-F-001` was Medium because the width mismatch visibly corrupted chrome but
left the document bytes intact and could be recovered by disabling
`ambiguous_wide`. Commit `c5a11c96` keeps that option authoritative for
document text while centrally selecting a same-width ASCII row for any Unicode
chrome glyph that would outgrow its fixed slot. The regression covers both the
complete glyph vocabulary and an actual grid write whose adjacent cell must
survive. The reproducer failed at the fixed baseline and passed after the fix
under Clang, Clang ASan/UBSan, and GCC 16 `MODULES=""`; the original failure was
also confirmed by hosted audit-control run `33815573832` across GCC, Clang,
ASan/UBSan, Linux arm64, macOS arm64, musl, and `MODULES=""`.

`YEW-F-002` was Medium because all bytes eventually arrived, but a completed
four-byte flag cluster remained absent from a live job buffer until the child
wrote again or exited. Commit `566b07b6` separates the contracts: interactive
reverse navigation retains its 64-codepoint approximation, while bounded job
read windows use an exact final-cluster edge. Odd and even long RI runs now
retain only the genuinely open final cluster. The reproducer failed at the
fixed baseline and passed after the fix under Clang, Clang ASan/UBSan, and GCC
16 `MODULES=""`; the original failure was also confirmed by hosted
audit-control run `33815573832` across the same cross-compiler,
cross-architecture matrix.

`YEW-F-003` was High because valid keycap text reached a `YEW_BUG` in the
renderer, terminating yew with exit 4. Commit `2fd132e1` keeps the final ASCII
scalar of a mixed run for the grapheme walker, preserves the bulk path for the
already-certain ASCII prefix, and makes fragmented zero-width appends
revalidate the joined cluster and update both grid cells when its width
changes. The reproducer is now an ordinary passing audit test; focused tests
also cover prefix/suffix placement, collision with a prior wide glyph, cursor
snapping, and right-edge clipping, while the grid fuzzer now generates
keycaps.

Sibling check: `yew_str_width` carried the same printable-ASCII-is-a-cluster
assumption and now uses the same safe-prefix boundary. `yew_str_clip` already
walks complete clusters, and the coordinate index feeds its final ASCII scalar
through the streaming grapheme state before consuming following Unicode, so
neither had the grid-cell finalization bug.

`YEW-F-004` was Medium because a documented configuration shape failed loudly
at startup but did not corrupt document bytes; quoting the option name was a
working recovery. Commit `a030d621` gives the full and pure-literal parsers one
shared map-key path that folds `IDENT ("." IDENT)+` only in entry position, so
ordinary field access keeps its prior meaning. The parser, runtime application,
and Fletch conformance corpus cover the result. The audit fixture's independent
stale value `clipboard.sync: "none"` was corrected to the documented `"off"`;
that corrected fixture still fails at the first dot on the fixed audit baseline.
The reproducer passes after the fix under Clang, Clang ASan/UBSan, and GCC 16
`MODULES=""`.

`YEW-F-005` was High because a valid Fletch `edit {}` block containing a
buffer-range replacement with two live cursors reached `yew_bug()` and exited
4. Commit `db0759ed` recognizes the outer `YEW_TXN_MACRO` as the cursor-set
aggregate it already is, defers its after-cursor snapshot until transaction
close, and removes the multi-cursor runner's temporary reason mutation. The
reproducer is now an ordinary passing audit test: replacement returns normally
and one undo restores the exact text and both cursor positions.

Sibling check: successful multi-cursor macro replay still records a MACRO node,
and failed replay restores the buffer and cursor set before closing the outer
transaction. Focused undo, Fletch-transaction, macro-replay, replacement, and
multi-cursor suites cover the shared edit choke point and commit/abort symmetry;
the multi-cursor and undo fuzzers remain clean.

`YEW-F-006` was Critical: unknown workspace data belongs to the user and a
normal parse followed by save silently deleted it. Commit `9e829cd9` retains
the parsed root and workspace maps for the lifetime of the state arena and
re-emits their unknown fields after canonical live fields, so known values
win without deleting future data. The hard-XPASS reproducer is now an ordinary
passing audit test, with a nested-literal unit regression alongside it.

Cluster hunt: root, workspace, options, groups, tabs, pane trees, windows,
cursors, views, jumps, file records, marks, changes, and undo records were
checked at the parse/emit boundary. Options already retained their whole map;
the root/workspace singleton loss is fixed. Identity-bearing group and tab
records exhibited the same reconstruction loss and were filed first as
`YEW-F-079`; the remaining nested record sites are included in that retained
record-family investigation rather than silently declared clean.

`YEW-F-007` was Critical: a normal save and restore silently reordered the
user's group-member sequence. Commit `bea3990b` records the requested
ordinals while tabs are opened, then applies them only after every member has
joined its group. The hard-XPASS reproducer's descending tab-array order now
restores as the intended f0, f1, f2 sequence and is an ordinary passing audit
test. Oversized positive ordinals are bounded before the C integer conversion
and retain the existing final-list clamp semantics.

Cluster hunt: every production `yew_group_add_member` and
`yew_group_set_ordinal` pairing was checked. Mouse moves operate on a complete
destination group, group navigation appends explicitly at the complete
group's end, and the picker/from-directory/self-open paths only append. Pane
restore builds all window slots before resolving focus. No second
partial-collection positional setter was found.

`YEW-F-008` was High because a plugin declaring `capabilities: []` could write
an arbitrary file by storing Fletch source in a macro register through
`ed.run("ed.reg.set", ...)` and replaying it. Commit `8995bb1f` records the
calling function's defining origin at the named-register write boundary and
compiles replay under that preserved authority. The hard-XPASS reproducer is
now an ordinary passing audit test: the replayed `io.write` raises a capability
error, plugin initialization fails cleanly, and no file is created.

Sibling check: both macro-source write doors, `ed.reg.set` and ranged
`ed.edit.yank`, attach provenance; lower-case replacement and upper-case append
are covered. A same-byte host rewrite invalidates the provenance-sensitive
cache and restores config authority, while a plugin rewrite retains its
principal and zero-capability mask after the writer's frame has returned.

`YEW-F-009` was Medium because the recorder's mandatory shrinker self-test no
longer exercised its injected divergence, leaving a release-control claim
unproved without changing user bytes or product behavior. Generator-pool
growth changed seed 20764's prefix from the pinned pair of buffer-end motions
and insert into an unrelated unit motion. Commit `1c11ebee` replaces that
random-seed dependency with a purpose-built session shared by the sentinel
and audit. `make test-roundtrip` now runs the planted fault and requires it to
exit through `SELFTEST/P1`, shrink from 96 to at most three events, and name
`ed.move.buf.end`; the ordinary legal-folding sentinel remains green.

`YEW-F-010` was Medium because storing an invalid macro reported success even
though its first replay failed. Commit `c362cefd` gives the store path a
side-effect-free strict compiler pass: free global references must resolve to
a declaration in the candidate source, a persistent runtime global, or a
prelude entry before the atomic register write. Forward references and live
runtime helpers remain valid, while a definitely missing name produces a
source-positioned diagnostic and leaves the old register untouched. The
candidate is never executed during validation, so editor, shell, and I/O side
effects still occur only on an explicit replay.

`YEW-F-011` was Medium because an equal-size syntax source replacement whose
nanosecond mtime was restored could retain the old compiled table. Commit
`2c9c7431` makes the source hash authoritative for every cache hit, including
builtin definitions whose metadata appears unchanged. A matching hash still
avoids syntax recompilation, and matching metadata avoids an unnecessary
cache-header rewrite; only changed bytes force table recompilation. The
isolated builtin-shaped reproducer now loads its replacement `y` rule and
records exactly one compile.

`YEW-F-012` was Medium because pending embeds stored a future guest definition
in `aux[ndef]`, contradicting the canonical-state law that every unused tail
cell is zero. Commit `43a82533` makes tail clearing unconditional. The pure
line phase reports unresolved guest identity through its existing result side
channel, `SynBuf` owns the deterministic budgeted idle-load queue, and deferred
resident guests use line-local EOL scratch. Pending states therefore retain no
hidden definition identity, while the 84-byte state layout, fallback rendering,
one-load-per-settle pacing, and subsequent invalidation behavior remain intact.

`YEW-F-013` was Medium because the JS and TypeScript known-wrong golden rows
lacked Sprint 42's required adjacent explanation of the value-flag heuristic.
Commit `838fd7e1` restores an ID-bearing comment on each affected `)`/`}` row,
regenerates the exact span output, and strengthens the audit to require every
row to carry the explanation locally. The pre-Sprint-41.5 golden guard now
pins these two intentional comment-only changes as explicit old/new hash rows;
the remaining 226 historical goldens retain a separate unchanged aggregate.

`YEW-F-014` was Medium because a stripped build gave `ed.lsp.complete`
different module-boundary semantics from every sibling command. Commit
`114f99fb` routes the stripped completion shim through `yew_mod_require` and
removes the unit test's special-case exclusion. Core index completion remains
available through `ed.compl.open`; invoking an LSP-named command without the
module now returns the exact documented error across the full command set.

`YEW-F-015` was Medium because Sprint 47's mandatory repository-wide snippet
policy scan matched ten unrelated core names and comments in addition to its
one intended LSP policy sentence. Commit `59d318cf` gives those internal
concepts precise names—bootstrap tab, provisional tree/model, and deferred
jump target—without changing behavior. The policy paragraph carries an
ID-bearing uniqueness note, and the literal source scan now returns exactly
that one line as its release contract requires.

`YEW-F-016` was Medium because the literal no-line-arithmetic gate rejected
three required one-based picker/error display edges even though protocol
positions remained zero-based. Commit `07ee554b` centralizes the explicit
zero-based-to-display conversion in the shared coordinate layer and routes
all three LSP presentation sites through it. Boundary tests cover zero, one,
and `UINT32_MAX`; picker labels and rename errors remain one-based while the
Sprint 46 scan over `src/mod/lsp/` is now empty.

`YEW-F-017` was Medium because interactive rebase was the one Git execution
path outside the static verb inventory and shared environment builder. Commit
`c96b4f91` adds the rebase descriptor and a Git-owned synchronous terminal
runner. The runner retains the nested-yew editor and real-TTY handover, but now
builds canonical global argv options, applies the same prompt/flush/pager/
locale and trace-removal policy as every other Git path, and owns mutation
invalidation. Its only policy exception is the two explicitly replaced Git
editor rows. A 43-assertion behavioral test captures the complete job spec;
the source-backed audit also rejects a future direct job from FUSS.

`YEW-F-018` was Medium because FUSS picker detail read the process wall clock
through `time(NULL)` instead of yew's anchored editor clock. Commit `b61d329b`
routes relative-time formatting through the startup wall-time anchor advanced
by monotonic editor time, with tests for missing anchors, elapsed time, and
future timestamps. It also replaces the impossible literal source policy with
a token-aware control: actual calls to the exact `time`, `clock`, and
`cpu_time` identifiers fail, while permitted helpers whose longer names end
in `clock` do not. The stripped-module shim preserves the same public seam as
a deterministic no-op.

`YEW-F-019` was Medium because the mandatory porcelain-v2 mutation control
did not detect the exact desynchronisation it claimed to pin. Commit
`b464953d` replaces the rename's original-path fixture with a legal filename
that begins like a recognized porcelain record and asserts those bytes
exactly. The source-independent reproducer now observes eight records under
one-NUL advancement instead of the seven valid entries. With both production
passes temporarily advanced only to the destination NUL, the named unit test
failed at its first parse assertion; restoring the two-NUL parser returned all
17 assertions to green. Production parser behavior is unchanged.

`YEW-F-020` was Medium because Sprint 51's formatting grep could not establish
the narrower argv-safety rule it was meant to enforce. Commit `0195ed1c`
replaces that literal control with a semantic one: the canonical argv builder
must copy discrete tail elements without any formatting call, its structural
unit test must remain registered, and the hostile-filename integration matrix
must retain exact post-`--` byte comparisons across stage, unstage, diff, and
blame. The focused unit test passes 138 assertions and the full FUSS command
surface passes 544. Legitimate owned status, patch, and picker-detail
formatting remains unchanged.

`YEW-F-021` was Medium because plugin teardown cleared every active hook and
registration and the collector reclaimed their closures, but the raw hook and
ledger lengths remained at their first-cycle high-water marks instead of
returning to the pre-enable values required by Sprint 54 and F14. Commit
`073489ff` clears removed rows and trims inactive suffixes while retaining
interior tombstones, so later live ledger ids remain stable. The focused
invariant test removes and reuses an interior slot before collapsing both
tables to zero; the integrated 20-plugin by 20-cycle control now returns every
registry length to its exact pre-enable value and still proves that every
closure is reclaimed.

`YEW-F-022` was Medium because Sprint 54 requires its author guide to quote the
honest `plug.h` trust warning verbatim while its literal word ban rejected the
warning's statement that capability gates do not create a sandbox. Commit
`4858a39c` replaces that contradictory goalpost with a semantic control: the
guide and header trust blocks remain byte-identical, the guide must explicitly
deny memory and resource isolation, and its sole `sandbox` mention must be the
negative disclaimer. Both the release script and audit reproducer reject an
injected positive isolation claim.

`YEW-F-023` was Medium because plugin commands executed normally but could
never be represented by the recorder. Commit `d2dc4ddf` makes recordability
implicit, installs each parseable local name in the global CMDWORD map, and
rejects collisions with core or enabled-plugin commands before registration.
The command registry also reserves motion syntax that would parse as a
different action. A lifecycle regression records a real plugin invocation,
proves that its CMDWORD is emitted, and replays the live closure; collision
failure and repeated multi-plugin teardown retain their zero-residue checks.

`YEW-F-024` was Medium because Sprint 58 calls
`.docs/audits/xfail-debt.md` the authoritative cross-surface debt table, but
it retained only four of the 79 finding IDs. Commit `56ce3f24` gives every
finding exactly one retained verdict, records fixing commits on closed rows,
and makes the passing audit guard require exact set equality with no missing,
unexpected, or duplicate IDs. A deliberate one-row deletion now hard-fails
the audit suite.

`YEW-F-025` was Medium because the script runner had no syntax or state for an
expected failure. Commit `da1f9cd0` parses strict leading
`# XFAIL: YEW-F-NNN reason` metadata, requires an active authoritative debt
row, reports eligible failures as `XFAIL`, and makes an unexpected pass a hard
`XPASS`. Runner/setup failures, timeouts, signals, corrupt protocols, unknown
IDs, and fixed IDs cannot satisfy the marker. Self-checks pin parsing, ledger
status, classification, and output; end-to-end fail/pass/fixed-ID probes pin
the exit statuses.

The unit audit and Fletch conformance runners retain their independent hard
XPASS paths.

`YEW-F-026` was Medium because PTY cases had no expected-failure identity or
hard unexpected-pass state. Commit `86ccb661` adds `PtyCase.xfail_id`, requires
its ID to have an active authoritative debt row, and permits only a stable
comparison against an existing mismatching golden to report `XFAIL`. A matching
golden is a hard `XPASS`; missing or unreadable goldens, setup and execution
failures, independent-run instability, cleanup failures, live children, and
descriptor leaks remain hard failures. Update mode refuses marked cases. The
runner self-check and an end-to-end match/mismatch/fixed-ID drill pin the
classification and exit-status contract.

`YEW-F-027` was Medium because the Fletch format scanner recognized direct
printf-family call tokens but accepted a macro-forwarded call carrying a
nonliteral user-controlled format. Commit `c371b5a4` resolves object-like macro
alias chains before classifying format sinks, so forwarding cannot hide the
argument subject to the literal-format rule. The positive control pins a
two-hop nonliteral alias as a violation while the same alias with a literal
format and the bounded `va_list` forwarding exception remain accepted.

`YEW-F-028` was Medium because the VM abort scanner accepted `abort()` reached
through a plainly named macro. Commit `f6c8075d` makes the Fletch-only scan
reject object-like aliases naming `abort` or `assert` as well as direct calls,
and carries a positive macro-forwarding control so a later direct-call-only
regression fails the gate.

`YEW-F-029` was Medium because the stable-sort ban accepted `qsort` behind a
macro even though the resulting call retained the unstable cross-libc ordering
the rule forbids. Commit `0e2ab552` rejects object-like aliases naming `qsort`
or `qsort_r` and adds an alias-specific positive control alongside the existing
direct-call control.

`YEW-F-030` was Medium because token pasting produced the forbidden
`__attribute__` spelling only after preprocessing. Commit `ccfc924c` rejects
the incomplete `__attribute` stem as well as the completed spelling, closing
the exact paste boundary while retaining the direct GNU-extension ban. An
explicit token-paste positive control pins the behavior.

`YEW-F-031` was Medium because the constructor check could be bypassed by
token-pasting both the attribute and `constructor` name. The same
`ccfc924c` boundary rejects the required pasted attribute stem before the
constructor extension can exist, and the isolated paired-paste reproducer now
fails the real gate.

`YEW-F-032` was Medium because token-pasted `pthread_create` reached the
forbidden threading API without leaving the contiguous `pthread` text the
gate searched for. Commit `a294794c` also rejects the `thread_*` API stem,
which has no valid use in yew source, and pins the pasted call with a positive
control while retaining the direct `pthread` and `threads.h` checks.

`YEW-F-033` was Medium because `__TIMESTAMP__` embeds filesystem-dependent
build time just as surely as the two macros already banned. Commit `381bddf0`
adds it to the compiler-time pattern, updates the diagnostic to cover the
whole class, and adds a positive control for the formerly omitted macro.

`YEW-F-034` was Medium because a macro-forwarded `mmap` call survived the
source ban while preserving the truncate/SIGBUS hazard the rule exists to
exclude. Commit `47046a40` rejects object-like aliases naming `mmap` and adds
an alias positive control while retaining the direct-call check.

`YEW-F-035` was Medium because the audited-allocation scan keyed on a direct
libc function token followed by `(` and accepted a macro-forwarded `malloc`.
Commit `066041c3` rejects object-like aliases naming any libc allocator in the
existing set and adds a forwarding positive control while preserving the
audited yew allocator boundary.

`YEW-F-036` and `YEW-F-037` were Medium because the two libc-owned allocation
checks required `NULL` to appear literally at the call site. Commit
`16441f04` follows a nearby pointer initialized to `NULL` into the relevant
`getcwd` or `realpath` argument, without rejecting fixed caller-owned buffers.
Internal positive controls pin both APIs, and the two isolated variable-alias
reproducers now fail the real gate.

`YEW-F-038` was Medium because the locale-dependent Unicode list included
`mbrtowc` but omitted its older stateful sibling `mbtowc`. Commit `af94a8c1`
adds the omitted API to the same bespoke-Unicode boundary and pins it with an
explicit positive control.

`YEW-F-039` was Medium because the native-loader list covered `dlsym` but
omitted the GNU versioned lookup `dlvsym`. Commit `f09c70e0` adds the versioned
resolver to the Fletch-only plugin boundary and pins the omission with a
dedicated positive control.

`YEW-F-040` was Medium because a macro-forwarded `strerror_r` call preserved
the incompatible ABI surface while evading the direct-call regex. Commit
`1cac4adf` rejects object-like aliases naming `strerror_r` and adds an
alias-specific positive control alongside the direct-call control.

`YEW-F-041` was Medium because `backtrace_symbols_fd` is part of the same
glibc/execinfo family but was absent from the musl-compatibility pattern.
Commit `aec0fe6b` adds the omitted sibling to the execinfo family and gives it
an explicit positive control.

`YEW-F-042` was Medium because `getopt_long_only` is a GNU extension adjacent
to the listed `getopt_long`, but the word-boundary shape let the longer name
pass. The same `aec0fe6b` portability commit adds it to the GNU API family and
pins it with a dedicated positive control.

`YEW-F-043` is Medium because C line continuation permits `long double` to
span physical source lines before preprocessing while grep evaluates each
line separately. Commit `4b2b0f02` reconstructs translation-phase-2 logical
lines in one deterministic scan, preserves source locations, and pins the
continued form with an internal positive control.

`YEW-F-044` is Medium because the shim honesty parser recognizes only a few
literal return expressions. A disabled action returning `(YEW_CMD_OK)` has
identical success semantics but passed `check-module-shims.sh`. Commit
`75bfedf5` recognizes any number of enclosing parentheses and adds both bare
and multiply-parenthesized internal positive controls.

`YEW-F-045` is Medium because the non-Unicode width gate searches four
symbolic/hex spellings, while the same code points in decimal form a local
width table that passed. Commit `31497135` recognizes the exact decimal and
hexadecimal code-point spellings with token boundaries and pins the decimal
form with an internal positive control.

`YEW-F-046` is Medium because a syntax definition can emit a packed decimal
foreground color without matching hex, RGB, or terminal escape spellings.
Commit `76f2fed1` closes the alternate spelling by rejecting color-role names
outside the excluded theme owner and pins the decimal form with an internal
positive control.

`YEW-F-047` is Medium because syntax-local cell width arithmetic needs none of
the two helper names the gate scans. A simple wide-threshold calculation
passed even though syntax is required to own byte spans only. Commit
`41512e2c` recognizes the East Asian threshold in hexadecimal and decimal
forms and pins the local calculation with an internal positive control.

`YEW-F-048` is Medium because the PTY rule says creation must use the audited
`posix_openpt` harness but does not scan for `posix_openpt` itself. A direct
caller in another PTY test passed the gate. Commit `674574eb` recognizes all
three PTY creation APIs plus `-lutil`, retains the original all-tests scan,
and restricts them to the four audited fixtures that require direct ownership.

`YEW-F-049` is Medium because shell token concatenation constructs the golden
update environment name at execution time while preventing its contiguous
appearance in workflow source. CI could therefore enable updates while the
gate stayed green. Commit `58684ebb` scans spliced logical workflow lines for
the prefixed update word even when quotes divide its runtime spelling and
makes empty workflow lists a successful no-op rather than a silent abort.

`YEW-F-050` is Medium because the piece-tree I/O ban lists `open`, `fopen`, and
`read`, but direct `pread` retains the forbidden file-I/O ownership and passes.
No product I/O was found. Commit `6cf5e51c` brings `pread` under the same token-
bounded ownership gate and adds an internal positive control for the spelling.

`YEW-F-051` is Medium because the insertion-preview rule bans one fill helper,
not destructive fill behavior. A loop assigning every grid cell directly
passed while violating the same compositional rendering contract. Commit
`910ea6be` detects zero-to-column-bound full-row loops that assign backing
cells, while preserving the bounded range shifts required for composition.

`YEW-F-052` is Medium because taking the address of `pane_root` and assigning
through that pointer replaces the live root without matching the direct
assignment pattern. Commit `6db23a13` rejects both direct replacement and
address-taking, with positive controls for both forms.

`YEW-F-053` is Medium because deterministic fuzzing bans `rand`, `srand`, and
one `time` spelling but omits the libc `random()` generator. A direct call
passes the actual gate. Commit `fff0404d` brings both `random()` and its
`srandom()` seeding function under the deterministic-campaign gate and adds
positive controls for each form.

`YEW-F-054` is Medium because the clipboard no-shell rule scans only `popen`
and `system`; executing `/bin/sh -c` directly with `execl` has the same
forbidden behavior and passes. No such product path was established. Commit
`702b5e92` brings direct variadic exec of common shell paths with `-c` under
the same gate and adds a positive control, while retaining argv-based
clipboard execution.

`YEW-F-055` is Medium because the job-data rule recognizes two formatting
shapes while a raw byte append into a buffer named `shell` performs equivalent
interpolation and passes. Commit `7c3f0347` detects raw appends whose
destination is a shell or command-line buffer and adds a positive control;
ordinary register command-line storage remains outside that boundary.

`YEW-F-056` is Medium because adjacent C literals construct an OSC 52 query
whose semicolon and question mark are separated only in source. The terminal
receives the forbidden query while grep reports green. Commit `7808dea7`
detects quote-and-whitespace-separated query markers while retaining the
original contiguous-query rule, with a positive control for each form.

`YEW-F-057` is Medium because the terminal boundary lists four calls but omits
`tcflush`, another direct terminal-control syscall. An out-of-boundary call
passes the actual gate. Commit `45f6137b` adds `tcflush` to the same guarded
owner rule and pins the spelling with a positive control.

`YEW-F-058` was Medium because exempting `register.c` let that file publish a
raw setter wrapper for forbidden callers. Commit `1519b8d8` makes the raw
named-register store private, migrates direct test setup through the existing
macro front door, and bans any reintroduction of the old product symbol.

`YEW-F-059` was Medium because exempting complete option implementation files
let those files publish raw write wrappers for forbidden callers. Commit
`02949a63` replaces file exemptions with a comment/literal-aware function-owner
gate covering both setter levels and only the five legitimate routing owners.

`YEW-F-060` was Medium because exempting the package implementation let it
publish a raw Git wrapper for startup callers. Commit `953b1e3c` makes the raw
transport file-private, removes its public and stripped-module surfaces, and
pins its three legitimate package-command owners with the shared C-call gate.

`YEW-F-061` is Medium because register-local Unicode width calculation can use
a decimal lookup table without importing or naming a yew width helper. The
shared Unicode-ownership correction in `31497135` catches that same lookup
table before the register-specific helper check, closing both findings with
one boundary rule.

`YEW-F-062` was Medium because the register column-arithmetic rule searched
three historical variable names followed by `.v`. Commit `d87c57f6` strips
comments and literals, discovers every `CCol`/`CellCol` declarator, and rejects
direct representation access independently of the variable name.

`YEW-F-063` was Medium because the four required-helper checks accepted names
in comments as proof of routing. Commit `f36b1e1e` shares a C comment/literal
stripper with the typed column check and requires an executable call token for
each coordinate helper.

`YEW-F-064` was Medium because oracle independence could not be established by
banning two implementation names. Commit `e79f2414` retains that defense but
also seals the reviewed naive array-of-lines implementation with portable
SHA-256 verification, forcing structural oracle changes through explicit
review.

`YEW-F-065` was Medium because retaining the generated-file marker after a
manual table edit satisfied the entire generated-UCD check. Commit `471a5064`
pins the exact `tables.c` digest produced by the offline regeneration lane;
local regeneration of all four vendored-UCD outputs was byte-identical.

`YEW-F-066` was Medium because `_Exit` terminated the process outside
`yew_bug` without matching the lowercase `exit()` scanner. Commit `4dc257b5`
checks both spellings by exact function owner while preserving audited
post-fork `_exit` calls.

`YEW-F-067` was Medium because the AI privacy scanner inferred body data from
a small list of identifier substrings. Commit `13eca642` replaces that guess
with an exact owner list for every ordinary AI log call; all other body-capable
logging must use the existing dual-gated debug sink.

`YEW-F-068` was Medium because the explicit unit registry inventory recognized
only definitions beginning exactly with `void`. Commit `b159ff7d` reserves the
`test_*` namespace for registry-owned tests regardless of linkage and renames
the existing private helpers so an unregistered static test cannot hide.

`YEW-F-069` was Medium because the PTY minimum-count check was conditional on
the registry file existing. Commit `95695028` makes the registry itself
required audit evidence, so its removal fails closed before any case or golden
inventory can be skipped.

`YEW-F-070` was Medium because a snapshot name held in a variable was invisible
to the missing-golden extractor. Commit `613e23aa` restricts every snapshot
selection to an auditable literal or the exact registered case name, and checks
literal names from both plain and SGR snapshot calls for committed goldens.

`YEW-F-071` was Medium because the PTY case extractor did not honor the C
preprocessor. Commit `e5cec738` removes provably dead constant branches before
inventorying cases and snapshots while conservatively retaining both sides of
module-dependent conditions.

`YEW-F-072` is Medium because the two designated lanes cannot currently
produce a performance verdict: both committed calibration references and the
arm64 baseline are absent, while the x86_64 baseline still carries an all-zero
template calibration vector. Hosted lanes continue to execute the harnesses
and hard sanity checks, but they are advisory by contract. With no designated
measurements, F15 cannot recompute a 30-run noise floor or compare a threshold
to it. This is an invariant-4 control mismatch, not evidence that a user-facing
budget is exceeded, and remains open for Sprint 59.

`YEW-F-073` was Medium because `perf-baseline-guard.sh` read only the changed
path list. Commit `aa77a9e6` establishes the audited history cutover and makes
every later baseline commit carry a rebaseline subject, a specific reason, and
at least one declared old-to-new metric pair verified against its numerical
diff. The pre-cutover classification remains retained in `audit-15-ci.md`.

`YEW-F-074` was High because invariant 5 requires byte-identical builds and
all four single-module profiles (`lsp`, `ai`, `fuss`, `plugins`) produced
different SHA-256 hashes across consecutive clean builds of the fixed product
baseline on arm64 macOS. Commit `16761aba` enables the Darwin linker's
`-reproducible` mode only for `SHIPPING=1`, retaining the `LC_UUID` and derived
ad-hoc signature required by current `dyld` while excluding volatile input
properties from both. Two clean minimal builds in different build trees,
stripped to the same release basename, are byte-identical at
`24918d773d8653f7a53bcdebbece3ec165c6e633bd8930f3d34eafd99eaa28a0`
and the stripped binary executes `--version`. The earlier audit experiment's
`-no_uuid` route was rejected during remediation because current Apple-silicon
`dyld` aborts such an executable. The regression also asserts that development
links retain their ordinary debugger metadata path.

`YEW-F-075` was Critical because excluded-module option writes either became
inert state or reported a generic unknown-option error. Commit `ee6f9894`
adds explicit module ownership to the option descriptor table, keeps every
owned key discoverable in all profiles, and gates validation, direct writes,
and transactional checkpoints through the canonical `yew_mod_require`
diagnostic before mutation. The hard-XPASS reproducer now inventories every
owned descriptor and passes in both the default and `MODULES=""` profiles.
Commit `0da3de82` also removes the redundant LSP option write from the shipped
core init file, whose descriptor default remains `here`, so the same default
configuration executes with or without optional modules.

Cluster hunt: checked all static and dynamic option descriptors, every
module-key prefix, direct/validated/transactional write paths, the shipped
runtime config, AI presets, and plugin config consumers; clean. The only
sibling was the redundant `lsp.open_in` row in `runtime/init.fl`, fixed in
`0da3de82`. `shadow.*` debounce and `compl.*` options remain core-owned by
their core arbitration and symbol-index completion contracts.

`YEW-F-076` was Medium because a corrupt but unused anchor-hash field in an
unsaved undo sidecar is accepted as current and then silently canonicalized
on its next write. Document bytes and the undo tree remain recoverable, but
the accepted-file byte round-trip required by Sprint 58 section 6.4 is false.
Coverage-guided mutation flipped one bit at header offset 40; the standalone
reproducer deterministically rebuilds that input. Commit `4c887676`
reconstructs the unsaved root identity and rejects a mismatched anchor before
installing the loaded tree. It also retains the truncation provenance of a
valid loaded sidecar so a full rewrite preserves the accepted header exactly.
The obsolete fuzz exception is gone; the exact-byte round-trip oracle passed
200,000 deterministic mutations (`seed=1`, corpus 7, hash
`231e0ce0abff3c1d`).

`YEW-F-077` was Medium because an ordinary rectangular yank clipped short rows
to their natural byte length but labeled the resulting register non-ragged.
Sprint 12 requires those rows to be right-padded to the rectangle's `CCol`
width at store time, so a later paste reproduces different geometry from the
selection the user yanked. The hard-XFAIL selects columns `[0, 2)` across
`a` and `bb`: the required rows are `a ` and `bb`, while the baseline stores
`a` and `bb`. The original buffer remains intact and the result is
recoverable, so this is Medium rather than data loss. Commit `792e32d8`
measures each clipped row in source `CCol` space and synthesizes only its
missing right-padding spaces at store time. Rows widened around indivisible
tabs, wide glyphs, or escaped invalid bytes remain byte-exact and are never
mistaken for short rows.

`YEW-F-078` is Medium because a durable journal authenticates its base with
the canonical path, byte size, and nanosecond mtime, but not file identity or
content. Another process can install different same-size bytes at that path
and restore the recorded mtime; probe and replay then splice the old file's
edit into the replacement instead of recovering the exact intended buffer.
The reproducer retains two hardlinks to the original base, so both the
mismatch and the correct recovery source remain observable and recoverable.
No disk file is silently overwritten by replay, making this Medium rather
than Critical. Commit `f65eea0d` extends the journal header with device/inode
identity and creates a durable base companion before the header is committed.
The normal same-filesystem path is a constant-space hardlink; a cross-device
fallback is a private CRC-authenticated copy. Replay of a replacement restores
that exact base before applying the journal, while save topology discounts
only the known active recovery link. Unsupported legacy logs and their bases
are preserved under collision-free stale names instead of being truncated.
The exact `Xalpha\n` reproducer, companion lifecycle units, the complete audit
suite, and a 200-process randomized kill campaign all pass.

`YEW-F-079` was Critical because Sprint 25's forward-compatibility contract is
not limited to singleton maps: every entity record carries user-owned
workspace state. Commit `9222b491` adds a sparse retained-record map keyed by
the live stable identities for groups, tabs, windows, buffers, cursors, and
marks, with compact state tokens for pane and history-entry records that have
no schema identity. Known live fields are emitted first and win; unknown
future fields retain their exact byte keys and nested values. The audit now
reorders tabs and split/closes a retained pane before checking the complete
record family, and is an ordinary passing test.

Cluster hunt: group, tab, pane, window, cursor, view, jumplist container and
entry, file, mark, changelist container and entry, and undo records were all
checked at the parse/emit boundary. The sweep found two siblings within the
same loss surface: pending named marks were omitted before hydration, and
retained map-key emission measured keys as C strings, truncating a future key
at an embedded NUL. Both are fixed and covered by the F079 audit. Cursor
re-identification was also made monotonic so undo cannot attach an old future
record to a replacement cursor. Root/workspace records remain covered by
`YEW-F-006`, and the options map was already retained wholesale. No further
record reconstruction site remains open.

## Unverified observations

Observations live in their front files. They have no IDs and are excluded
from every total.
