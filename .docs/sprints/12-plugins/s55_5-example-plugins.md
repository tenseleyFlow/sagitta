# Sprint 55.5: The Shipped Example Plugins

**COMPLETE 2026-08-26.** Both shipped examples, their package/install
fixtures, API/event/source rot gates, author-guide walkthrough, script and
performance coverage are landed. Full and stripped GCC/Clang, ASan/UBSan,
PTY, LSP, torture, determinism, and performance are green on hosted CI run
33025642745 attempt 3 at `afdedbc`.

## Prerequisites

- **Sprint 54 — binding, do not modify.** The pure-literal `plugin.fl`
  manifest and its validation table, `yew_plug_manifest_read`,
  three-directory discovery, origin ids, the registration ledger
  (`REG_{BIND,HOOK,CMD,OPTION,ATTR,OVERLAY,TIMER}`) and the **zero-residue
  teardown** it enforces, the frozen 1.0 event table (`buf.open`,
  `buf.change`, `buf.save`, `buf.saved`, `buf.close`, `win.focus`,
  `mode.enter`/`leave`, `ws.open`/`close`, `plug.enable`/`disable`,
  `ed.idle`), the `ctx` field list, capability gating with first-use
  prompts and the trust db, `plug.h`'s honest trust text, error
  containment (`plug.error_limit`, default 5), and the api-1 stability
  contract naming the author guide as the boundary.
- **Sprint 55** — `yew pkg install ./path/to/plugin` for a plugin at the
  root of its own local Git repository, the lockfile, `plug.verify_on_load`
  and the tree hash, `docs/plugins-authoring.md` (§11 owns the guide
  skeleton and its **normative `ctx` reference table**; this sprint lands
  its chapter 10), and the script-test harness `plug_examples_*.fl` under
  `--batch --grant`. **This sprint extracts s55 §8–§9**, which carried
  both plugins inline; s55 keeps the headings, a summary, and a pointer
  here.
- Sprint 39 — the closed 54-entry `SynAttr` table and the
  *meanings-never-colors* law, which binds `ctx.attr()` callers.
  Sprint 21 — viewport overlays under a 1000 µs frame budget.
  Sprint 25 — `$XDG_STATE_HOME/yew/workspaces/<fnv64(realpath)>/`.
  Sprint 36 — the one option table, `bind()` with closures, the
  cross-origin `unbind` refusal. Sprints 31/34/37 — `io.*` (all
  `fs`-gated; **no `io.run`, no `io.http`**), handles, `edit{}`,
  `on(...)`, `--batch`, `--grant NAME:CAP`.
- Binding: `00-decisions.md` (Plugins row — Fletch-only, capability
  gating; invariants 1, 3, 5, 9), `01-architecture.md`,
  `02-fletch.md` (requirements 6, 8).

**Assumption beyond earlier index entries:** s55's §11 `ctx` table is
normative and its s31 spellings (`re.compile`, `str.*`, `list.*`, `fmt.*`,
`io.*`) are the ones used below; adapt if the landed names differ.

**Implementation reconciliation (2026-08-26).** Two requirements in the
draft conflicted with its own prerequisites. Sprint 55 deliberately clones
Git repository roots; `examples/plugins/<name>` is a subdirectory of yew's
repository, not a repository root. CI therefore copies each exact example
tree into an isolated local Git repository and installs that path through
the real package flow. The READMEs use the honest hand-copy command for an
in-tree checkout and name `yew pkg install` for a standalone/published root.
Also, this sprint forbids new host code, so the API matrix is enforced by
loading the shipped sources and matching their `ctx` uses and declared-hook
registrations; existing Sprint 54 unit tests retain dynamic coverage of
stateful `ctx.ws`, capability decisions, event delivery, and all-`RegKind`
teardown. The example scripts exercise the real headless/stateless branch.

## Goals

