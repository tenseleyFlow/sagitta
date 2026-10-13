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

**Amendment S57.31-A1 (2026-09-24) — four caps follow the prompt extras.**
Sprint 57.31 (`A-s` sudo toggle, `A-e` edit-in-buffer, `A-h` man page)
adds 12,159 file-backed bytes on the pinned x86_64 GCC 13.3 lane at
`0c929005`: 10,942 in `core.ui`, 1,217 in `core.edit`. Full (2,225,864 to
2,238,152), FUSS-only (1,894,048 to 1,906,336), LSP-only (1,861,280 to
1,873,568) and plugins-only (1,832,608 to 1,848,992) cross their caps; each
moves to the next 16 KiB boundary: full 2,244,608, FUSS-only 1,916,928,
LSP-only 1,884,160, plugins-only 1,851,392. AI-only and minimal stay inside
their S57.30-A1 caps. The musl caps are not moved here: they are measured by
a separate lane and will be amended on its evidence if it crosses.

**Amendment S57.31-A2 (2026-09-24) — the musl caps follow Sprints
57.28-57.31.** The hosted musl static-PIE lane at `bb9ebe66` measures
musl-full 2,361,232 and musl-minimal 1,849,200, against the S57.26-A2
measurements 2,312,080 and 1,795,952: prompt editing and the yank stack,
cd-aware completion, prompt selection, prompt history and the prompt extras
together add 49,152 and 53,248 bytes, the same required core growth the
glibc amendments recorded one sprint at a time. They cross their caps by
1,936 and 14,192 bytes and move to the next 64 KiB boundary, the S59-A6
musl rule: musl-full 2,424,832, musl-minimal 1,900,544. Nothing else changes.

**Amendment S57.27-A1 (2026-09-25) — the glibc caps follow the shell
session.** Sprint 57.27 (the persistent `$SHELL -s` session, its frame
parser and `--yew-env0` record) adds 13,468 file-backed bytes on the pinned
x86_64 GCC 13.3 lane at `cfc06ea9`: 12,641 in `core.edit`, 757 in
`core.ui`, 78 in `core.main`; the libc residue grows 7,012. All six glibc
configurations cross their caps -- full 2,258,632, minimal 1,767,072,
LSP-only 1,885,856, AI-only 1,902,280, FUSS-only 1,918,624, plugins-only
1,861,280 -- and each moves to the next 16 KiB boundary: full 2,260,992,
minimal 1,769,472, LSP-only 1,900,544, AI-only 1,916,928, FUSS-only
1,933,312, plugins-only 1,867,776. The musl lane stays inside its
S57.31-A2 caps.

**Amendment S57.33-A1 (2026-09-26) — three caps follow the symbol index's
resident-memory cap.** Charging the symbol index for the memory it really
holds (strmap/arena/interner cost models, a profile-derived cap, the
one-shot "index full" notice) adds 7,792 file-backed bytes on the pinned
x86_64 GCC 13.3 lane at `9d6a6a5a`: 5,973 in `core.ws`, 1,600 in
`core.util`, 234 in `core.ui`. Full (2,266,824), minimal (1,775,264) and
plugins-only (1,869,472) cross their caps and move to the next 16 KiB
boundary: full 2,277,376, minimal 1,785,856, plugins-only 1,884,160.
AI-only, FUSS-only, LSP-only and the musl caps still hold.

