# Expected-failure debt

This is Sprint 58's authoritative cross-surface debt ledger. A confirmed
`YEW-F-###` finding adds exactly one row. Remediation strikes the reason,
records its fixing commit, and changes the status to `fixed`; rows are never
deleted, because the retained verdict is the audit trail. Deferred and
`wontfix` rows retain their XFAIL marker and name their target or rationale.

<!-- YEW-F-024: retaining every findings-ledger ID here prevents expected-
     failure debt from silently falling out of cross-surface tracking. -->

| ID | Surface | Reproducer | Reason | Status |
|---|---|---|---|---|
| YEW-F-001 | audit | `tests/audit/yew_f_001.c` | ~~ambiguous-wide doubles fixed-cell chrome glyphs~~ — fixed in `c5a11c96` | fixed |
| YEW-F-002 | audit | `tests/audit/yew_f_002.c` | ~~long RI output delays a completed flag cluster~~ — fixed in `566b07b6` | fixed |
| YEW-F-003 | audit | `tests/audit/yew_f_003.c` | ~~ASCII-base keycap leaves inconsistent grid width~~ — fixed in `2fd132e1` | fixed |
| YEW-F-004 | audit | `tests/audit/yew_f_004.c` | ~~full Fletch parser rejects bare dotted map keys~~ — fixed in `a030d621` | fixed |
| YEW-F-005 | audit | `tests/audit/yew_f_005.c` | ~~multi-cursor replacement exits inside a Fletch edit transaction~~ — fixed in `db0759ed` | fixed |
| YEW-F-006 | audit | `tests/audit/yew_f_006.c` | ~~workspace re-emission drops unknown root and workspace keys~~ — fixed in `9e829cd9` | fixed |
| YEW-F-007 | audit | `tests/audit/yew_f_007.c` | ~~workspace restore reorders group members from tab-array order~~ — fixed in `bea3990b` | fixed |
| YEW-F-008 | audit | `tests/audit/yew_f_008.c` | ~~unprivileged plugin macro replay inherits config authority~~ — fixed in `8995bb1f` | fixed |
| YEW-F-009 | audit | `tests/audit/yew_f_009.c` | ~~recorder folding self-test no longer reaches its injected fault~~ — fixed in `1c11ebee` | fixed |
| YEW-F-010 | audit | `tests/audit/yew_f_010.c` | ~~macro store accepts source that fails on first replay~~ — fixed in `c362cefd` | fixed |
| YEW-F-011 | audit | `tests/audit/yew_f_011.c` | ~~matching source metadata can retain stale syntax tables~~ — fixed in `2c9c7431` | fixed |
| YEW-F-012 | audit | `tests/audit/yew_f_012.c` | ~~pending embeds occupy a canonical state tail slot~~ — fixed in `43a82533` | fixed |
| YEW-F-013 | audit | `tests/audit/yew_f_013.c` | ~~JS/TS known-wrong golden rows lack the heuristic comment~~ — fixed in `838fd7e1` | fixed |
| YEW-F-014 | audit | `tests/audit/yew_f_014.c` | ~~stripped LSP completion bypasses the module hard error~~ — fixed in `114f99fb` | fixed |
| YEW-F-015 | audit | `tests/audit/yew_f_015.c` | ~~snippet-policy grep gate matches unrelated core code~~ — fixed in `59d318cf` | fixed |
| YEW-F-016 | audit | `tests/audit/yew_f_016.c` | ~~required 1-based display edges violate the LSP +/-1 gate~~ — fixed in `07ee554b` | fixed |
| YEW-F-017 | audit | `tests/audit/yew_f_017.c` | ~~interactive rebase bypasses the Git verb and environment boundary~~ — fixed in `c96b4f91` | fixed |
| YEW-F-018 | audit | `tests/audit/yew_f_018.c` | ~~FUSS picker detail bypasses the module clock discipline~~ — fixed in `b61d329b` | fixed |
| YEW-F-019 | audit | `tests/audit/yew_f_019.c` | ~~porcelain rename test survives the required one-NUL mutation~~ — fixed in `b464953d` | fixed |
| YEW-F-020 | audit | `tests/audit/yew_f_020.c` | ~~Git formatting gate rejects legitimate display formatting~~ — fixed in `0195ed1c` | fixed |
| YEW-F-021 | audit | `tests/audit/yew_f_021.c` | ~~plugin teardown retains raw hook and ledger lengths~~ — fixed in `073489ff` | fixed |
| YEW-F-022 | audit | `tests/audit/yew_f_022.c` | ~~plugin trust wording gate rejects its required warning~~ — fixed in `4858a39c` | fixed |
| YEW-F-023 | audit | `tests/audit/yew_f_023.c` | ~~plugin commands cannot enter the recorder CMDWORD space~~ — fixed in `d2dc4ddf` | fixed |
| YEW-F-024 | audit | `tests/audit/yew_f_024.c` | ~~cross-surface XFAIL debt table stops at F004~~ — fixed in `56ce3f24` | fixed |
| YEW-F-025 | audit | `tests/audit/yew_f_025.c` | ~~script tests have no XFAIL or hard-XPASS state~~ — fixed in `da1f9cd0` | fixed |
| YEW-F-026 | audit | `tests/audit/yew_f_026.c` | ~~PTY cases have no XFAIL or hard-XPASS state~~ — fixed in `86ccb661` | fixed |
| YEW-F-027 | audit | `tests/audit/f15_ban_misses.c` | ~~Fletch format ban accepts macro-forwarded nonliteral formats~~ — fixed in `c371b5a4` | fixed |
| YEW-F-028 | audit | `tests/audit/f15_ban_misses.c` | ~~Fletch abort ban accepts macro-forwarded abort~~ — fixed in `f6c8075d` | fixed |
| YEW-F-029 | audit | `tests/audit/f15_ban_misses.c` | ~~stable-sort ban accepts macro-forwarded qsort~~ — fixed in `0e2ab552` | fixed |
| YEW-F-030 | audit | `tests/audit/f15_ban_misses.c` | ~~C11-subset ban accepts token-pasted attribute syntax~~ — fixed in `ccfc924c` | fixed |
| YEW-F-031 | audit | `tests/audit/f15_ban_misses.c` | ~~explicit-registry ban accepts token-pasted constructors~~ — fixed in `ccfc924c` | fixed |
| YEW-F-032 | audit | `tests/audit/f15_ban_misses.c` | ~~single-thread ban accepts token-pasted pthread calls~~ — fixed in `a294794c` | fixed |
| YEW-F-033 | audit | `tests/audit/f15_ban_misses.c` | ~~reproducibility ban omits `__TIMESTAMP__`~~ — fixed in `381bddf0` | fixed |
| YEW-F-034 | audit | `tests/audit/f15_ban_misses.c` | ~~mmap ban accepts macro-forwarded calls~~ — fixed in `47046a40` | fixed |
| YEW-F-035 | audit | `tests/audit/f15_ban_misses.c` | ~~allocator ban accepts macro-forwarded libc allocation~~ — fixed in `066041c3` | fixed |
| YEW-F-036 | audit | `tests/audit/f15_ban_misses.c` | ~~cwd-allocation ban requires literal NULL spelling~~ — fixed in `16441f04` | fixed |
| YEW-F-037 | audit | `tests/audit/f15_ban_misses.c` | ~~realpath-allocation ban requires literal NULL spelling~~ — fixed in `16441f04` | fixed |
| YEW-F-038 | audit | `tests/audit/f15_ban_misses.c` | ~~locale-dependent Unicode ban omits `mbtowc`~~ — fixed in `af94a8c1` | fixed |
| YEW-F-039 | audit | `tests/audit/f15_ban_misses.c` | ~~native-loader ban omits `dlvsym`~~ — fixed in `f09c70e0` | fixed |
| YEW-F-040 | audit | `tests/audit/f15_ban_misses.c` | strerror_r ban accepts macro-forwarded calls | open |
| YEW-F-041 | audit | `tests/audit/f15_ban_misses.c` | musl backtrace ban omits `backtrace_symbols_fd` | open |
| YEW-F-042 | audit | `tests/audit/f15_ban_misses.c` | GNU-libc ban omits `getopt_long_only` | open |
| YEW-F-043 | audit | `tests/audit/f15_ban_misses.c` | long-double ban misses valid continued declarations | open |
| YEW-F-044 | audit | `tests/audit/f15_ban_misses.c` | shim-honesty gate accepts parenthesized success | open |
| YEW-F-045 | audit | `tests/audit/f15_ban_misses.c` | Unicode-width ban accepts decimal local tables | open |
| YEW-F-046 | audit | `tests/audit/f15_ban_misses.c` | syntax-color ban accepts packed decimal colors | open |
| YEW-F-047 | audit | `tests/audit/f15_ban_misses.c` | syntax-width ban accepts local width arithmetic | open |
| YEW-F-048 | audit | `tests/audit/f15_ban_misses.c` | PTY-creation ban omits direct `posix_openpt` callers | open |
| YEW-F-049 | audit | `tests/audit/f15_ban_misses.c` | CI golden-update ban depends on contiguous spelling | open |
| YEW-F-050 | audit | `tests/audit/f15_ban_misses.c` | piece-tree I/O ban omits `pread` | open |
| YEW-F-051 | audit | `tests/audit/f15_ban_misses.c` | shadow-preview ban accepts manual destructive fill | open |
| YEW-F-052 | audit | `tests/audit/f15_ban_misses.c` | FUSS drawer ban accepts indirect pane-root replacement | open |
| YEW-F-053 | audit | `tests/audit/f15_ban_misses.c` | deterministic-fuzz ban omits `random` | open |
| YEW-F-054 | audit | `tests/audit/f15_ban_misses.c` | clipboard shell ban omits direct shell exec | open |
| YEW-F-055 | audit | `tests/audit/f15_ban_misses.c` | job-interpolation ban accepts raw append into shell text | open |
| YEW-F-056 | audit | `tests/audit/f15_ban_misses.c` | OSC 52 query ban accepts split string literals | open |
| YEW-F-057 | audit | `tests/audit/f15_ban_misses.c` | terminal-syscall ban omits `tcflush` | open |
| YEW-F-058 | audit | `tests/audit/f15_ban_misses.c` | register choke-point ban accepts allowed-file wrappers | open |
| YEW-F-059 | audit | `tests/audit/f15_ban_misses.c` | option choke-point ban accepts allowed-file wrappers | open |
| YEW-F-060 | audit | `tests/audit/f15_ban_misses.c` | package-git ban accepts allowed-file wrappers on startup | open |
| YEW-F-061 | audit | `tests/audit/f15_ban_misses.c` | register-width ban accepts local lookup tables | open |
| YEW-F-062 | audit | `tests/audit/f15_ban_misses.c` | register-column ban depends on historical variable names | open |
| YEW-F-063 | audit | `tests/audit/f15_ban_misses.c` | register-helper presence gate accepts comments | open |
| YEW-F-064 | audit | `tests/audit/f15_ban_misses.c` | oracle-independence ban accepts copied renamed models | open |
| YEW-F-065 | audit | `tests/audit/f15_ban_misses.c` | generated-table ban verifies only a retained marker | open |
| YEW-F-066 | audit | `tests/audit/f15_ban_misses.c` | termination-site ban omits `_Exit` | open |
| YEW-F-067 | audit | `tests/audit/f15_ban_misses.c` | AI-body logging ban depends on variable names | open |
| YEW-F-068 | audit | `tests/audit/f15_ban_misses.c` | unit-registry ban omits static test definitions | open |
| YEW-F-069 | audit | `tests/audit/f15_ban_misses.c` | PTY minimum-case gate skips a missing registry | open |
| YEW-F-070 | audit | `tests/audit/f15_ban_misses.c` | PTY golden gate accepts computed missing names | open |
| YEW-F-071 | audit | `tests/audit/f15_ban_misses.c` | PTY orphan gate counts dead preprocessor rows | open |
| YEW-F-072 | audit | `tests/audit/yew_f_072.c` | designated performance evidence remains placeholder-only | open |
| YEW-F-073 | audit | `tests/audit/yew_f_073.c` | baseline history policy is not enforced | open |
| YEW-F-074 | audit | `tests/audit/yew_f_074.c` | ~~Darwin shipping clean rebuilds differ by Mach-O UUID~~ — fixed in `16761aba` | fixed |
| YEW-F-075 | audit | `tests/audit/yew_f_075.c` | ~~stripped builds accept module-only config as inert state~~ — fixed in `ee6f9894` | fixed |
| YEW-F-076 | audit | `tests/audit/yew_f_076.c` | accepted unsaved undo sidecars are not byte-canonical | open |
| YEW-F-077 | audit | `tests/audit/yew_f_077.c` | rectangular yank omits required short-row padding | open |
| YEW-F-078 | audit | `tests/audit/yew_f_078.c` | crash journal admits a same-metadata replacement inode | open |
| YEW-F-079 | audit | `tests/audit/yew_f_079.c` | ~~workspace re-emission drops unknown entity-record fields~~ — fixed in `9222b491` | fixed |

The Sprint 33 conformance-only ledger remains at
`tests/fletch/xfail-debt.txt` until the cross-surface runner migration lands.
That historical file is also empty.