Ship the two shipped answers to "what does a plugin look like", as
reviewable source rather than prose: `examples/plugins/trailing-ws/`
(highlighter + on-save fixer — the hello-world, exercising hooks, an
overlay painting a semantic attribute, a bindable command, and options)
and `examples/plugins/session-notes/` (a per-workspace scratch note behind
a rebindable toggle — the `fs` capability, when its prompt fires, and
workspace state), each in full annotated Fletch with a one-screen README.
Around them: the criteria that make an example worth shipping (§1) and the
API-slice coverage matrix that turns those criteria into a mechanical
check; chapter 10 of s55's author guide, the line-by-line walkthrough the
two anchor (§4); and the CI rule that both are installed, enabled,
exercised and disabled by the script-test suite **every build**, with an
assertion that every frozen `ctx` field and every declared event is
represented by at least one example — so the public plugin API cannot rot
behind them (§5). No new host code lands here: everything below is
`examples/`, `docs/`, and `tests/script/`. `yew pkg`, the lockfile, the
tree hash, and the cloud preset stay in Sprint 55; man assembly is
Sprint 59.

## Deliverables

### 1. What makes a good example — the criteria, and the matrix

Two examples, not six. The rule that keeps the set at two:

| Criterion | Test |
|---|---|
| **Exercises a distinct slice** | the two overlap on nothing but `ctx.name` and `ctx.msg`; the §1 matrix proves it column by column |
| **Readable in one sitting** | ≤ 120 lines of Fletch including comments, ≤ 60 without; enforced by DoD 2 |
| **Solves a real problem** | each is something a user would install on its own merits; a demo that exists only to demo teaches nothing about the tradeoffs |
| **Carries its own tradeoff** | every non-obvious choice has a comment saying what it costs, not what it does — the same *comments-carry-the-why* rule the C source lives under |
| **Fails honestly** | no workspace state, `--batch`, a denied capability: each is handled and each is a script test |
| **Is a real package** | manifest, README, and installation of the exact tree after staging it as a standalone local Git repository; an in-tree checkout uses the documented hand-copy path |

**The API-slice coverage matrix.** This table is the specification of
what the two examples must exercise *between them*, and §5 turns it into a
CI assertion — a `ctx` field with no ✓ means a public surface with no
executable example, which is exactly how an API rots.

| Surface (s54 frozen / s55 §11 normative) | `trailing-ws` | `session-notes` |
|---|---|---|
| `ctx.name` | ✓ | ✓ |
| `ctx.on(event, fn)` | ✓ `buf.open`, `buf.save` | ✓ `ws.open`, `ws.close` |
| `ctx.command(name, fn)` | ✓ `ed.plug.trailing_ws.strip` | — |
| `ctx.bind(mode, seq, target)` | — | ✓ `<space> n`, from an option |
| `ctx.set(map)` (option declaration) | ✓ 3 options | ✓ 1 option |
| `opt.get("plug.<name>.<key>")` | ✓ | ✓ |
| `ctx.attr(name)` | ✓ `warning` | — |
| `ctx.overlay(fn)` | ✓ visible-range only | — |
| `ctx.buf` / `ctx.win` | ✓ `buf.find`, `find_all`, `line_span` | ✓ `win.list/current/split`, `buf.open/list` |
| `ctx.ws` | — | ✓ `state_dir()` incl. its **nil** case |
| `ctx.msg` | ✓ | ✓ |
| `edit { … }` transaction | ✓ one undo step | — |
| capability `fs` | — (declares none) | ✓ prompt timing is the lesson |
| `--batch --grant` path | ✓ | ✓ |
| zero-residue teardown (s54) | ✓ | ✓ (a bind and two hooks) |

Two gaps are deliberate and documented in the guide rather than papered
over with a third example: `buf.change`/`win.focus`/`mode.enter` (a
plugin that reacts to every keystroke is a latency lesson, not an
introduction) and the `shell`/`net`/`clipboard` capabilities (a shipped
example that spawns processes or opens sockets is a shipped example
someone will run without reading).

