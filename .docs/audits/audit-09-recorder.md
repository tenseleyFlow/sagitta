# F09 REC — recorder and the round-trip law

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/fl/record.c`, `tests/roundtrip/`
Owners read: Sprint 35, Sprint 38

The product-code baseline remained immutable. This front extends audit and
round-trip controls, and adds two hard-XFAIL reproducers; no `src/` file
changed.

## Q1 — 200,000 generated sessions

probed, nothing found

- `YEW_RT_SEEDS=200000 YEW_RT_BASE_SEED=0 LC_ALL=C` completed across the
  six initial fixtures with zero P1–P5 divergences. The runner reported
  `seeds=200000 base=0 fixtures=6 corpus=21`.
- The law compares direct and replayed buffer bytes, complete cursor sets,
  registers, mode, and unit; one undo after replay restores E0; deterministic
  re-emission and AST dumps agree; and a second invocation exposes hidden
  state. The committed 21-case failure corpus also passed.

## Q2 — recordable-command generator coverage

probed, nothing found

- `make test-roundtrip-coverage` classified all 162 commands carrying
  `YEW_CMD_RECORDABLE`: 23 generated and 139 explicitly denied with a
  nonempty reason. It also checked every command-name/Fletch-word bijection.
- The s39–s55 additions cannot silently fall out of this check because it
  walks the live registry, not a copied command count. The four recordable
  `ed.shadow.*` commands and the recordable Git/FUSS commands have explicit
  state or side-effect reasons. Completion, LSP, and plugin-management
  commands do not carry `YEW_CMD_RECORDABLE` and therefore are outside the
  recorder law rather than unclassified within it.

## Q3 — count folding and shrinker sentinel

finding `YEW-F-009`

- The ordinary count sentinel passes: adjacent uncounted
  `ed.move.buf.end` commands are not folded into one counted invocation.
- The mandatory fault-seeding self-test no longer runs. Generator growth
  changed seed 20764 so its first event is `ed.move.unit.next`, not the two
  buffer-end motions followed by `ed.edit.insert.text("x")` required by the
  planted corruption. `YEW_RT_SELFTEST=1` exits 2 before injecting a fault
  and produces no three-event shrinker report.
- `tests/audit/yew_f_009.c` pins the documented generator prefix as a
  hard-XFAIL. The product's legal folding behavior still passes; the Medium
  finding is against the release-control mechanism and remains for Sprint 59.

## Q4 — deterministic emission across implementations

probed, nothing found within the available evidence

- On Darwin arm64, separately built switch-dispatch and computed-goto
  round-trip runners each completed 2,000 seeds plus the corpus and emitted
  byte-identical summaries. Every generated session independently performs
  P3's byte comparison between stored source and a second emission, and P4's
  two independent AST dumps.
- Baseline hosted run `34699266067` passed the round-trip suite on all four
  targets and under the GNU GCC, Clang, and computed-goto lanes. The exact
  product commit is shared by every lane.
- The hosted run did not retain a common emitted-source artifact for a
  post-hoc byte comparison between target jobs. That evidence limit is
  recorded below; it is not restated as a stronger cross-target claim.

## Q5 — VM-only replay path

probed, nothing found

- The required scan of `src/fl/record.c` finds zero calls matching
  `yew_cmd_invoke|yew_edit_|yew_textbuf_|yew_reg_set`. `replay_one` has one
  execution edge: `fl_call_chunk(ed->fl, fn, YEW_SRC_REPLAY)`.
- `tests/audit/f09_record_vm.c` makes that source claim executable. Its
  audit-only build replaces record.c's `fl_call_chunk` reference with an
  inert stub, then replays a valid macro whose real body would insert text.
  Exactly one stub call occurs while buffer bytes, cursor set, and macro
  register remain unchanged. No product hook or product object is altered.

## Q6 — replay across keymaps

probed, nothing found

- The round-trip runner now records an insert session through the shipped
  `runtime/init.fl` keymap. A second editor loads the same runtime and then
  shadows every key used by the session (`i`, `x`, and `<esc>`) with
  `ed.nop` before replay.
- Replay reaches the same buffer bytes, full cursor set, registers, mode,
  and unit. This proves the stored resolved-command program is independent
  of the current input map, rather than merely checking that a header parses.

## Q7 — store validation and keymap provenance

finding `YEW-F-010`

- `yew_macro_store` performs compile-only validation. A syntactically valid
  macro that inserts bytes and then calls an unresolved global is accepted
  and reported stored; its first replay returns `YEW_CMD_ERR_STATE`. The VM
  transaction rolls back the insertion, so the failure is visible and
  recoverable rather than data loss.
- `tests/audit/yew_f_010.c` asserts the Sprint 38 contract—store-time
  rejection with the register unchanged—and hard-XFAILs at the baseline.
  This Medium finding remains for Sprint 59.
- The `keymap:` value is emitted as a comment and parsed into header metadata.
  A full path search finds no execution-policy comparison or mismatch refusal.
  The existing radically-rebound-header unit and the Q6 control both pass.

## Q8 — undo count and replay rollback

probed, nothing found

- The focused macro-replay suite passed 7 tests and 140 assertions.
  `macro_replay_uses_one_undo_node_per_run` executes a five-count replay and
  requires exactly five successful undos, one inserted byte per node, with no
  sixth node.
- A new round-trip sentinel executes two successful insertion commands and
  an erroring third command. Replay reports `YEW_CMD_ERR_STATE` and leaves the
  buffer byte-identical to its empty pre-replay state. Existing single- and
  multi-cursor rollback controls also pass.

## Unverified observations

- Hosted run `34699266067` proves successful deterministic execution on GNU
  GCC, Clang, switch dispatch, computed-goto dispatch, and all four targets,
  but it did not upload one canonical emitted-source artifact per job. Exact
  cross-target byte equality therefore was not independently reconstructed
  after the run. The per-session P3 byte assertion and local cross-dispatch
  comparison found no divergence.

## Count

Raw 2 · deduped 2 · critical 0 · high 0 · medium 2 · low 0 · unverified 1.