**Sprint 20 conformance amendment S59-A8 (2026-10-13) — span recovery uses
the boolean DFA plus Pike VM.** Sprint 20's original §6c proposed recovering
leftmost-first spans by running a reverse DFA from the earliest end reported
by a forward DFA. That composition is not semantics-preserving: on `abcd`,
`/bc|abcd/` reaches the earlier end at byte 3 through `bc`, while the required
leftmost-first answer is `abcd` at bytes 0..4; a reverse scan starting at byte
3 cannot recover it. The implemented dispatcher therefore uses the DFA only
to reject a miss or bound a safe region for finite-length patterns, then lets
the Pike VM choose the authoritative span and captures. The compiled reverse
program remains shape-, cache-, and round-trip-tested for a future operation
whose semantics fit it, but no 1.0 execution path claims to run it. Search
streams through `TextIter` without the proposed `yew_re_window_fill`
materialization, Pike's current membership predicate is `list_has`, and `:g`
is a permanent non-goal rather than a Sprint 34 deferral. This amendment
supersedes only those stale Sprint 20/21 mechanism and evidence descriptions;
the public matching semantics, linear-time guarantee, memory limits, and
performance gates are unchanged. YEW-F-093 records the correction.

**Sprint 2 closeout amendment S59-A9 (2026-10-13) — deferred Unicode
surfaces are judged at their owning sprint boundaries.** Sprint 2's original
absence of `yew_wb_*` and a simple-case-fold entry point was a staging rule:
it prevented the Unicode foundation from silently pulling Sprint 16's word
unit engine or Sprint 20's regex semantics forward. It was not a permanent
1.0 non-goal. Sprint 16 subsequently delivered `src/unicode/wordbreak.*` and
its UAX #29 tests; Sprint 20 delivered the regex-owned simple-fold entry point
and its agreement corpus. The current F01 closeout therefore verifies that
ownership and coverage instead of requiring the shipped APIs to disappear.
Normalization remains the permanent byte-preservation non-goal, and the
original table/width/grapheme budgets are unchanged. This amendment
supersedes only Sprint 2 §11 and DoD 12's stale current-absence wording;
`YEW-F-095` records the correction.

**Terminal closeout amendment S59-A10 (2026-10-13) — later owning sprints
extend staging surfaces cumulatively.** Sprints 3–6 established the terminal,
input, renderer, and PTY-harness baselines before batch mode, the editor UI,
and later terminal features existed. Their future-sprint hard errors and the
original nine-key PTY environment were staging guards, not permanent 1.0
behavior: Sprint 37 now owns the non-TTY diagnostic and directs users to
`--batch`, Sprint 14 delivered file editing, and later owning sprints may add
only explicitly enumerated deterministic PTY environment keys. Likewise,
Sprint 6's VT sequence table was the initial closed set, not a freeze on later
contracted renderer work; every later sequence family must update the VT and
its rejection/round-trip coverage in the same change. Current mode-2026
evidence scans terminal escape spellings through `scripts/check-render.sh`,
not every unrelated decimal `2026` in the repository. This amendment changes
no terminal behavior or safety invariant; it supersedes only stale staging and
evidence wording in Sprints 3–6 and F02. `YEW-F-099` records the correction.

**Text-engine closeout amendment S59-A11 (2026-10-13) — delivered editing
surfaces replace their staging guards.** Sprints 9, 10, and 12 established
cursor sets, undo reason codes, and register storage before their user-facing
producers existed. Their Sprint 17 hard errors and empty `/`, `:`, and `#`
slots were staging guards, not permanent 1.0 behavior. Sprint 17 now owns
multi-cursor editing and blockwise-register production; Sprints 19, 21, 34,
and 47 own FILTER, REPLACE, MACRO, and LSP transactions; Sprints 18, 21, and
23 own the command-line, search, and alternate-file register producers.
Current closeout evidence therefore proves the shipped routes use the
original choke points and create the contracted single undo node rather than
requiring those routes to remain absent. Unknown or genuinely unimplemented
paths still hard-error under invariant 3. This amendment changes no product
behavior or text-safety invariant; it supersedes only stale staging wording
in Sprints 9/10/12 and F03. `YEW-F-100` records the correction.

