# Sprint 11: Buffer Torture — Differential Fuzz, Properties, Budgets

## Prerequisites

- Sprint 7 — `TextBuf`, `yew_textbuf_insert/delete`, `line_start/line_of/
  line_span`, `TextIter`, `TextSnap`, `yew_textbuf_check` (the structural
  invariant checker this sprint calls after every op), and the smoke-scale
  differential test in `tests/unit/test_piece_diff.c` that this sprint
  replaces with a real campaign.
- Sprint 8 — `yew_file_load`/`yew_file_save`, `FileMeta`, the fixture corpus
  and `tests/torture/` directory (created there; this sprint adds to it).
- Sprint 9 — `Cursor`, `CursorSet`, `MarkSet`, `yew_is_grapheme_boundary`
  (property P12), the unicode golden corpus under
  `tests/unit/fixtures/unicode/`.
- Sprint 10 — `EditCtx`, `yew_edit_insert/delete`, the undo tree and its
  navigation API (properties P6–P8), `yew_undo_dump`.
- Sprint 1 — unit harness, `make test`, CI lanes (gcc/clang/ASan/UBSan/
  determinism); Sprint 0 — `Bytebuf`, `Vec(T)`, `yew_log`, `YEW_BUG`.
- Binding: `00-decisions.md` **Quality bars** table (100 MiB open ≤ 150 ms;
  memory ≤ 1.6× file size for a clean open) and invariants 1, 2, 4, 5.
- Assumption beyond index entries: s01's CI has a *perf lane on a designated
  runner* (`01-architecture.md` names it). If the runner label does not
  exist yet, this sprint adds it as `perf-x86_64-linux-gnu` and the gate is
  advisory on every other runner (§8).

## Goals

This is where the text engine earns the right to be built on. Land a
differential fuzzer that drives `TextBuf` and a deliberately naive
array-of-lines oracle through the same op stream and proves them equal, with
a trace format, a shrinker, and a rule that every failure becomes a committed
regression test; a property suite naming and checking twelve algebraic laws
of the buffer (splice inverse, iterator ≡ materialize, line index ≡ full
recount, undo–redo identity, coalescing bounds); a large-file budget suite
over 100 MiB and 1 GiB fixtures generated deterministically by a script and
**never committed**; and committed perf baselines with a regression-gate
policy that will not flap itself into being disabled. Reference-hardware
calibration of the absolute budgets is Sprint 56 — this sprint installs the
harness, the baselines, and the gate mechanism, and pins the 1 GiB figures
that `00-decisions.md` does not name. No new `src/` surface ships here
beyond `scripts/` and `tests/`.

## Deliverables

### 1. The oracle — `tests/fuzz/oracle.h`, `tests/fuzz/oracle.c`

```c
typedef struct Oracle {
    Vec(Bytebuf) lines;   /* each line INCLUDES its EOL bytes;           */
                          /* the last element may lack them (no final NL)*/
} Oracle;

void oracle_init(Oracle *o, const u8 *bytes, u64 len);
void oracle_insert(Oracle *o, u64 at, const u8 *bytes, u64 len);
void oracle_delete(Oracle *o, u64 lo, u64 hi);
u64  oracle_len(const Oracle *o);
u64  oracle_line_count(const Oracle *o);
u64  oracle_line_start(const Oracle *o, u64 line);
u64  oracle_line_of(const Oracle *o, u64 off);
void oracle_materialize(const Oracle *o, Bytebuf *out);
```

