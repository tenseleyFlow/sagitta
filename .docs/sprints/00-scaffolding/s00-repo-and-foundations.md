# Sprint 0: Repo and Foundations

## Prerequisites

None. Empty repo on `trunk`. Binding docs: `.docs/plan/00-decisions.md`
(C11 subset, one GNU Makefile, GPL-3.0-only, MODULES set, exit-code
contract, the 10 invariants), `.docs/plan/01-architecture.md` (repository
layout — paths below come from it), `.docs/sprints/index.md`.

## Goals

Land the skeleton everything else stands on: repo layout, the one GNU
Makefile (out-of-tree `build/`, auto-deps, strict flags, and the
`MODULES="lsp ai fuss plugins"` plumbing from day one — excluded modules
compile out to hard-error shims, never silent no-ops), GPL-3.0 LICENSE and
README, the core containers (arena, vec, strmap, interner, bytebuf, stable
sort), `yew_log` to XDG state (never stdout/stderr — the TUI will own the
terminal), the `yew_bug()` exit-4 contract, the exit-code enum, and a
driver shell answering `--version`/`--help` with a `yew` executable build
target. At landing no editing happened: opening a file (or starting with no
args, which would launch the TUI) hard-erred naming Sprint 14. The shipped
startup paths are now owned by Sprints 14/36/37 under S59-A35. The bespoke test
harness lands in Sprint 1 (this sprint ships a provisional unit binary);
Unicode in Sprint 2; the terminal in Sprint 3; `--batch` in Sprint 37.

## Deliverables

### 1. Repository skeleton and licensing

```
Makefile  LICENSE  README.md  .gitignore
src/main.c  src/args.c  src/args.h
src/mod/mods.c  src/mod/mods.h
src/util/base.h  base.c  arena.c|h  vec.h  strmap.c|h  intern.c|h
         buf.c|h  sort.c|h  log.c|h
tests/unit/unit_main.c
scripts/                      (empty; smoke.sh lands in Sprint 1)
```

- `LICENSE`: verbatim GPL-3.0 text. `README.md`: one screen — what
  yew is (speed-first, bespoke-first, modal terminal editor + the
  Fletch language), build instructions (`make`, `make test`,
  `make MODULES="lsp ai"`), status ("pre-1.0 scaffolding"), license,
  sister-project links (Cgfried, fuss, facsimile).
- `.gitignore`: `build/`, `*.o`, editor droppings. Generated files never
  live outside `build/` — the one future exception, checked-in
  `src/unicode/tables.c`, is documented when Sprint 2 lands it.
- Directories not listed above (`src/term/`, `src/text/`, `src/fl/`, …)
  are created by the sprint that first puts a file in them; no `.gitkeep`.

### 2. The Makefile

One hand-written GNU Makefile. Core:

```make
CC      ?= cc
BUILD   ?= build
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins

KNOWN_MODS := lsp ai fuss plugins
BAD_MODS   := $(filter-out $(KNOWN_MODS),$(MODULES))
ifneq ($(BAD_MODS),)
$(error unknown MODULES '$(BAD_MODS)' (known: $(KNOWN_MODS)))
endif

# module name -> source dir (per 01-architecture.md: fuss lives in
# src/mod/git/, plugins in src/mod/plug/)
MODDIR_lsp := lsp
MODDIR_ai := ai
MODDIR_fuss := git
MODDIR_plugins := plug

CFLAGS := -std=c11 -pedantic -Wall -Wextra -Werror -Wvla -g -O2 \
          -MMD -MP -Isrc \
          -DYEW_WITH_LSP=$(if $(filter lsp,$(MODULES)),1,0) \
          -DYEW_WITH_AI=$(if $(filter ai,$(MODULES)),1,0) \
          -DYEW_WITH_FUSS=$(if $(filter fuss,$(MODULES)),1,0) \
          -DYEW_WITH_PLUGINS=$(if $(filter plugins,$(MODULES)),1,0)

CORE_SRC := $(shell find src -path 'src/mod/*' -prune -o -name '*.c' \
              -print | sort)
MOD_SRC  := src/mod/mods.c \
  $(foreach m,$(filter $(KNOWN_MODS),$(MODULES)), \
    $(filter-out %/shim.c,$(sort $(wildcard src/mod/$(MODDIR_$(m))/*.c)))) \
  $(foreach m,$(filter-out $(MODULES),$(KNOWN_MODS)), \
    $(sort $(wildcard src/mod/$(MODDIR_$(m))/shim.c)))
SRC := $(CORE_SRC) $(MOD_SRC)
OBJ := $(SRC:%.c=$(BUILD)/%.o)

$(BUILD)/yew: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)
$(BUILD)/%.o: %.c $(BUILD)/mods.stamp | dirs
	$(CC) $(CFLAGS) -c -o $@ $<
-include $(OBJ:.o=.d)
```

