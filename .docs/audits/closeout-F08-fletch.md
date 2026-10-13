# Closeout: F08 — Fletch language and VM

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s28, s29, s30, s31, s32, s33, s34, s36

## DoD items (every numbered item from every sprint above)

- [ ] s28.1 `.docs/fletch-spec.md` exists with all 16 sections; §16 empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.2 Preface traceability matrix maps all 8 design requirements from `02-fletch.md` to sections; no requirement unmapped.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.3 §2 EBNF is complete: every nonterminal defined exactly once; every terminal in §1's token inventory; grep finds no `TODO`/`TBD`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.4 §3 vocabulary table covers: 4 unit prefixes, 4 arrows + 4 alt-variants + 4 Unicode aliases, `H()`, `i"…"`, `del`, `esc`, counts, CMDWORD — with key origin for each row.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.5 §14 example is exactly the program above (≤ 35 lines), with normative expected results stated.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.6 Pinned decision sentences present verbatim-checkable: no implicit globals; capture by reference; insertion-ordered maps; `nil`/`false` falsy only; int `/` truncates; assignment is a statement; pure-literal grants nothing; capability checks read defining-module origin.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.7 Each § carries a "Conformance:" line naming its future `tests/fletch/` file.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.8 Closed error-kind set (11 kinds) and reserved-word set (22 words) enumerated; counts stated in the spec so s33 can assert them.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s28.9 Review sign-off recorded in the spec preface (date + reviewer); freeze declared. No code, no new directories outside `.docs/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.1 `src/fl/{lex.c,lex.h,ast.h,parse.c,parse.h}` build in both compiler lanes, zero warnings. The original touched-directory restriction was a Sprint 29 landing-scope control; release closeout preserves that history without requiring later owning sprints to leave the subsystem untouched.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.2 Token inventory implements every spec §1 row; unit tests cover each (≥ 60 lex assertions).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.3 Every spec §2 production has ≥ 1 positive dump-golden; every §2 terminal appears in some fixture. Coverage listed in a comment table at the top of `test_fl_parse.c` (production → test name) with zero gaps — s33 formalizes this, don't wait for it to be honest.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.4 `_Static_assert(sizeof(FlNode) <= 48)`. Lexer and parser allocation remains caller-arena-owned: AST construction takes the caller's arena, the lexer/parser implementation contains no direct allocator calls, and the repository allocator gate continues to reject raw libc allocation.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.5 Expected/found wording matches Deliverable 4 exactly in goldens; the four special-cased messages each have a fixture.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.6 All 8 recovery fixtures: exactly 1 diagnostic each.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.7 `fl_parse_literal` rejects all 5 negative classes naming pure-literal mode; accepts the s25 workspace-state schema example verbatim.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.8 Spec §14 example: parses with 0 errors; golden committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.9 Fuzzers: 10 min each, ASan/UBSan clean, no hang (alarm never fires), corpus committed and replayed in `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s29.10 `incomplete` flag: unit tests for `fn f(` (incomplete), `fn f((` (incomplete), `fn f()}` (error, not incomplete), trailing `+` (incomplete), trailing `+ 1` (complete).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.1 `src/fl/{value.c,value.h,vm.c,vm.h,opcodes.def,compile.c,compile.h, gc.c,gc.h}` build in the gcc and clang lanes with zero warnings under `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, and again with `FL_CGOTO=1` (where `vm.c` alone uses `-std=gnu11`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.2 `_Static_assert(sizeof(FlValue) == 16)`, `sizeof(FlObj) == 16`, `FL_OP__COUNT == 60`, `FL_OP__COUNT <= 0xF0` all present and passing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.3 All 60 opcodes are implemented, disassembled, and reached by at least one fixture; a coverage comment table at the top of `test_fl_compile.c` maps opcode → test with zero gaps (s33 formalizes; be honest now).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.4 Spec §14 example: compiles clean, `total == 17`, `counter` yields 9 then 10, `shout` returns `"MOTION"` — asserted, not printed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.5 Both dispatch modes produce byte-identical opcode traces over every fixture; `make FL_CGOTO=0 test` and `make FL_CGOTO=1 test` both exit 0.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.6 Map iteration order is asserted stable across processes and across a compaction; deleted-then-reinserted keys land at the end.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.7 Every root in the §9 table has a drop-it test; the GC-stress lane is green over `make test` plus the s29 corpus; valgrind reports zero definite leaks and ASan reports zero use-after-poison.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.8 Every `FlObj` allocation remains GC-owned through `fl_gc_alloc` and is covered by the enumerated-root and stress tests. Later host-side Fletch registries and configuration/recorder state may use the project's checked allocation wrappers for non-`FlObj` storage; the repository allocator gate continues to reject direct libc allocation.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.9 Upvalue semantics: by-reference sharing and per-iteration loop bindings each proven by a test whose failure mode is the classic bug.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.10 Reserved handle tags landed unconstructible in this sprint; Sprint 34 subsequently delivered their generation-checked constructors and type conversions. Sprint 31 delivered import execution; `FL_OP_IMPORT` remains compiled, disassembled, and covered by the 60-opcode conformance ledger.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.11 Amendment §16-A1 is filed in `.docs/fletch-spec.md` with reviewer and date; §9's stated kind count reads 12.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s30.12 Perf smoke numbers recorded in `tests/perf/baselines/fletch_smoke.txt` for both dispatch modes, so s33's gates start from measured ground.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.1 `src/fl/{std,stdstr,stdlist,stdmap,stdmath,stdfmt,stdio,stdre,module}.c` plus headers build in both compiler lanes with zero warnings; the sanitize, valgrind, determinism, and GC-stress lanes are green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.2 All seven modules registered; `yew fl --list-natives` (hidden flag, s32 documents it) prints **117** names in deterministic order and matches the tables in this file exactly. *(Corrected 2026-08-09, C1. The tables define 30 str + 19 list + 12 map + 29 math + 7 fmt + 13 io + 7 re = 117 functions; the ≥ 150 was a planning estimate written before they were finalized. The tables are the contract — this clause's own second half says so — so the count follows them rather than the other way round. The six `math` constants are surface but not natives and the flag does not list them; s33's ledger may want a `--list-surface` that does.)*
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.3 Every function in every table has ≥ 1 positive unit test and every listed error kind ≥ 1 negative test; a coverage comment table per test file maps function → tests with zero gaps.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.4 Grapheme law: `str.slice_bytes` raises `"index"` for every interior byte of every multi-byte cluster in the s02 golden corpus; no `str` function alters an invalid byte (round-trip asserted over the whole corpus).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.5 Every printf-family call **and every `fl_raise`** in `src/fl/` takes a format that is a string literal in our own source; the sole exception is a bounded vararg forwarder (a `v`-variant whose last argument is its own `ap`). `scripts/bans.sh` enforces it by counting top-level commas to the format position, and carries a seeded violation whose hit count it asserts. *(Corrected 2026-08-09, C1. The original clause asked that `grep -rn 'printf\|sprintf\|snprintf\|vsnprintf' src/fl/` show only `stdfmt.c`. It cannot: `bytebuf_printf` matches `printf`, and the disassembler, the diagnostics and `fl_raise` all use it — with literal formats. The grep was a proxy for the rule; the rule is now stated directly and is strictly stronger, because it also covers `fl_raise`, which the grep never saw.)*
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.6 Capability delegation fixture green: plugin → config-helper → `io.read` is **denied**; the same helper under a config origin succeeds. `grep -n 'caps' src/fl/std*.c` shows no path reading a VM-global or `frames[0]` grant.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.7 Import cycle message names all four lines in load order; `./a.fl` and `a.fl` load once; a file imported under two origins loads twice and the two module maps are distinct objects.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.8 `fmt.repr` round-trips 500 generated pure-literal values through `fl_parse_literal`; `fmt.f` fuzz (100 000 templates) is crash- and hang-free with every outcome a string or a `"type"` raise.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.9 `re` replays s20's golden match table with identical spans; empty-match scanning terminates on a 1 MiB subject; every template-error case fires.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.10 Case mapping is available with regenerated tables; `make unicode-tables && git diff --exit-code src/unicode/` is clean and the `unicode` CI lane is green. *(Corrected 2026-08-09, C1. `yew_case_map`/`yew_case_map_utf8` are EMITTED by `scripts/gen-unicode-tables.c` into `src/unicode/tables_case.c`, behind `src/unicode/case.h`; there is no hand-written `src/unicode/case.c` and adding one would put hand-maintained code in a generated file or an empty wrapper beside it. The clause named a file; what it wanted was the capability and the clean regeneration, both of which hold.)*
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.11 Later owning sprints delivered the editor surfaces `bind`, `set`, `on`, `buf`, and `win` through the capability-checked host API. The permanent 1.0 boundary remains: there is no `io.run` or `io.http` Fletch stdlib symbol anywhere in `src/fl/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s31.12 `math.random` reproduces a committed golden sequence; the determinism lane's double-run diff is clean. *(Clarified 2026-08-09, C1. `rng_state` is a file-static and therefore process-global, so the pristine DEFAULT-seed sequence is only observable in a fresh process — `yew fl` shows it, a shared test binary cannot. The golden is pinned after an explicit `math.seed(1)`, which tests the same thing: the generator's output for a known state.)*
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.1 `src/fl/repl.c|h` plus the `yew fl` subcommand build in both compiler lanes with zero warnings; sanitize, valgrind, determinism, GC-stress and both `FL_CGOTO` settings are green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.2 All seven `yew fl` invocation forms in §1 work; `--help` documents them and `scripts/smoke.sh` covers every exit-code row (0, 1, 1, 3, 4).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.3 s18's unit suite passes **unchanged**; the `CompReq.ud` widening lands; `grep -rn 'char .*\[512\]' src/fl/repl.c` finds nothing — there is no second line editor. **(amended, C1)** The prompt shares the TEXT layer (TextBuf, Cursor, `yew_edit_*`, grapheme motion) rather than s18's key handler, so there is no `yew_cmdline_edit_key` extraction and s18 is untouched.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.4 The s29 `incomplete` matrix drives the REPL correctly (5 pinned cases + the 12-line entry); the trial parse emits zero output to the printing sink, asserted via a capturing sink.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.5 Pty goldens for the REPL session and for the post-`yew_bug` terminal restore are committed and byte-exact; a second run of the harness is byte-identical. **(amended, C2)** The harness VT models only the ALTERNATE screen's grid, so a primary-screen child's goldens pin `alt=0`, the cursor and the modes; the session's content is asserted against the byte log with `ptc_expect_output`/`ptc_reject_output`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.6 Trace format: every row of the frame-naming table asserted; call sites are the CALL instruction's own `line:col` (fixture with known columns); elision fires at 32 frames.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.7 Suggestions: ≥ 25 table rows green, ordering asserted exactly, and the out-of-scope negative case passes. No site scores against the global interner — `grep -n 'yew_intern_str' src/fl/` shows no use inside the suggestion code.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.8 `fuzz_fl_vm` runs 10 min ASan-clean and 10 min GC-stress-clean; every per-input invariant is checked; after an uncaught raise the VM satisfies `sp == stack && nframes == 0 && nhandlers == 0`; `tests/fuzz/crashes/` is empty; corpus committed and replayed by `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.9 `fl_chunk_check` is present, runs in checked builds after every compile, and each of its four checks has a unit test that fires it via a deliberately corrupted chunk (test-only mutator, never a shipped API).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.10 Every §9 invariant row produces the structured report (driven by a test-only injection hook), exits 4, and `YEW_FL_DUMP_BAD_CHUNK` writes a dump that `fl_disasm_chunk`'s own reader accepts. No bare `abort()` or `assert()` in `src/fl/` — `scripts/bans.sh` gains the rule.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.11 `vm.h` carries the no-verifier decision and its condition (any future bytecode loader lands with a real verifier, same commit) as a comment with the why, and this file's §8 is cited by section.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s32.12 Sprint 37 delivered `yew --batch`, and Sprint 34 delivered `bind`/`set`/`on` under the real editor host. The standalone REPL still uses the null host, so `@[…]` raises `"motion"` with a trace naming `<repl>`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.1 `tests/fletch/` contains ≥ 20 conformance files/directories covering all 16 spec sections; `make test-fletch` is green and part of `make test`, and the whole suite completes in < 20 s on the dev machine.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.2 `scripts/check-fletch-coverage.sh` exits 0, and all seven checks are proven to fire by a seeded violation in the meta-suite (spec section without a test, native without `COVERS`, kind without a case, stale ledger, missing `Conformance:` target, undefined production, unknown opcode).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.3 `ledger.txt` is committed and `run.c --ledger && git diff --exit-code tests/fletch/ledger.txt` is clean from a fresh clone.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.4 Totals in `ledger.txt`: sections 16/16, EBNF productions 38/38 (or whatever §2 defines — the count comes from the spec, not from this file), natives N/N matching `yew fl --list-natives`, kinds 12/12, opcodes 60/60.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.5 `14-example.fl` is byte-identical to the spec §14 block (diffed by the runner) and asserts all four normative results (`total == 17`; 9 then 10; `dup` defined; `shout` → `"MOTION"`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.6 Counts asserted by fixtures, not printed: 22 reserved words, 12 error kinds, 16 sections, 60 opcodes.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.7 Meta-suite: 11 cases, expected totals asserted; a directive inside a string literal is proven not to fire; a typo'd directive is a configuration error.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.8 Hard perf gates green on the dev machine and enforced in the perf lane: `config_load < 1 ms`, `motion_dispatch ≤ 1 µs/op`, `startup ≤ 2 ms`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.9 `tests/perf/baselines/fletch_dev.txt` committed with all 10 rows, each carrying a `why`; the two-condition regression gate proven by a seeded 20 % slowdown failing and a seeded 5 % slowdown passing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.10 `strbuild_concat/strbuild_join > 20` asserted at n = 5 000, with the comment explaining that the quadratic path is documented, not fixed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.11 GC-stress and both `FL_CGOTO` settings green over the whole suite; `# GC_STRESS: 0` opt-outs ≤ 2 and each carries a reason.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.12 The landing deferrals were tested rather than stubbed. Sprints 34, 35, and 37 subsequently replaced those guards with live editor bindings, the recorder round-trip harness, and `yew --batch`; their shipping suites now provide the release-closeout evidence.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s33.13 **MILESTONE proven**: the four artifacts of §6 are green in CI — `yew fl hello.fl`, `yew fl -e`, the REPL pty golden, and the coverage gate. `--help` and the README document `yew fl`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.1 `make test` green on gcc and clang at `-std=c11 -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes clean; the GC-stress lane green over the whole script suite.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.2 `_Static_assert(sizeof(FlHandle) == 8)`; `grep -n 'Buffer \*\|Win \*\| Cursor \*\|TextBuf \*' src/fl/handle.h` finds no struct field — handles carry no pointers.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.3 Dead-handle matrix green: for all five kinds, use-after-free, use-after slot-reuse, and use-after-object-close each raise kind `"handle"` and never dereference (ASan-clean under the crafted-handle fuzzer).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.4 Generation wraparound retires the slot; a unit test forces `gen = 0xFFFFFFFF` and asserts the slot never returns to the free list.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.5 **The side-door gate**: the Deliverable-3 grep prints nothing; `scripts/check-cmd-dispatch.sh` still passes; a seeded direct `yew_edit_insert` call in `flapi.c` makes `make test` fail. Every `FlBindDesc` row with `cmd != NULL` resolves at boot (YEW_BUG on typo), and every row with `cmd == NULL` is proven read-only by the same grep.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.6 Every `YEW_CMD_RECORDABLE` command has a unique, valid `word`; registering a recordable command without one is `YEW_BUG` (unit-tested), and `yew_cmd_by_word` round-trips against `yew_cmd_desc(id)->word` for every registered command.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.7 `edit{}` matrix: all 9 rows of §5 asserted, incl. multi-buffer commit and reverse-order rollback; a nested `edit{}` produces exactly **one** undo node (`yew_undo_dump` node count asserted); refactoring a script into helper functions does not change the node count.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.8 All 11 events fire at their documented dispatch points; `buf.change` and `cursor.move` fire **exactly once** per input burst under a 4 000-key paste fixture (asserted by a counting hook).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.9 Containment: a hook throwing in each of the 11 events leaves the editor running, rolls back, and disables at exactly 5; a self-triggering `buf.change` hook runs once and logs one drop line.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.10 Capability model: the plugin→user-helper chain test proves the check reads the defining module's origin; `fl_cap_check` denial raises `"capability"` and is catchable.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.11 Spec amendment A2 present in `.docs/fletch-spec.md` §16; §9 lists 13 kinds; s33's count assertion updated in the same commit.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.12 Perf gates: motion-op dispatch ≤ 1 µs; 10⁶ query calls < 300 ms; baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.13 ≥ 200 assertions across the three unit files; ≥ 25 script tests.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s34.14 The landing guards named their owning sprints rather than silently stubbing: Sprint 36 delivered `bind`/`unbind`, Sprint 35 delivered macro record/replay, and Sprint 37 delivered the `--batch` refusal policy.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.1 `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/ determinism lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.2 **Self-config milestone**: `keys_default.c` contains only the panic keymap (≤ 6 rows); every other default binding comes from `runtime/init.fl`, proven by the golden diff against s13/s14/s16/s17's tables (zero missing, zero extra).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.3 `runtime/init.fl` parses with zero diagnostics, passes spec §15 style checks, imports nothing, and its parse+run is **< 1 ms** on the CI runner with a committed baseline.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.4 Every §2 option row: default equals the owning sprint's C constant (asserted), validator fires, scope enforced, `on_change` tested.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.5 Grep gate: `grep -rn 'yew_opt_set' src/` hits only `option.c`, `flapi.c`, and the `:set` handler; a seeded fourth call site fails `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.6 `:set tabwidth 4` and `set({tabwidth: 4})` are observably identical (same table entry, same `on_change`, asserted by a script test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.7 Trust: all five invalidation rows fire; a moved directory re-prompts; a same-path/different-inode directory re-prompts; a content change re-prompts; `never` never prompts again; the db round-trips sorted with unknown keys preserved; no prompt is ever issued without a tty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.8 **Reload zero-residue**: registry lengths equal across 20 reloads; hooks fire exactly once after any number of reloads; a deleted `set()` line restores the default; a parse error leaves the previous config live and fully functional (script-tested by binding a key, breaking the file, reloading, and pressing the key).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.9 Sprint 58 retired the legacy differential only after the 500-state and Sprint 25 corpus handover matrix passed. The retained corpus exercises the shipping Fletch codec byte-for-byte and pins rejection, determinism, and idempotence.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.10 `--clean` loads only the panic keymap, skips workspace state and the workspace config, and is documented in `--help`; `yew --clean` no longer hard-errors naming this sprint.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.11 Perf: config load < 1 ms; reload with 500 binds ≤ 5 ms; baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.12 Later owning sprints delivered the staged configuration surfaces: macro library loading in Sprint 38, plugin configuration in Sprint 54, and `--batch` configuration policy in Sprint 37.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s36.13 ≥ 180 assertions across the four unit files; ≥ 20 script tests; pty goldens byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.

Contract reconciliation: Later owning sprints delivered the staged Fletch surfaces.
Sprint 58 retired only the completed legacy-codec handover scaffolding.
Current closeout therefore proves the shipping implementations while
retaining the standalone-REPL motion and no-`io.run`/`io.http` boundaries.

## Open audit findings against this front

- `YEW-F-110` (Medium): freeze-time and point-in-time kind/native inventories
  are still expressed as current-tree totals.

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
