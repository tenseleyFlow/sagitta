# yew — Locked Decisions

Every decision below is settled. Reopening one requires explicit discussion
and an update to this file. Decided 2026-07-31 during the planning stage.

## Identity

| Decision | Choice |
|---|---|
| Project / binary | `yew`; no legacy binary alias |
| Scripting language | **Fletch** — fletching steers arrows; Fletch steers cursors. Files: `*.fl` |
| License | GPL-3.0-only (family-uniform with Cgfried/ARMFORTAS/afs-*) |
| Version arc | empty repo → **v1.0.0** (unlike Cgfried's v0.1.0 arc — the user asked for 1.0.0) |
| Prefixes | `yew_*` C symbols, `YEW_*` env vars; Fletch subsystem uses `fl_*` symbols |
| Branching | `trunk` default; terse imperative commits, no co-authors, no trailers |

## Implementation stack

| Decision | Choice |
|---|---|
| Language | C11 subset: C99 core + `_Static_assert`, anonymous struct/union, `alignas`. No VLAs, no `__attribute__`, no statement expressions in our own source |
| Threads | **None in core.** Single-threaded `poll(2)` event loop; parallelism comes from subprocesses (LSP servers, shell jobs, curl). Revisit post-1.0 only |
| Dependencies | C stdlib + POSIX only. No ncurses, no libgit2, no TLS lib, no regex lib, no JSON lib — all bespoke. Adding a dependency requires discussion + CLAUDE.md update + justification |
| Optional runtime tools | `git` (FUSS mode), `$SHELL` (E mode), `curl` (cloud AI only), `fish` (`:!` completion oracle, S57.26-A1). All degrade gracefully when absent |
| Build system | One hand-written GNU Makefile. No autotools/cmake/meson, ever |
| Compilers | gcc and clang, zero warnings under `-std=c11 -pedantic -Wall -Wextra -Werror` |
| Feature modules | Compile-time: `MODULES="lsp ai fuss plugins"` (all on by default). Core (modal editing, Fletch, highlighting, search, workspace) is not excisable. Excluded modules hard-error with a clear message, never silently no-op |
| Targets | Closed set: `x86_64-linux-gnu` (first), `arm64-linux`, `x86_64-linux-musl` (static, the embedded profile), `arm64-macos`. FreeBSD post-1.0 |

## Editor core

| Decision | Choice |
|---|---|
| Buffer structure | **Piece tree**: original buffer (read fully into memory; no mmap — a file truncated under an mmap is a SIGBUS, and no-corruption outranks cleverness) + append-only add buffer + balanced tree of pieces carrying byte *and* line counts. O(log n) edit/lookup, natural undo, cheap snapshots |
| Undo | Operation-log undo **tree** (not a stack) over the piece table; transactions group multi-cursor/macro edits into one undo step; serialized into workspace state |
| Encoding | UTF-8 internal and on disk. Bespoke decoder, grapheme-cluster segmentation, East-Asian-width + emoji width from generated (checked-in) Unicode tables. Invalid bytes preserved round-trip via lossless escapes — never mangled. CRLF/BOM detected, preserved, indicated in statusline |
| Saving | Atomic always: write temp in same dir, `fsync`, `rename`, `fsync` dir. Fallback to in-place write-with-backup when the dir is unwritable or the file is a symlink/hardlink that must be preserved. Crash journal for unsaved changes |
| Terminal | Bespoke: termios raw mode, ANSI/VT output, truecolor with 256/16 fallback, kitty keyboard protocol (negotiated), bracketed paste, SGR mouse, focus events, synchronized output (mode 2026). Damage-tracked cell-grid double buffer — O(damage) renders, never full repaints per keystroke |
| Terminal restore | Guaranteed on every exit path: normal quit, panic handler, fatal signals. A crashed yew never leaves the terminal raw |
| Search | Bespoke regex: Thompson NFA/DFA hybrid, no backtracking (no catastrophic blowup — speed first), literal fast path via memchr/Boyer-Moore-Horspool |
| Clipboard | Registers internal; system clipboard via OSC 52 (bespoke, zero deps); optional xclip/wl-copy/pbcopy subprocess fallback |
| Modes | L (home), W, B unit modes; H, I, E, F action modes; Esc always returns to L. See overview.md MODES |
| Workspace state | Dir = workspace, keyed by realpath hash under `$XDG_STATE_HOME/yew/workspaces/` (never pollutes the repo). Format: Fletch data literals (dogfood), atomic writes, versioned schema |
| Config | `$XDG_CONFIG_HOME/yew/init.fl` + optional workspace `.yew.fl` (trusted-dir prompt on first load). Ships with a good default config; `yew --clean` ignores user config |
| Tab groups | Facsimile model verbatim: groups hold **no member lists**; tabs carry `group_id` + `group_ordinal`; stable `tab_id`s; membership computed, never cached; `last_active_member` is a path; empty groups auto-dissolve; lazy hydration (deferred tabs, buffer allocated on first view) |
| Mouse | Supported (click, drag, scroll, resize) via a clickable-region registry populated by render passes — rendering and hit-testing share one source of truth. Keyboard never requires it |
| Layout | Layout computes spans in cells; drawing consumes them — multibyte text can never desync clicks from glyphs |

## Fletch

| Decision | Choice |
|---|---|
| Flavor | Hybrid: first-class terse motion primitives (recorder output **is** valid Fletch source) embedded in a real language — variables, functions, loops, conditionals, modules |
| Roles | One language for everything: config, keybindings, macros, plugins, syntax definitions, workspace state data, headless batch scripts |
| Implementation | Bespoke lexer → parser → bytecode compiler → VM (switch dispatch; computed-goto behind a feature test). Tagged-union values, interned strings, mark-sweep GC with explicit roots |
| Value types | nil, bool, int (i64), float (f64), string, list, map, function/closure, and editor handles: buffer, cursor, span, window, regex |
| Headless mode | `yew --batch script.fl files…` — batch editing without a terminal (the good version of `vim -es`) |
| Non-goals for 1.0 | No JIT, no FFI/native plugins, no threads in the VM |

## Feature modules

| Decision | Choice |
|---|---|
| Highlighting | Bespoke incremental line-state-machine engine (built-in, not a module). Definitions are declarative Fletch data compiled to tables. Ships ≥ 12 languages + default dark/light themes |
| Shadow completions | Three providers, one ghost-text UX with distinct provenance styling: (1) buffer/workspace symbol index (no LSP needed), (2) LSP, (3) AI. Accept by word / line / all |
| LSP | Bespoke JSON + JSON-RPC over stdio pipes. Incremental didChange. Diagnostics, completion, hover, goto-def, references, rename, symbols for 1.0 |
| AI | Bespoke HTTP/1.1 client for localhost (ollama etc.) — no TLS needed; cloud (OpenAI-compatible + Anthropic APIs) via optional `curl` subprocess. Off by default, explicit opt-in, documented redaction/privacy defaults |
| FUSS mode | Port of fuss's design: dirty-files-first tree, status glyphs (green `↑` staged, red `✗` modified, grey `✗` untracked, blue `↓` incoming), depth-aware sibling navigation, type-to-fuzzy-jump (fuss scoring), git verbs, mode recolors the footer. All git via subprocess |
| Plugins | Fletch-only for 1.0 (small binary, no ABI hazard). Discovery under XDG dirs, event-hook API, capability gating (fs/shell/net access prompts). `yew pkg` git-based installer. Cloud-storage workflow ships as a preset config, not code |

## Quality bars (CI-enforced, numeric)

| Gate | Budget |
|---|---|
| Keypress → paint | p99 ≤ 5 ms (reference hw, warm) |
| Cold start → first paint | ≤ 20 ms, default config |
| Open 100 MB file | ≤ 150 ms to interactive |
| Scroll throughput | full-viewport redraw ≥ 120 fps equivalent |
| Binary size (full modules, stripped, x86_64) | ≤ 2 MiB; minimal MODULES build ≤ 1,576,960 bytes (S59-A7) |
| Memory | ≤ 1.6× file size for a clean open |

Budgets are locked as *gates* from the sprint that lands each subsystem;
exact reference-hardware calibration happens in Sprint 56.

**Amendment S57-A1 (2026-08-31) — binary-size evidence floor.** The original
planning values were 1.5 MiB full and 900 KiB minimal. Before either value
had ever passed, Sprint 57's pinned GCC/glibc build measured 1,947,336 bytes
full and 1,463,968 bytes minimal. The minimal binary's required `.text` and
`.rodata` alone total 1,277,826 bytes, 356,226 bytes beyond the entire old
file budget before data, relocations, or ELF metadata. A fresh pinned musl
build independently measured 2,049,936 bytes full and 1,546,096 bytes
minimal; its minimal `.text + .rodata` is 1,374,518 bytes, already beyond the
old 1.3 MiB static cap. The table therefore records the first achievable
hard gates for the implemented 1.0 feature matrix. The displaced 1.5 MiB /
900 KiB pair remains a post-1.0 optimization ratchet, not a gate that can be
made green by changing measurement, dropping required features, or silently
enabling Sprint 57's deferred `-Os`/LTO/lazy-langpack work.

**Amendment S57-A5 (2026-09-12) — the musl-minimal gate follows required
core growth.** The last green pre-Sprint-57.12 hosted static build measured
1,562,480 bytes minimal and 2,082,704 bytes full. The pushed Sprint 57.12
candidate measured 1,582,960 bytes minimal and 2,090,896 bytes full. Its
pinned glibc minimal build remains below the general 1.5 MiB gate at
1,488,544 bytes, and its ledger attributes only 3,680 bytes of new object
sections to the required selection, clipboard, and safe job-return work; the
20,480-byte static-file step is predominantly ELF page/layout amplification,
not accidental module retention. Removing those core editor features would
violate the core-preservation stop rule. The musl-minimal gate alone therefore
moves to the next 64 KiB boundary above the measured floor: 1,600 KiB
(1,638,400 bytes). The 2 MiB full gate, 1.5 MiB glibc minimal gate, measurement
recipe, feature matrix, and post-1.0 optimization ratchets are unchanged.

**Amendment S57-A6 (2026-09-12) — the musl-full gate follows required UI
growth.** Amendment S57-A5 measured the full static PIE at 2,090,896 bytes,
only 6,256 bytes below its 2 MiB cap. The completed Sprint 57.13 mouse,
context-menu, and tab work measures 2,115,472 bytes: a 24,576-byte increase.
The pinned glibc full ledger records the same 24,576-byte on-disk increase,
with 17,360 bytes of object-section growth in `core.ui`, and still passes its
unchanged 2 MiB gate at 2,000,584 bytes. This is attributable required-feature
growth, not optional-module retention or a footprint explosion. The musl-full
gate therefore moves to the next 64 KiB boundary above the measured floor:
2,112 KiB (2,162,688 bytes). The glibc full and minimal gates, musl-minimal
gate, measurement recipe, feature matrix, and post-1.0 optimization ratchets
are unchanged.

**Amendment S59-A1 (2026-09-15) — typing RSS growth admits one Linux
residency quantum.** Sprint 59's first designated ARM64 Linux campaign used
the precise `/proc/self/smaps_rollup` checkpoints required by F072. Across
21 identical 10,000-key observations, ordinary paint-to-session RSS growth
was 176,128--466,944 bytes; three observations were exactly 2,097,152 bytes
above that range (2,293,760 or 2,297,856 bytes). The old 2 MiB ceiling
therefore rejected one kernel residency quantum rather than a retained
per-key allocation. The session-leak gate moves to 3 MiB: enough for one
2 MiB step plus the measured sub-0.5 MiB growth, while two steps still fail.
All clean/default/workspace/open footprint gates, every latency gate, and
the editor feature set are unchanged.

**Amendment S59-A2 (2026-09-15) — a noisy relative ratchet falls back to
its hard absolute budget.** F072's first complete 30-run x86_64 campaign at
`c45d7286` found six rows whose one-sided p95 noise met or exceeded the old
blanket 100-permille relative threshold: many-buffer navigation (106),
profiler overhead (1000, solely the integer step from 1 to 2 permille),
default/clean/dumb startup (101/300/112), and first-key paint after a 100 MiB
open (276). Two more rows, syntax render share and Linux closed-buffer RSS
growth, were zero in all 30 runs and therefore have no defined relative
ratio. Every observation remained inside the locked absolute budget: the
noisy-tail values were respectively 208,982 ns / 2 permille / 7,519,766 ns /
3,198,860 ns / 5,613,910 ns / 3,989,479 ns / 0 permille / 0 bytes against
5 ms / 20 permille / 20 ms / 20 ms / 20 ms / 5 ms / 180 permille / 4 MiB.
Those eight rows use `enforcement=budget`: their absolute budget is still
hard on both designated runners and their baseline remains recorded, but no
10-percent relative verdict is issued. All other relative rows retain the
100-permille ratchet; all passed the recomputation.

The same campaign's ARM64 preflight measured null-exec spawn fractions of
292, 349, and 354 permille while raw first paint remained 3.96--4.56 ms,
far below the unchanged 20 ms product budget. The harness-validity ceiling
moves from 300 to 400 permille so a faster editor does not invalidate its own
measurement; the editor still accounts for at least 60 percent of raw first
paint. This changes neither a user-facing latency budget nor any editor
feature.

**Amendment S59-A3 (2026-09-15) — profiler agreement excludes measured ARM
PTY variance.** F072's designated ARM64 campaign stopped when the profiler's
external-versus-internal p99 cross-check reported an aggregate median of 257
permille against the old 250-permille limit. The editor's independently
measured instrumentation overhead remained 1 permille, all 29,703--30,000
painted samples matched the internal call counts exactly, and every
user-facing latency remained inside its absolute budget. Twelve isolated
many-buffer repetitions on the same pinned runner measured external deltas
of 204, 240, 234, 218, 254, 178, 229, 238, 253, 248, 253, and 253 permille:
median 239, range 178--254, with four ordinary observations above the former
cutoff. Replacing the median PTY floor with its p99 was tested and rejected:
the p99 transport tail varied from 225,455 to 266,017 ns beside 74,149--76,530
ns medians and could over-correct the cross-check to 833 permille.

The hard `latency.prof_external_delta` agreement limit therefore moves from
250 to 300 permille. The existing median echo-floor normalization, exact
sample-count equality, median-of-three anti-flap verdict, 20-permille profiler
overhead limit, and `enforcement=all` policy remain unchanged. No editor
latency, startup, memory, size, or feature gate changes. The raw evidence is
retained in `.docs/audits/evidence/F072-prof-crosscheck-arm64.txt`.

**Amendment S59-A4 (2026-09-18) — corrected startup observation retains a
majority-work harness bound.** `5a3ac30c` made the startup harness accept the
legal ordering where yew paints before the terminal answers its synchronized-
update capability query. The old observer discarded that completed frame and
waited for a later synchronization marker, so S59-A2's 400-permille ceiling was
calibrated against a different, late timestamp. On the pinned ARM64 runner, 50
corrected observations measured spawn-floor fractions with p05 281, median
370, p95 407, and maximum 424 permille. First paint measured p05 2.95 ms,
median 3.24 ms, p95 3.98 ms, and maximum 11.62 ms; all observations remained
inside the unchanged 20 ms product budget. A subsequent campaign stopped on
the ordinary triplet 439, 417, and 317 permille (median 417); across its first
twelve observations the maximum was 459 permille.

The harness-validity ceiling therefore moves from 400 to 500 permille. This is
the semantic majority-work boundary: null exec may consume at most half of the
raw first-paint sample, so the measured editor path still accounts for at
least half. The median-of-three verdict, `enforcement=all`, and the 20 ms
user-facing first-paint budget remain unchanged. No editor code or feature is
removed or weakened.

**Amendment S59-A5 (2026-09-21) — three measured-noisy ARM rows retain hard
absolute gates.** F072's complete 30-run campaign on the pinned ARM64 Linux
guest at `c29b3077` passed every individual quick and huge budget run, with
calibration scale 1143 before and 981 after (a 14.2 percent change, inside
the 15 percent refusal boundary). The noise-floor recomputation nevertheless
rejected three relative ratchets. Huge-search p99 had p05 1,108,981 ns,
median 1,224,756 ns, and p95 1,471,495 ns (202 permille one-sided noise),
while every run stayed below its calibrated 5 ms reference budget. Hostile
regex on 64 KiB had p05 74,416 ns, median 83,816 ns, and p95 96,553 ns
(152 permille noise), far below its 50 ms budget. Typing RSS growth was
quantized between approximately 0.2 MiB and 0.45 MiB, with two 2 MiB
residency steps: p05 196,608 bytes, median 200,704 bytes, and p95
2,293,760 bytes. All 30 runs remained below the 3 MiB gate established
by S59-A1. A 10-percent relative verdict on any of these rows would
therefore fail on unchanged source and a stable designated host.

These three rows use `enforcement=budget` under the S59-A2 rule: the same
absolute limits remain hard on both designated runners, their baselines
remain required and recorded, and all other relative ratchets are
unchanged. The completed raw campaign is reanalyzed without rerunning or
discarding observations; its source commit, input hashes, and original
three-failure result remain named in the committed evidence. No editor
code, feature, or user-facing limit changes.

**Amendment S59-A6 (2026-09-21) — size gates follow measured required editor
growth.** The first pushed Sprint 59 candidate at `b5632851` includes 513
commits since `b3f32645`, notably required core editing, tab, and workspace
features. On the same hosted x86_64 size lane, minimal grew 61,440 bytes
(1,509,024 to 1,570,464), full grew 65,536 bytes (2,000,584 to
2,066,120), and every single-module profile grew 65,536--69,632 bytes.
The full 2 MiB and minimal 1.5 MiB hard gates still pass unchanged, as do
AI-only and plugins-only. Only LSP-only (1,697,440 against 1,695,744) and
FUSS-only (1,730,208 against 1,726,464) cross their old single-module
caps; these move to the next 16 KiB boundaries, 1,703,936 and 1,736,704
bytes. The hosted musl static PIE measures 2,185,104 full and 1,664,880
minimal, versus the previous 2,115,472 and 1,599,344. Their old caps
are surpassed by 22,416 and 26,480 bytes, so each moves to the next
64 KiB boundary: 2,228,224 full and 1,703,936 minimal. The roughly
64 KiB common rise across configurations is evidence of required core
growth, not a module-retention explosion. No feature is removed, and the
measurement recipes and all other size gates remain unchanged.

**Amendment S59-A7 (2026-09-22) — minimal gate follows empty-row safety.**
The pinned GCC/glibc `size` lane at `2dea5dc2` measures 1,574,560 bytes for
the minimal stripped binary, versus 1,570,464 before the null-pointer
arithmetic fix in block-selection capture. The ledger attributes just 41
additional `.text` bytes to `core.edit`; the on-disk 4,096-byte step is file
layout amplification. The old 1,572,864-byte minimal cap is short by 1,696
bytes. The required no-undefined-behavior fix cannot be removed to preserve
an obsolete cap, so the minimal cap alone moves to the next 4 KiB boundary,
1,576,960 bytes. The full file remains 2,066,120 bytes; all four
single-module profiles still pass their existing caps. No feature, compiler
profile, measurement rule, or other gate changes.

**Amendment S57.26-A1 (2026-09-22) — fish is an optional completion oracle.**
When `fish` is on `PATH` and `shell.complete_fish` is `auto`, `:!`
completion may ask it for candidates for a command yew has no spec for.
It is queried asynchronously, never on the keystroke path, with user
configuration NOT loaded (`fish -N`) and the user's completion and function
directories added back explicitly. Absent fish degrades to the `--help`
layer exactly as absent `git` degrades FUSS mode. No test's pass/fail may
depend on fish being installed: the suite runs against a stub, and the one
real-fish test skips, loudly, when fish is absent.

**Amendment S57.26-A2 (2026-09-23) — size gates follow the shell completion
engine.** Sprints 57.23–57.26 add `:!` completion that rivals fish: a
shell context lexer, command specs, `--help` learning, a fish oracle and
history suggestions. The user asked for that engine explicitly and chose
this amendment over shrinking or modularising it. On the pinned hosted
x86_64 GCC 13.3 size lane, trunk moved from `243b6b26` to `b123e903` and
every glibc profile grew by 122,880–126,976 bytes: full 2,066,120 to
2,193,096, minimal 1,574,560 to 1,701,536, AI-only 1,709,768 to
1,836,744, FUSS-only 1,730,208 to 1,853,088, LSP-only 1,697,440 to
1,820,320, plugins-only 1,668,768 to 1,795,744. The committed ledgers
attribute 119,569 bytes of it to `core.ui` (the five completion modules),
1,257 to `core.edit` and 774 to `core.util`; no optional module grew, so
this is required core growth, not module retention. **The full 2 MiB
evidence floor is crossed.** Each hosted cap moves to the next 16 KiB
boundary above its measurement, the rule S59-A6 applied: full 2,195,456,
minimal 1,703,936, LSP-only 1,835,008, AI-only 1,851,392, FUSS-only
1,867,776, plugins-only 1,802,240. The hosted musl static PIE grew the
same 126,976 bytes, full 2,185,104 to 2,312,080 and minimal 1,668,976 to
1,795,952; each moves to the next 64 KiB boundary, the S59-A6 musl rule:
2,359,296 and 1,835,008. Full and minimal now sit about 2.4 KB under
their caps, so the next core feature will need its own amendment. The
1.5 MiB full / 900 KiB minimal post-1.0 ratchets are unchanged, as are the
measurement recipes and every other gate. No feature is removed.

**Amendment S57.32-A1 (2026-09-23) — size gates follow prompt editing and
cd-aware completion.** Sprint 57.28 gives the `:` prompt the readline editing
set and a yank stack shared with Insert mode; Sprint 57.32 makes `:!`
completion follow `cd` through the line. Both were asked for by the user
after dogfooding the completion engine, and the user directed that the
S57.26-A2 route be taken again rather than waiting. On the pinned hosted
x86_64 GCC 13.3 size lane at `3ff3c17c`, every glibc profile grew
20,480-24,576 bytes: full 2,193,096 to 2,213,576, minimal 1,701,536 to
1,726,112, AI-only 1,836,744 to 1,857,224, FUSS-only 1,853,088 to
1,873,568, LSP-only 1,820,320 to 1,840,800, plugins-only 1,795,744 to
1,820,320. The ledgers attribute 18,140 bytes to `core.ui` (the effective-
directory lexer and prompt keys), 1,429 to `core.edit` and 940 to
`core.text` (the yank stack); no optional module grew. Each hosted cap moves
to the next 16 KiB boundary: full 2,228,224, minimal 1,736,704, LSP-only
1,851,392, AI-only 1,867,776, FUSS-only 1,884,160, plugins-only 1,835,008.
The musl caps set by S57.26-A2 are unchanged. Nothing else changes.

**Amendment S57.29-A1 (2026-09-24) — two single-module caps follow a layout
step.** Sprint 57.29 (prompt selection and the clipboard) adds 3,718 file-
backed bytes on the pinned x86_64 GCC 13.3 lane at `8b4058f2` (about 1.9 KB
in `core.ui`, 1.8 KB in `core.edit`). Full and minimal stay inside their
S57.32-A1 caps (full's file does not move; minimal steps one 4 KiB page to
1,730,208). FUSS-only and LSP-only each step three pages, 1,873,568 to
1,885,856 and 1,840,800 to 1,853,088, crossing their caps by 1,696 bytes
each: on-disk layout amplification, as S59-A7 recorded, not module growth.
Those two caps alone move to the next 16 KiB boundary: FUSS-only 1,900,544,
LSP-only 1,867,776. Nothing else changes.

**Amendment S57.30-A1 (2026-09-24) — two caps follow prompt history.**
Sprint 57.30 (Up/Down as history, the table's edges, substring history,
`C-r` and token search) adds 10,229 file-backed bytes on the pinned x86_64
GCC 13.3 lane at `f09a2503`: 9,784 in `core.ui`, 445 in `core.edit`. Full
stays inside its S57.32-A1 cap at 2,225,864 (2,360 bytes of headroom
remain). AI-only (1,861,320 to 1,869,512) and minimal (1,730,208 to
1,738,400) cross their caps by 1,736 and 1,696 bytes; those two alone move
to the next 16 KiB boundary: AI-only 1,884,160, minimal 1,753,088. Nothing
else changes.

## Non-negotiable invariants (enforced from Sprint 0)

1. **No data loss, ever.** Atomic saves; kill -9 at any instant never
   corrupts the file on disk; crash journal recovers unsaved work. Torture
   test in CI (Sprint 8).
2. **No byte confusion.** Grapheme/width correctness for emoji, ZWJ
   sequences, CJK, combining marks — fuzzed and golden-tested. Invalid UTF-8
   round-trips losslessly.
3. **No silent stubs.** Unimplemented paths hard-error naming the sprint
   that finishes them.
4. **Latency budgets are CI gates**, not aspirations.
5. **Deterministic rendering.** Same state → byte-identical cell grid; the
   pty test harness asserts grids exactly.
6. **Terminal restore guarantee** on every exit path.
7. **Bespoke first.** stdlib + POSIX only; dependency additions are a
   CLAUDE.md-level event.
8. **Single-threaded core.** Concurrency = event loop + subprocesses.
9. **Modal paradigm first.** Every feature keyboard-reachable; mouse is an
   accelerator, never a requirement.
10. **Recorder/Fletch round-trip.** A recorded macro is always valid,
    readable Fletch source. Tested continuously from Sprint 35 on.
