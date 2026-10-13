# F07 UI/workspace pre-closeout evidence

- Date: 2026-10-13
- Baseline: `85a81ca748620c02a97097712618029b067b37aa`
- Last current/evolved F07-owned product-source change: `ff0373ca`
- Last current/evolved F07-owned test-surface change: `9bb1415f`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F07 for closeout without signing it. Its author also
authored F07's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F07-ui.md`.

## Scope and provenance

The original F07 audit is `.docs/audits/audit-07-ui.md`. It reviewed detached
product baseline `41fef416` and found the two Critical workspace-state defects
`YEW-F-006` and `YEW-F-007`. The F006 cluster hunt later filed
`YEW-F-079`. All three product findings are fixed and retain executable audit
reproducers.

The current recheck began from
`/private/tmp/yew-s59-f07-recheck-20261013.pMglqw/manifest.md` at commit
`23065e19`. It found and closed two contract-only defects without changing
product behavior:

- `YEW-F-106` / S59-A17 distinguishes later-delivered tab, group, picker,
  F-mode mouse, document-menu, and tab-into-pane surfaces from the one retained
  LSP hover-on-pointer boundary. It also records Sprint 36's completed legacy
  state-codec handover instead of requiring the retired differential file.
- `YEW-F-107` / S59-A18 replaces literal token bans with the actual ownership
  rules: Unicode conversion remains centralized, workspace replacement uses
  the shared atomic-file API, and one event-loop branch dispatches mouse
  events to `yew_mouse_event`.

At this baseline all 107 audit registry entries pass, both remediation ledgers
report zero active XFAILs, and F07 has zero open confirmed findings. The latest
broad current/evolved F07 product and test commits are identified above;
subsequent commits through the baseline are other product fronts,
documentation, audit controls, and contract reconciliation.

The adjacent `lsp-wolf-default` worktree was not merged again; its work is
already present through `origin/trunk`. The Sprint 58 tier-1 soak remains
frozen against product `4c99e134` plus runner-only child `4f483f31`. This
packet did not change, restart, or claim that separate campaign.

## Current-baseline executable evidence

### Strict builds and focused units

Apple Clang and Homebrew GCC 16 each built `yew`, `unit_tests`, `audit_tests`,
`pty_runner`, `demo_paint`, all seven F07 fuzz targets, and the state/finder/
mouse performance probes under the repository's locked
`-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy with zero warnings.
A fresh Apple Clang ASan/UBSan tree built the editor, unit/audit/PTY harnesses,
and all seven fuzz targets with zero warnings.

Twenty-five current-surface filters passed with identical counts under Clang,
GCC, and ASan/UBSan. The filters intentionally follow evolved ownership and
therefore may include later consumers sharing a prefix; their counts are
inclusive evidence, not a claim that old source-file counts stayed frozen.

| Front slice | Filters | Tests / assertions per configuration |
|---|---|---:|
| s22 panes/regions | `layout_`, `region_` | 56 / 191,266; 12 / 76 |
| s23 tabs/strips | `tabs_`, `strip_` | 44 / 1,528; 25 / 1,516 |
| s24 groups | `groups_`, `grouppicker_`, `groupnav_` | 30 / 2,404,516; 19 / 396; 35 / 557 |
| s25 state | `ws_key_`, `state_corpus_`, `state_schema_`, `ws_recover_`, `ws_restore_`, `ws_save_` | 10 / 168; 13 / 12,782; 12 / 1,288; 15 / 362; 18 / 365; 27 / 379 |
| s26 discovery | `fuzzy_`, `rank_`, `walk_`, `gitignore_`, `picker_`, `pickers_`, `typejump_` | 18 / 256; 10 / 74; 36 / 2,195,679; 27 / 110; 35 / 709; 10 / 216; 13 / 285 |
| s27 mouse/feel | `mouse_`, `ctxmenu_`, `drag_`, `degrade_`, `invariant9_` | 76 / 5,872; 24 / 243; 52 / 4,024; 10 / 3,196; 13 / 245 |

The source inventory contains 525 named tests across the 25 directly owned or
evolved files. Darwin ASan supplies no LeakSanitizer, and Valgrind is
unavailable on this host; Linux leak and descriptor evidence remains a release
blocker.

Both fresh non-default builds independently reported:

```text
audit: 107 tests, 0 failures
```

### Focused fuzz evidence

The current-baseline cross-compiler/sanitizer sample exercises each F07 fuzz
surface with the contract-sized structural budgets and bounded parser/router
samples. Deterministic hashes agreed wherever the same seed and budget were
used. The full four-seed 100,000-case campaigns remain release evidence rather
than being silently replaced by this sample.

```text
fuzz_panes: seed=1 iters=100000 hash=21b22df4d2a18c23 ok
fuzz_tabs: seed=1 iters=10000 hash=7f6650298b07dd0e ok
fuzz_groups: seed=1 iters=5000 hash=36e80942c96ccf86 ok
fuzz_state: seed=1 iters=10000 hash=4d398728927655f6 ok
fuzz_fuzzy: seed=1 iters=100000 hash=21b22df4d2a18c23 ok
fuzz_gitignore: seed=1 iters=100000 hash=21b22df4d2a18c23 ok
```

The pane run deliberately exceeded Sprint 22's 10k requirement. Tab and group
runs used their exact 10k/5k structural budgets. State used a bounded 10k
sample; fuzzy and gitignore used 100k. Those three hashes also agreed under
Clang, GCC, and ASan/UBSan. The mouse fuzzer's fixed 25k-event session was
stopped after nearly ten minutes per configuration so it would not compete
indefinitely with the authoritative soak; no partial result is claimed. The
full state and mouse campaigns are listed as blockers.

### Terminal, state, and chrome surfaces

Current Apple Clang PTY evidence covers every registered `s22_` through
`s27_` case and all 48 `chrome_` variants. Each selected case is run twice by
the harness and compared with its committed final-screen golden. This covers
pane focus/resize, multibyte tab spans, group entry and digit timing, exact
workspace resume, finder highlighting, unfocused-pane wheel behavior, group
dwell, context-menu identity, and all truecolor/no-color/16-color/ASCII
chrome variants.

The state-focused units additionally prove the 40-tab one-read restore,
group-id remap, unknown-field retention, byte-canonical re-emission, corrupt
file discipline, single-writer/stale-owner behavior, repository cleanliness,
and ordered delegation to the shared atomic writer.

### Performance and policy

The state, 100,000-file finder, and 1,000-motion mouse policy self-tests and
local gates were executed after the focused fuzz work. Their measured local
results are candidate evidence only; the designated cross-target performance
matrix remains required.

```text
perf-state: tabs=512 leaves=16 bytes=361860
  emit_ms=1.036 / 5.000 ok; parse_ms=2.302 / 10.000 ok
  restore_tabs=40 restore_ms=1.023 / 20.000 ok; file_reads=1 ok