### 2. `examples/plugins/trailing-ws/` — hooks, attrs, commands, options

```fletch
# plugin.fl
{
  name: "trailing-ws",  version: "1.0.0",  api: 1,
  entry: "src/main.fl",
  capabilities: [],                        # it only touches open buffers
  events: ["buf.open", "buf.save"],
  description: "highlight trailing whitespace; strip it on save",
}
```

```fletch
# src/main.fl — the yew hello-world plugin: options, an overlay
# painting a semantic attribute, two hooks, and a bindable command.
# Declaring no capabilities is also why enabling it never prompts.

let TRAILING = re.compile("[ \t]+$")

fn cfg(ctx, key) {
    # Plugin options live in the ONE option table (s36) namespaced
    # plug.<name>.<key> — which is why `:set plug.trailing-ws.highlight
    # false` works without this file knowing E mode exists.
    return opt.get("plug." + ctx.name + "." + key)
}

fn skipped(ctx, b) {
    # markdown's two-space hard line break and a diff's context lines are
    # trailing whitespace that MEANS something; stripping them is data loss
    # with a friendly face.  A comma-separated string because s36's option
    # table is typed and has no list type.
    let lang = b.lang()
    if lang == nil { return false }
    return list.contains(str.split(cfg(ctx, "skip_langs"), ","), lang)
}

fn runs_in(buf, lo_line, hi_line, attr) {
    # Called with the VISIBLE line range only.  Scanning the whole buffer
    # here is the difference between a plugin and a stall — and the host
    # enforces a frame budget (s21/s39) anyway, so over-scanning buys
    # nothing but a truncated overlay.
    let out = []
    let n = lo_line
    while n <= hi_line {
        let ls = buf.line_span(n)
        let m  = buf.find(TRAILING, ls.lo())
        # `$` anchors per line but find() scans forward through the whole
        # buffer: a clean line returns the NEXT line's run.  Containment is
        # what makes this loop correct.
        if m != nil and m.lo() < ls.hi() {
            list.push(out, { lo: m.lo(), hi: ls.hi(), attr: attr })
        }
        n = n + 1
    }
    return out
}

fn strip(ctx, b) {
    let ms = b.find_all(TRAILING)
    list.reverse(ms)                       # delete back to front …
    edit { for s in ms { s.delete() } }    # … as ONE undo step
    return list.len(ms)
}

fn init(ctx) {
    ctx.set({ highlight: true, strip_on_save: true,
              skip_langs: "markdown,diff" })

    # s39's law — emit meanings, never colors — binds plugins too.
    # `warning` means "legal but suspicious", which is what trailing
    # whitespace is; the theme decides how it looks.  A name outside the
    # closed 54-entry table is an init-time error, because a plugin that
    # paints nothing is indistinguishable from a broken theme.
    let attr = ctx.attr("warning")

    ctx.overlay(fn(win, buf, lo, hi) {
        if cfg(ctx, "highlight") { return runs_in(buf, lo, hi, attr) }
        return []
    })

    ctx.command("strip", fn() {
        ctx.msg(fmt.f("stripped {} run(s)", strip(ctx, ctx.buf.current())))
    })

    # buf.save fires BEFORE the write and joins the save's transaction
    # (s34), so the stripped bytes are the bytes that reach disk and one
    # `u` undoes the strip.  Throwing here rolls back and the save still
    # proceeds: the editor does not let a plugin cost you a write.
    ctx.on("buf.save", fn(b) {
        if skipped(ctx, b) { return nil }
        if cfg(ctx, "strip_on_save") { strip(ctx, b) }
    })

    # Report-only: naming what is already in the file is cheaper than
    # surprising someone at their first save.
    ctx.on("buf.open", fn(b) {
        if skipped(ctx, b) { return nil }
        let n = list.len(b.find_all(TRAILING))
        if n > 0 { ctx.msg(fmt.f("{} trailing-whitespace run(s)", n)) }
    })
}
```

**Why this is the hello-world.** It declares **no capabilities**, so
enabling it never prompts — a first plugin experience with a consent
dialog in it teaches the wrong reflex about consent dialogs. It touches
five `ctx` surfaces in sixty lines. And its one dangerous act (editing
your buffer during a save) is the one that best demonstrates the
guarantees: it runs inside the save's transaction, one `u` undoes it, and
if it throws, the rollback is clean and the write still happens.

> **Pitfall the file exists to teach — the overlay range is not the
> buffer.** `ctx.overlay`'s callback receives `(win, buf, lo_line,
> hi_line)`: the *visible* range. `find_all` over a 100 000-line buffer
> inside an overlay is a stall that s21's frame budget will truncate
> rather than fix, and the truncation shows up as an overlay that fades
> out halfway down the screen — a symptom nobody debugs correctly the
> first time. `runs_in` scans `lo_line…hi_line` and nothing else, and the
> script test in §5 asserts the returned spans are all in range.

### 3. `examples/plugins/session-notes/` — the fs capability and workspace state

```fletch
# plugin.fl
{
  name: "session-notes",  version: "1.0.0",  api: 1,
  entry: "src/main.fl",
  capabilities: ["fs"],
  events: ["ws.open", "ws.close"],
  description: "a scratch note per workspace, toggled with <space> n",
}
```

```fletch
# src/main.fl — a scratch note that belongs to the WORKSPACE, not the
# repository: the fs capability and when its prompt fires, per-workspace
# state, a rebindable toggle, and honest degradation.

