# Sprint 57: Size Budgets, the Allocation Audit, and the musl Embedded Profile

## Prerequisites

- Sprint 0 — the one GNU Makefile and its `MODULES` plumbing verbatim:
  `KNOWN_MODS := lsp ai fuss plugins`, `MODDIR_lsp/ai/fuss/plugins :=
  lsp/ai/git/plug`, the `-DYEW_WITH_*` defines, `$(BUILD)/mods.stamp` and
  its every-object dependency, the **shim convention** (enabled → real
  sources minus `shim.c`; disabled → `shim.c` only; never both), the module
  registry (`YewMod`, `yew_mod_enabled/name/require`) and its **canonical
  message** `this build has no <name> module; rebuild with 'make
  MODULES="… <name>"'`. Also `yew_xmalloc/xrealloc/xcalloc` (OOM →
  `YEW_BUG`), `Arena`, `Bytebuf`, `yew_sort_stable`, the `--version`
  `modules:` line, and the sorted-`find` link-order rule.
- Sprint 1 — `scripts/bans.sh` and its exception discipline, the CI lane
  set, the `modules` lane already building `MODULES=""` and smoke-testing
  `modules: none`, and the determinism lane's **byte-identical clean
  rebuild** check (which is why `__DATE__`, `__TIME__` and absolute-path
  `-D` flags are banned).
- Sprint 2 — the locale/wide-char ban (`wcwidth`, `wcswidth`, `mbrtowc`,
  `wchar.h`, `setlocale`, `iconv` over `src/`) that this sprint's glibc-ism
  audit builds directly on; `src/unicode/tables.c` as the
  generated-and-committed precedent for a checked-in generated file.
- Sprint 6 — the pty harness (no `libutil`, no `forkpty`). Sprint 37 —
  `yew --batch` and `tests/script/`, the only way to drive an editor inside
  a 64 MiB QEMU guest. Sprint 11 — `gen-bigfile`, `make fixtures`,
  `fixtures.sha`, the baseline and gate policy.
- **Sprint 56** — `yew_rss_bytes`/`yew_rss_peak_bytes` and their
  single-`#ifdef` rule, `tests/perf/baselines/`, `perf-baseline-guard`, the
  designated-runner scheme, the calibration limits, and `make size`
  hard-erroring and naming this sprint.
- Sprint 25 — the state format's **integer-only** value model
  (`ratio_permille: i64`, no floats in v1) and its reason: `%f` honours
  `LC_NUMERIC`. Sprint 45 — `json.c`'s `strtod` comment, legal only because
  `setlocale` is never called. Sprint 48 — the bespoke HTTP/1.1 **localhost**
  client (`MODULES=ai`). Sprints 39–42 (syntax + 13 langpacks), 28–38
  (Fletch), 22–26 (panes/tabs/workspace/finder) — all **core**, not modules.
- **Assumptions beyond earlier index entries**: (1) each module directory
  ships exactly one public header consumed by core
  (`src/mod/lsp/lsp.h`, `src/mod/ai/ai.h`, `src/mod/git/git.h`,
  `src/mod/plug/plug.h`); if a module landed several, §6's parity check
  reads all headers in the directory and nothing else changes. (2)
  `runtime/` (default `init.fl`, syntax defs, themes, tutor) is discovered
  on disk at a compiled-in prefix; §2.4 adds an *optional* embedded form for
  the single-file profile and does not change the default.
- Binding: `00-decisions.md` — the **Targets** row (closed set:
  `x86_64-linux-gnu`, `arm64-linux`, `x86_64-linux-musl` static embedded,
  `arm64-macos`), the **Feature modules** row (core is not excisable;
  excluded modules hard-error, never silently no-op), the binary-size and
  memory quality bars, and invariants 3, 5, 7, 8.

## Goals

Close the size and portability half of Campaign 13. Land binary-size budgets
per `MODULES` configuration as CI gates, with a bespoke per-module size
ledger over `nm`/`size` output that tracks where every kilobyte lives and
fails when a module's `.text` grows past tolerance. Land the allocation
audit: an arena-discipline checklist per subsystem, a debug allocator that
counts allocations per call site so a per-keystroke allocation has a *name*
rather than a symptom, and peak-RSS gates per fixture. Land the
`x86_64-linux-musl` static embedded profile — pinned toolchain, an audit
table of glibc-isms that must not appear, static-PIE linking, section
garbage collection and `strip` — and **prove it on a constrained target**
with a pinned reference machine and a hard RAM ceiling. Verify the
minimal-build feature matrix: `MODULES=""` must still be a complete modal
editor with Fletch, highlighting, workspaces and macros, the excision
boundaries must hold symbol-for-symbol, and every hard-error shim must name
its own module honestly. Finally take the `arm64-linux` and `arm64-macos`
lanes green. **MILESTONE: embedded proof.** Adversarial audits are Sprint
58; packaging and reproducible release builds are Sprint 60.

**Core-preservation stop rule.** Footprint work may remove unreachable code,
fix accidental retention, or improve representation without changing observable
behavior. It may not excise core features, narrow the supported feature matrix,
weaken an invariant, or trade away correctness, recovery, determinism, or the
locked latency bars. If a measured floor shows that a footprint gate cannot be
met under those constraints, stop implementation, record the measurement and
cause, and amend or defer the gate explicitly before continuing. S57-A1 is the
first application of this rule; a green size lane is never evidence that a
feature regression is acceptable.

## Deliverables

### 1. Binary-size budgets per `MODULES` — `make size`, `scripts/size.sh`

**The measurement is pinned before the budget**, because "binary size" has
four defensible definitions and a gate needs exactly one:

> The size of the **stripped** file on disk, `stat -c %s`, from a
> `-O2` build with the shipping flag set, on `x86_64-linux-gnu` with glibc,
> built by the CI gcc lane's compiler.

`size -A -d` section sums are recorded alongside (they exclude ELF headers,
the program header table and section-header strings, so they read ~4–8 KiB
lighter) but the *gate* is the on-disk number, because that is what a
package manager and a user's disk both see.