**Text-engine ownership closeout amendment S59-A12 (2026-10-13) — editable
mutations remain routed while construction surfaces stay direct.** Sprint 10's
original whole-tree grep described the source graph before later sprints added
initial hydration and internal scratch projections. User-visible editable
document mutation still enters through `EditCtx`, preserving undo, marks,
cursors, journal, and change notification as one operation. Direct
`TextBuf` calls are also valid for piece-tree internals, undo replay, initial
hydration before an edit context is exposed, and internal no-undo/read-only
or temporary projections whose state is not an editable document. Every such
exception remains individually auditable; it is not authority to bypass the
edit choke point. Sprint 12's public raw `yew_reg_set` staging API was likewise
strengthened into private `reg_set` storage plus typed public producer and
yank/delete routes; `yew_reg_append` remains the tested A–Z append primitive.
This amendment changes no product behavior or text-safety invariant. It
supersedes only stale ownership spellings in Sprints 10/12 and F03;
`YEW-F-101` records the correction.

**Modal closeout amendment S59-A13 (2026-10-13) — delivered modal surfaces
replace their staging guards.** Sprints 14 and 16 established mode entry and
structural-unit contracts before all of their later consumers existed. Their
future-sprint mode errors, inert syntax-provider slot, and single-cursor
expansion guard were staging behavior, not permanent 1.0 behavior. Sprints
16/17/18/52 delivered W/B/H/E/F; Sprint 40 installed the syntax provider; and
Sprint 17 delivered multi-cursor expansion through the original modal,
unit-engine, and transaction choke points. Current closeout evidence proves
those shipped paths retain keyboard reachability, unit-engine progress,
cursor invariants, rollback, and one-transaction behavior instead of
requiring the paths to remain absent. Sprint 17's other boundaries remain
intentional for 1.0: match lifting is literal-only, ghost completion is
single-cursor, and LSP edits require one primary cursor. Those guards remain
tested under invariant 3. This amendment changes no product behavior or modal
invariant; it supersedes only stale staging wording in Sprints 14/16 and F04.
`YEW-F-102` records the correction.

**Modal ownership closeout amendment S59-A14 (2026-10-13) — viewport
consumers and cell geometry are not Unicode algorithms.** Sprint 14's naive
single-window viewport deliberately limited direct field access so Sprint 15
could replace it locally. Later owning sprints added serialized workspace
restore, tab-view preservation, status and search state, pane/window cloning,
Fletch inspection, and Git display integration. Those are legitimate
consumers of viewport state; they do not transfer ownership of generic
follow, wrap, scroll, or clamp algorithms away from `src/ui/viewport.c`.

The architecture's "width math" rule means Unicode-width measurement and
coordinate conversion: width tables, grapheme measurement, tab-aware text
measurement, and byte/character/grapheme/cell conversion remain centralized
in `src/unicode/`. UI code necessarily adds, subtracts, clips, and places the
resulting cell counts to perform layout and drawing. It may not call
locale-dependent width APIs, embed codepoint-width tables or ranges, or
reimplement text-to-cell conversion. This clarification preserves invariant
2 while replacing token greps that reject ordinary identifiers and cell
geometry. It changes no product behavior; it supersedes only Sprint 14 DoD 10,
Sprint 15 DoD 10, and their F04 closeout copies. `YEW-F-103` records the
correction.

**Execute closeout amendment S59-A15 (2026-10-13) — delivered command
surfaces replace their staging guards.** Sprint 18 established the E-mode
parser, range, and registry routes before their later consumers existed.
Sprints 19/21/32/36 subsequently delivered shell execution, substitute and
search addresses, named marks, Fletch evaluation, and config reload through
those original choke points. Current closeout evidence must prove the shipped
routes and their failure semantics instead of requiring them to keep naming a
future sprint.

Two boundaries remain intentional and are not staging guards. `:g` rejects a
bespoke global-command mini-language in favor of Fletch's query API, and
`ed.shell.term` rejects a terminal emulator. The later `:!!` command hands
Yew's real terminal to one child and restores it; it does not create a
pty-backed terminal buffer or weaken the emulator non-goal. This amendment
changes no product behavior. It supersedes only stale staging wording in
Sprints 18/19 and F05; `YEW-F-104` records the correction.