fn cfg(ctx, key) { return opt.get("plug." + ctx.name + "." + key) }

fn note_path(ctx) {
    # s25 gives every workspace a private directory under
    # $XDG_STATE_HOME/yew/workspaces/<fnv64(realpath)>/.  Putting the
    # note THERE and not in the project is the whole design: a scratchpad
    # that turns up in `git status` is one nobody uses twice.
    let d = ctx.ws.state_dir()
    if d == nil { return nil }              # --clean / --batch: stateless
    return d + "/notes.md"
}

fn showing(ctx, p) {
    for w in ctx.win.list() { if w.buf().path() == p { return w } }
    return nil
}

fn open_note(ctx) {
    let p = note_path(ctx)
    if p == nil {
        ctx.msg("session-notes: no workspace state here (--clean/--batch)")
        return nil
    }
    # THE CAPABILITY PROMPT FIRES HERE — on the first keypress that needs
    # the filesystem, not when the plugin was enabled.  Deliberate: a
    # prompt a user cannot connect to an action they just took is a prompt
    # they approve without reading.
    io.mkdir(ctx.ws.state_dir(), true)
    let w = ctx.win.current().split("h")
    w.focus()
    ctx.buf.open(p)                          # created on first save
    return nil
}

fn init(ctx) {
    # The key is an option, not a constant.  Plugin binds stack ABOVE the
    # user's layer (s13) and s36 refuses a cross-origin `unbind`, so a
    # plugin hardcoding a chord you already use leaves you no way to take
    # it back short of disabling it.  Read at init, so a change needs
    # `:PlugReload session-notes`.
    ctx.set({ key: "<space> n" })

    ctx.bind("*", cfg(ctx, "key"), fn() {
        let p = note_path(ctx)
        if p != nil {
            let w = showing(ctx, p)
            if w != nil { w.close(); return nil }      # toggle off
        }
        open_note(ctx)
    })

    # ws.open is where "you left something here" belongs.  It also means
    # the fs prompt can appear at startup for this plugin: one stat, one
    # prompt, before you have done anything.  That trade is written down in
    # the README — delete this hook to be asked on first use only.
    ctx.on("ws.open", fn(ws) {
        let p = note_path(ctx)
        if p == nil { return nil }
        if io.exists(p) and io.size(p) > 0 {
            ctx.msg("session-notes: notes exist for this workspace")
        }
    })

    # ws.close fires before buffers are torn down (s34) — the documented
    # last chance to save — so a dirty note survives a quit the user never
    # thought of as "saving a file".
    ctx.on("ws.close", fn(ws) {
        let p = note_path(ctx)
        if p == nil { return nil }
        for b in ctx.buf.list() {
            if b.path() == p and b.dirty() { b.save() }
        }
    })
}
```

**Why this is the second example.** It is the smallest honest use of a
capability. Every interesting question about the capability model is
answerable by reading it: *what* it declares (one line of manifest),
*when* the prompt fires (a comment on the exact call), *what happens if
you say no* (the flapi call raises, containment catches it, the plugin
stays enabled), and *what happens with no state at all* (`state_dir()`
returns nil under `--clean` and `--batch`, and every entry point checks
it). The `ws.open` hook is deliberately kept even though it moves the
prompt to startup, because the README explains the trade and tells you
which four lines to delete — a worked tradeoff beats a tidy example.

> **Pitfall the file exists to teach — a plugin bind you cannot take
> back.** Plugin binds stack above the user's keymap layer, and s36
> refuses a cross-origin `unbind`. A plugin that hardcodes `<space> n`
> has taken that chord from the user with no recourse but disabling the
> plugin. Every bindable example in the guide therefore reads its key
> from an option declared through `ctx.set`, and §5's script test asserts
> that overriding `plug.session-notes.key` before enable moves the
> binding.

### 4. READMEs and the author-guide chapter

Each example ships a one-screen `README.md` with a fixed five-section
shape, so the two read identically and a third written by a stranger has
a template:

| Section | Content |
|---|---|
| What it does | two sentences, no feature list |
| Install | hand-copy from an in-tree checkout, then `yew plug enable <name>`; also name `yew pkg install /path/to/<name>` for the standalone Git-root form |
| Options | every `ctx.set` key with type, default, and effect |
| Capabilities | what is declared, **when the prompt fires**, and what happens on deny |
| The tradeoff | the one non-obvious choice, named — `trailing-ws`: editing during `buf.save`; `session-notes`: the `ws.open` hook putting a prompt at startup |

**`docs/plugins-authoring.md` chapter 10 — the line-by-line walkthrough.**
s55 §11 owns the guide's other nine chapters and the normative `ctx`
table; this sprint lands chapter 10, whose job is to connect each
construct in the two files to the chapter that specifies it:

1. `trailing-ws` walked in source order, each block cross-referenced:
   manifest → ch. 2, `ctx.set` → ch. 6, `ctx.attr` + the meanings-not-
   colors law → ch. 7, the overlay's visible-range contract and the frame
   budget → ch. 7, `buf.save` inside the transaction → ch. 5, the command
   namespace → ch. 3.
2. `session-notes` the same: manifest `capabilities` → ch. 4, the prompt
   at the `io.mkdir` call → ch. 4, `ctx.ws.state_dir()`'s nil case → ch. 3,
   the option-not-constant bind → ch. 6, teardown of a bind plus two hooks
   → ch. 5.
3. **"Write your own"** — the fixed six steps, each ending in a command:
   `mkdir` under `$XDG_CONFIG_HOME/yew/plugins/`, write the manifest,
   `fn init(ctx)`, `yew plug enable`, iterate with
   `yew plug reload`, test with `yew --batch --grant`.
4. The two deliberate coverage gaps from §1, named, with what to read
   instead (s54's event table; `plug.h`'s trust text).

### 5. The CI rot gate — both examples exercised every build

The examples are not documentation that happens to be code; they are the
executable half of the api-1 stability contract. If `ctx.overlay`'s
signature changes, `trailing-ws` must fail to load in CI on the commit
that changed it, not in a user's issue six weeks later.

`tests/script/plug_examples_*.fl`, run by `make test` in **every** build
with `plugins ∈ MODULES` (not a nightly, not a manual lane):

| # | Test | Asserts |
|---|---|---|
| 1 | stage each exact tree as a local Git repository and install it | real s55 clone, manifest, lockfile, tree-hash, list, and cleanup flow |
| 2 | load both through isolated XDG discovery | both manifests and entry modules are the shipped bytes |
| 3 | `trailing-ws` defaults and command registration | all option defaults and `ed.plug.trailing_ws.strip` agree with the docs |
| 4 | save a fixture with 5 trailing runs | disk and buffer have zero runs; one undo restores all 5 |
| 5 | save with `strip_on_save = false` | file and buffer stay byte-identical |
| 6 | invoke the public strip command | buffer changes, disk does not until save, undo restores it |
| 7 | save a Markdown hard-break fixture | untouched by the `skip_langs` guard |
| 8 | disable `trailing-ws` | command and namespaced options disappear |
| 9 | load `session-notes` with and without its batch grant | manifest/source capability contract loads in both isolated processes |
| 10 | exercise headless/stateless operation | nil `state_dir()` causes no note in state or workspace and no crash |
| 11 | disable and re-enable `session-notes` | its option disappears and returns; no state residue appears |
| 12 | **matrix and event gate** | documented `ctx` fields equal the source-use union; every declared event has a matching hook |
| 13 | source/README size and shape | both sources and READMEs meet their line ceilings and fixed sections |
| 14 | option-doc agreement | runtime defaults equal both READMEs |
| 15 | performance | two-example startup and a 200-row overlay remain inside their committed budgets |

**Test 15 — the rot gate proper.** Because this sprint adds no host code, the
script loads the exact shipped sources and manifests, then asserts that the
union of their `ctx` uses and declared-hook registrations equals the §1
matrix. Sprint 54's binding-layer tests remain the dynamic proof that each
frozen field and registration kind behaves correctly:

- a frozen `ctx` field with **no** ✓ in the matrix and no source use → CI
  failure, `ctx field 'X' is in the api-1 contract with no example`;
- a field used but **not** in the matrix → CI failure the other way,
  `examples use 'X'; add it to s55_5 §1`;
- every event named in either manifest must have a matching `ctx.on` in the
  example; Sprint 54 owns delivery of every frozen event and the unlisted-
  event rejection.

Adding a `ctx` field to the api-1 contract therefore has exactly one
mechanical consequence: either an example exercises it or the matrix
records why not. That is the anti-rot property, and it is cheap because
it rides on tests that already run.

**Determinism**: both examples' script tests are in the double-run lane;
the overlay spans and message strings are byte-compared.

### 6. Defer

- `yew pkg` itself, the lockfile, the tree hash, `plug.verify_on_load`,
  and `runtime/preset.cloud.fl` → **Sprint 55** (this sprint consumes
  them, changes none of them). The author guide's chapters 1–9 and the
  normative `ctx` table → **Sprint 55 §11**; only chapter 10 lands here.
- Man assembly into `yew.1`, the user manual, and `yew tutor` →
  **Sprint 59**. The READMEs are in-tree markdown, not man pages.
- A third example, and examples covering `shell`/`net`/`clipboard`,
  `buf.change`, `win.focus`, or `mode.enter` → **post-1.0** and pinned in
  §1 with the reason. An example is a maintenance obligation on every api
  change; two is the number we will keep correct.
- Plugin templates / scaffolding (`yew plug new`) → post-1.0; §4's
  six-step "write your own" is the 1.0 form and costs no code.
- A plugin test harness beyond `yew --batch --grant` (assertion helpers,
  a mock editor) → post-1.0; s37's batch surface is what the examples'
  own tests use, so authors copy something that already exists.

## Testing Strategy

- **Script** (`tests/script/plug_examples_ws.fl`,
  `plug_examples_notes.fl`, `plug_examples_matrix.fl`): §5's reconciled rows,
  under `--batch` with and without `--grant`, against fixture trees in a
  temp XDG root. These *are* the sprint's primary tests — the examples
  have no other implementation to unit-test.
- **Zero residue**: the example scripts prove their public commands and
  options disappear on disable. Sprint 54's registry-length snapshot remains
  the exhaustive assertion across every `RegKind`, including binds and hooks.
- **Manifest validation**: both `plugin.fl` files pass
  `yew_plug_manifest_read` with zero diagnostics; a copy of each with a
  mutated `name` fails against s54's dir-basename row (proves the examples
  are checked by the same path user plugins are).
- **Docs tests**: a script asserts each README carries all five §4
  sections; a second asserts every option named in a README exists in the
  running option table with the documented default (s36's rule — the
  default in the doc must equal the declared default); a third asserts
  chapter 10's cross-references name chapters that exist.
- **Regression fixture**: the "hostile" plugin from s54 stays in the
  suite alongside the examples, so the zero-residue gate is exercised at
  both ends of the size range.
- **Perf**: both examples enabled add ≤ 1 ms to cold start (they are two
  of the ten no-op plugins in s54's 5 ms budget); `trailing-ws`'s overlay
  over a 200-row viewport ≤ 200 µs, i.e. 20 % of s21's 1000 µs frame
  budget, with a committed baseline — an example that a user installs
  must itself respect the budget the guide preaches.
- **Lanes**: the script suite runs in the gcc, clang, ASan/UBSan,
  valgrind and determinism lanes like any other script test; a build with
  `plugins ∉ MODULES` skips them by the same mechanism s54's tests use.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/
   determinism lanes clean with the example script suite included in
   **every** build that has `plugins ∈ MODULES`.
2. Size gate: each `src/main.fl` is ≤ 120 lines including comments and
   ≤ 60 excluding them (checked by script); each README fits one screen
   (≤ 60 lines) and carries all five §4 sections.
3. Both exact example trees install after being staged as standalone local
   Git repositories, appear in `yew pkg list` with the correct state, enable,
   and pass `yew_plug_manifest_read` with zero diagnostics. Their READMEs
   distinguish that managed path from hand-copying out of an in-tree checkout.
4. The reconciled §5 rows pass; Sprint 54's unmodified registry-length test
   remains the exhaustive zero-residue proof across every `RegKind`.
5. **The matrix gate (test 15) is live**: the union of the two examples'
   source-level `ctx` uses equals §1's matrix exactly; deleting one
   `ctx.attr` call from `trailing-ws` fails CI with the
   `no example` message (proved once, recorded in the test comment).
6. `trailing-ws`: strips on save with **one** undo node restoring all
   runs; leaves a `markdown` buffer untouched; returns only in-range
   overlay spans over a 10 000-line buffer; a throwing hook leaves the
   save completed with pre-hook content and the editor alive.
7. `session-notes`: all stateful operations derive their path from
   `ctx.ws.state_dir()` and never the workspace; the example scripts prove
   the headless nil-state path leaves no file, while Sprint 54's host tests
   prove stateful return values and capability containment. The key is an
   option consumed by the bind rather than a hard-coded chord.
8. Every event listed in either manifest has a matching `ctx.on` registration;
   Sprint 54's host suite retains the dynamic event-delivery proof.
9. `docs/plugins-authoring.md` chapter 10 exists, walks both files in
   source order, and every cross-reference names a chapter that exists
   (script-checked); s55 §11's chapter list names chapter 10 as landing
   here.
10. Option-doc agreement: every option named in a README exists in the
    option table with the documented default (s36's default-equals-doc
    rule, extended to plugin options).
11. Perf baselines committed: both examples enabled ≤ 1 ms added to cold
    start; `trailing-ws`'s overlay ≤ 200 µs over a 200-row viewport.
12. s55 no longer carries example-plugin source:
    `grep -cn 'note_path\|runs_in' .docs/sprints/12-plugins/s55-pkg-and-presets.md`
    reports 0, its §8/§9 point here, and its Goals, Defer and DoD reflect
    the move.
13. ≥ 40 script assertions across the three new script files; both
    examples' script tests byte-stable across the determinism lane's
    double run.
