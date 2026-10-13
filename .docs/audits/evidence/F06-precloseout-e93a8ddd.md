# F06 regex/search pre-closeout evidence

Date: 2026-10-13  
Baseline: `e93a8ddd62ac4f20e7929a1c9a38ca04858dca05`  
Product-source commit: `ff0373ca` (the following commit changes ledgers only)  
Host: Darwin 25.4.0 arm64  
Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0  
Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and four-target release evidence remain.**

This packet prepares the F06 closeout without signing it. Its author also
authored remediation commits in this front and therefore cannot be the
Sprint 59 §2 reviewer. A future reviewer must establish the baseline again,
re-run or independently inspect the cited witnesses, and decide each checkbox.

## Scope and provenance

The independent discovery passes remain at:

- `/private/tmp/yew-s59-f06-closeout-20261012-a2412c6b/evidence/regex/report.md`
- `/private/tmp/yew-s59-f06-closeout-20261012-a2412c6b/evidence/conformance/report.md`

They reviewed detached commit `a2412c6b`. Their confirmed issues were filed,
deduplicated, and retained as `YEW-F-082` through `YEW-F-094`. No claim from
those reports was silently discarded. At this baseline all 94 audit registry
entries are `PASS`, both remediation ledgers report zero active XFAILs, and
the F06 closeout reports zero open findings.

The recheck scratch manifest is
`/private/tmp/yew-s59-f06-recheck-20261013.QmKk0P/manifest.md`. The canonical
checkout was clean at capture and was not changed while the executable checks
below ran. The adjacent `lsp-wolf-default` worktree was clean and was not
merged again; its commits are already present through `origin/trunk`.

## Current-baseline executable evidence

### Strict builds

Apple Clang built `build/yew`, `build/unit_tests`, `build/audit_tests`, and
`build/pty_runner` with the repository's locked C11 warning flags. Homebrew
GCC 16 independently built `build-f06-e93-gcc/yew`, `unit_tests`, and
`audit_tests` with:

```text
make -j2 BUILD=build-f06-e93-gcc CC=gcc-16 \
  build-f06-e93-gcc/unit_tests \
  build-f06-e93-gcc/audit_tests \
  build-f06-e93-gcc/yew
```

Both builds exited 0 with zero warnings. The GCC build exercises the fixed
Darwin fault-interposer recipe from `YEW-F-088`; product and tests compile
with GCC while Apple Clang owns only the required Mach-O interposer slices.

### Unit and terminal surfaces

The following current-baseline unit filters passed under Apple Clang and GCC
16:

| Filter | Result |
|---|---|
| `re_` | 346 selected tests / 2,336,260 assertions |
| `searchui_` | 21 / 259 |
| `replace_` | 26 / 41,237 |
| `overlay_` | 19 / 455 |
| `jumplist_` | 13 / 195 |
| `changelist_` | 6 / 92 |

The broad `re_` substring filter intentionally selects non-F06 names that
contain that spelling; the narrower filters and named witnesses below are the
closeout evidence. No filtered run failed.

All 17 registered Sprint 21 PTY cases passed under the real terminal harness:

```text
YEW_PTY_FILTER=s21_ build/pty_runner \
  --demo build/demo_paint --yew build/yew
```

This includes `s21_zero_width_prefix_final_newline`,
`s21_zero_width_prefix_no_final_newline`,
`s21_zero_width_global_terminates`, and
`s21_overlay_damage_is_narrow` with their committed byte-stable goldens.

### Audit and sanitizer surfaces

`build/audit_tests` and `build-f06-e93-gcc/audit_tests` each reported:

```text
audit: 94 tests, 0 failures
```

