# Sprint 56: Latency Gates — In-Loop Profiling, Calibration, Committed Budgets

## Prerequisites

- Sprint 0/1 — the one Makefile, `Bytebuf`/`Arena`, `yew_log`, `YEW_BUG`,
  exit codes; unit harness, `make test`, `scripts/bans.sh`, the CI lane set,
  and the rule that sanitized and plain object trees never mix (`BUILD=`).
- Sprint 5 — `Grid`, `Render`, `yew_render_frame`, `tests/perf/perf_render.c`
  with byte-count rows **hard** and timing rows *advisory until this sprint*.
  s05 §8 is the deferral this sprint discharges.
- Sprint 6 — the pty harness: `yew_pty_spawn`, the constructed-envp table,
  the four probe profiles (`modern`/`nokitty`/`nosync`/`dumb`), `VtScreen` +
  `vt_feed`, `PtyCase`/`ptc_spawn`/`ptc_keys`/`ptc_settle`/`ptc_snapshot`,
  `YEW_PTY_BUDGET_MS`, the closed sequence set. Sessions are scripted in key
  **spellings**, not bytes.
- Sprint 11 — **the baseline and gate policy this sprint reuses rather than
  reinvents** (its §8), plus `gen-bigfile`, `make fixtures`,
  `make fixtures-verify`, `tests/perf/fixtures.sha`, the six fixture
  profiles, and the `ru_maxrss` KiB-vs-bytes single-normalization rule.
- Sprint 14 — `Ed`, `yew_loop_run` and its numbered phases,
  `yew_loop_deadline`, `yew_now_ms`, `YEW_INPUT_BURST_MAX`, the
  drain-then-render rule. Sprint 15 — viewport, `full_damage`,
  `tests/perf/scroll.c`, the 120 fps gate. Sprint 13/18 — the named-command
  registry (`"ed.*"`) and E mode's `:` prompt, how `:prof` is reached.
- Sprint 33 — the bench precedent: median of 11, warmup discarded,
  `CLOCK_MONOTONIC` only, the `why` column, `PERF_GATE=1`, the two-condition
  gate. Sprints 39/41 — `perf_syn.c`, the frozen syn fixtures, the fake
  clock that turns a frame-budget assertion into a gate. Sprint 37 —
  `yew --batch`. Sprints 20/21 — regex and search. Sprints 22–25 — panes,
  tabs, deferred tabs, `ratio_permille`'s integer-only reasoning.
- **Assumptions beyond earlier index entries**: (1) Campaign 10's files are
  not yet drafted; this sprint needs only that Sprint 49 ships a **mock AI
  backend** (a local process speaking the s48 HTTP/1.1 localhost path with
  scripted, fixed-latency replies) and that Sprints 46/47 ship an equivalent
  mock language server — if either lands under another name, §5's fixture
  class binds to that name and nothing else changes. (2) s01's CI has a perf
  lane on a **designated runner**, `perf-x86_64-linux-gnu` (s11 added it if
  absent). (3) `yew_rss_bytes()` is s11 §7's normalization function,
  promoted from `tests/perf/` into `src/util/` here.
- Binding: `00-decisions.md` **Quality bars** (keypress → paint p99 ≤ 5 ms;
  cold start → first paint ≤ 20 ms; 100 MB open ≤ 150 ms; scroll ≥ 120 fps
  equivalent; memory ≤ 1.6× file size) and invariants 4, 5, 7, 8.

## Goals

Make invariant 4 real. Build the profiler *into the event loop* — per-phase
timers behind `YEW_PROF=1`, a ring buffer of the last N frames, a `:prof`
report and a headless dump — so every budget investigation in this sprint
and every one after it is done with yew's own numbers and no external
tool. Then land the calibration story honestly: wall-clock gates run on one
designated runner lane, every other lane runs the same suite *functionally*,
a three-component calibration microbenchmark measured once per runner
produces an integer scale factor, budgets are expressed in calibrated units,
and the places where that scheme cannot help are written down instead of
papered over. Then the gate suite itself in `tests/perf/`: keypress → paint
p99 through the s06 pty harness with timestamped injection over scripted
10 000-keystroke sessions across five fixture classes, exec-to-first-paint,
the 100 MB open end to end, scroll throughput, search on 1 GiB, and memory
ceilings from `getrusage` checkpoints — with committed baselines, s11's
anti-flap regression policy reused verbatim, and a triage playbook mapping
symptom → phase → the tool that shows it. Binary size, the allocation audit
and the musl embedded profile are Sprint 57; adversarial audits are 58.

## Deliverables

### 1. In-loop frame instrumentation — `src/util/prof.h`, `src/util/prof.c`

```c
typedef enum {                 /* the s14 loop's phases, in loop order   */
    YEW_PH_POLL,               /* poll(2) wait — excluded from TOTAL     */
    YEW_PH_INPUT,              /* read + yew_input_feed + signal drain   */
    YEW_PH_DISPATCH,           /* yew_dispatch_key ×N, tick, timers      */
    YEW_PH_JOBS,               /* LSP/AI/shell/git pump (s19,46,49,51)   */
    YEW_PH_LAYOUT,             /* yew_layout                             */
    YEW_PH_SYN,                /* yew_syn_settle slice (s39)             */
    YEW_PH_RENDER,             /* yew_render_frame → Bytebuf             */
    YEW_PH_WRITE,              /* the one write(2) + yew_grid_flip       */
    YEW_PH_COUNT
} YewPhase;

typedef struct ProfFrame {     /* exactly 64 bytes — one cache line      */
    u64 seq;                   /* frame number since yew_prof_init       */
    u64 t_mono_ns;             /* frame start, CLOCK_MONOTONIC           */
    u32 ph_ns[YEW_PH_COUNT];   /* per phase, excluding POLL from total   */
    u32 total_ns;              /* measured, NOT summed — see below       */
    u32 bytes_out;             /* bytes the frame wrote to the tty       */
    u16 keys;                  /* keys dispatched in this frame          */
    u16 flags;                 /* YEW_PF_* bitmask                       */
} ProfFrame;
_Static_assert(sizeof(ProfFrame) == 64, "one frame is one cache line");

enum { YEW_PF_FULL_DAMAGE = 1u<<0, YEW_PF_RESIZE = 1u<<1,
       YEW_PF_JOB_IO      = 1u<<2, YEW_PF_MARK   = 1u<<3,
       YEW_PF_BURST_CAP   = 1u<<4 /* s14's YEW_INPUT_BURST_MAX hit */ };

typedef struct Prof {
    bool       on;
    ProfFrame *ring;  u32 cap, head, n;   /* cap = YEW_PROF_RING frames  */
    u64        seq, frame_t0, phase_t0;
    YewPhase   open;                      /* the phase currently timing  */
    u64        overhead_ns;               /* self-measured, §1 pitfall   */
    char       mark[32];                  /* last :prof mark label       */
} Prof;

u64  yew_now_ns(void);                            /* CLOCK_MONOTONIC     */
void yew_prof_init(Prof *p, Arena *a, bool on);   /* ring from the arena */
void yew_prof_frame_begin(Prof *p);
void yew_prof_phase(Prof *p, YewPhase ph);        /* closes the open one */
void yew_prof_frame_end(Prof *p, u16 keys, u32 bytes_out, u16 flags);
void yew_prof_reset(Prof *p);
void yew_prof_write(const Prof *p, Bytebuf *out); /* the ONE formatter   */
```