- Targets: `all` (default: one `build/yew` executable), `test`
  (Sprint 0: builds and runs `build/unit_tests`), `clean` (`rm -rf
  $(BUILD)`), `install` (installs one `bin/yew` executable under
  `$(DESTDIR)$(PREFIX)/bin`, honors both variables).
- `$(BUILD)/mods.stamp` contains the literal `$(MODULES)` value; the
  recipe rewrites it only when the value changes, and every object
  depends on it. Why: the `-DYEW_WITH_*` flags change with `MODULES`, and
  stale objects from a different module set would link a franken-build.
- The per-module shim convention (pinned now, exercised when each module
  lands): every `src/mod/<dir>/` ships a `shim.c` implementing the same
  public header as the real module, every function routed through
  `yew_mod_require()` (Deliverable 3). Enabled → real sources minus
  `shim.c`; disabled → `shim.c` only; never both.
- Pitfalls:
  - `$(shell find … | sort)` — unsorted find order varies by filesystem,
    leaks into link order → nondeterministic binaries. Sort it now
    (invariant 5's spirit, and Sprint 60's reproducible releases).
  - Order-only prerequisite (`| dirs`) for `mkdir -p` of build subdirs;
    normal dir prereqs cause spurious rebuilds (dir mtime churn).
  - `-MMD -MP`, not `-MD`: skip system-header deps; `-MP` prevents "No
    rule to make target" after a header deletion.
  - No `-D` flags embedding absolute paths, no `__DATE__`/`__TIME__` —
    breaks the determinism lane (Sprint 1) and reproducible releases
    (Sprint 60).
  - The unit-test link excludes `$(BUILD)/src/main.o` (two `main`s
    otherwise); filter it out explicitly, don't reorder `find`.

### 3. Module registry (`src/mod/mods.h`, `src/mod/mods.c`)

```c
typedef enum {
    YEW_MOD_LSP, YEW_MOD_AI, YEW_MOD_FUSS, YEW_MOD_PLUGINS, YEW_MOD_COUNT
} YewMod;

bool        yew_mod_enabled(YewMod m);   /* from YEW_WITH_* defines */
const char *yew_mod_name(YewMod m);      /* "lsp" "ai" "fuss" "plugins" */
/* False + canonical message when disabled; every module entry point
 * (and every shim.c function) routes through this. */
bool        yew_mod_require(YewMod m, char *err, size_t errsz);
```

Canonical message (pinned; smoke-tested from Sprint 1 on):
`this build has no <name> module; rebuild with 'make MODULES="… <name>"'`.
Why a runtime check when the compile already excluded the code: invariant
3 — a keybinding, Fletch call, or config option that reaches a missing
module must produce this exact explanation, never a silent no-op and
never an unresolved symbol.

### 4. Logging and the bug contract (`src/util/log.h`, `src/util/log.c`)

```c
typedef enum { YEW_LOG_DEBUG, YEW_LOG_INFO, YEW_LOG_WARN, YEW_LOG_ERROR }
    YewLogLevel;
void yew_log(YewLogLevel lvl, const char *fmt, ...);

typedef struct YewLogSink {
    void (*write)(void *user, YewLogLevel lvl, const char *msg);
    void *user;
} YewLogSink;
void yew_log_set_sink(const YewLogSink *s);  /* NULL = default file sink */

_Noreturn void yew_bug(const char *file, int line, const char *fmt, ...);
#define YEW_BUG(...) yew_bug(__FILE__, __LINE__, __VA_ARGS__)
```

- Default sink appends `<ISO-8601-UTC> <level>: <msg>\n` to
  `$XDG_STATE_HOME/yew/log` (fallback `~/.local/state/yew/log`),
  creating the directory `0700` lazily. `YEW_LOG=<path>` overrides the
  path; `YEW_LOG_LEVEL=debug|info|warn|error` filters (default `info`).
- **`yew_log` never writes to fds 0/1/2 — not even on failure.** The TUI
  will own the terminal (Sprint 3 on); a log line on stdout corrupts the
  cell grid. If the log file cannot be opened, messages drop silently.
  Pitfall: the classic sin is an `fprintf(stderr, …)` fallback "just for
  errors" — it is exactly the case that garbles a user's screen.
- The sink receives `(lvl, msg)` *before* timestamp formatting. This is
  why tests never scrape the log file: Sprint 1's harness installs a
  capturing sink and asserts on level + message structurally.
- `yew_bug` writes a structured report — `yew: internal error at
  <file>:<line>: <msg>` plus a "please report this" line — to the log
  *and* to stderr (legal here: the process is dying; from Sprint 3 the
  crash path restores the terminal first), then exits `YEW_EXIT_BUG` (4).
  Never a bare `abort()` on a reachable path.
