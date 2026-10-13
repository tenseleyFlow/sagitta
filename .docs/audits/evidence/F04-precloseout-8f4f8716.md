# F04 modal-semantics pre-closeout evidence

- Date: 2026-10-13
- Baseline: `8f4f8716930d6dfbbf43a877247c8521d34a1927`
- Last F04-owned product-source change: `46760066`
- Last current/evolved F04-owned test-surface change: `8b7faf0c`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F04 for closeout without signing it. Its author also
authored F04 remediation and contract-reconciliation commits and therefore
cannot be Sprint 59's independent reviewer. A future reviewer must establish
the baseline again, independently inspect or rerun the cited witnesses, and
decide every checkbox in `.docs/audits/closeout-F04-modal.md`.

## Scope and provenance

The original F04 audit is `.docs/audits/audit-04-modal.md`. It reviewed
detached product baseline `41fef416`; hosted close run `34011188190` at
`de70ed63` passed its complete matrix. That audit recorded 352 core commands,
154 recordable commands, 12,102 command-word assertions, 196 active runtime
bindings, 57 panic-map rows, four 200,000-operation unit-fuzz seeds, and the
then-current keyboard-reachability surface. Those are useful historical
results, not executions against this packet's baseline.

The current recheck began from
`/private/tmp/yew-s59-f04-recheck-20261013.F3XBlD/manifest.md`. It filed and
closed two contract findings without changing product behavior:

- `YEW-F-102` / S59-A13 reconciled old staging guards with shipped W/B/H/E/F
  modes, the installed syntax provider, and multi-cursor expansion. It
  explicitly retained the intentional 1.0 boundaries: regex match lift is
  rejected, ghost completion requires one cursor, and LSP edits require one
  primary cursor.
- `YEW-F-103` / S59-A14 replaced placeholder-era ownership greps with the
  actual invariants. Later persistence, tabs, status, search, Git, editor,
  and Fletch code may consume viewport state for their assigned duties, while
  generic viewport algorithms remain in `src/ui/viewport.c`. Unicode
  measurement and conversion remain in `src/unicode/`; UI may operate on the
  measured cell counts.

At this baseline all 103 audit registry entries pass, both remediation
ledgers report zero active XFAILs, and F04 has zero open confirmed findings.
The latest scoped product change is `46760066`; subsequent scoped change
`8b7faf0c` affects the current/evolved test surface, and later commits through
this baseline are documentation and audit-control changes. The adjacent
`lsp-wolf-default` worktree was not merged again; its work is already present
through `origin/trunk`.

Long campaigns were deliberately omitted while the Sprint 58 tier-1 soak
owns four cores. That independent soak remains frozen against product commit
`4c99e134` plus runner-only child `4f483f31`; it is separate evidence and
cannot close this front.

## Current-baseline executable evidence

### Strict builds and owned units

Apple Clang and Homebrew GCC 16 each built `yew`, `unit_tests`,
`audit_tests`, `pty_runner`, `fuzz_units`, and `fuzz_multicursor` under the
repository's locked C11 warning policy. A fresh Apple Clang ASan/UBSan tree
also built the product and focused binaries. Every registered test defined by
the current/evolved F04-owned unit sources was invoked by exact name under all
three configurations:

| Sprint | Current/evolved owned sources | Tests | Assertions |
|---|---|---:|---:|
| s13 | `test_keyid.c`, `test_keyseq.c`, `test_cmdreg.c`, `test_keymap.c`, `test_chord.c`, `test_count.c` | 20 | 25,611 |
| s14 | `test_loop.c`, `test_modes.c`, `test_edit_cmds.c`, `test_s14_text.c` | 51 | 3,253 |
| s15 | `test_viewport.c`, `test_wrap.c`, `test_statusline.c`, `test_message.c` | 38 | 707,069 |
| s16 | `test_wordbreak.c`, `test_word.c`, `test_block.c`, `test_units.c` | 31 | 58,007 |
| s17 | `test_select.c`, `test_sel_actions.c`, `test_multicursor.c`, `test_hmode.c`, `test_draw.c` | 86 | 2,254,263 |
| **Total** | **23 source files** | **226** | **3,048,203** |

The exact source ownership has evolved since Sprints 13–17. This table names
the present files rather than hiding current coverage behind obsolete source
counts. Darwin ASan supplies no LeakSanitizer, and Valgrind is unavailable on
this host; Linux leak and descriptor evidence remains a release blocker.