Implementation rule, binding: **the oracle is optimized for obviousness and
nothing else.** `oracle_insert` materializes the whole buffer into one flat
`Bytebuf`, `memmove`s, and re-splits on `'\n'` from scratch. O(n) per op is a
feature — it is the reason a reviewer can read it in ninety seconds and
believe it. Line count is `lines.len` (with the empty-buffer special case
documented in a comment, since "how many lines does a 0-byte file have"
is exactly the kind of question a differential test exists to settle: **1**,
matching s07's `sub_lfs(root)+1`).

**Pitfall — no shared code.** The oracle must not call, include, or
copy-paste one line of `src/text/piece.c` or its helpers, and must not use
`yew_textbuf_*` for any purpose including setup. A helper shared between
subject and oracle makes its bugs invisible to the comparison. The only
shared code is `Bytebuf`/`Vec` from `src/util/`, which s00 already unit-tests
independently. CI grep gate in DoD 12.

### 2. Operation generator — `tests/fuzz/fuzz_textbuf.c`

PRNG: a `xorshift64*` implemented in the test file. **Never `rand(3)`** —
glibc and musl produce different sequences from the same seed, so a CI
failure on one lane would be unreproducible on the other (same determinism
doctrine as s00's ban on unstable sort). Seed from `YEW_FUZZ_SEED` (hex),
default a fixed set of four seeds in CI.

Weighted op mix (weights are out of 100; `YEW_FUZZ_MIX=<name>` selects an
alternate profile — `typing`, `paste`, `undo`, `lines`):

| Op | W | Notes |
|---|---|---|
| `ins` small, 1–8 B | 26 | the typing case; drives s07 coalescing |
| `ins` medium, 9 B–4 KiB | 11 | |
| `ins` large, 4 KiB–1 MiB | 2 | paste; also stresses the add store |
| `del` small, 1–8 B | 17 | backspace |
| `del` range, up to 64 KiB | 9 | spans many pieces (s07's `del_rec`) |
| `del` all | 1 | empty-buffer transitions both ways |
| `line_start` / `line_of` / `line_span` query | 10 | checked immediately |
| `iter` + partial materialize compare | 5 | from a random start offset |
| `snap` / `release` | 4 | interleaved lifetimes, up to 4 live snapshots |
| `undo` | 5 | |
| `redo` | 3 | |
| `undo_boundary` | 2 | forces s10 node splits mid-burst |
| `undo_to` random live node | 2 | branch navigation |
| `save` + reload round-trip | 1 | via a temp file; s08 path in the loop |
| `check` (explicit `yew_textbuf_check`) | 2 | |

Position distribution, applied to every op that needs an offset:

| Class | W | Definition |
|---|---|---|
| uniform | 40 | `rand % (len+1)` |
| piece seam | 25 | an exact boundary between two pieces (queried from a debug accessor `yew_textbuf_seam_at(tb, k)`) — s07's `at == lbytes` pitfall lives here |
| extremes | 15 | 0, `len`, `len-1`, 1 |
| line edges | 10 | a random `line_start` or `line_span().hi` |
| locality | 10 | the previous op's end offset ± 0–3 (drives the coalescing fast path and its failure modes) |

Offsets are **raw byte offsets, never snapped to a grapheme or codepoint
boundary** — `TextBuf` is byte-oriented by design (s07) and must survive an
insert in the middle of a UTF-8 sequence with byte-exact round-tripping.

Content classes for insert payloads:

| Class | W | Content |
|---|---|---|
| ASCII | 30 | printable + tabs |
| UTF-8 multi-byte | 18 | valid 2/3/4-byte sequences, uniform over planes |
| grapheme clusters | 9 | ZWJ families, RI flag pairs, skin tones, Hangul jamo, keycaps — drawn from the s02/s09 golden corpus |
| **invalid UTF-8** | 10 | lone continuation `0x80`, overlong `C0 80`, truncated `E2 82`, `0xFE`/`0xFF`, CESU/surrogate encodings `ED A0 80` |
| sequence splitter | 5 | inserts *between* the bytes of an existing multi-byte sequence (offset chosen by scanning for a continuation byte) |
| NUL / binary | 5 | includes `\0` runs; s08's binary path must not care |
| newline-dense | 8 | `'\n'` every 1–3 bytes — stresses the `lfs` index |
| **huge line** | 5 | 64 KiB–8 MiB with **zero** newlines |
| CRLF | 10 | `\r\n` runs, plus lone `\r` and split `\r`/`\n` inserts |

Trace format — text, one op per line, hand-editable (this is what the
shrinker emits and what regression fixtures store):

```
# yew textbuf trace v1
# seed=0x9e3779b97f4a7c15 mix=typing base=empty
ins 0 5 68656c6c6f
ins 5 1 0a
del 2 4
line_of 3
undo
snap 0
undo_to 7
release 0
```

`base=` is `empty`, `fixture:<path>`, or `gen:<len>:<seed>`. Payloads are
lowercase hex, no separators. A trace is replayable by
`build/fuzz_textbuf --replay <file>`, which is also how CI reproduces a
nightly failure.

### 3. Equivalence checking — tiered

Checking everything after every op makes each op O(n) and the fuzzer never
reaches a deep, interesting state. Tiers:

| Check | Cadence |
|---|---|
| `yew_textbuf_len == oracle_len` | every op |
| `yew_textbuf_line_count == oracle_line_count` | every op |
| `yew_textbuf_check(tb)` (s07 structural invariants) | every op |
| `line_start`/`line_of` agreement on 8 random arguments | every op |
| piece count ≤ `2·ops_so_far + 1` | every op |
| full materialize `memcmp` vs oracle | every 32 ops |
| chunk-iterator concatenation ≡ materialize | every 32 ops |
| every line's `line_span` vs oracle, all lines | every 512 ops, or every op while `len < 64 KiB` |
| cursor/mark invariants (P12) | every op |
| **all of the above** | after the final op, always |

**On any mismatch, do not report immediately.** Re-run the trace from op 0
with every check at every-op cadence (deterministic replay makes this exact),
find the *first* divergent op, and report that index — a mismatch discovered
at op 8 192 is usually caused thousands of ops earlier, and shrinking from
the wrong index converges on the wrong bug.

### 4. Shrinking — `tests/fuzz/shrink.c`

`bool shrink(Trace *t, FailurePred p)` where the predicate is **the specific
failure**: `{first_divergent_op_index, check_id}` or `{assertion file:line}`
for a `YEW_BUG`/sanitizer abort. **Pitfall:** a predicate of "the process
died" lets the shrinker wander onto a different bug — usually a segfault from
a nonsense trace — and the minimized case then explains nothing.

Passes, run to fixpoint or until the budget (10 s wall / 5 000 replays,
whichever first) expires:

1. **ddmin bisect** — try the trace's second half, then first half, then
   quarters, keeping any subset that still fails.
2. **Greedy op removal** — one forward sweep, drop each op, keep the drop if
   the failure survives. Offsets of later ops are *not* rewritten; an op
   whose offset is now out of range is clamped, and the pass retries once
   with the un-clamped variant.
3. **Per-op simplification**, each accepted only if the failure persists:
   payload → all `'a'`; payload length → 1; offset → toward 0 (binary
   search); content class → ASCII; large op → small op; `undo_to` → `undo`.
4. **Deduplicate** identical adjacent ops.

Output: the minimized trace, plus a C snippet ready to paste into
`tests/unit/test_piece_regress.c`. Binding rule: **every fuzz failure lands
as a committed unit test in the same PR as its fix.** Regression cases are
code in a file a human reads, not opaque blobs in a corpus directory — the
corpus directory holds only the raw traces of *open* findings.

### 5. Property suite — `tests/unit/test_textbuf_props.c`

Each property is a named, seeded generator plus an assertion. `YEW_PROP_N`
(default 500 per property in `make test`, 20 000 in the nightly lane) sets
the case count.

| # | Property | Statement |
|---|---|---|
| P1 | splice inverse | ∀(off, bytes): `insert(off,b)` then `delete([off,off+|b|))` restores byte-identical content, identical `line_count`, and piece count ≤ start + 2 |
| P2 | delete inverse | ∀ range r: `d = read(r); delete(r); insert(r.lo, d)` restores byte-identical content |
| P3 | iterator ≡ materialize | concatenating every chunk from `begin(0)` equals `materialize()`; from `begin(k)` equals the suffix at k, ∀k in a sampled set incl. every seam |
| P4 | line index ≡ full recount | `line_count() == 1 + count(0x0A in materialize())`; `line_start(l)` equals the l-th recomputed start ∀l |
| P5 | line round-trip | `line_of(line_start(l)) == l` ∀l; `line_start(line_of(o)) <= o < line_start(line_of(o)+1)` ∀o (last line: `< len+1`) |
| P6 | **undo–redo identity** | after any op sequence, N `undo` then N `redo` gives byte-identical content, identical `line_count`, identical cursor `pos`/`anchor`/`goal`, and `yew_undo_current` back to the same node id |
| P7 | undo to root | undoing to the root node reproduces the load-time content byte-exactly |
| P8 | branch preservation | edit A, undo, edit B: A's node is live and `yew_undo_to(A)` reproduces A's content byte-exactly; repeated for 8-deep, 3-wide random trees |
| P9 | coalescing invariants | k sequential adjacent 1-byte inserts with no intervening op → piece-count delta ≤ 2; the same k inserts at scattered offsets → delta ≥ k−1; **no zero-length piece ever exists**; every `sub_bytes`/`sub_lfs`/`sub_count` equals recomputation; piece count ≤ 2·ops+1 |
| P10 | snapshot immutability | a `TextSnap`'s content is invariant under any number of subsequent edits; both release orders leak-free (valgrind lane) |
| P11 | save/load round-trip | `materialize → save → load → materialize` is byte-identical for every generated content class, including invalid UTF-8, NULs, CRLF, and no-final-newline (this is invariant 1's property face) |
| P12 | cursor/mark invariants | after every op, every mark is in `[0, len]` and every cursor `pos` satisfies `yew_is_grapheme_boundary` (s09) |

P9's `2·ops+1` bound is the structural one: each insert splits at most one
piece into two and adds one; each delete splits at most two. State it in the
test's comment so a future optimization that *lowers* it does not read as a
violation.

### 6. Large-file fixtures — `scripts/gen-bigfile.c`, `make fixtures`

A C generator, not a shell script: 1 GiB of shell `printf` is minutes, and
shell is a portability and determinism hazard (locale-dependent formatting).
Built to `build/gen-bigfile`; fixtures land in `build/fixtures/` and are
**never committed** (`.gitignore`: `build/`). Generation is cached by a
`<name>-<size>-<seed>.stamp` file so the nightly lane regenerates once.

Determinism contract: `xorshift64*` from the given seed, byte-identical
output for the same `(profile, size, seed)` on every target and libc; no
`rand`, no `time`, no locale-sensitive formatting, no filesystem order
dependence. `make fixtures-verify` hashes each fixture against a committed
FNV-64 in `tests/perf/fixtures.sha` — the fixtures are not committed, but
their *hashes* are, so a generator regression is caught immediately.

| Profile | Size | Shape |
|---|---|---|
| `100m-code` | 100 MiB | ~64 B/line ASCII, source-like indentation, ~1.6 M lines |
| `100m-utf8` | 100 MiB | 30 % multi-byte, ZWJ/RI clusters, mixed CRLF, 1 % invalid bytes |
| `100m-allnl` | 100 MiB | every byte is `'\n'` — the `lfs` worst case (see §7 note) |
| `1g-code` | 1 GiB | ~64 B/line, ~16 M lines |
| `1g-longlines` | 1 GiB | 4 096 lines of 256 KiB each |
| `1g-noline` | 1 GiB | **zero** newlines — one line; worst case for line ops and for any accidental scan |

### 7. Budget suite — `tests/perf/perf_textbuf.c`, `make perf` / `make perf-huge`

Timing: `CLOCK_MONOTONIC`, 3 warm-up iterations discarded, then N samples;
report p50/p99/max in ns. RSS: `/proc/self/statm` × page size on Linux;
`getrusage(RUSAGE_SELF).ru_maxrss` elsewhere. **Pitfall:** `ru_maxrss` is
KiB on Linux and **bytes** on macOS — normalize in one function with the
`#ifdef` confined to it (same single-site discipline as Cgfried's host
sniff), or the arm64-macos lane reports a 1 000× regression on day one.

| Metric | Fixture | Gate |
|---|---|---|
| open → interactive (load + line index) | `100m-code` | ≤ **150 ms** (locked, `00-decisions.md`) |
| open → interactive | `100m-utf8` | ≤ 150 ms |
| open → interactive | `1g-code` | ≤ **2.0 s** (pinned here; proportional to the locked 100 MiB figure with headroom; s56 recalibrates) |
| open → interactive | `1g-noline` | ≤ 2.0 s — proves no code path scans for lines linearly |
| RSS after clean open | `100m-code` | ≤ **160 MiB** (1.6× locked) |
| RSS after clean open | `1g-code` | ≤ 1.6 GiB |
| RSS after clean open | `100m-allnl` | **informational, no gate** — s07 documented this degenerate case as out of the 1.6× budget (8 B of `lfs` per 1 B line). The number is recorded in the baseline so a *regression* is visible even though the absolute value is exempt. Revisited only in Sprint 56, per s07. |
| insert 1 B at offset 0, p99 over 10 000 ops | `1g-code` | ≤ 50 µs |
| insert 1 B at EOF, p99 over 10 000 ops | `1g-code` | ≤ 50 µs |
| edit-at-both-ends: 10 000 alternating head/tail 1 B inserts | `1g-code` | total ≤ 1.0 s; RSS growth ≤ 8 MiB |
| `line_start(random)`, p99 over 100 000 | `1g-code` | ≤ 5 µs |
| `line_of(random)`, p99 over 100 000 | `1g-code` | ≤ 5 µs |
| `line_start(random)`, p99 | `1g-noline` | ≤ 5 µs |
| full materialize throughput | `1g-code` | ≥ 1.0 GiB/s |
| atomic save of a dirty buffer (incl. fsync) | `1g-code` | ≤ 6.0 s, on the runner's real filesystem — never tmpfs, since fsync on tmpfs measures nothing |
| 10 000 undo transactions then 10 000 undos | `100m-code` | ≤ 400 ms total; `UndoTree` bytes ≤ `undo.bytes_max` |

`make perf` runs the 100 MiB set (every PR, designated runner).
`make perf-huge` runs the 1 GiB set (nightly + release gate) — 4 GiB of
generated fixtures and multi-second measurements do not belong in a PR lane.

### 8. Committed baselines and the regression gate

Baselines live at `tests/perf/baselines/<runner-id>.txt`, committed:

```
# yew perf baseline v1  runner=perf-x86_64-linux-gnu
# metric                        p50_ns        p99_ns      rss_bytes
open.100m-code               118400000     131900000      148897792
insert.head.1g-code              18200         41300              0
```

Gate policy — designed so it will not be turned off:

1. **Absolute budget violation is always a hard failure**, on every runner,
   advisory mode or not. Budgets are CI gates (invariant 4).
2. **Relative regression**: p99 more than **10 %** above the baseline fails —
   but only on the designated runner. Elsewhere the same condition emits a
   warning (`YEW_PERF_ADVISORY=1` is set for non-designated runners).
3. **Noise policy**: each metric is measured 3 times per run; the *median*
   of the three p99s is compared. A metric must exceed the threshold in
   **2 of 3** runs to fail. Single-sample perf gates flap, get muted, and
   then the project has no perf gate at all — which is strictly worse.
4. **Improvements do not auto-update.** A p99 more than 20 % *below* baseline
   emits a "rebaseline me" notice; the number only moves when a human moves
   it.
5. **Rebaselining is its own commit.** `make perf-update` rewrites the file;
   the commit message starts `perf: rebaseline` and lists the metric deltas.
   CI's `perf-baseline-guard` fails any commit that touches both
   `tests/perf/baselines/` and `src/` — so a performance regression can
   never be laundered through the same diff that caused it.
6. Baseline files are per runner id; an unknown runner id runs advisory-only
   and prints the line it would have committed.

### 9. Fuzz campaign operation

- `make fuzz` (every PR): 4 fixed seeds × 200 000 ops × 3 mixes, ~60 s
  total, under ASan+UBSan. Fixed seeds mean the PR lane is reproducible;
  the nightly lane is where randomness lives.
- `make fuzz-long` (nightly): random seeds, 30 minutes wall, all mixes,
  ASan+UBSan; any failure writes `tests/fuzz/crashes/<seed>-<idx>.trace`,
  runs the shrinker, and attaches the minimized trace to the CI artifact.
- Corpus hygiene: `tests/fuzz/crashes/` holds only *open* findings. A fixed
  finding's minimized trace is deleted from there and its C form lives in
  `tests/unit/test_piece_regress.c` forever.
- Optional, non-gating: `make fuzz-libfuzzer` builds an
  `-fsanitize=fuzzer` entry point (clang only) whose `LLVMFuzzerTestOneInput`
  interprets the input bytes as a PRNG seed stream for the same generator.
  Not a dependency and not a CI gate — the seeded generator is the fuzzer.

### 10. Defer

- Reference-hardware calibration of every budget, and promoting the 1 GiB
  numbers from "pinned with headroom" to "measured on the reference box" →
  Sprint 56; this sprint owns the harness and the mechanism. Render/scroll/
  keypress latency gates → 56 (no renderer is wired to a buffer yet);
  binary-size budgets → 57.
- kill -9 / fault-injection torture is **s08's** and already in CI; this
  sprint does not duplicate it, it only adds P11 so the fuzzer exercises the
  save path under random content.
- Regex, Fletch, input-parser, and syntax fuzzers → Sprints 20, 29–32, 4, 39;
  the trace/shrink/regression-commit discipline established here is the
  template each reuses.
- Multi-buffer and concurrent-view stress → Sprint 22+; single `TextBuf`
  only here (invariant 8: no threads).

## Testing Strategy

- **Unit** (`test_oracle.c`): the oracle is itself tested — 40 hand-written
  cases against literal expected strings, including empty buffer, no final
  newline, a delete spanning the final newline, insert at `len`, all-newline
  content, and a 0-byte file's line count. A wrong oracle turns a
  differential suite into theatre.
- **Unit** (`test_textbuf_props.c`): P1–P12, `YEW_PROP_N` cases each, four
  seeds. **Unit** (`test_piece_regress.c`): the growing file of minimized,
  formerly-failing traces as explicit C — starts with whatever the campaign
  finds and never shrinks.
- **Fuzz** (`fuzz_textbuf.c`): the §2/§3 campaign; `--replay` proven by
  replaying a committed trace to a byte-identical `yew_undo_dump` and
  materialization.
- **Shrinker self-test** (`test_shrink.c`): a synthetic predicate ("fails iff
  op #k is a delete of length ≥ 7 after ≥ 3 inserts") planted in a 5 000-op
  trace — the shrinker returns ≤ 5 ops still satisfying it.
- **Perf**: `make perf` on the designated runner, `make perf-huge` nightly.
- **Determinism**: one seed gives a byte-identical trace file and final
  materialization across gcc/clang and across two runs; `gen-bigfile`
  produces byte-identical fixtures on both lanes (`make fixtures-verify`).
- **Sanitizers**: the whole fuzz and property suite under ASan and UBSan in
  the PR lane; valgrind runs the properties at `YEW_PROP_N=50` (snapshot
  refcounting and undo blob compaction are the leak surfaces).

## Definition of Done

1. `make test`, `make fuzz`, `make perf` green under gcc and clang, zero
   warnings at `-std=c11 -pedantic -Wall -Wextra -Werror`.
2. Differential campaign: 4 seeds × 200 000 ops × 3 mixes with zero
   divergences; the every-op tier (`len`, `line_count`,
   `yew_textbuf_check`, piece bound) runs on all 200 000.
3. All 12 properties implemented and green at `YEW_PROP_N=500` × 4 seeds;
   P6 (undo–redo identity) and P8 (branch preservation) additionally green
   at `YEW_PROP_N=20 000` in the nightly lane.
4. Invalid UTF-8, NUL, CRLF, sequence-splitting, and huge-line content
   classes are each provably exercised (per-class op counters printed at
   the end of a run and asserted non-zero) and round-trip byte-exactly
   through P11 — invariant 2 held under fuzz.
5. A deliberately introduced bug in `piece.c` (an off-by-one at the
   `at == lbytes` seam, applied in a scratch commit) is caught by the
   campaign within 5 000 ops on all four seeds — the suite is proven to
   have teeth, not just to be green.
6. Shrinker self-test reduces a 5 000-op trace to ≤ 5 ops; the shrinker's
   output replays to the same failure.
7. `gen-bigfile` output is byte-identical across gcc/clang lanes and two
   runs for all 6 profiles; `make fixtures-verify` green; `git status` is
   clean after `make fixtures` (nothing generated outside `build/`).
8. Every §7 gated metric is measured and within budget on the designated
   runner: 100 MiB open ≤ 150 ms, 1 GiB open ≤ 2.0 s, RSS ≤ 1.6× on both
   code fixtures, head/tail insert p99 ≤ 50 µs on 1 GiB, `line_start` p99
   ≤ 5 µs on `1g-code` **and** `1g-noline`.
9. Edit-at-both-ends on 1 GiB: 10 000 alternating inserts in ≤ 1.0 s with
   ≤ 8 MiB RSS growth.
10. `100m-allnl` RSS is recorded in the baseline as informational with the
    s07 exemption cited in a comment; it does not fail the build.
11. Baselines committed for the designated runner; the gate fires on a
    deliberately slowed build (scratch commit adding a sleep) and does not
    fire across 5 consecutive unmodified runs (no flapping);
    `perf-baseline-guard` rejects a mixed `src/` + baseline commit.
12. Grep gate: `grep -rn 'yew_textbuf_\|piece\.h' tests/fuzz/oracle.c` →
    nothing; `grep -rn '\brand\b\|srand\|time(NULL)' tests/fuzz scripts/` →
    nothing.
13. ASan/UBSan/valgrind/determinism lanes green including the fuzz and
    property binaries.
14. ≥ 150 assertions across `test_oracle.c`, `test_textbuf_props.c`,
    `test_shrink.c`, and `test_piece_regress.c`.