| Config | `MODULES=` | Budget | Source |
|---|---|---|---|
| full | `lsp ai fuss plugins` | **≤ 2.0 MiB** | `00-decisions.md`, amended S57-A1 |
| minimal | *(empty)* | **≤ 1.5 MiB** | `00-decisions.md`, amended S57-A1 |
| lsp-only | `lsp` | ≤ 1 656 KiB | minimal + measured 120 KiB delta |
| ai-only | `ai` | ≤ 1 676 KiB | minimal + measured delta rounded to 140 KiB |
| fuss-only | `fuss` | ≤ 1 686 KiB | minimal + measured delta rounded to 150 KiB |
| plugins-only | `plugins` | ≤ 1 636 KiB | minimal + measured delta rounded to 100 KiB |
| musl static-PIE, full | `lsp ai fuss plugins` | ≤ 2.0 MiB | §4; a static build carries libc |
| musl static-PIE, minimal | *(empty)* | ≤ 1.5 MiB | amended S57-A1 evidence floor |

- The four single-module budgets are **derived, not invented**: the amended
  minimal gate plus that module's measured on-disk delta over the minimal
  build, rounded up to the next 10 KiB, written into
  `tests/size/budgets.txt` with the derivation in the `why` column and
  re-derived (not re-guessed) when a module grows on purpose. The §2 ledger
  still identifies which section and symbol caused a changed delta.
- **Additivity check, not additivity assumption.** `full` must be ≤ the sum
  of the four single-module deltas over `minimal`, plus 5 % for shared glue.
  Heavier means a module pulls in something the others already pay for, and
  the ledger says which.
- `make size` builds all six configurations into disjoint `BUILD=` trees
  (s01's rule; `mods.stamp` forces the relink but disjoint trees let the six
  run in parallel), strips each, prints the table, exits nonzero on any
  breach. CI runs it in a `size` lane on `trunk` and every PR.
- Pitfall: bare `strip` on GNU binutils keeps `.comment` and `.note.*`; use
  `strip -s -R .comment -R .note.ABI-tag`, recorded in the Makefile, or the
  number moves when a distro changes its default `strip` behaviour.
- Pitfall: do not gate on `-Os`. We ship `-O2`; measuring a size we do not
  ship is a gate on a fiction. If `-O2` will not fit, the answer is less
  code, not a different flag.

### 2. The per-module size ledger

**Amendment S57-A2 — compiler-dependent evidence is not cross-compiler
byte identity.** A size ledger records compiler identity and compiler-produced
symbol and section sizes, so a real GCC ledger cannot be byte-identical to a
real Clang ledger without deleting the evidence the ledger exists to preserve.
The committed ledgers remain pinned-GCC artifacts and are compared across two
invocations over the same build. The runtime-blob and initramfs generators,
whose output is compiler-independent by contract, are compiled with both GCC
and Clang and their outputs are byte-compared. This tightens the meaningful
determinism checks rather than normalizing away genuine toolchain differences.

#### 2.1 `scripts/size-ledger.sh` — spec

```
scripts/size-ledger.sh --build BUILDDIR --binary PATH [--without-gc PATH]
                       [--baseline FILE]
                       [--format txt|tsv] [--top N]
```

Bespoke by necessity: `bloaty` would be a dependency and invariant 7 forbids
one for a build-time nicety. Inputs, all from the toolchain we require:

| Source | Command | Gives |
|---|---|---|
| per-object symbols | `$(NM) --print-size --size-sort --radix=d -S $obj` | symbol → size, per **object file** |
| per-object sections | `$(SIZE) -A -d $obj` | `.text/.rodata/.data/.bss` per object |
| final sections | `$(SIZE) -A -d $binary` | the link's real section sizes |
| final size | `stat -c %s $binary` | the gated number |

`--without-gc` names the stripped comparison binary built from the same
configuration with `GC_SECTIONS=0`; when present, the header records its
on-disk size and `gc_saved_bytes`. The committed full and minimal ledgers
always provide it.

**Attribution runs over `build/src/**/*.o`, never over the linked binary** —
symbol → source is then a path lookup, not a DWARF query, because the object
path *is* the source path. `nm -l` on the final binary needs DWARF line info
(absent for many symbols at `-O2`, gone entirely after `strip`) and
attributes inlined code to whichever function survived.

Aggregation: object → directory → **bucket**, a fixed set matching the
architecture's module map:

```
core.util  core.unicode  core.term  core.text  core.edit  core.ui
core.ws    core.fl       core.syn   core.search  core.main
mod.lsp    mod.ai        mod.fuss   mod.plugins
runtime.embedded   (only when EMBED_RUNTIME=1, §2.4)
libc+crt           (the link delta, §2.3)
```

`src/mod/git/` maps to `mod.fuss` (the s00 `MODDIR` table's one non-obvious
row) — the script reads the mapping from the Makefile rather than
hardcoding it, so the two can never disagree.

#### 2.2 Output format — pinned

```
# yew size ledger v1  config=full  binary=build/yew  stripped=1
# toolchain: gcc 14.1.0 / GNU nm 2.42 / GNU size 2.42   target=x86_64-linux-gnu
# on-disk 1416328   sections 1409112   link_overhead 7216

bucket           .text   .rodata     .data      .bss     total    Δbase   pct
core.fl         241880     38104      1096      2048    283128    +1204  20.0%
core.syn        158904    102880       320       512    262616     +   0  18.6%
core.text       121448      6120       208     16384    144160    -  96  10.2%
core.term        96312     18744       144      4096    119296    +   0   8.4%
core.unicode     14208    121392         0         0    135600    +   0   9.6%
...
mod.lsp          92104     11208        96      1024    104432    +2880   7.4%
mod.fuss         84920      9640        72       512     95144    +   0   6.7%
mod.ai           58312      6104        48       256     64720    +   0   4.6%
mod.plugins      41008      4920        40       256     46224    +   0   3.3%
unattributed      3104       880         0         0      3984    +   0   0.3%
libc+crt         ......                                   ......         ....
-- totals       1102... etc.

-- top 12 symbols by size
  18432  core.syn      src/syn/engine.c        yew_syn_line
  12288  core.unicode  src/unicode/tables.c    yew_width_ranges
  ...
-- grown since baseline (>1 KiB)
  +2880  mod.lsp       src/mod/lsp/features.c  lsp_rename_apply
  +1204  core.fl       src/fl/vm.c             fl_vm_run
```

- `Δbase` and the *grown* block appear only with `--baseline`.
- `pct` is of the attributed total, integer permille rendered — **no `%f`**
  anywhere in the script's arithmetic path (s25's `LC_NUMERIC` reasoning;
  the shell's `printf` honours it too). All arithmetic is integer `awk` with
  `LC_ALL=C` exported at the top of the script.