### Focused semantic controls

The following controls produced the same passing results under Apple Clang,
GCC 16, and ASan/UBSan:

| Control | Result |
|---|---:|
| command-registry/CMDWORD round trip | 1 test / 17,781 assertions |
| composed chord and plugin ownership | 20 assertions |
| current runtime defaults and dotted keys | 3 tests / 2,866 assertions |
| 1,000-cursor failure-at-700 rollback | child 4,017 assertions; intercept parent 29 |
| rectangular drawn-cell/delete-span identity | 44,131 assertions |
| installed syntax-provider strict progress | 353 assertions |
| all-engine monotonicity | 1,139 assertions |
| deliberately broken fixed-point engine rejection | 1 assertion |
| intentional multi-cursor boundary guards | 36 assertions |
| keyboard reachability with mouse disabled | 13 tests / 245 assertions |
| tutor keyboard/filter surface | 5 tests / 216 assertions |
| panel keyboard/filter surface | 21 tests / 665 assertions |
| completion-menu keyboard/filter surface | 9 tests / 241 assertions |

Two `build/yew --help-cmds` executions were byte-identical and each emitted
408 nonempty command/help lines. `scripts/check-cmd-dispatch.sh` passed its
tree check and planted-negative self-test:

```text
dispatch: named-command integrity ok
```

`make test-roundtrip-coverage` also passed at this baseline:

```text
roundtrip: ok seeds=2000 base=0 fixtures=6 corpus=21 count-sentinel=1 edit-store-noop=1 cross-keymap=1 third-command-rollback=1 P1-P5
```

### Deterministic fuzz samples

`fuzz_units` enforces a 100,000-operation minimum. Two plain executions and
one ASan/UBSan execution produced the same output:

```text
fuzz_units: seed=5904 ops=100000 hash=02ee86e4ec8c6e2d ok
```

Two plain `fuzz_multicursor --iters=80` executions and one ASan/UBSan
execution also agreed byte-for-byte:

```text
fuzz_multicursor: seed=5904 iters=80 corpus=5 hash=3990deb5f5c7d1af
fuzz_multicursor: cursor_ops=12079 insert=10948 delete_next=267 delete_prev=864 cursors=1..500 selected_sets=210 overlap_skips=142 counts=116 ok
```

These are bounded deterministic current-baseline samples. They do not replace
the four full unit-fuzz seeds, four full multi-cursor seeds, or hosted
sanitizer campaigns required by the sprint contracts.

### Performance

The current `perf-scroll`, `perf-units`, and `perf-multicursor` gates passed:

```text
scroll nowrap 80x24   5023.23 fps
scroll wrap   80x24    865.29 fps
scroll nowrap 200x60  1540.62 fps
scroll wrap   200x60   252.46 fps

units line  100000 calls total=24.485 ms  max=0.031 ms
units word  100000 calls total=352.715 ms max=0.071 ms
units block 100000 calls total=85724.185 ms max=1.030 ms; zero >5 ms
units char  100000 calls total=3.738 ms   max=0.001 ms
units nested-depth=32 max=0.021 ms
units block-source 100000 calls total=36.396 ms max=0.015 ms
units block-comma 1000 calls total=371.966 ms max=0.464 ms

multicursor 1000  p99=3.554 ms  (<5 ms)
multicursor 10000 p99=21.861 ms (<50 ms)
```

The scroll results exceed the 120-fps floor in all four configurations.
These focused gates do not replace the designated-runner cold-start,
keypress-to-paint, and full performance matrices.

### Terminal and tutor surfaces

`make test-tutor` passed all five unit tests / 216 assertions, its CLI and
rotation checks, and all twelve current tutor PTYs:

```text
tutor_ch01 tutor_ch02 tutor_ch03 tutor_ch04 tutor_ch05 tutor_ch06 tutor_ch07
tutor_nogit tutor_list tutor_reset tutor_clean tutor_resume
```

This closes the original audit's local observation that the then-future tutor
could not yet be assessed. The same product and PTY source also passed the
complete F02 PTY suite twice immediately before this packet: 557 cases in the
default environment and 557 with `COLORTERM=truecolor`. Intervening commits
changed documentation and audit-control material, not product or PTY source.
That is same-source evidence, not a fresh full F04 PTY execution.

### Static ownership and policy

- `scripts/bans.sh` passed after S59-A13/A14.
- `src/edit/keymap.c` and `src/edit/dispatch.c` contain no clock/time source;
  chord time enters through the fake-clock-capable dispatch input.