**Execute ownership closeout amendment S59-A16 (2026-10-13) — Unicode and
pipe gates express ownership across locked platforms.** Sprint 18's prompt
reuse law means Unicode measurement, grapheme boundaries, and coordinate
conversion use the shared text/Unicode engines. Command names, comments,
`tabwidth` state, and arithmetic on already-measured cells are not duplicate
Unicode implementations and cannot be rejected by a token grep.

Sprint 19's descriptor law means all production pipe creation routes through
`yew_pipe_cloexec` and every resulting descriptor is close-on-exec. Linux and
FreeBSD use `pipe2(O_CLOEXEC)`. Locked macOS has no `pipe2`, so its portable
fallback uses `pipe()` followed immediately by `FD_CLOEXEC`; invariant 8's
single-threaded core and the ban on signal-handler forks make that fallback
race-free. Direct pipe creation outside the helper remains forbidden, as do
`system()` and `popen()`. This amendment changes no product behavior or
descriptor/Unicode invariant. It supersedes only stale literal greps in
Sprints 18/19 and F05; `YEW-F-105` records the correction.

**UI/workspace closeout amendment S59-A17 (2026-10-13) — delivered UI
surfaces replace their staging guards.** Sprints 22–27 established pane,
tab, group, workspace-state, picker, and mouse contracts in sequence. Their
future-sprint errors and inert paths were staging guards, not permanent 1.0
behavior. Sprint 23 delivered the tab command surface; Sprint 24 assigned
negative tab-region payloads to group identity; Sprint 53 delivered
directory-to-group opening; Sprints 38/47/52/54–55 delivered the command,
symbol, F-mode, and plugin/package discovery surfaces; and Sprints
52/57.13/57.22 delivered F-mode mouse, document context menus, and
tab-into-pane. Sprint 36 ran the required hand-written-versus-Fletch state
codec differential and then retired the legacy implementation and its
differential-only test, leaving the shipping Fletch corpus round-trip as the
frozen-format guard. Current closeout evidence proves those delivered paths
through their original registry, region, state, picker, and mouse choke
points rather than requiring them to remain absent. LSP hover-on-pointer
remains outside 1.0; keyboard `ed.lsp.hover` is the accessible hover route.
This amendment changes no product behavior. It supersedes only stale staging
wording in Sprints 22–27 and F07; `YEW-F-106` records the correction.

**UI/workspace ownership closeout amendment S59-A18 (2026-10-13) — geometry,
atomic replacement, and mouse dispatch have semantic boundaries.** Unicode
measurement and coordinate conversion remain centralized in `src/unicode/`;
UI layout may perform geometry over the resulting cell counts. Workspace
state code must route durable replacement through the shared Sprint 8 atomic
file API, and direct `rename()` and `fsync()` calls remain forbidden under
`src/ws/`; prose, type names, and calls to the shared helper are not
violations. Mouse escape decoding remains in `src/term/input.c`, exactly one
event-loop branch in `src/edit/loop.c` dispatches `YEW_EV_MOUSE` to
`yew_mouse_event`, and `src/ui/mouse.c` owns routing and behavior. Other
modules may define regions or maintain state consumed by that router; they
may not create another event dispatch site. This amendment changes no
product behavior or ownership invariant. It supersedes only the impossible
literal greps in Sprints 22/25/27 and F07; `YEW-F-107` records the
correction.