- `YEW_PROF=1` turns it on; anything else (unset, `0`, empty) leaves it off.
  Read once at `Ed` construction into `ed->prof.on` — **no global** (s14:
  the only global is s03's tty sliver). `tests/perf/` harnesses own their
  own `Prof`.
- `YEW_PROF_RING` defaults to **4096 frames** (256 KiB, ~70 s of 60 fps
  typing), settable, clamped to `[64, 262144]`. Arena-allocated once; a full
  ring overwrites the oldest frame and never allocates.
- **`yew_prof_phase` closes the previous phase and opens the next** — one
  call per boundary, not a begin/end pair. Pitfall: begin/end pairs get
  mismatched by an early `return` and the frame then attributes 4 ms of
  dispatch to layout. With a boundary API the only unclosed phase is
  whatever was open at `frame_end`, which closes it unconditionally.
  Re-entering a phase accumulates — correct when jobs pump twice.
- **`total_ns` is measured independently, never summed**, and the report
  prints `unaccounted = total − Σph` as its own row. A phase table that sums
  to the total by construction cannot show the 900 µs spent in a function
  nobody instrumented, which is exactly the 900 µs you are hunting.
- `YEW_PH_POLL` is recorded but excluded from `total_ns` and every budget:
  it is time asleep waiting for the user. Kept because nonzero poll time
  *inside* a burst means the drain loop is not draining (s14's pitfall).
- **Instrumentation cost.** `2 + nphases` `clock_gettime` calls per frame;
  vDSO, ~20–25 ns each, ~250 ns against 5 ms. `yew_prof_init` measures the
  complete phase-clock and ring-write path over 1024 frames in five batches,
  records the median uninterrupted batch in `overhead_ns`, and resets the
  calibration frames before returning. DoD 2 caps that measured cost at
  **2 % of external keypress p99**. The separately launched profiler-off/on
  p99 delta is diagnostic only because scheduler placement and DVFS can move
  a tail percentile between processes. Pitfall: never compare a
  `YEW_PROF=1` number to a committed baseline — the gate suite runs the
  profiler off, and §5's cross-check is the one place both numbers meet.
- Pitfall: `CLOCK_MONOTONIC`, not `CLOCK_MONOTONIC_RAW` (uncorrected, drifts
  against every other timestamp in the system) and not `rdtsc` (not
  invariant across cores or frequency states; s33 already banned it).

Call sites, pinned — this is the s14 loop with the boundaries marked:

```c
for (;;) {
    int tmo = yew_loop_deadline(ed, yew_now_ms());
    yew_prof_phase(&ed->prof, YEW_PH_POLL);
    if (poll(fds, nfds, tmo) < 0 && errno != EINTR) return yew_bug_io();
    yew_prof_frame_begin(&ed->prof);          /* the frame starts on wake */
    yew_prof_phase(&ed->prof, YEW_PH_INPUT);    /* … drain + signals …   */
    yew_prof_phase(&ed->prof, YEW_PH_DISPATCH); /* … keys, tick, timers  */
    yew_prof_phase(&ed->prof, YEW_PH_JOBS);     /* … pump async …        */
    yew_prof_phase(&ed->prof, YEW_PH_LAYOUT);   /* … if layout_dirty …   */
    yew_prof_phase(&ed->prof, YEW_PH_SYN);      /* … yew_syn_settle …    */
    yew_prof_phase(&ed->prof, YEW_PH_RENDER);   /* … yew_render_frame …  */
    yew_prof_phase(&ed->prof, YEW_PH_WRITE);    /* … write + flip …      */
    yew_prof_frame_end(&ed->prof, nkeys, nbytes, flags);
}
```

`yew_prof_frame_begin` fires **after** `poll` returns, so a frame's clock
starts when the user's bytes arrived, not when we went to sleep. Zero-damage
frames (s05 §6 emits 0 bytes) are still recorded, with `bytes_out == 0` —
counting them is how you notice the loop waking up for nothing.

### 2. `:prof` — the report command and the headless dump

Registry entries (s13's one dispatch surface; reachable from keys, E mode,
Fletch and the recorder alike):

| Command | E-mode form | Effect |
|---|---|---|
| `ed.prof.report` | `:prof` | render §2's report into a scratch buffer, open it in the current pane |
| `ed.prof.reset` | `:prof reset` | `yew_prof_reset` — clears the ring, keeps `on` |
| `ed.prof.dump` | `:prof dump PATH` | `yew_prof_write` to `PATH` (atomic, s08 path) |
| `ed.prof.mark` | `:prof mark LABEL` | tag the next frame `YEW_PF_MARK` and store `LABEL` |
| `ed.prof.frames` | `:prof frames [N]` | one line per frame for the last N (default 40) — the raw view |

With `YEW_PROF` unset every one of these returns the s00 canonical shape of
a refusal: `profiling is off; restart with YEW_PROF=1` — a message, not a
silent empty report (invariant 3's spirit applied to a diagnostic surface).

Report format, pinned (golden-tested against a **synthetic** ring so the
test has no timing dependence):

```
# yew prof v1  frames=4096 dropped=0 overhead_ns=23 mark=paste-100k
phase        p50      p90      p99      max    share   calls
input         41       88      210     1904    0.9%     4096
dispatch     118      302     1441    18302    6.1%     4096
jobs           0        0       92    12008    0.4%     4096
layout        62      140      388     2201    1.7%     3110
syn          204      690     2980    41007   14.2%     2884
render       401     1122     3410    22190   19.7%     4096
write         88      205      902    11400    3.6%     3110
unaccounted  ...                              53.4%
TOTAL       1004     2711     9880    98442
poll     (asleep, excluded)
--- worst frames
  seq      total_ns  keys  bytes  flags        top phase
  20418       98442     1   6122  FULL,RESIZE  syn 41007
  ...
```

- Percentiles come from sorting a copy of the ring — 4096 elements, a few
  hundred microseconds, and this is an interactive command. No streaming
  estimator: an approximate p99 in a tool whose whole job is chasing a p99.
- `share` is `Σphase / Σtotal` as integer permille rendered with one
  decimal — **integer arithmetic only**, no `printf("%f")`. s25 pinned why
  (`LC_NUMERIC` makes `%f` emit `0,5` in a `de_DE` locale); the same
  reasoning binds every number this sprint prints.
- `calls` counts frames in which the phase was nonzero — how you see that
  layout ran on 3110 of 4096 frames.
- **One formatter.** `:prof`, `:prof dump`, the headless dump and the golden
  test all go through `yew_prof_write` into a `Bytebuf`. Same discipline as
  s06's snapshot comparator: the file *is* the assertion.

Headless: under `yew --batch` (s37) with `YEW_PROF=1`, `yew_prof_write` runs
at exit to `$YEW_PROF_OUT` if set, else
`$XDG_STATE_HOME/yew/prof-<pid>.txt`, path logged at INFO. Batch has no
tty and no frames in the s14 sense, so a "frame" is one script statement
boundary and the phase set collapses to `DISPATCH`/`JOBS`/`SYN`, the rest
reporting zero calls — stated in the file header so nobody reads a zero as
a fast renderer.

### 3. External tools — deep dives only, never a gate

`make perf-symbols` builds into `build-prof/` (disjoint tree, s01's `BUILD=`
rule) with `-O2 -g -fno-omit-frame-pointer`. Nothing in CI consumes these.

| Tool | Invocation | Use it for | Do not |
|---|---|---|---|
| `perf record` | `perf record -g --call-graph=fp -- build-prof/yew --batch tests/perf/sessions/typing.fl f` | which functions own a phase | ...quote its overhead-inflated wall clock as a budget number |
| `perf stat` | `perf stat -e cycles,instructions,cache-misses,branch-misses -- …` | is this phase memory-bound or branch-bound | ...compare counts across machines |
| `perf record -p` | spawn under s06's harness, then attach by pid printed by the runner | profiling the pty child | ...`perf record -- build/pty_runner` — that profiles the *runner* |
| `valgrind --tool=callgrind` | `callgrind_annotate` over a `--batch` run | exact call counts and inclusive costs; finding an accidental O(n²) | ...believe any timing. Callgrind is ~50× slower and reorders nothing but distorts everything |
| `valgrind --tool=massif` | `--time-unit=B` | where peak heap lives | ...use it for RSS — massif measures the heap, `getrusage` measures the process |
| `valgrind --tool=dhat` | `--tool=dhat` on a batch session | short-lived allocation churn | ...replace Sprint 57's per-call-site allocation counter with it |

Pitfall, stated in `tests/perf/README`: the first move on any latency
symptom is `YEW_PROF=1` + `:prof` — it costs 2 % and answers "which phase"
in one keystroke. `perf` answers "which function" and costs a build;
callgrind answers "how many calls" and costs a coffee. Reaching for `perf`
before reading the phase table is how an afternoon disappears optimizing a
function that owns 3 % of the frame.

### 4. Reference-hardware calibration — and what it cannot do

**The problem, first.** CI runners are shared virtual machines with
neighbours we cannot see; observed p99 for one commit on a hosted
`ubuntu-latest` runner varies 2–4× across runs (frequency scaling, SMT
siblings, host steal time). A wall-clock gate there is a coin flip, and a
coin-flip gate gets muted within two weeks — leaving the project with *no*
perf gate, which is strictly worse than an honest advisory one (s11 §8's
reasoning; this is the sprint that acts on it).

**The design.**

| Lane class | What runs | What gates |
|---|---|---|
| `perf-x86_64-linux-gnu` (designated, pinned host) | the quick suite on trunk and the 1 GiB extension nightly/release, `PERF_GATE=1` | absolute budgets **and** relative regressions vs the committed baseline |
| `perf-arm64-linux` (designated) | the same quick + nightly/release split, `PERF_GATE=1` | same, against its **own** reference vector and baseline file |
| every other lane (gcc, clang, sanitize, modules, macos, musl) | the full suite, `YEW_PERF_ADVISORY=1` | **functional only**: every metric must run to completion, produce a finite nonzero number within its sanity range, and every *deterministic* row (byte counts, frame counts, allocation counts) is a hard failure exactly as today |

Functional-mode sanity ranges exist so the advisory lane still has teeth: a
metric returning 0 ns, `INT64_MAX`, or > 100× its budget is a **hard**
failure everywhere, because that is a broken harness or a broken feature,
not noise. A metric at 3× budget on a noisy runner is a warning line and
nothing more.

**The calibration microbenchmark — `tests/perf/calib.c`, `make calib`.**
Three components, because our budgets are not dominated by one resource:

| # | Component | Workload | What it tracks |
|---|---|---|---|
| C1 | pointer chase | 4 MiB shuffled cyclic permutation of 64-bit indices, 2 M dependent loads | memory/L2–L3 latency — what the s05 diff's 391 KiB working set pays |
| C2 | scalar mix | branchy integer kernel: 4 M iterations of xorshift + compare + unpredictable branch | IPC and branch prediction — the dispatch and regex paths |
| C3 | bandwidth | `memcpy` 64 KiB → 64 KiB, 16 k times | the grid flip, materialize, and load paths |

- No I/O, no syscalls in the measured region, no allocation after warmup,
  single-threaded (invariant 8). Each component: 3 warmups discarded, then
  11 runs, **median** (s33's method, reused).
- The frozen reference vector is committed at
  `tests/perf/calib-reference.txt` and the reference machine is **written
  down in prose in that file** — CPU model, core count, memory type and
  speed, kernel version, libc, compiler and version, governor, and whether
  SMT was enabled. A calibration whose reference machine is undocumented is
  a magic number, and magic numbers get "fixed" by whoever the gate annoys.

```
# yew calibration reference v1
# machine: AMD Ryzen 7 5800X (8c/16t, SMT on), 32 GiB DDR4-3600 CL16,
#          Linux 6.9.7, glibc 2.39, gcc 14.1.0, governor=performance,
#          ASLR on (default), measured 2026-08-xx, 11-run medians
c1_chase_ns      184200000
c2_scalar_ns      41800000
c3_bandwidth_ns   96300000
```

Scale, integer fixed point, permille — one number per runner:

```c
/* Geometric-mean-free integer combination: equal weights, integer only.
 * A float here would drag in LC_NUMERIC on the print path (s25) and
 * round differently between gcc and clang (invariant 5).              */
u32 yew_calib_scale_permille(const CalibVec *m, const CalibVec *ref) {
    u64 s = 0;
    s += (u64)m->c1 * 1000 / ref->c1;
    s += (u64)m->c2 * 1000 / ref->c2;
    s += (u64)m->c3 * 1000 / ref->c3;
    return (u32)(s / 3);                 /* 1000 == reference speed     */
}
```

A calibrated budget becomes a wall-clock limit as
`limit_ns = budget_ns * scale_permille / 1000`. Budgets in
`tests/perf/budgets.txt` are written in **calibrated units** (reference
nanoseconds) and the runner does the conversion, so the quality-bar numbers
in `00-decisions.md` appear literally in the file.

`make calib` writes `build/calib.txt` and caches it keyed by
`(runner-id, kernel-release, cpu-model)`; the perf lane re-measures at the
**start and end** of every run.

**The limits, plainly.**

1. **One scalar cannot describe three axes.** A machine 30 % faster at
   pointer chasing and 20 % slower at memcpy gets one middling scale, and at
   least one of its budgets is then wrong. Mitigated only by refusing to
   gate widely: two pinned hosts ever compare wall clocks.
2. **Syscall, page-fault and filesystem costs are not modelled at all.**
   C1–C3 touch none of them, so the **100 MB open**, the **atomic save** and
   **cold start → first paint** are *not* calibrated — gates on the
   designated runners only, advisory everywhere else, full stop. Scaling a
   `read(2)`-dominated number by a memory-latency ratio would be fiction
   with a decimal point on it.
3. **Calibration is measured before the run, not during it**, so a
   neighbour arriving mid-run is invisible. Mitigation: the end-of-run
   re-measurement. Scales differing by more than **15 %** make the lane exit
   `perf: runner unstable (scale 1042 → 1310); no verdict` with a distinct
   status — *no result*, not a pass. A lane that says "I could not measure
   this" beats one that says green for the wrong reason.
4. **Refusal band.** `scale_permille` outside `[500, 3000]` disqualifies the
   runner from gating: below 500 the calibration is measuring something
   other than what we think (a cached page, a broken clock); above 3000 a
   5 ms budget maps past 15 ms, where scheduler quanta dominate.
5. **A scale factor never crosses an ISA.** arm64 gets its own reference
   vector and baselines; cross-architecture scaling would silently absorb a
   missing NEON path into "that machine is 1.4× slower".
6. **It does not make a laptop a gate.** `make perf` on an undesignated host
   prints the scale, the word `ADVISORY`, and the baseline line it *would*
   have committed (s11 §8 rule 6), exiting 0 on relative regressions.
   Thermal throttling moves p99 more than every optimization in this sprint.

### 5. The gate suite — `tests/perf/`

```
tests/perf/
  calib.c  calib-reference.txt  calib-reference-arm64.txt
  budgets.txt                         calibrated units, the single source
  perf_latency.c                      §5.1 keypress → paint (pty)
  perf_startup.c                      §5.2 cold start, exec → first paint
  perf_open.c                         §5.3 100 MB open, end to end
  perf_search.c                       §5.5 search on 1 GiB
  perf_mem.c                          §5.6 getrusage checkpoints
  perf_render.c  scroll.c  perf_syn.c  perf_textbuf.c  perf_fletch.c
                                      (s05/s15/s41/s11/s33 — unchanged,
                                       re-pointed at budgets.txt)
  sessions/*.keys                     scripted keystroke sessions
  fixtures/  fixtures.sha             (s11's generator + hashes)
  baselines/<runner-id>.txt           committed
```

**Baseline path unification.** s11 and s33 write `tests/perf/baselines/`
(plural); s05 and s41 wrote `tests/perf/baselines/` (singular). This sprint
moves the two singular files into `baselines/` in one mechanical commit that
touches no `src/`, and `bans.sh` gains a check that
`tests/perf/baselines/` does not exist. Two spellings of a directory is how a
baseline gets edited in the copy nobody reads.

#### 5.1 Keypress → paint p99 — `perf_latency.c`

Measured **from outside the process**, because that is the number the user
experiences. Method, on top of the s06 harness:

1. `yew_pty_spawn` with the constructed envp (s06 §2), profile `modern`,
   200×50, `YEW_PROF` **unset** (the gate measures the shipping build).
2. Wait for the probe to reach DONE and for `ptc_settle` quiet — key timing
   must not race capability negotiation.
3. Per key: stamp `t0 = yew_now_ns()` **immediately** before the `write(2)`
   of that key's bytes to the master, read the master until `vt_feed`
   reports a completed frame, stamp `t1` at the read that completed it.
   Sample = `t1 − t0`.
4. Frame-completion signal: under `modern` the ESU (`CSI ?2026l`); under
   `nosync` the frame's final cursor-position sequence plus a ≥ 1 ms quiet
   gap. Both pinned in `vt.h` as `vt_frame_complete(VtScreen*)` so the
   harness never guesses.
5. **Zero-output frames are not samples.** A key that changes nothing (`Esc`
   in L with nothing pending) emits 0 bytes and has no paint to wait for;
   those are counted separately as `no_paint`. Recording them as 0 µs would
   deflate p99 by whatever fraction of the session is no-ops.

Pitfall — **the harness has its own floor.** `perf_latency --floor` measures
the pty round trip with a trivial echo child (`tests/perf/echo_child.c`:
read a byte, write BSU + a cell + ESU) and reports `pty_floor_ns`, typically
15–30 µs. **Reported, never subtracted**: 25 µs is 0.5 % of the budget, and
subtracting a noisy constant from a p99 is false precision. The only
assertion is that the measured keypress p99 exceeds `pty_floor_p50` — if it
did not, the harness would be measuring itself.

Session scripts, committed, one key spelling per line in s06's `ptc_keys`
vocabulary, so one file drives kitty and legacy encodings alike:

```
# tests/perf/sessions/typing.keys — 10000 keys, 92% insert
i
h e l l o space w o r l d
esc
...
```

| Session | Shape | Keys |
|---|---|---|
| `typing.keys` | I mode, ASCII prose with newlines every ~70 chars | 10 000 |
| `navigate.keys` | L/W/B motions, page jumps, `gg`/`G`-class extremes | 10 000 |
| `edit.keys` | mixed: motion, delete, undo, redo, paste from register | 10 000 |
| `multicursor.keys` | H-mode selection → 40 cursors → simultaneous insert | 10 000 |
| `search.keys` | `/` prompt, incremental match, `n`/`N` cycling | 10 000 |

Fixture classes — every session runs against every class that applies:

| Class | Fixture | Why it is here |
|---|---|---|
| `small` | 400-line C file | the common case; nothing may regress it |
| `huge` | `100m-code` (s11's generator) | piece-tree depth, line index, no accidental linear scans |
| `syntax` | `c_kitchen.c` + `c_comment_bomb.c` (s41's frozen fixtures) | `YEW_PH_SYN` under a pathological state chain |
| `many-buffers` | a **50-buffer** workspace, 20 hydrated, 30 deferred (s24 lazy hydration) | per-frame work that scales with buffer count is the classic slow leak |
| `assist` | LSP mock (s46/47) + AI ghost text against the **s49 mock backend**, both live, ghost text visible | `YEW_PH_JOBS` inside the keystroke frame; s43's staleness law under load |

| Metric | Gate | Calibrated | Lane |
|---|---|---|---|
| keypress → paint p99, `typing` × `small` | **≤ 5 ms** | yes | designated |
| … `typing` × `huge` | ≤ 5 ms | yes | designated |
| … `edit` × `syntax` | ≤ 5 ms | yes | designated |
| … `multicursor` × `small` | ≤ 5 ms | yes | designated |
| … `navigate` × `many-buffers` | ≤ 5 ms | yes | designated |
| … `search` × `huge` | ≤ 5 ms | yes | designated |
| … `typing` × `assist` | ≤ 5 ms | yes | designated |
| keypress → paint **max** (any session) | ≤ 40 ms, informational + logged with its frame | no | all |
| `no_paint` fraction, `typing` | recorded | — | all |
| frames per 10 000 keys, `typing` | ≤ 10 000 **hard, deterministic** | no | **all** |

That last row survives a noisy runner: s14's drain-then-render rule makes a
burst one frame, so frame count is a *deterministic* property of the
dispatch logic. A regression that renders per keystroke fails the clang lane
before anyone looks at a percentile.

**Cross-check that makes the instrumentation trustworthy.** Each session runs
a second time with `YEW_PROF=1` and `:prof dump`; the report's `KEYPAINT` p99
(`TOTAL` for frames with exactly one decoded key and nonzero terminal output)
must agree with the external p99 within **25 %**, and its call count must
equal the harness's painted-key sample count. Aggregate `TOTAL` intentionally
includes timer/job/background wakes and is not the same population. A larger
gap means either a phase is unmeasured (fix §1's call sites) or the pty path
lies (fix the harness). DoD 6.

#### 5.2 Cold start → first paint — `perf_startup.c`

`t0` is stamped in the parent immediately before `fork()` in
`yew_pty_spawn`; `t1` is the read delivering the first byte of the first
**BSU** (`CSI ?2026h`) under `modern`, or the first CUP under
`nosync`/`dumb` — both markers pinned in `vt.h`. The `fork`+`execve` cost is
in the number and we do not pretend otherwise: `tests/perf/nullexec.c`
(write one byte, `_exit(0)`) is spawned identically to give
`spawn_floor_ns`, the report prints `first_paint`, `spawn_floor` and their
difference, and the **gate is on the raw `first_paint`** — a user's shell
pays the fork too.

| Metric | Gate |
|---|---|
| exec → first paint, default config, empty buffer | **≤ 20 ms** |
| exec → first paint, `--clean` (no user config) | ≤ 20 ms |
| delta (default − `--clean`) | recorded; cross-checks s33's `config_load < 1 ms` gate |
| exec → first paint, default config, 50-buffer workspace resume (s25) | ≤ 20 ms — proves deferred tabs actually defer |
| exec → first paint, `--batch` (no tty) → script start | recorded |
| `spawn_floor` | recorded, must be < 30 % of the measured first paint |

Pitfall, and a **gate row rather than a note**: first paint must not wait on
the capability probe (s14's startup order pins this), so the `dumb` profile
— harness silent, s03's probe timing out — must produce the *same*
first-paint number as `modern`. If it does not, the 500 ms
`YEW_PROBE_TIMEOUT_MS` appears as a 500 ms cold start on every real terminal
that ignores DECRQM.

#### 5.3 The 100 MB open — `perf_open.c`

s11 §7 gates the *in-process* load + line index at ≤ 150 ms; this sprint
adds the end-to-end number `00-decisions.md` actually promises —
`exec → interactive`, meaning first paint with the file's first screen
rendered and the cursor placed, measured through the pty as in §5.2.

| Metric | Fixture | Gate |
|---|---|---|
| exec → interactive | `100m-code` | **≤ 150 ms** |
| exec → interactive | `100m-utf8` | ≤ 150 ms |
| exec → interactive | `100m-allnl` | recorded, **no gate** (s07/s11's documented degenerate case) |
| first keypress → paint after that open | `100m-code` | ≤ 5 ms |

s11's promise that `100m-allnl` is "revisited only in Sprint 56" is
discharged here: **the exemption stands.** 8 bytes of line index per 1 byte
of line is inherent to the structure and the alternatives were weighed in
s07; it stays informational, stays in the baseline so a *regression* is
visible, and `00-decisions.md`'s 1.6× bar is documented as applying to files
with realistic line lengths.

#### 5.4 Scroll throughput

s15's `tests/perf/scroll.c` gate (≥ 120 fps equivalent full-viewport redraw,
both wrap modes, 80×24 and 200×60) is re-run with the whole stack in the
frame — highlighting, gutter, statusline — against `md_kitchen.md` (s41)
and a plain 10 kLOC file. 120 fps equivalent is **8.3 ms per frame**; the
phase shares are recorded from a `YEW_PROF=1` companion run, and s05's
25 %-of-frame renderer slice and s41's 18 % syn slice are asserted as
*shares*, not only as absolutes.

#### 5.5 Search latency on 1 GiB — `perf_search.c`

| Pattern class | Example | Fixture | Gate |
|---|---|---|---|
| literal, present early | `needle` at 1 % | `1g-code` | first match ≤ 20 ms |
| literal, absent | `zzqqxx` | `1g-code` | full scan ≤ **2.0 s** |
| literal, no-newline file | `needle` | `1g-noline` | full scan ≤ 2.0 s — proves no per-line materialize |
| regex, anchored simple | `^static ` | `1g-code` | full scan ≤ 4.0 s |
| regex, alternation | `\b(foo\|bar\|baz)_[0-9]+\b` | `1g-code` | full scan ≤ 6.0 s |
| regex, backtracker-hostile | `(a+)+b` on 64 KiB of `a` | synthetic | ≤ 50 ms — s20's no-backtracking guarantee, expressed as a budget |
| incremental search keystroke | `/nee` → `/need` → `/needle` | `1g-code` | each keystroke ≤ **5 ms** to first on-screen match |

The last row is the one that matters for the quality bar: incremental search
must never scan the whole gigabyte synchronously inside a keystroke frame.
It searches forward from the viewport under a bounded budget and continues
next frame; the gate is what proves it.

#### 5.6 Memory ceilings — `perf_mem.c` and `getrusage` checkpoints

```c
u64 yew_rss_bytes(void);       /* src/util/rss.c — the ONE #ifdef site  */
u64 yew_rss_peak_bytes(void);  /* getrusage(RUSAGE_SELF).ru_maxrss      */
```

**Pitfall, carried from s11 §7 and now shipped in `src/`:** `ru_maxrss` is
KiB on Linux and **bytes** on macOS — one function, one `#ifdef`, or the
arm64-macos lane reports a 1000× regression on its first green run.
`yew_rss_bytes` reads the precise `Rss:` total from
`/proc/self/smaps_rollup` on Linux for *current* RSS, falling back to
`/proc/self/statm` on older kernels and to `ru_maxrss` elsewhere.  The
precise source matters for delta gates: Linux documents the scalable
`statm` RSS counters as asynchronous, which can move pre-paint pages into a
later checkpoint.  Current RSS remains unavailable on macOS, so the
after-close row below is Linux-only.

Checkpoints, emitted at INFO under `YEW_PROF=1` and asserted by the suite:

| Checkpoint | Where |
|---|---|
| `argv` | after `yew_args_parse`, before any I/O |
| `config` | after config load (s36) |
| `loaded` | after the first `yew_file_load` |
| `paint` | after the first `yew_render_frame` + write |
| `session` | after a 10 000-keystroke session |
| `closed` | after closing every buffer and running one GC (s30) |

| Metric | Fixture | Gate |
|---|---|---|
| peak RSS after clean open | `100m-code` | **≤ 1.6× file size** (160 MB) |
| peak RSS after clean open | `100m-utf8` | ≤ 1.6× |
| peak RSS at `paint`, empty buffer, default config | — | ≤ 12 MiB |
| peak RSS at `paint`, `--clean`, empty buffer | — | ≤ 8 MiB |
| peak RSS, 50-buffer workspace resume | `many-buffers` | ≤ 40 MiB — deferred tabs, so 30 buffers cost nothing |
| RSS growth across the 10 000-key `typing` session | `small` | ≤ 3 MiB — S59-A1 admits one observed 2 MiB Linux residency quantum plus sub-0.5 MiB retained growth |
| RSS at `closed` − RSS at `config` (Linux only) | `100m-code` | ≤ 4 MiB — the closed-buffer leak check |
| peak RSS, `assist` class with LSP + AI mocks live | `small` | ≤ 24 MiB (our side only; the servers are separate processes) |

The `typing` growth row is the one that catches a per-keystroke allocation
that never frees; Sprint 57's per-call-site allocation counter is what
*names* the offender once this row goes red.

### 6. Committed baselines and the regression-tolerance policy

**s11 §8 is the policy. It is reused, not rewritten.** Restated compactly
so this file is self-sufficient, with this sprint's two additions marked:

1. An absolute timing-budget violation is a hard failure on a designated
   runner. Elsewhere it warns, unless it exceeds the functional sanity
   ceiling of 100× the budget. Deterministic rows remain hard everywhere,
   as §4 specifies; shared hosted timing is never promoted into a verdict.
2. Relative regression: p99 more than **10 %** above baseline fails, only on
   the designated runner; elsewhere it warns (`YEW_PERF_ADVISORY=1`).
3. Anti-flap: each metric measured **3 times per run**, the **median of the
   three p99s** compared, and a metric must exceed the threshold in **2 of
   3** runs to fail. Single-sample gates flap, get muted, and then the
   project has no gate.
4. Improvements never auto-update: > 20 % below baseline prints "rebaseline
   me" and nothing else.
5. Rebaselining is its own commit — `make perf-update`, message starting
   `perf: rebaseline`, listing deltas — and `perf-baseline-guard` **fails
   any commit touching both `tests/perf/baselines/` and `src/`**, so a
   regression cannot be laundered through the diff that caused it.
6. Unknown runner id → advisory only, printing the line it would commit.
7. **New here:** every row carries a `why` (s33's rule, extended to all
   baselines); the checker rejects a `why` that is empty or still says
   `s11 initial`/`s33 initial` when the number moved by more than 5 %.
8. **New here:** the header records `scale_permille` and the calib vector.
   A baseline taken at scale 1180 compared against a run at 1450 is refused
   with `perf: calibration drift 1180 → 1450; rebaseline or fix the
   runner` — a silently rehosted runner must not look like a regression.

```
# yew perf baseline v2  runner=perf-x86_64-linux-gnu
# calib scale_permille=1042 c1=191900000 c2=43100000 c3=99400000
# metric                              p50_ns      p99_ns    rss_bytes  why
latency.typing.small                  704000     3110000            0  s56 initial
latency.typing.assist                 981000     4402000            0  s56 initial
startup.first_paint.default          9420000    14880000            0  s56 initial
open.100m-code.e2e                 121400000   139200000    148897792  s56 initial
search.literal_absent.1g-code     1402000000  1611000000            0  s56 initial
mem.session_growth.small                   0           0      1441792  s56 initial
```

### 7. Triage playbook — symptom → phase → the tool that shows it

Committed as `tests/perf/README` and reproduced here because it is the
sprint's actual product for the next five years.

| Symptom | Likely phase | First look | Then |
|---|---|---|---|
| Typing feels laggy only in one file | `YEW_PH_SYN` | `:prof` share column | `yew syn check --coverage`; is a rule backtracking? s41's per-line p99 row |
| Laggy in every file, worse on big ones | `YEW_PH_DISPATCH` | `:prof` p99 + worst frames | callgrind for call counts — an accidental O(n) line walk |
| Laggy only with LSP on | `YEW_PH_JOBS` | `:prof frames` around the spike | s46 generation counters: is a stale response being applied? |
| Laggy only after an hour | `YEW_PH_*` all up, `unaccounted` up | RSS checkpoints | Sprint 57's per-site allocator; fragmentation or an unbounded cache |
| Frame count > keystroke count | dispatch/loop | the deterministic frames-per-key row | s14's drain-then-render order was broken |
| High `unaccounted` | not instrumented | add a `yew_prof_phase` boundary | `perf record -g` to find which function owns it |
| Nonzero `poll` inside a burst | input drain | `:prof frames` | the read loop is exiting early — `YEW_INPUT_BURST_MAX` or an `EAGAIN` mishandled |
| First paint slow, only on some terminals | startup | `perf_startup` under the `dumb` profile | something is blocking on s03's probe |
| First paint slow everywhere | startup | `--clean` delta | config load (s33's `config_load` gate) or eager file work |
| Scroll stutters, typing fine | `YEW_PH_RENDER` | s05 byte-count rows | damage widened to whole rows; or `CSI K` suppressed |
| Big paste freezes for a second | input + dispatch | `YEW_PF_BURST_CAP` flag on the frame | burst cap working as designed, or coalescing missing in s07 |
| Memory 3× file size | — | `perf_mem` checkpoints | massif for heap shape; is a snapshot (s07 `TextSnap`) unreleased? |
| Slow only under `MODULES=""` | build config | `make MODULES="" perf` | a shim doing work instead of erroring (invariant 3) |
| Green locally, red in CI, no diff | runner | the calib scale in the failure output | limit 3 or 4 of §4 — read the "runner unstable" line before touching code |

### 8. CI wiring and Make targets

| Target | Runs | Where |
|---|---|---|
| `make calib` | §4's microbench, writes `build/calib.txt` | everywhere |
| `make perf` | the 100 MiB-class suite, all of §5 except the 1 GiB rows | every PR lane (advisory) + designated (gating) |
| `make perf PERF_GATE=1` | as above, gates enabled | designated runners only |
| `make perf-huge` | the 1 GiB rows (§5.5, s11's 1 GiB set) | nightly + release |
| `make perf-symbols` | `build-prof/` with frame pointers | manual |
| `make perf-update` | rewrite baselines for this runner | manual, own commit |
| `make bench-fletch PERF_GATE=1` | s33's suite, re-baselined here | designated |

CI gains `perf-x86_64-linux-gnu` and `perf-arm64-linux` as required lanes on
`trunk` and nightly, advisory on fork PRs (no designated-runner access).
Every other lane gains `make perf YEW_PERF_ADVISORY=1` — the suite must
*run* everywhere or it rots (s33's rule).

### 9. Defer

- **Binary-size budgets per `MODULES`, the size ledger, the allocation
  audit, the debug allocator, the musl static embedded profile** →
  Sprint 57; `make size` hard-errors naming it. Adversarial audits and
  extended fuzz → 58; reproducible releases and packaging → 60.
- Threading or any parallel render/parse path → **never in 1.0**
  (invariant 8). If a budget cannot be met single-threaded, the fix is the
  algorithm; `:prof` exists to find which one.
- `s41_5`'s embedded-language dispatch carries its own latency hazard
  (s41 §6 reason 2) and gets its budget row there, not here. Per-plugin
  latency accounting (s54) rides in `YEW_PH_JOBS`; a per-plugin breakdown
  is post-1.0.

## Testing Strategy

- **Unit** (`test_prof.c`): `sizeof(ProfFrame) == 64`; ring wrap at `cap+1`
  keeps exactly `cap` and reports `dropped`; a phase entered twice
  accumulates; `frame_end` closes an unclosed phase; `unaccounted` equals a
  deliberately planted gap exactly; `yew_prof_reset` empties without
  freeing; `YEW_PROF` unset leaves `ring == NULL` and every entry a no-op.
- **Unit** (`test_prof_format.c`): `yew_prof_write` over a **synthetic**
  ring of 200 hand-built frames, byte-compared against a committed golden —
  no timing in the test at all. Percentiles checked against hand-computed
  values for n = 1, 2, 3, 100, 4096; `grep -n '%f\|%g\|%e' src/util/prof.c`
  empty.
- **Unit** (`test_calib.c`): `yew_calib_scale_permille` over ≥ 12 vectors
  including the reference itself (→ 1000) and a uniform 2× machine
  (→ 2000); the refusal band at 499/500/3000/3001; drift at 14/15/16 %.
- **Pty**: `prof_report_golden` — `YEW_PROF=1`, `:prof`, snapshot the
  scratch buffer's *structure* with digits masked (`--mask-digits`
  snapshot mode) so the UI is golden-tested without pinning timings;
  `prof_off_message` — the exact refusal with `YEW_PROF` unset.
- **Script** (`tests/script/prof_*.fl`): `--batch` with `YEW_PROF=1` writes
  to `YEW_PROF_OUT`, the file parses, and the batch phase-collapse holds
  (`render`/`write` report 0 calls).
- **Harness self-check**: `perf_latency --floor` must report a
  `pty_floor_p50` in `[2 µs, 500 µs]`; outside that the suite exits nonzero
  before measuring anything.
- **Gate-has-teeth drill** (s11 DoD 11's shape): three scratch commits, each
  reverted — (a) `usleep(2000)` in `yew_dispatch_key` fails `latency.*`;
  (b) a render moved inside the key loop fails the **deterministic**
  frames-per-key row on every lane; (c) a 5 % slowdown does **not** fail.
  Plus five consecutive unmodified designated-runner runs, zero failures.
- **Determinism lane**: the formatter golden and the `.keys` scripts are
  byte-stable across two runs; timing rows are excluded from the diff by an
  explicit list, and `bans.sh` rejects a wildcard exclusion.
- **Sanitizers**: `test_prof`, `test_calib`, `perf_latency --floor` under
  ASan/UBSan; valgrind on the prof unit binary (the ring is arena-allocated,
  so a leak there means the arena is wrong).
- **Advisory-mode proof**: the gcc lane runs the full suite with
  `YEW_PERF_ADVISORY=1` and stays green under drill (a) while the
  deterministic rows still fail. That asymmetry is the design; it gets a
  test.

## Definition of Done

1. `make && make test && make perf` green on gcc and clang, zero warnings
   under the locked flags; ASan/UBSan, valgrind, determinism, bans green.
2. `sizeof(ProfFrame) == 64` asserts at compile time; the complete measured
   profiler phase-clock/ring path costs ≤ **2 %** of external keypress p99
   over the 10 000-key `typing` session; with `YEW_PROF` unset the ring is
   never allocated.
3. `:prof`, `:prof reset`, `:prof dump`, `:prof mark`, `:prof frames` are
   `ed.prof.*` commands reachable from E mode, a key and Fletch; all five
   refuse with the pinned message when profiling is off. The report golden
   is byte-identical across two runs from a synthetic ring, and no float
   formatting exists in `src/util/prof.c`.
4. `make calib` produces a scale on every lane; both reference vectors are
   committed **with their machines described in prose**; the refusal band
   and the 15 % drift abort fire under unit test and a seeded CI vector.
5. Every §5 row is within budget on the designated runner: keypress p99
   ≤ 5 ms on all seven session × class combinations; cold start ≤ 20 ms with
   and without `--clean` **and under the `dumb` profile**; 100 MB open
   ≤ 150 ms end to end; scroll ≥ 120 fps equivalent with highlighting on;
   incremental search ≤ 5 ms/keystroke on 1 GiB; every RSS row inside its
   ceiling.
6. The external-vs-`YEW_PROF` p99 cross-check agrees within 25 % on all
   seven combinations after subtracting the contemporaneous median PTY echo
   floor from the external endpoint, and `perf_latency --floor` reports a
   plausible `pty_floor_p50`.
7. Frames-per-10 000-keys ≤ 10 000 is a **hard** gate on every lane, proven
   to fail when a render is moved inside the key loop.
8. Advisory mode proven: a seeded 2 ms slowdown fails the designated lane
   and passes (warning only) on the gcc lane, while the deterministic rows
   fail on both.
9. Baselines committed for both designated runners in `tests/perf/baselines/`
   with the v2 header and a `why` per row; `tests/perf/baselines/` (singular)
   is gone and `bans.sh` rejects it; s05's and s41's rows live in the new
   home.
10. Anti-flap proven exactly as s11 §8 specifies: median-of-3 with
    2-of-3-to-fail, five consecutive unmodified runs with zero failures, a
    seeded 5 % slowdown passing, a seeded 20 % failing, and
    `perf-baseline-guard` rejecting a mixed `src/` + baseline commit.
11. `tests/perf/sessions/` holds five committed 10 000-key `.keys` scripts;
    every §5.1 fixture class is exercised, and the `assist` class runs
    against the s46/47 and s49 mocks with ghost text asserted present in the
    VT screen, not assumed.
12. `tests/perf/README` carries the §7 triage table and the "read the phase
    table before reaching for perf" rule; `make perf-symbols` builds a
    disjoint `build-prof/` tree.
13. The `100m-allnl` exemption is discharged in writing per §5.3 — s11's
    "revisited in Sprint 56" resolves to a decision, not another deferral.
14. `make size` hard-errors naming Sprint 57; ≥ 140 assertions across
    `test_prof.c`, `test_prof_format.c` and `test_calib.c`.