- `src/edit/` contains no `wcwidth`, `0x4E00`, or `0x1F3` spelling.
- `src/ui/` contains no direct `wcwidth` call or embedded Unicode-width
  table/range. Text measurement and coordinate conversion route through
  `src/unicode/`; UI arithmetic uses already-measured cell geometry.
- Direct `Viewport.top` users were classified by lifecycle duty under
  S59-A14 rather than falsely passing Sprint 14's obsolete placeholder grep.
- `keys_default.c` remains data plus the single installer, and
  `scripts/check-cmd-dispatch.sh` proves named-command dispatch cardinality.
- The live mode/syntax/multi-cursor checks agree with S59-A13; the three
  intentional Sprint 17 boundary guards still fail loudly and name their
  constraints.
- `make test-audit` passed 103/103 audit controls; findings, XFAIL debt,
  burndown, priority, deferral, and closeout governance all agree.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-004` | full parser accepts documented bare dotted map keys; runtime-default execution and audit reproducer pass |
| `YEW-F-102` | S59-A13 and `1c8709f0` align shipped modes, syntax-provider installation, and multi-cursor expansion while retaining three intentional 1.0 guards |
| `YEW-F-103` | S59-A14 and `610e8833` align viewport/Unicode ownership with legitimate later consumers and centralized measurement/conversion |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s13.1 | strict Apple Clang/GCC 16 builds, exact owned-unit runs, fresh ASan/UBSan tree; Valgrind remains outstanding |
| s13.2 | `keyseq_atom_roundtrip`, `keyseq_default_tables_roundtrip`, current `test-roundtrip-coverage` |
| s13.3 | `keymap_build_validations`, including Escape-prefix rejection |
| s13.4 | `chord_full_prefix_timeout_and_refeed`, `chord_dead_sequence_escape_and_release`; empty clock/time search |
| s13.5 | `keymap_layer_ownership_and_listing_determinism`, composed-plugin ownership control |
| s13.6 | three `count_` tests, count sentinel in round-trip coverage, `YEW_COUNT_MAX` clamp cases |
| s13.7 | two byte-identical 408-line `--help-cmds` runs; deterministic/nonempty-help registry tests |
| s13.8 | green `scripts/check-cmd-dispatch.sh`, including its planted second-call-site rejection |
| s13.9 | `cmd_registry_invocation_and_deferred`, live shipped-mode entries, and S59-A13 staging reconciliation |
| s13.10 | `keymap_binary_lookup_matches_oracle_and_budget` one-million-lookup control |
| s13.11 | 20 current/evolved s13 tests / 25,611 assertions |
| s13.12 | dispatch checker plus the single `yew_keys_default_install` function-definition surface |
| s14.1 | same-source complete PTY suite; live open/edit/save cases and `s17_modal_milestone_saves` successor |
| s14.2 | same-source PTY fixture-preservation cases and current file/edit owned units |
| s14.3 | typeahead/coalescing controls in the current loop and PTY surfaces; exact designated burst rerun outstanding |
| s14.4 | focused multicursor latency and scroll gates; designated keypress/cold-start gate outstanding |
| s14.5 | historical live-editor kill-9 torture; exact-baseline full sweep outstanding |
| s14.6 | same-source terminal-restore PTYs and historical tty torture; exact-baseline full rerun outstanding |
| s14.7 | current mode/edit units, live W/B/H/E/F reachability, modal milestone PTY, and S59-A13 |
| s14.8 | `edit_escape_closes_typing_as_one_undo_step`, `edit_insert_motion_splits_typing_into_two_undo_steps` |
| s14.9 | current s14 journal/failure-state units and same-source dirty/recovery prompt PTYs |
| s14.10 | classified viewport consumers, generic algorithms in `viewport.c`, and S59-A14 |
| s14.11 | current named-command dispatch checker |
| s14.12 | 51 current/evolved owned tests / 3,253 assertions; same-source full PTYs; Valgrind outstanding |
| s15.1 | `viewport_` follow controls and the six named `wrap_` rule tests |
| s15.2 | `wrap_randomized_sum_matches_naive_oracle` with progress budget |
| s15.3 | `viewport_resize_roundtrip_and_gutter_digit_damage`, `s15_resize_roundtrip` |
| s15.4 | current four-way `perf-scroll` results, all above 120 fps |
| s15.5 | `statusline_metadata_position_percent_and_mode_chip`, priority and role tests, `s15_mode_l/i` |
| s15.6 | statusline position unit plus `s15_position_unicode` PTY |
| s15.7 | `s15_metadata_crlf`, `mixed`, `bom`, and `binary_invalid` PTYs |
| s15.8 | `message_expiry_and_replacement_rules` and severity/prompt style controls |
| s15.9 | `viewport_resize_roundtrip_and_gutter_digit_damage` 999-to-1000 coverage |
| s15.10 | empty locale-width/table search, Unicode helper call sites, and S59-A14 |
| s15.11 | `s15_degenerate` 4x1 PTY under same source plus sanitizer draw/unit coverage |
| s15.12 | 38 current/evolved owned tests / 707,069 assertions; same-source byte-stable PTY suite |
| s16.1 | `wordbreak_unicode_16_corpus` against the checked-in corpus |
| s16.2 | `word_motion_reference_rows`, word variant, and subword reference-row controls |
| s16.3 | all-engine law/monotonicity controls and `units_conformance_rejects_fixed_point_engine` |
| s16.4 | `block_plain_text_has_three_distinct_containment_levels`, `s16_block_prose_expand` |
| s16.5 | `block_c_fixture_has_four_hand_computed_levels`, syntax-provider progress, `s16_block_c_expand`, S59-A13 |
| s16.6 | `block_indent_absorbs_interior_not_trailing_blank_lines` |
| s16.7 | `block_scope_quote_and_comment_suppression` |
| s16.8 | `block_scan_budget_falls_through_before_distant_pair`; current block-source/comma timing samples |
| s16.9 | deterministic 100,000-op `fuzz_units` sample under plain and ASan/UBSan; full four-seed run outstanding |
| s16.10 | empty `wcwidth|0x4E00|0x1F3` search under `src/edit/` |
| s16.11 | installed-provider progress, independent per-cursor selection stacks, and S59-A13 |
| s16.12 | 31 current/evolved owned tests / 58,007 assertions; current `perf-units` and same-source PTYs |
| s17.1 | `hmode_borrows_l_w_b_and_i_char_units`, sticky entry, keyboard-entry PTYs |
| s17.2 | three `select_rect_` edge families plus rectangular action units |
| s17.3 | 44,131-assertion `draw_rect_selected_cells_equal_deleted_span_cells` control |
| s17.4 | current `sel_actions_` registry/action coverage across char, line, and rectangle selections |
| s17.5 | `multicursor_200_insert_is_one_undo_and_one_journal_sync` and live one-undo PTY |
| s17.6 | `multicursor_1000_failure_at_700_rolls_back_and_syncs_once` child/parent proof |
| s17.7 | overlap/merge/permutation units and deterministic current multi-cursor fuzz sample |
| s17.8 | `multicursor_adjust_random_oracle`, integrated edit/motion fuzz, current fuzz hash |
| s17.9 | `draw_primary_is_only_hardware_cursor_target_in_raw_frame`, secondary-cursor draw units/PTYs |
| s17.10 | current 1,000-cursor p99 3.554 ms and narrow keyboard-selection damage control |
| s17.11 | `multicursor_boundary_guards_name_their_constraints`, 36 assertions, and S59-A13 |
| s17.12 | 86 current/evolved owned tests / 2,254,263 assertions; same-source s17 PTYs |
| s17.13 | `s17_modal_milestone_saves` exercises L/W/B/H/I, arrows, Escape, and save |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit predates its remediation and decisions S59-A13/A14.
2. No hosted exact-baseline full matrix exists for `8f4f8716`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The full four-seed unit/multi-cursor fuzz campaigns, complete live-editor
   torture sweep, and every designated latency/performance campaign were not
   rerun on this baseline. Focused current samples and historical campaigns
   are not substitutes.
5. The complete PTY suite is same-source evidence from the immediately
   preceding F02 packet, not a fresh F04 execution. A closeout reviewer may
   require a new full run on the eventual signed commit.
6. Sprint 58's soak is frozen against product commit `4c99e134`. At packet
   creation it has 4/56 durable streams complete and four `fuzz_grapheme`
   streams running. It neither blocks local Sprint 59 work nor substitutes
   for Sprint 59's exact closeout matrix.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected the changed F04 contracts and current modal
surface, replayed every audit reproducer, and exercised focused builds, units,
fuzz samples, performance checks, tutor paths, and audit governance. It does
not claim that passing samples prove the entire modal front correct.