- `main()` is the single return-to-OS site; **only `yew_bug` may call
  `exit()`** — scattered exits make the contract unauditable (Sprint 1
  CI greps for violations).

### 5. Exit-code contract (`src/util/base.h`)

| Code | Meaning | Emitted by |
|---|---|---|
| 0 | success | driver |
| 1 | error (bad flags, unimplemented surface, editor error) | driver |
| 2 | `--batch` script failure (Sprint 37+) | batch runner |
| 3 | I/O error (unreadable file, unwritable state dir) | driver/file layer |
| 4 | internal bug — structured report, never a bare crash | `yew_bug` |

```c
enum { YEW_EXIT_OK = 0, YEW_EXIT_ERR, YEW_EXIT_BATCH, YEW_EXIT_IO,
       YEW_EXIT_BUG };
#define YEW_VERSION "1.0.0-dev"
```

### 6. Driver shell (`src/main.c`, `src/args.c`, `src/args.h`)

```c
typedef struct {
    bool version, help, clean;
    const char  *batch_script;   /* --batch <file>: recognized, deferred */
    const char **files; size_t nfiles;
    bool selftest_bug;           /* hidden: --selftest-bug */
} YewArgs;
/* Pure — no I/O; errors formatted into err. Returns exit code or -1
 * meaning "proceed". Unit-testable without a process. */
int yew_args_parse(YewArgs *out, int argc, char **argv, Bytebuf *err);
```

- `--version` → line 1 exactly `yew 1.0.0-dev`; line 2 `modules: lsp
  ai fuss plugins` (enabled set in `KNOWN_MODS` order, `modules: none`
  when empty). Stdout, exit 0. One `YEW_VERSION` define (Sprint 60
  audits this contract).
- `--help`: one `static const char help_text[]` in labeled sections
  (Usage / Options / Environment: `YEW_LOG`, `YEW_LOG_LEVEL`); stdout,
  exit 0. Deferred flags are listed with their sprint so `--help` never
  lies about what works.
- Any file argument, or no arguments at all (which would start the TUI):
  `yew: error: the editor is not yet implemented: Sprint 14 (modes
  L and I)`, stderr, exit 1. `--batch` → error naming Sprint 37;
  `--clean` → error naming Sprint 36. Unknown option: error naming it,
  exit 1. Stderr is legal in the driver *before* the TUI exists; from
  Sprint 3 on, only pre-terminal-setup paths may use it.
- Hidden `--selftest-bug` calls `YEW_BUG("selftest")` — the only way to
  smoke-test the exit-4 contract from a script (Sprint 1 uses it).

### 7. Core containers (`src/util/`)

`base.h`: `u8/u16/u32/u64/i8/i16/i32/i64` over `<stdint.h>`,
`YEW_ARRAY_LEN`, `yew_xmalloc/xrealloc/xcalloc` (OOM → `YEW_BUG`).
Language box (decision-locked): C99 core + `_Static_assert`, anonymous
struct/union, `alignas` — no VLAs, no `__attribute__`, no statement
expressions, no `threads.h` in our own source.

- **arena.c|h** — phase-lifetime bulk free, never per-node free (Fletch
  compilation units, per-frame render scratch). `void *arena_alloc(Arena
  *a, size_t size, size_t align);` `char *arena_strdup(Arena *, const
  char *);` `void arena_free_all(Arena *);` Geometric block chain (start
  64 KiB, ×2). Pitfall: alignment arithmetic on `uintptr_t`; `align`
  must be a power of two (assert) — misaligned `Type*` loads are UB that
  detonates on arm64 (a locked target).
- **vec.h** — macro-generated type-safe dynamic array: `VEC_DECL(VecU32,
  u32)` expands to a struct `{ u32 *data; size_t len, cap; }` plus
  `VecU32_push/reserve/free` inline functions of the *concrete* type.
  Pitfall: no generic `vec_push(void **data, …)` helper punning through
  `void**` — a strict-aliasing violation `-O2` gcc will miscompile.
  Every function the macro emits touches only `T*`.
- **strmap.c|h** — string→`void*` map that **iterates in insertion
  order**: entries in an append-only array; the hash table stores
  indices into it; iteration walks the array. Why non-negotiable:
  hash-order iteration reaching any output is the classic nondeterminism
  bug — it would break deterministic rendering (invariant 5), workspace
  serialization (Sprint 25), and Fletch's language-level guarantee of
  insertion-ordered maps (`02-fletch.md`). Design it out now, repo-wide.
  API: `strmap_put/get` (put on an existing key replaces the value,
  keeps the original slot order), `strmap_len`, `StrmapIter` yielding in
  put order. FNV-1a, no seed randomization (determinism beats
  DoS-hardening on trusted input). No delete in Sprint 0; the sprint
  that needs it adds tombstones without disturbing iteration order.