**Fletch closeout amendment S59-A19 (2026-10-13) — delivered language and
host surfaces replace their staging guards.** Sprints 30–36 established the
VM, standard library, REPL, conformance suite, editor API, and configuration
system in sequence. Their future-sprint errors and inert paths were landing
guards, not permanent 1.0 behavior. Sprint 31 delivered import execution;
Sprints 34–38 delivered editor handles/APIs, recorder round-trip, batch mode,
configuration, and macro-library loading; and Sprint 54 delivered plugin
configuration. Sprint 36 completed the required legacy-versus-Fletch state
codec differential, then Sprint 58 retired that test-only legacy codec as the
contract explicitly required. Current closeout proves the delivered surfaces
through their shipping conformance, unit, script, PTY, and corpus guards.
The standalone `yew fl` REPL still has no editor host, so motion execution
there raises `"motion"`; `io.run` and `io.http` remain absent from the 1.0
Fletch stdlib. This amendment changes no product behavior. It supersedes only
stale staging wording in Sprints 30–36 and F08; `YEW-F-108` records the
correction.

**Fletch ownership closeout amendment S59-A20 (2026-10-13) — parser arenas
and GC ownership have semantic boundaries.** Lexer and parser AST allocation
remains caller-arena-owned. Every language `FlObj` allocation remains owned
and traced by the Fletch collector through `fl_gc_alloc`. Later Fletch host
support—origin and hook registries, configuration and recorder state, handle
tables, transaction bookkeeping, and the macro library—may use the project's
checked allocation wrappers for non-`FlObj` storage. Direct libc allocation
remains forbidden by the repository-wide allocator gate. Sprint 29's
touched-directory restriction was a landing-scope history constraint, not a
current-tree release gate after later sprints extended the subsystem. This
amendment changes no product behavior or allocation invariant. It supersedes
only the historical scope restriction and impossible allocator token greps in
Sprints 29/30 and F08; `YEW-F-109` records the correction.

**Fletch inventory closeout amendment S59-A21 (2026-10-13) — freeze-time
inventories remain history while release gates follow append-only amendments.**
Sprint 28 froze 11 error kinds and an empty amendment section. Sprint 30 A1
raised that inventory to 12, and the later A2 amendment raised the current
inventory to 13. Sprint 31's seven core module tables contribute exactly 117
native functions; the `error` prelude and later owner surfaces bring the
current deterministic `yew fl --list-natives` inventory to 187. Sprint 33's
ledger is generated from those current sources of truth, so closeout requires
K/K and N/N agreement rather than preserving its landing-day totals. The
freeze snapshots remain recorded as historical evidence; they are not current
release totals. This amendment changes no language surface or product behavior
and supersedes only stale point-in-time totals in Sprints 28/30/31/33 and F08;
`YEW-F-110` records the correction.

**Recorder closeout amendment S59-A22 (2026-10-13) — later owners replace
recorder staging guards with shipped integration evidence.** Sprint 35
delivered recording, emission, and VM-only replay. Sprint 38 subsequently
delivered the indicator, browser, ordinary-buffer edit/store flow, and macro
library through that same recorder and VM path. Sprint 41's Fletch language
pack now highlights macro scratch buffers, and Sprint 55's package mechanism
owns distribution of macro collections. Sprint 38 deliberately exposed no
callable staging commands for those latter integrations, so current closeout
tests their shipped owners instead of inventing hard-error stubs. Interactive
recording remains refused in batch mode, and cross-version migration remains
post-1.0. This amendment changes no product behavior and supersedes only
stale staging wording in Sprints 35/38 and F09; `YEW-F-111` records the
correction.

**Macrolib namespace closeout amendment S59-A23 (2026-10-13) — injectivity
is the executable collision contract.** Unique filename stems produce dotted
`<stem>.<binding>` canonical names; a bare alias exists only when the binding
equals that same stem; and one-byte stems are rejected to keep register names
disjoint. Those public rules make cross-file and register collisions
unreachable. Closeout therefore tests every reachable namespace row and the
injectivity premises, then reviews the retained defensive rejection branch;
it does not weaken the namespace to manufacture an impossible filesystem
fixture. This amendment changes no product behavior or diagnostic path and
supersedes only the unreachable collision-fixture wording in Sprint 38 and
F09; `YEW-F-112` records the correction.