The last product change was also exercised under ASan/UBSan through the
`overlay_`, `searchui_`, and `message` unit filters. Those checks cover the
new overlay-span lifetime, cursor-row damage, and prompt-message paths in
`ff0373ca`. Earlier F06 remediation commits carried their own focused
ASan/UBSan checks, but this packet does not promote that collection into an
exact-baseline full sanitizer regression.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-005` | audit reproducer passes; multi-cursor replace exits cleanly |
| `YEW-F-081` | `re_long_literal_bypasses_vm_limit`; audit reproducer passes |
| `YEW-F-082` | `re_whole_literals_follow_utf8_decoder_boundaries`; audit reproducer passes |
| `YEW-F-083` | `re_search_from_offset_and_back`; overlapping backward audit passes |
| `YEW-F-084` | `fl_re_accepts_quoted_text_to_semantic_limit`; audit passes |
| `YEW-F-085` | `re_rep2_rejects_impossible_direct_metadata`; audit passes |
| `YEW-F-086` | tracked binding plans and s20/s21 contracts; closeout checker passes |
| `YEW-F-087` | fragmented-piece long-literal audit passes with the live-piece precondition |
| `YEW-F-088` | native GCC product/unit/audit graph builds warning-free |
| `YEW-F-089` | `re_character_class_limit` accepts 256 and rejects class 257 |
| `YEW-F-090` | `re_dfa_two_flush_fallback` observes two flushes and Pike handoff |
| `YEW-F-091` | three real-command zero-width PTYs and saved-byte assertions pass |
| `YEW-F-092` | exact `/na` to `/nai` damage interval `[0,6)` with no full damage |
| `YEW-F-093` | decision S59-A8, s20/s21 contracts, and F06 language agree |
| `YEW-F-094` | executable golden-table floor is 150 rows |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s20.2 | `src/search/pike.c` streaming `TextIter`; `tests/perf/re_throughput.c` RSS gate |
| s20.3 | `test_re_dfa_handles_pathological_patterns`; `perf-re-pathological`; no backtracking engine |
| s20.4 | `re_golden_table_bytes`, `re_golden_table_textbuf`, `re_leftmost_first_is_pinned`, `YEW-F-094` |
| s20.5 | `re_engines_agree_on_match_or_not`, `re_engines_agree_over_piece_trees`, `re_engines_agree_on_spans`; S59-A8 |
| s20.6 | regex differential campaign and oracle-skip accounting in the prior independent report |
| s20.7 | compile-fuzz campaign and in-range-offset assertions in the prior independent report |
| s20.8 | `re_error_table`, `re_non_goals_are_not_sprint_deferrals`, `re_error_offsets_are_always_in_range` |
| s20.9 | `re_empty_width_loop_terminates`, `re_empty_width_deep_subject` |
| s20.10 | `re_match_spans_interleaved_pieces`, `re_multibyte_spans_pieces`, `re_prefilter_rejects_mid_sequence_candidates`, `YEW-F-087` |
| s20.11 | `regex_internal.h` size assertion; `re_limits_fire_during_emission`; `re_character_class_limit` |
| s20.12 | `re_dfa_two_flush_fallback`, `YEW-F-090` |
| s20.13 | registered tests and assertions in the four regex unit files |
| s21.2 | shared command-line `Cursor`/`TextBuf` path in `searchui.c` and Sprint 18 prompt tests |
| s21.3 | `searchui_cancel_restores_cursor_goal_and_viewport`; `s21_search_cancel_restores_grid` |
| s21.4 | five named `search_smartcase_*` tests, including escaped-uppercase and `\Wfoo` |
| s21.5 | `overlay_scan_is_bounded_by_viewport_and_lookahead`; `perf-search-latency` |
| s21.6 | `replace_whole_run_is_one_undo_step`, `replace_ten_thousand_matches_is_one_undo_step`, `replace_confirm_run_ended_with_q_is_one_undo_step` |
| s21.7 | three `s21_zero_width_*` PTYs and goldens |
| s21.8 | `replace_rows`, `replace_rejects_unknown_escapes`, and template corpus rows |
| s21.9 | `overlay_damage_is_diffed_not_blanket`, `s21_overlay_damage_is_narrow`, `YEW-F-092` |
| s21.10 | 13 `jumplist_` and six `changelist_` focused tests |
| s21.11 | `jumplist_serializer_round_trips_in_memory`; no file-I/O calls in `src/edit/jumplist.c` |
| s21.12 | `s21_global_is_a_non_goal`; manual `:!rg` guidance; named-mark PTY |
| s21.13 | search fuzz target and regex-quote round-trip target |
| s21.14 | registered assertions across replace/search/overlay/jumplist unit files |

Rows s20.1 and s21.1 deliberately remain outside the candidate table because
their wording requires complete compiler, sanitizer, Valgrind, and
determinism evidence, not merely the focused local checks above.

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The two
   independent passes predate the remediations they caused.
2. No hosted CI run exists for `e93a8ddd`; local `trunk` is ahead of
   `origin/trunk`. Sprint 59 DoD 15 requires the complete four-target matrix at
   the commit each closeout signs.
3. Valgrind is not available on this Darwin host. Linux evidence must come
   from CI or a designated Linux runner.
4. The required exact-baseline full performance, determinism, torture, and
   fuzz evidence has not been rerun here. The 14-target Sprint 58 soak is
   concurrently consuming four cores against its intentionally frozen product
   baseline; this recheck did not distort it with competing long campaigns.
5. The Sprint 58 soak does not substitute for Sprint 59's exact closeout
   matrix. It remains separate evidence against product commit `4c99e134`.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected the changed F06 paths and their contracts,
replayed every audit reproducer, and exercised focused units and terminal
cases on both local compilers. It did not claim that passing samples prove the
entire regex/search front correct, and it found no new confirmed issue within
that scope.