perf-finder: items=100000 open_ms=0.633 / 16.000 ok
  first_key_ms=2.005 / 5.000 ok
  narrowing_frame_ms=2.009 / 5.000 ok
  worst_slice_ms=2.007 / 3.000 ok
perf-mouse: menu_hover renders=19 / 20, 0.375 ms ok
  chevron_hover renders=6 / 10, 0.091 ms ok
  burst=1000 events_ms=0.033 / 5.000, renders=0 / 0 ok
  router_allocations=none ok
```

All three policy self-tests passed in strict mode. Measurements were taken
while the separate four-core Sprint 58 soak was active, so the designated
hardware matrix remains authoritative despite the wide local margins.

Static ownership checks at this baseline found:

- no locale-width call, Unicode width table/range, or duplicate text-to-cell
  conversion under `src/ui/`; layout code operates on measured cell counts;
- all pane, tab, group, and delivered picker surfaces present in the named
  command registry;
- no stored dirty/modified boolean in `Tab`; rendering calls
  `yew_tab_modified` and therefore derives state from undo;
- no authoritative group member array/count and no stored residency flag;
- no float conversion/formatting in `src/ws/`, no direct `rename()` or
  `fsync()` call there, and all durable writers routed through
  `yew_file_write_atomic*`;
- no `nftw`, `ftw`, or `scandir` call in the bespoke workspace walker;
- no ad-hoc picker input editor matching the retired small-buffer pattern;
- terminal input is the only mouse decoder, `src/edit/loop.c` contains the
  sole `YEW_EV_MOUSE` dispatch branch, and `src/ui/mouse.c` owns routing;
- `src/ui/mouse.c` neither calls `yew_vp_follow` nor allocates in its router.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-006` | `ws_restore_preserves_unknown_root_and_workspace_fields`; audit reproducer; fix `9e829cd9` |
| `YEW-F-007` | group member restore follows persisted tab-array order; audit reproducer; fix `bea3990b` |
| `YEW-F-079` | retained unknown entity fields survive re-emission and replacement; audit reproducer; fix `9222b491` |
| `YEW-F-106` | S59-A17 and `a5081521` align delivered UI surfaces and the completed Sprint 36 codec handover |
| `YEW-F-107` | S59-A18 and `a5081521` express Unicode, atomic-write, and mouse-dispatch ownership semantically |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s22.1 | strict Clang/GCC builds; `layout_` and `region_` under all three configurations; full exact-baseline suite/Valgrind remain outstanding |
| s22.2 | all 11 `s22_` PTYs, each internally replayed twice; full-suite determinism remains outstanding |
| s22.3 | `layout_split_refuses_below_minimum`, vertical/cap variants, `s22_split_refused_when_too_narrow` |
| s22.4 | `layout_shrink_then_grow_restores_rects`, `layout_collapses_rather_than_destroying_on_shrink`, resize PTY evidence |
| s22.5 | `s22_click_focuses_and_lands_on_grapheme`, `s22_border_beside_wide_glyphs`, exhaustive layout/hit agreement |
| s22.6 | `layout_drag_moves_by_delta_and_esc_restores`, `s22_drag_border` |
| s22.7 | empty duplicate-width implementation search, shared Unicode calls, S59-A18 |
| s22.8 | named registry rows for every `ed.pane.*`; command-focused layout units |
| s22.9 | named registry rows for the live `ed.tab.*` surface; S59-A17 |
| s22.10 | 100k pane-fuzz seed under Clang/GCC/ASan; `region_empty_query_is_counted` |
| s23.1 | `tabs_` 44 / 1,528 and `strip_` 25 / 1,516 under all configurations |
| s23.2 | 10k tab-oracle seed under Clang/GCC/ASan with identical hash; additional random lane remains outstanding |
| s23.3 | all six `s23_` PTYs, including CJK click spans and undo-derived marker |
| s23.4 | empty stored dirty/modified-field search; `tabs_modified_derives_from_undo_state` |
| s23.5 | close/storm/cap unit coverage under ASan; Linux Valgrind leak witness remains outstanding |
| s23.6 | `tabs_cap_refuses_and_mutates_nothing`; checked return sites in restore/FUSS/shell/LSP/group/picker callers |
| s23.7 | dirty-prompt units and `s23_dirty_close_prompt`; exact read-only write-failure review remains for reviewer |
| s23.8 | `tabs_goto_maps_zero_to_ten_and_refuses_out_of_range` |
| s23.9 | `region_tab_payload_sign_convention`, `tabs_negative_region_payload_enters_the_group`, S59-A17 |
| s23.10 | selected PTYs replay twice; complete full-suite determinism remains outstanding |
| s24.1 | 84 selected group tests / 2,405,469 assertions under Clang/GCC/ASan; Valgrind remains outstanding |
| s24.2 | structural header review; `groups_label_count_is_computed_not_stored`; `yew_tab_is_resident` asks buffer allocation |
| s24.3 | exact 5k group fuzz seed under Clang/GCC/ASan; additional random lane remains outstanding |
| s24.4 | `groups_opening_a_forty_file_group_reads_one_file` |
| s24.5 | `groupnav_walk_is_reversible`, complete ordered walk fixture |
| s24.6 | `groupnav_enter_at_edge_depends_on_direction`, explicit resume/fallback tests |
| s24.7 | digit-jump clock/modifier/two-digit/member units and `s24_digit_jump_is_immediate` |
| s24.8 | `grouppicker_ticks_survive_directory_navigation`, set/count units, `s24_picker_chrome` |
| s24.9 | shared strip span/region units and `s24_click_enters_a_group`; exhaustive every-span PTY remains reviewer evidence |
| s24.10 | 5k fuzz invariant plus `groups_removing_the_last_member_auto_dissolves` and prune controls |
| s24.11 | live registry rows for all group commands and `ed.group.from_dir`; S59-A17 |
| s24.12 | selected PTYs replay twice; complete full-suite determinism remains outstanding |
| s25.1 | strict Clang/GCC and fresh ASan builds plus focused units; full suite/Valgrind/determinism remain outstanding |
| s25.2 | `s25_resume_exact`, `s25_resume_survives_resize`, restore cursor/goal/viewport units |
| s25.3 | `ws_restore_of_forty_tabs_reads_one_file` |
| s25.4 | `state_schema_remaps_group_ids`, `ws_restore_remaps_group_ids`, empty direct file-id assignment search |
| s25.5 | 13 `state_corpus_` tests / 12,782 assertions and save/restore/save fixpoint |
| s25.6 | `state_corpus_contains_no_floats`, source guard, empty workspace float scan |
| s25.7 | 15 `ws_recover_` tests / 362 assertions, including retention/no-overwrite/every-bad-input rows |
| s25.8 | `ws_save_write_is_atomic_in_order`, `ws_save_has_no_second_atomic_primitive`, shared-helper-only static review, S59-A18 |
| s25.9 | `ws_save_leaves_a_git_checkout_clean`, `ws_save_never_writes_into_the_workspace` |
| s25.10 | live-reader, message-once, stale-lock, kill-9 stale-lock, own-pid and malformed-lock units |
| s25.11 | `state_corpus_holds_no_undo_payload`, `state_schema_undo_is_a_reference_only`, state-emitter review |
| s25.12 | current cross-toolchain state-fuzz sample; required 100k × 4-seed campaign remains outstanding |
| s25.13 | audit-07 codec-handover evidence, retained corpus tests, S59-A17 |
| s25.14 | 95 selected state/workspace tests / 15,344 assertions; history-scope witnesses remain for independent review |
| s26.1 | strict Clang/GCC and fresh ASan builds plus discovery units; full suite/Valgrind/determinism remain outstanding |
| s26.2 | `fuzzy_table_rows`, `fuzzy_parity_corpus`, 100k fuzzy sample |
| s26.3 | basename-tier/ranking-order unit family in `test_fuzzy.c` |
| s26.4 | deterministic/sliced/symlink-loop walker units; empty forbidden-library scan |
| s26.5 | 27 gitignore tests / 110 assertions; prune/syscall walker units; gitignore fuzz sample |
| s26.6 | strict `perf_finder` local result; designated target matrix remains outstanding |
| s26.7 | picker narrowing-equivalence/property units selected by `picker_` |
| s26.8 | `picker_selection_survives_a_refilter` and the F07 plugin-picker identity audit witness |
| s26.9 | `picker_filter_line_is_the_cmdline`, empty retired small-buffer scan |
| s26.10 | file/buffer/undo/command picker units plus live symbol/plugin later consumers |
| s26.11 | `pickers_preview_allocates_no_buffer`, binary-preview witness |
| s26.12 | complete 11-test type-jump timing/expiry/exact-match family |
| s26.13 | live command/symbol/plugin discovery and F-mode type-jump routes; S59-A17 |
| s26.14 | five `s26_` PTYs replayed twice and current discovery assertion counts; full-suite determinism remains outstanding |
| s27.1 | strict Clang/GCC and fresh ASan builds plus mouse/feel units; full suite/Valgrind/determinism remain outstanding |
| s27.2 | `mouse_the_router_is_the_only_dispatch_site`, one decoder/loop/router static chain, S59-A18 |
| s27.3 | 76 selected mouse tests, inert/unlisted/block-region controls |
| s27.4 | captured strip/group/pane target tests and `ctxmenu_acts_on_the_target_captured_at_open` |
| s27.5 | wheel-under-pointer units and `s27_wheel_unfocused_pane`; empty `yew_vp_follow` scan |
| s27.6 | double/triple-click engine-agreement units and `s27_double_click_mode_chip` |
| s27.7 | exact dwell boundary and three-group pass-through units; `s27_dwell_opens_member_strip` |
| s27.8 | `drag_never_mutates_the_tab_array_before_release` and cancellation/count-change controls |
| s27.9 | 24 context-menu tests / 243 assertions and four chrome-menu variants |
| s27.10 | 13 invariant-9 equivalence tests / 245 assertions |
| s27.11 | degrade unit family plus 48 truecolor/no-color/16-color/ASCII chrome PTYs |
| s27.12 | all 48 `chrome_` PTYs replayed twice; complete full-suite determinism remains outstanding |
| s27.13 | strict `perf_mouse` local result; the fixed-size mouse-fuzz sample did not complete and the required four-seed campaign remains outstanding |
| s27.14 | document menu, F-mode mouse, and tab-into-pane successor units/PTYs; retained keyboard LSP-hover route; S59-A17 |
| s27.15 | 175 selected mouse/drag/menu/degrade/invariant tests / 13,580 assertions per configuration |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit and this author-authored packet cannot supply Sprint 59's required
   non-author verdicts.
2. No hosted exact-baseline full matrix exists for `85a81ca7`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The complete exact-baseline `make test`, full-suite determinism, workspace
   kill-9 torture, designated performance matrix, and all required four-seed
   fuzz campaigns were not rerun here. Focused current checks and historical
   F07 evidence are not substitutes.
5. The Sprint 58 soak is frozen against product `4c99e134`. At packet
   creation it retains four durable `fuzz_utf8` rows while four
   `fuzz_grapheme` streams run. It neither blocks local Sprint 59 work nor
   substitutes for Sprint 59's exact closeout matrix.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected the changed F07 contracts and current UI/
workspace ownership boundaries, replayed every audit reproducer, and exercised
focused builds, units, fuzz, terminal, state, chrome, performance, and policy
controls. It does not claim that passing samples prove the entire UI/workspace
front correct.