**Syntax state closeout amendment S59-A24 (2026-10-13) — binding embedded
state and delivered owners replace landing guards.** Sprint 39 landed a
40-byte flat state tuple and staged later definition, theme, language-pack,
and embedding surfaces behind hard errors. Sprints 40–42 delivered the first
three surfaces; binding Sprint 41.5 widened the tuple to the current 84-byte
stack of qualified `(definition, context)` frames, removed `SynState.def`,
and preserved four-byte per-line interned entry ids; Sprint 42.5 expanded the
pack without changing that representation. Current closeout therefore proves
the frame bounds, canonical zero tails, unchanged non-embedding states,
balanced embed exits, and shipped owner surfaces rather than requiring their
historical hard errors. This amendment changes no product behavior or memory
budget. It supersedes only stale state/staging wording in Sprints 39–42 and
F10; `YEW-F-113` records the correction.

**Syntax ownership and inventory closeout amendment S59-A25 (2026-10-13) —
release gates follow the theme boundary and checked pack manifest.** Syntax
matching and definition code continues to emit semantic attributes and owns
no colors or width math. Sprint 41's `theme.c`/`theme.h` is the sole syntax
subsystem exception that parses and degrades colors; the repository ban
explicitly excludes only those files. Sprint 42's 17-definition / 14-language
cohort remains a historical milestone, Sprint 41.5 expanded it to 19/16, and
binding Sprint 42.5 owns the current deterministic 48-definition inventory
through `tests/syn/manifest.tsv`. Current closeout preserves the original
cohort checks and also follows the live manifest, all-pack strict/coverage
gates, cold-load limit, and fuzz corpus. This amendment changes no syntax,
theme, or detection behavior. It supersedes only stale ownership scope and
point-in-time inventory wording in Sprints 39/42 and F10; `YEW-F-114` records
the correction.

**Completion/LSP closeout amendment S59-A26 (2026-10-13) — later owners
replace shadow, lifecycle, and feature staging guards.** Sprint 43 reserved
three explicit shadow-provider slots; Sprint 44 delivered the index provider,
Sprint 47 delivered the LSP provider and its nine feature commands, and Sprint
49 delivered the AI provider. Sprint 46 delivered full-build server startup
and restart, while Sprint 47 filled the LSP documentation slot without adding
documentation to index-only completion items. Current closeout therefore
tests full-build provider registration, lifecycle, capability gating,
completion resolution, and feature behavior rather than historical
`lands in Sprint` messages. Deliberately unregistered test providers remain
visible, stripped LSP commands retain the exact module-boundary error, index
items still render `(no documentation)`, and the named post-1.0 features
remain non-goals. This amendment changes no product behavior. It supersedes
only stale staging wording in Sprints 43–46 and F11; `YEW-F-115` records the
correction.

**Completion/LSP ownership closeout amendment S59-A27 (2026-10-13) —
determinism scans follow lexical tokens and their actual owners.** Sprint 44's
integer-only ranking rule belongs to `src/ws/sym*.c|h`, not unrelated
workspace-state owners, and its ban concerns the `float`, `double`, and
`qsort` tokens rather than those byte sequences inside ordinary words. Sprint
45's sole JSON real formatter is the annotated `%.17g` conversion in
`yew_jsonw_real`; its source gate must recognize precision-bearing conversion
specifiers rather than only bare `%g`, `%f`, or `%e`. Current closeout uses a
token-aware scan over symbol-index sources and a conversion-specifier scan
that returns exactly that formatter. This amendment changes no ranking, JSON,
or locale behavior. It supersedes only invalid grep wording in Sprints 44/45
and F11; `YEW-F-116` records the correction.

