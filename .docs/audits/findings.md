# Sprint 58 findings ledger

Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`  
Next available ID: `YEW-F-005`

IDs are assigned only after a reproducer fails at the fixed baseline. They
are never reused, renumbered, or deleted. Resolution changes status and keeps
the historical row and body.

| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-001 | M | open | F01 UNI | ambiguous-wide doubles fixed-cell chrome glyphs | tests/audit/yew_f_001.c | s27 §7 |
| YEW-F-002 | M | open | F01 UNI | long RI output delays a completed flag cluster | tests/audit/yew_f_002.c | s19 §3 |
| YEW-F-003 | H | open | F01 UNI | ASCII-base keycap leaves inconsistent grid width | tests/audit/yew_f_003.c | s05 §3 |
| YEW-F-004 | M | open | F04 MODAL | full Fletch parser rejects bare dotted map keys | tests/audit/yew_f_004.c | spec §2 `entry` |

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

## Unverified observations

Observations live in their front files. They have no IDs and are excluded
from every total.
