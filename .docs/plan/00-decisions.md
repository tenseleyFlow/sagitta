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
| Optional runtime tools | `git` (FUSS mode), `$SHELL` (E mode), `curl` (cloud AI only). All degrade gracefully when absent |
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
| Binary size (full modules, stripped, x86_64) | ≤ 2 MiB; minimal MODULES build ≤ 1.5 MiB |
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