**AI closeout amendment S59-A28 (2026-10-13) — Sprint 50 owners replace
Sprint 48/49 command staging guards.** Sprint 48 delivered HTTP, curl, backend
adapters, and their registered command boundary; Sprint 49 delivered the
shadow provider and local aggregate stats; Sprint 50 delivered opt-in,
enable/disable, trust, redaction, presets, privacy, and status. The earlier
future-sprint hard errors were landing guards, not permanent behavior. Current
closeout therefore proves the shipped command surfaces, off-by-default and
zero-network boundaries, and stripped-module errors rather than requiring
`ed.ai.enable` or `ed.ai.stats` to remain deferred. Multi-file context, chat,
tool use, provider-retention promises, and a native TLS implementation remain
the documented 1.0 non-goals. This amendment changes no product behavior. It
supersedes only stale staging wording in Sprints 48/49 and F12;
`YEW-F-118` records the correction.

**AI ownership closeout amendment S59-A29 (2026-10-13) — TLS and stats scans
follow implementation boundaries.** Yew continues to contain no TLS
implementation: HTTPS routes through curl, while the AI error taxonomy,
diagnostics, and routing code necessarily name TLS. The executable source gate
bans TLS implementation/library symbols rather than the required error and
message surface. Aggregate AI stats remain local and contain no prompt or
completion bytes; `stats.c` owns persistence and formatting, `stats.h` owns
its API, `ai_int.h`/`ai.c` own the editor-state pointer and lifecycle, and
`shadow_ai.c` records aggregate events. Prompt and transport owners may not
access stats. This amendment changes no transport, privacy, logging, or stats
behavior. It supersedes only raw ownership scans in Sprints 48/49 and F12;
`YEW-F-119` records the correction.

**Git/FUSS closeout amendment S59-A30 (2026-10-13) — shipped owners and the
binding compact-FUSS keyboard replace landing guards.** Sprint 51 staged F
mode and later Git/editor commands; Sprint 52 delivered F mode and the Git
verbs; Sprint 53 delivered hunks, blame, diff views, status, and directory
groups. Their future-sprint hard errors were landing guards. The binding
Sprint 57.5/current FUSS model also resolves the action/type-jump conflict:
ordinary valid filename bytes search visible rows, actions use Alt plus their
prior key, and Ctrl+Shift+/ opens the effective-binding-derived action picker.
Current closeout tests those shipped owners and bindings. Conflict resolution
and sub-hunk staging remain documented post-1.0 boundaries. This amendment
changes no product behavior. It supersedes only stale staging/key wording in
Sprints 51/52 and F13; `YEW-F-120` records the correction.

**Git/FUSS ownership closeout amendment S59-A31 (2026-10-13) — clock, argv,
and width gates follow semantic operations.** The tokenized call scan fixed by
`YEW-F-018` bans actual `time`, `clock`, and `cpu_time` calls while permitting
helper identifiers containing those byte sequences. The structural argv
builder and hostile-filename matrix fixed by `YEW-F-020` forbid formatted
path/ref/message elements while permitting formatting of owned display text.
FUSS display-cell measurement continues to route through `src/unicode/`;
ordinary `strlen` calls that compute byte lengths are not width math. This
amendment changes no clock, subprocess, path, or Unicode behavior. It
supersedes only invalid literal scans in Sprints 51/52 and F13;
`YEW-F-121` records the correction.

**Plugin closeout amendment S59-A32 (2026-10-13) — shipped package and
documentation owners replace landing guards.** Sprint 55 delivered `yew pkg`;
the Sprint 54 hard error naming its future owner is landing history, not a
current closeout requirement. Sprint 59 delivered the assembled plugin man
pages, so closeout tests the shipped page and its checker rather than a future
assembly notice. The semantic honesty control fixed by `YEW-F-022` remains
binding: plugin docs must deny memory/resource isolation and may use
`sandbox` only in negative disclaimers. Package registries, dependency
resolution, install hooks, auto-update, native plugins, and editor-loop
package installation remain explicit non-goals. This amendment changes no
plugin or package behavior; `YEW-F-122` records the contract correction.

