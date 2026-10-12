# Closeout: F10 — Syntax and themes

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s39, s40, s41, s42

## DoD items (every numbered item from every sprint above)

- [ ] s39.1 `make && make test` green on gcc and clang under `-std=c11 -pedantic -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.2 `_Static_assert`s hold: `sizeof(SynState) == 40`, `sizeof(SynSpan) == 8`, `YEW_ATTR__COUNT <= 64`; `YEW_ATTR__COUNT == 54` asserted so a silent table edit fails the build.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.3 **No colors in the engine**: `grep -rniE '#[0-9a-f]{6}|rgb|\b38;2\b|\b48;5\b' src/syn/` returns nothing, and `grep -rn 'yew_cp_width\|yew_str_width' src/syn/` returns nothing (width math stays in `src/unicode/`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.4 The depth-cap balance law: 40 nested pushes followed by 40 pops returns the exact entry state id; the test fails if `lost` handling is removed (proved once with a deliberately broken build in the test comment).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.5 The splice table: all 8 rows unit-tested; `entry.len` equals `yew_textbuf_line_count` after every mutation in every test; a 10 000-line paste performs exactly one `Vec` splice (counter asserted).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.6 Fixpoint economy: typing one character inside a line of a 100 000-line fixture re-runs **≤ 2 lines** (counter asserted) and completes in ≤ 60 µs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.7 Bounded propagation: `/*` inserted at line 1 of a 1 000 000-line fixture never makes a single `yew_syn_settle` call exceed its `budget_us` (fake clock, 10 000 calls sampled), the viewport is correct one frame later, and settling completes to a verified fixpoint.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.8 Differential oracle: 100 000 random edits × 4 seeds — incremental state array and span dumps identical to from-scratch highlighting, zero divergences.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.9 Fuzz: 10 minutes each for the line fuzzer and the edit fuzzer, zero findings, alarm never fires, ASan/UBSan clean; corpus committed and replayed in `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.10 Progress law: a definition containing a rule that matches empty is accepted and terminates on every line of the fuzz corpus; removing the forced-advance branch makes the test hang (documented in the test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.11 Cache effectiveness: redrawing an unchanged 200-row viewport runs `yew_syn_line` **zero** times (counter asserted); a one-row scroll runs it once.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.12 Deferrals hard-error naming their sprint: loading a `.fl` definition (Sprint 40), theme loading (Sprint 41), `yew_syn_lang_for` returning anything but `YEW_LANG_NONE` (Sprints 41/42), any `SynState.def != 0` (`YEW_BUG`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s39.13 ≥ 180 assertions across the five new unit files; `make perf` runs the syn scenarios and writes `tests/perf/baselines/syn.txt`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.1 `make && make test` green on both compilers, zero warnings; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.2 `.docs/syntax-def.md` documents every key in §1–§4 with a type, a default, and an example; contains `ini.fl` verbatim plus the rule-by-rule table, the 6-line trace table, the TextMate porting section, and the new-definition checklist. `grep -n 'TODO\|TBD'` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.3 `yew syn check --strict runtime/syntax/ini.fl` exits 0 with zero warnings; the same command over 17 deliberately broken fixtures (`tests/syn/bad/*.fl`, one per §9 row) produces exactly the tabled message with the correct `line:col`, one diagnostic each.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.4 First-byte self-check green for every rule of every shipped definition over all 256 byte values; narrowing any set by one bit fails the test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.5 B-mode: `yew_block_provider_syntax_install` no longer hard-errors; the provider returns spans on an INI fixture giving ≥ 4 hand-computed containment levels, and `false` in all four fallback conditions.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.6 **s16 is not regressed**: its `test_block.c` suite and DoD 4's prose fixture pass unchanged with the provider installed; a `.txt` buffer still yields ≥ 3 levels.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.7 `yew_syn_in_string_or_comment` makes s16's three heuristic cases exact (`don't`, `x = 'a[b'`, `/* ) */`) with a definition loaded, and the heuristic still handles them without one.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.8 Cache: the four-step ladder unit-tested; a touched-but-unchanged file does **not** recompile (counter == 0); every corruption class recompiles rather than failing; `YEW_NO_SYN_CACHE=1` and `--clean` bypass it; cold-vs-warm `--tables` dumps are byte-identical.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.9 Laziness: opening one `.ini` file compiles exactly **one** definition (counter asserted); `scripts/gen-langtab` output regenerates byte-identically in CI (same discipline as the Unicode tables).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.10 Deferrals hard-error naming their sprint: `ed.theme.set` (41), an `embed:` key (its own sprint), a definition naming a language shipped in 41/42 that does not exist yet (`no definition for 'rust' — Sprint 42`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.11 Fuzz: 10 minutes on the definition fuzzer, zero findings, ASan/UBSan clean; corpus committed and replayed in `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.12 Perf: `yew_syn_lang_for` ≤ 0.5 µs per path; `ini.fl` cold compile ≤ 3 ms; cache load ≤ 200 µs; the cold-start baseline is unchanged within 5 %.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s40.13 ≥ 200 assertions across the five new unit files; ≥ 6 golden span files for `ini.fl`, all byte-stable across two runs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.1 `make && make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.2 `yew syn check --strict runtime/syntax/{c,fletch,sh,make,markdown}.fl` exits 0 with zero warnings; `yew syn compile --all` is idempotent.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.3 **100 % context and rule coverage** for all five definitions against their committed fixtures (`yew syn check --coverage`), enforced in CI.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.4 Every pitfall in §1–§5 has a named golden fixture: C's `//`-containing- `/*`, `"/*"`-in-string, backslash-continued directive and string, unknown escape → `error`; sh's four heredoc forms plus `<<-` with spaces not terminating and the quoted-delimiter no-expansion rule; make's tab-vs-spaces pair; markdown's longer-closing-fence and fence-containing-backticks pair.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.5 The Fletch cross-check passes on the spec §14 example: every lexer token covered by exactly one span with an allowed attr; all four `motion.*` attrs appear.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.6 Both themes define all 54 attrs explicitly (`grep -c` per file == 54 and a unit test asserting no fallback was consulted during their load).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.7 `.docs/syntax-def.md` gains the heredoc idiom (§3's deferred-push pattern) and the `unit:` guidance; `.docs/themes.md` documents the format, every field, both defaults, and the degradation ladder.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.8 All 108 tabled 256-colour values equal `yew_rgb_to_256` of their hex (test regenerates and compares; a mismatch fails CI).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.9 Theme switch: one full repaint, ≤ 2 ms at 200×50, and **zero** `yew_syn_line` calls (counter asserted).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.10 `NO_COLOR` (empty and non-empty) emits no `38;`/`48;` parameters in a full-frame golden; `TERM=dumb` emits no SGR at all; `YEW_COLORS=16` emits only 30–37/90–97/40–47/100–107.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.11 Underline colours: `58;2;R;G;B` + `59` emitted for `error`/`warning` runs at truecolor only; `sizeof(Cell) == 20` still asserts; s05's fuzz assertion (no sequence outside the accepted set) extended to accept `58`/`59` and re-run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.12 All §10 budgets green with committed baselines; the `c_comment_bomb.c` no-frame-over-1 ms assertion is a hard gate under the fake clock; scroll throughput on `md_kitchen.md` still ≥ 120 fps equivalent with highlighting on.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.13 `SynState.def == 0` asserted for every state produced by all five definitions across the whole fixture corpus; an `embed:` key errors naming `s41_5`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s41.14 ≥ 90 golden span files across the five languages, all byte-stable across two runs; ≥ 120 assertions across the three new unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.1 `make && make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.2 Eleven new definition files — `python rust go javascript typescript fortran fortran_fixed json jsonc yaml toml` (8 languages; typescript, fortran_fixed and jsonc are shared-body variants) — compile, and `yew syn check --strict runtime/syntax/*.fl` exits 0 with zero warnings across all 17 files / 14 languages.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.3 **100 % context and rule coverage** across all 17 definition files (14 languages) against the committed fixtures; CI fails on any unfired rule.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.4 Every trap in §1–§8 has a named golden: python's nested-quote f-string and `{{`; rust's 20-deep comment, `r###`, and all four lifetime/char rows; go's backtick-with-`\n`; js's eight `value` rows, two documented failures, and `` `a${`b${c}`}` ``; fortran's fixed-form column map, `&` continuation, `'it''s'`, and `'C:\path\n'`; json's escape and number violations plus the `.json`-vs-`.jsonc` pair; yaml's six out-of-scope constructs and its `|2-` block scalar; toml's six string forms and every datetime shape.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.5 Fortran form detection: ≥ 20 hand-scored inputs correct, including the tie rule and the option override; a fixed-form statement at column 73 styles as `comment` and at column 72 as code (pty golden).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.6 The rust depth case: 20 nested comments leave the exit state exactly equal to the entry state after all closers, `degraded` set and cleared correctly (removing s39's `lost` handling fails this test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.7 Differential: 100 k random edits × 4 seeds × 8 new languages, zero divergences against from-scratch highlighting.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.8 `SynState.def == 0` for every state produced by every definition across the entire fixture corpus (asserted in the differential harness).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.9 All §9 budgets green on every fixture with per-language baseline rows committed; the minified 2 MiB JSON line opens to first paint ≤ 20 ms; cold start with all 17 definition files installed compiles ≤ 1 definition.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.10 Fuzz: all definitions replayed through the s39 line fuzzer and the s40 definition fuzzer for 10 minutes each, zero findings, ASan clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.11 `.docs/syntax-def.md` updated: go named as the template definition; the four aux matchers each cite their real consumer (sh, rust/python/ toml, markdown, yaml); the `value` flag documented with the js heuristic as its worked example.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s42.12 ≥ 140 golden span files across the eight new languages, all byte-stable across two runs; ≥ 100 assertions across the three new unit files.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