- **intern.c|h** — string interner returning stable `u32` ids in
  first-intern order; id 0 reserved as "invalid". `u32 yew_intern(
  Interner *, const char *s, size_t len);` `const char *yew_intern_str(
  const Interner *, u32 id);` Backed by arena + strmap, so ids are
  deterministic too. Future consumers: grapheme intern ids in `Cell`
  (Sprint 5), Fletch identifiers (Sprint 29).
- **buf.c|h** — `Bytebuf`: `bytebuf_append(Bytebuf *, const void *,
  size_t)`, `bytebuf_printf`, `bytebuf_push_u8`; geometric growth. The
  substrate for the render escape stream (Sprint 5) and Fletch chunk
  emission (Sprint 30).
- **sort.c|h** — `void yew_sort_stable(void *base, size_t n, size_t
  elem_sz, int (*cmp)(const void *, const void *, void *ctx), void
  *ctx);` STABLE mergesort with scratch buffer. Why: `qsort` is not
  stable, and glibc/musl disagree on tie order → cross-libc
  nondeterminism in completion ranking, finder results, tab ordering.
  Raw `qsort` is banned repo-wide from this sprint on (Sprint 1 CI
  greps for it).

### 8. Defer

- File args and no-arg TUI start → hard error naming Sprint 14.
- `--batch` → Sprint 37; `--clean` → Sprint 36; `yew fl` → Sprint 32;
  `yew pkg` → Sprint 55 (the last two are unknown-argument errors for
  now; they become named deferrals when their arg forms land).
- Unit-test *harness* (registry, runner, capturing sink, `make test`
  contract) → Sprint 1; this sprint's `unit_main.c` is a plain `main()`
  of asserts, replaced there. Note now: registration will use an
  explicit registry table, never `__attribute__((constructor))`.
- `scripts/smoke.sh`, CI, ban greps → Sprint 1. `make test-script` /
  `test-pty` / `fuzz` targets → Sprint 1 (as hard errors).

## Testing Strategy

- **Unit (provisional):** `tests/unit/unit_main.c` exercises every
  container: arena alignment (1-byte alloc then `alignas`-16 struct,
  check address), vec growth past 3 reallocations, strmap 10k inserts
  then iterate asserting exact insertion order + replace-keeps-order,
  interner id stability and round-trip, stable sort on duplicate keys
  asserting original relative order, `bytebuf_printf`. Log: install a
  capturing sink, assert level + message; `yew_mod_require` for a
  disabled module returns false with the canonical message (build a
  `MODULES=""` variant in CI from Sprint 1; locally by hand).
- **Driver (manual this sprint, smoke.sh from Sprint 1):** the exact
  commands live in the DoD below; run them by hand against both a full
  and a `MODULES=""` build.
- **Both compilers:** `make clean && make CC=gcc`, then `CC=clang` —
  zero warnings on both.

## Definition of Done

1. `make` produces one `build/yew` executable with gcc
   and clang, zero warnings, `-std=c11 -pedantic -Wall -Wextra -Werror
   -Wvla` on every TU.
2. `./build/yew --version | head -1` is exactly `yew 1.0.0-dev`;
   line 2 is `modules: lsp ai fuss plugins`. A `make MODULES=""` build
   prints `modules: none`. Exit 0 both.
3. `./build/yew --help` exits 0 with sectioned stdout and an unknown flag
   exits 1 naming the flag. Current closeout exercises the shipped file/TUI,
   batch, and clean paths through their Sprints 14/37/36 owners rather than
   reinstating their landing-day hard errors.
4. `./build/yew --selftest-bug` exits 4; stderr matches
   `yew: internal error at src/…:<line>: selftest` plus the report
   line; the same report appears in the log file.
5. `grep -rn '\bexit(' src/ | grep -v util/log.c` returns nothing;
   `main()` is the only return-to-OS site.
6. `scripts/bans.sh` supplies the token-aware ban control for actual raw
   `qsort`, attribute, and thread constructs, with seeded negative fixtures;
   explanatory comments and data are permitted.
7. `XDG_STATE_HOME=$(mktemp -d) ./build/yew --version >out 2>err`
   leaves `err` empty and `out` exactly the two version lines; the log
   dir, if created, is mode 0700. No yew invocation writes log text
   to stdout/stderr.
8. Provisional unit binary runs green under `make test`; ≥ 12
   container/log assertions including the insertion-order and
   stable-sort determinism cases.
9. Rebuilding with a different `MODULES` value (no `make clean`)
   relinks correctly — `mods.stamp` forces it; `--version` reflects the
   new set.
10. `make install DESTDIR=$(mktemp -d) PREFIX=/usr` installs one `bin/yew`
    executable; `make clean` removes `build/`.
11. LICENSE (GPL-3.0 verbatim), README, `.gitignore` present;
    `git status` clean after `make && make test` (nothing generated
    outside `build/`).