- Sorting is by total descending, ties broken by bucket name ascending —
  an explicit rule, because a tie broken by `nm` output order differs
  between binutils and llvm-nm and would make the committed ledger unstable
  (the same class of bug s00's stable-sort mandate exists to prevent).

#### 2.3 The three pitfalls that make a naive ledger lie

1. **Per-object sums never equal the binary.** `--gc-sections` deletes
   unreferenced sections, the linker inserts alignment padding, and the
   PLT/GOT/`.rela`/`.eh_frame_hdr`/crt objects belong to nobody. The script
   reports `link_overhead = on_disk − Σ(object sections)` as its own line
   and **never silently distributes it** across buckets. A ledger whose
   columns are made to add up by fudging is worse than one that admits a
   7 KiB residue.
2. **`nm` reports size 0 for symbols without `.size` directives** (hand
   assembly, some compiler-emitted thunks, symbols from crt objects). Those
   land in `unattributed` with their count printed, never dropped. If
   `unattributed` exceeds 2 % of the total, the script exits nonzero — that
   threshold is what stops attribution silently rotting.
3. **`-ffunction-sections` renames sections** to `.text.<symbol>`,
   `.rodata.<symbol>`, `.data.rel.ro.<symbol>`. The aggregator folds any
   `.text*` into `.text` and so on, and a section name matching none of the
   four families is reported by name rather than discarded. Without the
   fold, the musl profile's ledger (§4 turns those flags on) reads as all
   zeroes and looks like a triumph.

Additional pitfalls: pin `NM ?= nm` and `SIZE ?= size` from the *same*
toolchain as `CC` (the cross builds in §7 need `aarch64-linux-gnu-nm`, and
mixing a host `nm` with a cross object silently produces nothing); export
`LC_ALL=C` so `sort` collates identically everywhere; and use
`--radix=d` so sizes are decimal on every binutils version.

#### 2.4 Committed ledgers and the growth gate

`tests/size/ledger-{full,minimal}.txt` are **generated and committed** — the
discipline of `src/unicode/tables.c` and s33's `tests/fletch/ledger.txt`. CI
regenerates and diffs, so drift is impossible.

| Gate | Rule |
|---|---|
| absolute | §1's per-config budget, hard, every lane that can build that config |
| per-bucket growth | a bucket's `total` growing > **5 %** or > **16 KiB** (whichever is larger) vs the committed ledger fails, naming the top grown symbols |
| unattributed | > 2 % of total fails |
| ledger freshness | regenerate + `git diff --exit-code tests/size/` clean |
| rebaseline | `make size-update`, own commit, message starts `size: rebaseline`, `why` filled in; `perf-baseline-guard` (s11/s56) extended to `tests/size/` so a size regression cannot ride in the diff that caused it |

**Optional embedded runtime** (`make EMBED_RUNTIME=1`):
`scripts/gen-runtime-blob.c` walks `runtime/` in `LC_ALL=C` sorted order and
emits `build/gen/runtime_blob.c` — one `static const u8` array plus an
insertion-ordered index — so the musl profile can ship as a single file with
no data directory. Deterministic by construction (sorted walk, no timestamps,
no paths in the output). **Off by default**: distro packages want a real
`runtime/` directory they can patch. It gets its own `runtime.embedded`
ledger bucket and budget row (≤ 220 KiB at the current langpack count).

### 3. The allocation audit

#### 3.1 Arena-discipline review checklist

One row per subsystem; each is a question a reviewer answers `yes` in the
sprint's audit note, and most map to a mechanical check.

| Subsystem | Arena lifetime | Must not `malloc` | Mechanical check |
|---|---|---|---|
| `src/fl/` compile | one arena per compilation unit, freed after `fl_compile` | AST nodes, interned identifiers, chunk scratch | `perf_alloc` records 0 heap allocs between `fl_parse` and `arena_free_all` |
| `src/fl/` VM | GC heap (s30) + a per-call frame arena | frames, temporaries | GC stress lane already covers lifetime; alloc counter covers churn |
| `src/term/render.c` | none — the `Bytebuf` is reused across frames | **anything, per frame** | 0 allocs per frame after warmup (§3.2 gate) |
| `src/term/grid.c` | buffers allocated at init/resize only | per-put allocation | 0 allocs outside `init`/`resize` |
| `src/text/piece.c` | add-buffer chunks grow geometrically; nodes from a node pool | per-edit node `malloc` | allocs per 1 000 inserts ≤ 40 (chunk growth only) |
| `src/edit/` dispatch | per-frame scratch arena, reset each frame | per-key allocation | 0 allocs per keystroke (§3.2 gate) |
| `src/syn/` | span cache + state array, sized per buffer | per-line allocation | 0 allocs per `yew_syn_line` |
| `src/ui/` layout | per-layout arena, reset each layout | `Rect`/`Region` heap churn | 0 allocs per layout after the first |
| `src/ws/` state | one arena per load/save | per-entry `strdup` | reset between loads |
| `src/search/` | program arena per compiled regex; the matcher allocates nothing | per-match allocation | 0 allocs per `yew_re_exec` |
| `src/mod/lsp/` | **one arena per message** (s45's decision) | per-node `malloc` in the JSON parser | 0 allocs between frame boundaries beyond the arena's own blocks |
| `src/mod/ai/`, `git/`, `plug/` | per-request / per-invocation arena | streaming buffers reallocated per token | growth bounded, checked by RSS row |

#### 3.2 Debug allocator with per-call-site counters

`make ALLOCDBG=1` (its own `BUILD=build-adbg` tree, s01's rule) defines
`YEW_ALLOC_DEBUG` and turns the existing entry points into macros — inside
the C11 box, no `__attribute__`, no linker tricks:

```c
#if YEW_ALLOC_DEBUG
void *yew_xmalloc_at (size_t n,           const char *f, int l);
void *yew_xcalloc_at (size_t c, size_t n, const char *f, int l);
void *yew_xrealloc_at(void *p, size_t n,  const char *f, int l);
void  yew_xfree_at   (void *p,            const char *f, int l);
#  define yew_xmalloc(n)      yew_xmalloc_at((n), __FILE__, __LINE__)
#  define yew_xcalloc(c,n)    yew_xcalloc_at((c),(n), __FILE__, __LINE__)
#  define yew_xrealloc(p,n)   yew_xrealloc_at((p),(n), __FILE__, __LINE__)
#  define yew_xfree(p)        yew_xfree_at((p), __FILE__, __LINE__)
#endif

typedef struct AllocSite {           /* open-addressed, 4096 fixed slots  */
    const char *file; int line;      /* pointers into string literals     */
    u64 calls, bytes, live, peak_live;
} AllocSite;

void yew_alloc_reset(void);                 /* zero the table + counters  */
u64  yew_alloc_calls(void);                 /* since reset — the gate     */
void yew_alloc_report(Bytebuf *out);        /* sorted: calls desc, then   */
                                            /* file/line asc (tie rule)   */
```

- Keyed on `(file, line)`; `__FILE__` has a stable address per TU, so the
  hash uses the pointer for speed and `strcmp` on collision (two TUs get two
  addresses for the same path).
- Fixed 4096 slots, no growth; overflow increments a single `overflow` site
  reported by name. An allocator that allocates to track allocations is a
  reentrancy bug waiting to happen.
- `free` needs the size, so every block carries a header large enough for
  the site index, generation, size and magic. It retains the original two
  `max_align_t` members and has a 32-byte minimum: Darwin arm64's
  `sizeof(max_align_t) == 8` makes the original 16-byte spelling too small
  for the 24-byte metadata. The returned pointer is offset past the union,
  preserving `alignof(max_align_t)` and the tested `alignas(16)` allocation
  on all four locked targets (s00's arena pitfall, one layer up).
- Pitfall, stated in the file and in `tests/perf/README`: **never take a
  timing number from an `ALLOCDBG=1` build.** The header changes sizes,
  alignment and cache behaviour; it answers *who allocates*, not *how long*.
- Reports at exit under `YEW_ALLOC_OUT=<path>`. `:alloc` is deliberately
  **not** a command: a product surface that works in only one build
  configuration is exactly the shape invariant 3 forbids.

**The gate that matters** — `tests/perf/perf_alloc.c`, in the `alloc` CI
lane:

| Scenario | Gate |
|---|---|
| steady-state typing: allocs during frames 100–10 000 of `typing.keys` | **0** |
| allocs per frame during `navigate.keys` after warmup | **0** |
| allocs per `yew_render_frame` | **0** |
| allocs per `yew_syn_line` | **0** |
| allocs per `yew_re_exec` | **0** |
| allocs per 1 000 sequential 1-byte inserts | ≤ 40 |
| allocs during a full 100 MB open | ≤ 2 000 |
| live bytes at `closed` − live bytes at `config` (s56 checkpoints) | ≤ 64 KiB |

A failure prints the offending sites with counts, so "typing allocates" is
immediately "`src/ui/statusline.c:212` allocates 3 times per frame".

#### 3.3 Peak-RSS gates per fixture

s56 §5.6 owns the checkpoint plumbing and the latency-fixture ceilings; this
sprint adds the size-flavoured rows, gated in the `size` and `embedded`
lanes:

| Metric | Fixture / config | Gate |
|---|---|---|
| peak RSS, clean open | `100m-code` | ≤ 1.6× file size (locked bar, re-gated per build config) |
| peak RSS, clean open, **minimal** build | `100m-code` | ≤ 1.6× — excising modules must not cost memory |
| peak RSS, clean open, **musl static** | `100m-code` | ≤ 1.6× |
| peak RSS at first paint, minimal, empty buffer | — | ≤ 6 MiB |
| peak RSS at first paint, musl static, empty buffer | — | ≤ 6 MiB |
| peak RSS, 13 langpacks all compiled + 50-buffer workspace | — | ≤ 48 MiB |
| peak RSS inside the constrained target | §5 | ≤ **24 MiB** |

### 4. The `x86_64-linux-musl` static embedded profile

#### 4.1 Toolchain, pinned

| Choice | Decision | Why |
|---|---|---|
| CI toolchain | container `alpine:3.20`, `gcc` + `musl-dev` + `binutils`, versions recorded in `.github/workflows/ci.yml` and echoed into the ledger header | one artifact, reproducible, no cross-file juggling |
| local/dev toolchain | `musl-cross-make` `x86_64-linux-musl-` prefix, gcc ≥ 12 | works on a glibc host without a container |
| **not** used | `musl-gcc` | it is a spec-file wrapper over the *host* gcc: it mixes host headers, it does not reliably link static-PIE, and it produces different results on two developers' machines |

`make TARGET=x86_64-linux-musl` selects `CC=$(MUSL_CC)` (default
`x86_64-linux-musl-gcc`, overridable) and:

```make
ifeq ($(TARGET),x86_64-linux-musl)
CFLAGS  += -ffunction-sections -fdata-sections -fPIE
LDFLAGS += -static-pie -Wl,--gc-sections -Wl,--build-id=none \
           -Wl,-z,noexecstack
STRIPFLAGS := -s -R .comment -R .note.ABI-tag
endif
```

#### 4.2 Audit table — glibc-isms that must not appear

Each row is a `scripts/bans.sh` entry **plus** a link-time check: after the
static link, `nm -u build-musl/yew` must be empty (a static-PIE binary
has no undefined symbols), and `readelf -d` must report no `NEEDED`.

| Item | Verdict | Why it bites on musl | Enforcement |
|---|---|---|---|
| `dlopen`/`dlsym`/`dlclose`/`dlerror` | **banned** | musl's static `dlopen` always fails; and we have **no native plugins** — plugins are Fletch-only for 1.0 (locked decision, s54) | grep `src/`; `nm -u` empty |
| `setlocale`, `LC_*`, `nl_langinfo`, `localeconv` | **banned** (already, s02 §10) | musl's locale support is a stub; more importantly `LC_NUMERIC` would make `%f` emit `0,5`. **s25 already anticipated this**: the state format is integer-only (`ratio_permille`), and s45's `strtod` use is legal *only* because `setlocale` is never called | s02's ban, re-greped in DoD 8 |
| `wchar.h`, `wcwidth`, `wcswidth`, `mbrtowc` | **banned** (already, s02 §10) | musl's `wcwidth` differs from glibc's by several hundred codepoints — invariant 5 would break across libcs | s02's ban |
| `iconv`, `iconv_open` | **banned** (already, s02 §10) | musl ships a minimal iconv with a different charset set; **s02 owns all encoding** and there is nothing to convert | s02's ban |
| `getaddrinfo` on **names** | **restricted** — see §4.3 | musl has no NSS, ignores `/etc/nsswitch.conf`, and behaves differently for `localhost` when `/etc/hosts` is absent or minimal (common in containers and initramfs images) | §4.3's rule + tests |
| `strerror_r` | **banned** | GNU returns `char *`, POSIX returns `int` — the same call compiles to two different meanings. We are single-threaded (invariant 8), so plain `strerror` is correct and unambiguous | grep `src/` |
| `execinfo.h`, `backtrace`, `backtrace_symbols` | **banned** | glibc-only; musl has none, so a `yew_bug` report using it would not build. s00's `yew_bug` already writes a structured report without it | grep `src/` |
| `qsort_r` | **banned** (already, s00) | glibc and BSD/musl disagree on argument order *and* comparator signature; `yew_sort_stable` is the repo's only sort | s01's `qsort` ban, widened to `qsort_r` |
| `getline`, `getdelim`, `asprintf`, `%m`, `err.h`, `error.h` | **banned** | GNU extensions; some exist in musl, some do not, and none are needed | grep `src/` |
| `getopt_long` | **banned** | musl's permutation behaviour differs; s00 hand-rolls `yew_args_parse` anyway (and it is pure, hence unit-testable) | grep `src/` |
| `__attribute__`, ctors, `.init_array` | **banned** (already, s00/s01) | and it **pays off here**: `--gc-sections` cannot drop an `.init_array` entry we never emit, so section GC is safe by construction | s01's ban |
| `threads.h`, `pthread` | **banned** (already, s01) | musl's static-PIE + threads has its own stack-size story we do not want to own (invariant 8) | s01's ban |
| `mmap` | **banned** (already, s01) | locked decision: no mmap (SIGBUS on truncate vs invariant 1) | s01's ban |
| `program_invocation_name` | **banned** | glibc-only | grep `src/` |

#### 4.3 `getaddrinfo` and the s48 localhost client

The AI module's HTTP/1.1 client (s48) talks to `127.0.0.1`/`::1`/`localhost`
and nothing else — no TLS, no public DNS. The rule, enforced in `src/mod/ai/http.c`:

1. Try `inet_pton(AF_INET, …)` then `inet_pton(AF_INET6, …)`. A literal
   address **never** reaches the resolver — the common configuration,
   byte-identical on every libc.
2. Only for a non-literal host, call `getaddrinfo` with
   `hints.ai_flags = AI_ADDRCONFIG`, `ai_socktype = SOCK_STREAM`, and
   **iterate every returned address in order** until one connects. Taking
   only `res[0]` is the classic failure: glibc applies RFC 3484/6724
   destination sorting, musl orders differently, and a host whose first
   entry is `::1` with no IPv6 loopback bound then works on glibc and fails
   on musl.
3. A resolution failure is a **clean, named error** through the module's
   normal path — never a hang. `getaddrinfo` has no timeout on either libc,
   so the hostname path logs at WARN *before* calling, leaving a breadcrumb.
4. Tests: the musl lane runs the s48 client suite with a normal
   `/etc/hosts`, with one containing only `127.0.0.1 localhost`, and with
   **no** `/etc/hosts` — all three must connect or fail with the named
   error; none may hang. The third is the case that differs between libcs,
   and it is what §5's initramfs actually has.

#### 4.4 Linking, stripping, and verification

| Step | Command | Verify |
|---|---|---|
| compile | `-ffunction-sections -fdata-sections -fPIE` | ledger's `.text.*` fold (§2.3) is exercised |
| link | `-static-pie -Wl,--gc-sections -Wl,--build-id=none` | `file` reports `ELF 64-bit LSB pie executable … static-pie linked` |
| no dynamic deps | — | `readelf -d` has no `NEEDED`; `nm -u` is empty; glibc `ldd` reports a static executable, while Alpine/musl `ldd` lists only `/lib/ld-musl-x86_64.so.1` (the static PIE self-relocator) and no libraries |
| strip | `strip -s -R .comment -R .note.ABI-tag` | on-disk size within §1's musl budgets |
| GC effect | `make TARGET=… GC_SECTIONS=0` for comparison | recorded in the ledger header as `gc_saved_bytes` |
| determinism | build twice, `sha256sum` | byte-identical (s01's rule; `--build-id=none` is what makes it hold) |

Pitfalls:

- **`-static` and `-pie` together are not `-static-pie`.** The former
  produces a non-relocatable static binary (and on some toolchains a broken
  one); `-static-pie` needs `rcrt1.o`, which musl provides and glibc
  historically did not. Passing the wrong pair fails at run time, not at
  link time, which is the worst place to find out.
- `--gc-sections` without `-ffunction-sections` does approximately nothing
  and looks like it worked. Record `gc_saved_bytes` so the flag pair's value
  is a number, not a belief.
- Do **not** add `-Wl,--icf=all` (identical code folding) — it is a gold/lld
  feature, unavailable on the pinned binutils, and it makes the §2 ledger's
  symbol attribution meaningless by merging distinct functions.
- Keep `-Wl,-z,noexecstack` explicit: a single object without a
  `.note.GNU-stack` marker silently makes the whole binary's stack
  executable, and static binaries are exactly where that goes unnoticed.

### 5. PROVEN ON TARGET — the constrained profile

Two reference targets, with different jobs, both written down:

| Role | Target | Runs |
|---|---|---|
| **CI gate** | QEMU `qemu-system-x86_64 -machine q35 -cpu qemu64 -m 64 -nographic -kernel <distro bzImage> -initrd build/embed.cpio.gz -append 'console=ttyS0 quiet'` | every `trunk` push and nightly |
| **hardware corroboration** | **Raspberry Pi Zero 2 W class** — Cortex-A53 quad, **512 MB RAM**, arm64 — the `arm64-linux` static build | manually, once per release, result recorded in `.docs/embedded-runs.md` |

QEMU is the gate because it is reproducible on any runner; the SBC is the
corroboration because an emulator cannot tell you about a real memory
controller. Claiming the emulated run proves hardware performance would be
the same dishonesty s56 §4 refuses, so we do not claim it.

`scripts/embed-image.sh` builds `build/embed.cpio.gz`: static busybox (from
the pinned Alpine container), the stripped static-PIE `yew`, the
`runtime/` tree (or `EMBED_RUNTIME=1` and none), the fixtures below, and an
`/init` that runs the checklist and prints a machine-readable result block.
Image target **≤ 12 MiB**.

**Hard RAM ceiling: 64 MiB of guest RAM.** Within it, the checklist (each
row a `yew --batch` script or a pty case run inside the guest):

| # | Check | Assertion |
|---|---|---|
| 1 | `yew --version` | exit 0, `modules:` line correct for the config |
| 2 | boot → first paint on an empty buffer | completes; `VmHWM` recorded |
| 3 | open a 4 MiB C source, render, cursor to EOF, save | byte-identical round trip (invariant 1) |
| 4 | 2 000-keystroke `typing` session | completes; no OOM in `dmesg` |
| 5 | syntax highlighting live on the 4 MiB file | spans match the host-produced golden |
| 6 | regex search across the 4 MiB file | same match offsets as the host |
| 7 | undo tree: 500 edits, undo all, redo all | content byte-identical (s11's P6) |
| 8 | workspace save → quit → resume | tabs, panes, cursors restored (s25) |
| 9 | record a macro, replay it | round-trip law (invariant 10) |
| 10 | crash path: `--selftest-bug` | exit 4, structured report, terminal restored |
| 11 | peak RSS across 2–9 | **≤ 24 MiB** (`/proc/self/status` `VmHWM`) |
| 12 | 32 MiB guest, same 4 MiB file | either completes, or **refuses with a clean named error** — never an OOM kill. `dmesg` must contain no `Out of memory` line naming yew |

**What is gated and what is not**, plainly: under TCG emulation the wall
clock is 10–40× the host's and varies with the host's load, so **no timing
row is a gate here**. The gates are completion, correctness against
host-produced goldens, `VmHWM`, and the absence of an OOM kill. Row 12 is
the interesting one — it gates *graceful degradation*, a property an
emulator measures perfectly well.

`make embedded` builds the image and runs the checklist; `make embedded-gate`
also enforces rows 11 and 12. **MILESTONE: embedded proof** is rows 1–12
green plus §1's musl size budgets.

### 6. The minimal-build feature matrix

`MODULES=""` must still be a complete modal editor:

| Capability | In minimal? | Landed by |
|---|---|---|
| L/W/B/H/I/E modes, unit engines, multi-cursor | **yes** | s13–s18 |
| Piece tree, undo tree, atomic save, crash journal | **yes** | s07–s10 |
| Unicode: grapheme, width, lossless invalid bytes | **yes** | s02 |
| Terminal stack: raw mode, kitty proto, damage render, 2026 | **yes** | s03–s05 |
| **Fletch**: VM, stdlib, config, macros, `--batch`, `yew fl` | **yes** (core) | s28–s38 |
| **Syntax highlighting**, all 13 langpacks, both themes | **yes** (core) | s39–s42 |
| Regex engine, search/replace | **yes** | s20–s21 |
| **Workspaces**, tabs, tab groups, panes, resume | **yes** | s22–s25 |
| Fuzzy finder, mouse, registers, OSC 52 clipboard | **yes** | s12, s26, s27 |
| Shadow completion from the **buffer/workspace symbol index** | **yes** (core) | s43–s44 |
| LSP (diagnostics, completion, hover, goto-def, …) | no → `lsp` | s45–s47 |
| AI shadow completion, HTTP client | no → `ai` | s48–s50 |
| FUSS mode, gutter hunks, blame, git verbs | no → `fuss` | s51–s53 |
| Plugin system, `yew pkg` | no → `plugins` | s54–s55 |

**Excision boundary verification** — three independent checks, because each
catches a different failure:

1. **Symbol check.** For each excluded module, `nm build-min/yew` must
   contain **zero** symbols defined by that module's real sources. The
   script derives the symbol set from the *enabled* build's per-object `nm`
   output (§2), so it needs no hand-maintained list and cannot drift.
2. **Object check.** The link line for `MODULES=""` contains exactly one
   object per module directory — its `shim.o` — and none of the real ones.
   Asserted by parsing `$(MOD_SRC)` from `make -n`.
3. **Behaviour check**, in two parts.
   - *Parity*: every function declared in the module's public header has a
     definition in `shim.c`. A missing shim is a link error today, but a
     header function that quietly stopped being declared is not — so
     `bans.sh` gains a declaration-vs-shim parity check, the same shape as
     s01's unregistered-test check.
   - *Honesty*: every user action calls `yew_mod_require(YEW_MOD_<X>, …)`
     and **returns the failure**, never success. A passive lifecycle hook or
     query instead returns a neutral value/no-op and never claims that an
     unavailable action occurred; pure module parsers return failure. This
     distinction preserves the core event loop while still satisfying
     invariant 3. `tests/unit/test_mod.c` enumerates the user-facing command
     entry points in a `MODULES=""` build and asserts the failure return, the
     s00 canonical message, and that `<name>` is `lsp`/`ai`/`fuss`/`plugins`
     respectively — **not** a generic "module missing". `bans.sh` rejects a
     directly successful action shim and proves the rule with a seed.
4. **Reachability check.** In the minimal build, `ed.mode.enter "F"`, every
   `ed.git.*`/`ed.ai.*` action, every LSP action except
   `ed.lsp.complete`, `yew pkg`, and every config option those modules own
   must produce the canonical message through the *user-facing* path — pty
   and `tests/script/` cases, not only unit tests. `ed.lsp.complete` retains
   its documented core symbol-index fallback; removing that fallback to make
   the module boundary more uniform would gut a required minimal-build
   feature. The registry keeps the entries (so `:` completion still lists
   them with a "requires module" note) rather than hiding them: a user who
   typed `:blame` deserves to be told why, not that it does not exist.

`scripts/smoke.sh` already asserts `modules: none` here (s01 §5); this
sprint adds a row asserting each module name appears in exactly one refusal.

### 7. `arm64-linux` and `arm64-macos` lanes go green

#### 7.1 The cross-compile / native story

| Lane | Runner | Build |
|---|---|---|
| `arm64-linux` | native arm64 Linux runner | `make` — native, so tests run for real |
| `arm64-linux-cross` (local dev) | x86_64 + `aarch64-linux-gnu-gcc` + `qemu-aarch64-static` under binfmt | `make CROSS=aarch64-linux-gnu-` ; `NM`/`SIZE`/`STRIP` follow the same prefix |
| `arm64-macos` | `macos-14` (Apple silicon) | `make` — native |
| `arm64-linux` static | native arm64 + musl | §4's flags, size recorded (budget is x86_64's, see below) |

`CROSS=` sets `CC`, `NM`, `SIZE`, `STRIP`, `AR` from one prefix — a cross
build with a host `nm` produces an empty ledger and no error (§2.3's
pitfall), so the prefix is applied in one place.

#### 7.2 What arm64 actually catches

| Hazard | Consequence | Where it is caught |
|---|---|---|
| Misaligned loads | s00's arena `align` power-of-two rule is UB-on-x86, **fault-on-arm64** | the arm64 lane runs the unit suite under `-fsanitize=alignment,undefined` |
| `char` is **unsigned** on both arm64 targets | `char c = *p; if (c < 0)` never fires; a UTF-8 continuation-byte test silently changes meaning | the repo uses `u8` for bytes by convention; s02's decoder fuzz corpus (0x80–0xFF through every path) is what proves it, re-run on the lane |
| Weaker memory ordering | none for us — single-threaded (invariant 8) | noted so nobody "fixes" it with barriers |
| `long double` differs (128-bit on arm64 Linux, 64-bit on macOS) | we have no `long double`; Fletch floats are `f64` | `grep -rn 'long double' src/` → empty, added to `bans.sh` |
| Different `int` promotion of `sizeof` results in printf | `%zu` used consistently | `-Wformat` under `-Werror`, already on |

#### 7.3 macOS quirks, and the honest limit of the lane

| Quirk | Effect | Handling |
|---|---|---|
| `ru_maxrss` is **bytes**, not KiB | a 1000× phantom regression on day one | s11 §7's pitfall, shipped as s56's single-`#ifdef` `yew_rss_bytes` — the macOS lane is what proves it |
| No `/proc` | *current* RSS unavailable | `yew_rss_bytes` falls back to `ru_maxrss`; the after-close leak row (s56 §5.6) is **Linux-only** and says so |
| No `-static-pie`, no static libSystem | Apple does not support static linking of the system library | **the musl static profile is Linux-only, explicitly.** macOS binary size is *recorded*, not gated; `00-decisions.md`'s size bars say "stripped x86_64" and mean it |
| `strip` flags differ (`-S` vs `-s`, no `-R`) | the §1 recipe fails | the size lane runs on Linux only; macOS records `stat -f %z` for the record |
| Case-insensitive filesystem by default | `Foo.c` and `foo.c` are one file; s25 hashes a realpath into a workspace key | a fixture opening `Foo.c` and `foo.c` in one session, asserting one buffer on macOS and two on Linux — a *documented* platform difference, not a bug to paper over |
| `poll(2)` on device fds is historically quirky | our fds are ptys and pipes, which are fine | the s06 pty suite runs in full on the lane, which is the actual proof |
| The Darwin raw-PTY input queue is smaller than s14's 4 KiB raw-key fixture | one 4,096-byte harness write becomes several readiness windows, so counting those windows as one input burst is false evidence | the macOS raw-key frame gate uses a queue-resident 512-key write, then feeds the remainder before checking the same 4,096-key final golden; Linux retains the exact 4,096-key one-window gate, and the full 4 KiB bracketed-paste gate remains unchanged on both hosts |
| `posix_openpt`/`grantpt`/`ptsname` | present; `ptsname_r` absent (s06 already avoids it) | no change |
| Terminal.app has no truecolor, no kitty protocol, no mode 2026 | our `dumb`/`nosync` paths matter more here than on Linux | the CI runner is headless: **the pty harness is our own fake terminal, so its goldens are identical to Linux and prove nothing about Terminal.app.** Real-terminal verification (Terminal.app, iTerm2, kitty, Ghostty, WezTerm) is a **manual release checklist** item owned by Sprint 60, and this sprint writes that checklist rather than pretending CI covers it |

That last row is the lane's honest boundary: `arm64-macos` green means the
build, the POSIX layer, the alignment story and byte-level rendering are
correct on Darwin — not that any macOS terminal emulator was exercised.

### 8. Defer

- Adversarial audits, extended fuzz campaigns, the findings ledger →
  Sprint 58. **Packaging** (AUR, brew tap, RPM spec), the `--version`
  contract audit and **reproducible release builds** (`SOURCE_DATE_EPOCH`,
  deterministic archives) → Sprint 60; this sprint's byte-identical rebuild
  check is that work's *input*, not a substitute for it.
- FreeBSD and any target outside `00-decisions.md`'s closed set → post-1.0.
  `make TARGET=<unknown>` hard-errors listing the four supported targets.
- Native (C ABI) plugins → **never in 1.0**; §4.2's `dlopen` ban is a
  decision, not a gap, and `src/mod/plug/` errors naming Fletch-only plugins
  if a native module path is ever requested.
- `-Os`/LTO experiments, `--icf`, profile-guided section ordering, and
  per-langpack lazy loading to shrink `core.syn` → post-1.0; each breaks
  either the ledger's attribution or the determinism lane. Amendment S57-A1
  retains the original 1.5 MiB / 900 KiB planning pair as the ratchet those
  experiments pursue without making 1.0's required feature matrix depend on
  unscoped compiler or runtime redesign work (s40's lazy *compilation*
  already covers the cold-start half of the langpack cost).

## Testing Strategy

- **Unit** (`tests/unit/test_alloc.c`, with the debug accounting assertions
  selected by `ALLOCDBG=1`): site table insert/lookup/
  collision on two TUs sharing a `__FILE__` spelling; overflow past 4096
  slots increments the named overflow site and does not allocate; header
  alignment asserted with an `alignas(16)` allocation; `realloc` moves
  accounting to the new size without changing the site; `yew_alloc_reset`
  zeroes; report ordering is calls-desc then file/line-asc (tie rule).
- **Unit/policy** (`tests/unit/test_mod.c`, built for `MODULES=""`, plus
  `scripts/check-module-shims.sh`): disabled command families are driven and
  assert failure, canonical messages, and the correct module name. The policy
  check enumerates boundary-header declarations and shim definitions so a new
  module function cannot be added without its disabled definition (§6.3).
- **Script** (`scripts/size-ledger.sh` self-tests, `tests/size/meta/`): a
  hand-built object set with known sizes, a symbol with no `.size`, a
  `.text.foo` section (fold check), a bucket over the 5 % growth threshold,
  and a stale committed ledger — each must produce the specified output or
  exit code, with the failing item named.
- **Size lane**: `make size` over all six `MODULES` configurations; the
  additivity check; the two committed ledgers regenerated and diffed; a
  seeded 40 KiB `static const` array in `src/mod/lsp/` must fail the
  per-bucket growth gate naming that symbol, and must **not** fail after
  `make size-update` in its own commit.
- **Alloc lane**: `make ALLOCDBG=1` in `build-adbg`, then `perf_alloc`'s
  named rows. A seeded `yew_xmalloc(16)` inside `yew_render_frame` must fail
  the zero-alloc row and name `render.c:<line>`.
- **musl lane**: build in the pinned Alpine container; `file`, `readelf -d`,
  `ldd`, `nm -u` verifications (`ldd` may list the musl loader as its sole
  row because musl implements `ldd` through that loader); the full `make test`, `make test-pty` and
  `make test-fletch` suites under musl; the §4.3 `/etc/hosts` matrix; two
  builds `sha256sum`-identical.
- **Embedded lane**: `make embedded-gate` — image ≤ 12 MiB, the twelve
  checklist rows, `VmHWM` ≤ 24 MiB, the 32 MiB graceful-degradation row with
  `dmesg` scraped for OOM kills. Goldens for rows 5 and 6 are produced on
  the host and carried into the image, so the guest compares against the
  host's answer rather than its own.
- **arm64 lanes**: `arm64-linux` runs `make test`, `test-pty`, `test-fletch`,
  `make perf YEW_PERF_ADVISORY=1` (s56), and the alignment/UBSan pass;
  `arm64-macos` runs the same minus the size and musl targets, plus the
  case-insensitive-filesystem fixture and an explicit assertion that
  `yew_rss_bytes` returns a plausible value (a macOS regression here shows
  as 1000× and is trivially catchable).
- **Determinism**: both pinned-GCC ledgers are byte-identical across two
  invocations over the same shipping objects. `runtime_blob.c` for the real
  runtime tree, plus a controlled embed image and its file list, are
  byte-identical across two runs and when their generators are compiled by
  GCC and Clang. Actual cross-compiler binary manifests retain honest binary
  sizes and therefore are not falsely required to match (S57-A2).
- **Bans**: each new `bans.sh` row is proven by seeding a violation and
  removing it — `dlopen(`, `strerror_r(`, `backtrace(`, `long double`,
  `getopt_long(`, `program_invocation_name`, a header function with no
  shim, and a shim returning success.

## Definition of Done

1. `make` and `make test` green on gcc and clang; the pinned GCC `make size`
   lane green; zero warnings
   under the locked flags; ASan/UBSan, valgrind, determinism, bans, size,
   alloc, musl, embedded, arm64-linux and arm64-macos lanes all green on
   `trunk`.
2. `make size` enforces all eight §1 rows; **full ≤ 2.0 MiB** and
   **minimal ≤ 1.5 MiB** stripped on x86_64-linux-gnu, with the measurement
   definition (stripped, on-disk, `-O2`) stated in `tests/size/README` and
   the four single-module budgets carrying their derivation in `why`.
3. The additivity check passes: `full` ≤ Σ(single-module deltas) + 5 %.
4. `scripts/size-ledger.sh` produces the §2.2 format; `ledger-full.txt` and
   `ledger-minimal.txt` are committed, regenerate byte-identically, and
   `git diff --exit-code tests/size/` is clean from a fresh clone.
   `unattributed` ≤ 2 %; `link_overhead` is reported as its own line and
   never distributed; `.text.*` folding proven by the musl ledger being
   non-empty.
5. Per-bucket growth gate proven both ways: a seeded 40 KiB array fails
   naming its symbol; `make size-update` in its own commit clears it; and
   `perf-baseline-guard` rejects a commit touching both `src/` and
   `tests/size/`.
6. `ALLOCDBG=1` builds in `build-adbg`; all named §3.2 rows green, including
   **zero allocations per frame during steady-state typing**, zero per
   `yew_render_frame`, zero per `yew_syn_line`, and zero per `yew_re_exec`;
   a seeded per-frame allocation fails and names its `file:line`.
7. All §3.3 RSS rows green, including 1.6× on `100m-code` for the full,
   minimal **and** musl-static builds.
8. The §4.2 audit table is fully enforced: every "banned" row has a
   `bans.sh` entry proven by a seeded violation; `nm -u build-musl/yew`
   is empty; `readelf -d` shows no `NEEDED`; `ldd` reports no library beyond
   musl's static-PIE self-relocating loader; `file` reports `static-pie linked`; s02's locale/iconv/
   wide-char ban re-greps clean.
9. The §4.3 rule is implemented and tested: literal addresses never reach
   the resolver, every `getaddrinfo` result is tried in order, and the s48
   client suite passes with a full `/etc/hosts`, a minimal one, and none —
   with no hang in any case.
10. The musl build is byte-identical across two runs (`--build-id=none`),
    within its §1 budgets, and `gc_saved_bytes` is recorded in the ledger
    header.
11. **MILESTONE: embedded proof** — `make embedded-gate` green: image
    ≤ 12 MiB, all twelve checklist rows pass inside a **64 MiB** QEMU guest,
    peak `VmHWM` ≤ 24 MiB, and the 32 MiB run degrades with a clean named
    error and no OOM kill. The hardware corroboration run (Pi Zero 2 W
    class, 512 MB) is recorded in `.docs/embedded-runs.md` with its numbers
    and its date.
12. Minimal-build matrix verified by all four §6 checks: zero excluded-module
    symbols in `nm`, exactly one `shim.o` per module on the link line,
    header-to-shim parity enforced by `bans.sh`, and every shim entry point
    returning a failure whose message names **its own** module (`lsp`, `ai`,
    `fuss`, `plugins` — asserted per module, not generically). `MODULES=""`
    passes the full `make test`, `test-pty`, `test-fletch` and
    `test-script` suites and edits, highlights, searches, saves, resumes a
    workspace, and records and replays a macro.
13. `arm64-linux` and `arm64-macos` lanes are **required** and green,
    including the pty suite; the alignment sanitizer pass is clean on arm64;
    the `ru_maxrss` unit normalization is asserted on macOS; and the
    macOS-terminal limitation of §7.3 is written into
    `.docs/release-checklist.md` as a manual Sprint 60 item rather than
    claimed as covered.
14. `make TARGET=<anything outside the closed four>` hard-errors listing the
    supported targets; `grep -rn 'dlopen\|strerror_r\|backtrace\|long double\|getopt_long' src/`
    is empty; `test_alloc.c`, `test_mod.c`, the shim parity/honesty positive
    controls, and the ledger meta-suite all pass. Structural header-to-shim
    enumeration replaces a brittle assertion-count proxy (S57-A2).