**Plugin import closeout amendment S59-A33 (2026-10-13) — shipped runtime
import evidence names binding Amendment A3.** Fletch Amendment A2 belongs to
Sprint 34's `handle` value kind. Sprint 55's fourth, last-searched
`$YEW_RUNTIME_DIR` import row is binding Amendment A3 in
`.docs/fletch-spec.md` section 16 and its conformance fixture. Current plugin
closeout cites A3. This amendment changes no import order or runtime behavior;
`YEW-F-123` records the correction.

**Plugin module-diagnostic closeout amendment S59-A34 (2026-10-13) — the
shared module seam owns stripped-plugin errors.** Sprint 0's
`yew_mod_require` predates the plugin module and provides one clear diagnostic
for every excluded feature: `this build has no <name> module; rebuild with
'make MODULES="… <name>"'`. Sprint 54's later plugin-local byte string and
static-owner requirement diverged from that established seam even though the
shipped plugin entry points correctly return its error and exit 1. Current
closeout therefore requires the shared plugins diagnostic byte-exact and the
same no-silent-stub behavior as every other module; it does not fork plugin
diagnostics from their core owner. This amendment changes no product behavior
and supersedes only Sprint 54's divergent string/owner wording and F14;
`YEW-F-126` records the correction.

**Foundation staging closeout amendment S59-A35 (2026-10-13) — later owners
replace scaffolding hard errors.** Sprints 0/1 intentionally reserved file and
TUI startup, `--clean`, batch execution, script tests, PTY tests, and fuzzing;
Sprint 37 likewise reserved batch macro replay and plugin grants. Sprints
2/6/14/36/37/38/54 subsequently shipped those surfaces. Current closeout
tests their live owners and retains only genuine non-goals such as
`--batch-strict`; it does not require delivered features to regress into
landing-day stubs. This amendment changes no product behavior and supersedes
only staging wording in Sprints 0/1/37 and F15; `YEW-F-127` records the
correction.

**Executable-name closeout amendment S59-A36 (2026-10-13) — yew owns one
binary path.** The project rename mechanically collapsed Sprint 0's historical
binary/alias pair into impossible requirements for `build/yew` and `bin/yew`
to alias themselves. The current Makefile correctly builds one
`build/yew` executable and installs it once as `bin/yew`. Closeout tests those
single paths; no self-referential symlink is created. This amendment changes
no build or install behavior and supersedes only the corrupted Sprint 0 and
F15 alias wording; `YEW-F-128` records the correction.

**Foundation scan closeout amendment S59-A37 (2026-10-13) — semantic bans
distinguish code from prose and corpus data.** Sprint 0/1's landing greps were
strengthened by F15's token-aware ban and explicit-registration controls.
Current closeout uses those executable controls and their seeded negative
fixtures: explanatory `qsort` prose and a Kotlin `constructor` keyword in
syntax corpus data are not forbidden C operations. Actual raw-sort,
constructor-attribute, GNU-extension, and unregistered-test code remains
banned. This amendment weakens no C11 or determinism rule and supersedes only
the obsolete literal scans in Sprints 0/1 and F15; `YEW-F-129` records the
correction.

**Foundation timing closeout amendment S59-A38 (2026-10-13) — landing-day
suite time remains history, not the expanded aggregate budget.** Sprint 1's
sub-10-second measurement covered its provisional unit harness and smoke test.
Today's `make test` deliberately includes the later audit, PTY, Fletch,
script, tutor, manual, corpus, package, and torture owners. Current closeout
requires that expanded suite in its hosted CI budget and applies dedicated
Sprint 37/56 performance gates to product latency; it does not remove tests to
recreate the Sprint 1 wall time. This amendment changes no test or performance
gate and supersedes only Sprint 1/F15's scope ambiguity; `YEW-F-130` records
the correction.

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
