# Sprint 58 adversarial audit index

Audit opened: 2026-09-03  
Replacement baseline established: 2026-09-12
Baseline commit: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Baseline hosted run: `34699266067` (22 standard push jobs passed)
Audit-control head at opening: `6272b0932aeccb00c880d014559ce5a790edf6f8`  
UCD version: 16.0.0

**Campaign status: ACTIVE AT F13.**

F01–F08 ran against the original baseline
`41fef4166fe6bf127f36b8b9f6eb653a454a28c1`; their reports, findings, and
hosted evidence remain immutable historical records. Sprint 57.9 and the
subsequent field work deliberately reopened product code before F09. The
replacement product commit above passed the complete hosted matrix, and the
required F01–F08 delta-applicability review and focused controls are recorded
below. F09–F15 use the replacement baseline.

## Build matrix of record

| Target lane | Commit of record | Evidence |
|---|---|---|
| `x86_64-linux-gnu` | `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3` | hosted run `34699266067` |
| `arm64-linux` | `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3` | hosted run `34699266067` |
| `x86_64-linux-musl` | `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3` | hosted run `34699266067` |
| `arm64-macos` | `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3` | hosted run `34699266067` |

The baseline built the six shipping module sets: full
(`lsp ai fuss plugins`), minimal (`MODULES=""`), and the `lsp`, `ai`, `fuss`,
and `plugins` single-module profiles.

## Replacement-baseline delta requalification

The replacement contains the Sprint 57.9 structural motion/group repair and
later field work through selection/clipboard/job return, in-process
`:!yew`, FUSS action gating, and the mouse/context-menu surface. The following
review classifies applicability; it does not reopen, rewrite, remediate, or
renumber a closed front.

| Front | Delta applicability | Focused replacement control |
|---|---|---|
| F01 unicode | `src/unicode/` is byte-identical to the original baseline; terminal/UI consumers changed | complete F01 consumer corpus (53 corpus cases plus all 2²⁴ three-byte inputs) and 10,000-cluster model/VT comparison pass; `YEW-F-001`–`003` remain XFAIL |
| F02 terminal | affected: SGR no-button motion decoding and the temporary DEC 1003 lifecycle changed | terminal/input unit filters and all three `audit_terminal_*` PTYs pass, including restore-blob coverage |
| F03 text | affected: aggregate cut/paste transaction admission and undo handling changed | multicursor, selection-action, save, undo, job-return, and audit hard-XFAIL controls pass; no F03 finding changes status |
| F04 modal | affected: B-mode structural traversal, Shift+Arrow highlight entry, key normalization, command registry, and E/F prompt routing changed | block/H/multicursor/selection unit filters and `s57_9_block_*` plus `s57_12_shift_arrow_highlight` PTYs pass; `YEW-F-004` remains XFAIL |
| F05 execute | affected: safe `:!yew` self-open, bare `!`, and transient job-buffer return changed shell/job flow | shell/job unit filters and `s57_11_shell_self_open`, `s57_12_job_output_quit_returns`, and hostile-paste PTYs pass |
| F06 regex | `src/search/` is byte-identical to the F06-close tree; selection and LSP-rename consumers changed | replace, smartcase, selection, and LSP-rename unit filters pass; `YEW-F-005` remains XFAIL |
| F07 UI/workspace | affected: group picker/close, positional tabs, FUSS drawer actions, tabs, regions, mouse routing, and context menus changed; state parse/emit/save is byte-identical to the F07-close tree | group storm, layout-region, context-row/menu, FUSS/group and all `s57_13_*` PTYs pass; `YEW-F-006` and `YEW-F-007` remain XFAIL |
| F08 Fletch | affected at the recorder prompt boundary and by registry/batch growth; VM, origin, GC, and capability implementation did not change | recorder/macro/Fletch-transaction unit filters and audit hard-XFAIL controls pass; `YEW-F-008` remains XFAIL |

Local controls ran at `d6c740f0`, whose only delta from the replacement product
commit is `.docs/sprints/index.md`. `make test-audit` passed 8 hard-XFAIL
reproducers, the F01 corpus and VT probes, and both ledger checks. Focused unit
filters passed 555 invocations and 5,038,739 assertions; the 20 focused PTY
cases above matched their committed final-screen goldens. The complete hosted
run remains the cross-compiler and cross-architecture proof for the exact
product baseline.

## Audit-harness confirmation

Hosted run `33815573832` passed at audit-control head `3e0edb57`, including
the F01 hard-XPASS suite under GCC, Clang, ASan/UBSan, arm64 Linux, arm64
macOS, musl, and the minimal `MODULES=""` profile. The three F01 failures
therefore have second-machine and cross-architecture confirmation; the
product-code baseline remains the immutable commit above.

Hosted run `33826009408` passed at F02 close head `7d7aad16`, including the
451-case strict-VT suite, terminal restore and adversarial burst coverage,
GCC, Clang, ASan/UBSan, arm64 Linux, arm64 macOS, musl, determinism, and the
minimal `MODULES=""` profile. The product-code baseline remains immutable.

Hosted run `33836915903` passed at F03 close head `b9e604d4`, including the
new save-identity, register-routing, cross-EOL paste, and LSP-rename controls
under GCC, Clang, ASan/UBSan, arm64 Linux, arm64 macOS, musl, determinism,
and the minimal `MODULES=""` profile. Its predecessor exposed a
scheduler-dependent SGR transcript in the FUSS PTY harness; `b9e604d4`
normalizes the unobservable gap between its first and final cell-bearing
frames. The product-code baseline remains immutable.

Hosted run `34011188190` passed at F04 close head `de70ed63`, including all
22 standard push jobs: GCC, Clang, ASan/UBSan, arm64 Linux, arm64 macOS,
musl, determinism, LSP, Fletch dispatch, and the minimal `MODULES=""`
profile. The product-code baseline remains immutable.

Hosted run `34283504177` passed all 22 standard push jobs at post-F05
audit-control head `ef14ff7e`. It includes the F05 execute controls, strict
452-case PTY coverage, five determinism repeats, both hosted arm64 targets,
musl, the strict performance lane, and the complete fixed-depth sanitizer
fuzz graph. The AI-shadow sanitizer campaign retained seed 1 and 20,000
iterations, used its bounded 30-second per-input watchdog, and completed with
hash `7000f4676382f7ac`. The product-code baseline remains immutable, and this
run qualifies the control head for opening F06.

The successful sanitizer log also emitted a recover-mode UBSan diagnostic at
`src/edit/sel_actions.c:289` while
`draw_rect_selected_cells_equal_deleted_span_cells` passed. Apple clang does
not reproduce it in the focused `halt_on_error=1` run. This is an explicit
out-of-scope handoff to F07 for the product path and F15 for gate honesty; it
is not counted as a finding until the reproducer-first and confirmation rules
are satisfied.

Hosted run `34514196947` passed at F07 close head `d5b95578`, including the
new four-size rendered-region control under GCC, Clang, ASan/UBSan, arm64
Linux, arm64 macOS, musl, determinism, PTY, and the minimal `MODULES=""`
profile. The two F07 Critical workspace-state findings and its explicit
Sprint 59 tutor observation remain recorded; the product-code baseline
remains immutable.

Hosted run `34550652527` passed at F08 audit-control head `6247804a`,
including the F008 hard-XFAIL under GCC, Clang, ASan/UBSan, arm64 Linux,
arm64 macOS, musl, determinism, PTY, Fletch dispatch, and the minimal
`MODULES=""` profile. It also confirms that the F008 isolated-file
diagnostic is byte-stable across the repeated full-suite comparison. The
product-code baseline remains immutable.

## External tools at opening

| Tool | Version |
|---|---|
| git | 2.55.0 |
| clangd | Apple clangd 21.0.0 (`arm64-apple-darwin25.4.0`) |
| interactive shell | zsh 5.9 (`arm64-apple-darwin25.0`) |
| E-mode fixture shell | `/bin/sh` |

## Front ledger

`pending` means the front has not been handed to its reviewer. Counts remain
zero until its report closes; silence never counts as evidence.

| Front | File | Status | Raw | Deduped | Crit | High | Med | Low | Unverified |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| F01 unicode | `audit-01-unicode.md` | closed | 3 | 3 | 0 | 1 | 2 | 0 | 3 |
| F02 terminal | `audit-02-terminal.md` | closed | 0 | 0 | 0 | 0 | 0 | 0 | 3 |
| F03 text | `audit-03-text.md` | closed | 0 | 0 | 0 | 0 | 0 | 0 | 2 |
| F04 modal | `audit-04-modal.md` | closed | 1 | 1 | 0 | 0 | 1 | 0 | 1 |
| F05 execute | `audit-05-exec.md` | closed | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F06 regex | `audit-06-regex.md` | closed | 1 | 1 | 0 | 1 | 0 | 0 | 2 |
| F07 UI/workspace | `audit-07-ui.md` | closed | 2 | 2 | 2 | 0 | 0 | 0 | 1 |
| F08 Fletch | `audit-08-fletch.md` | closed | 1 | 1 | 0 | 1 | 0 | 0 | 2 |
| F09 recorder | `audit-09-recorder.md` | closed | 2 | 2 | 0 | 0 | 2 | 0 | 1 |
| F10 syntax | `audit-10-syntax.md` | closed | 3 | 3 | 0 | 0 | 3 | 0 | 1 |
| F11 LSP | `audit-11-lsp.md` | closed | 3 | 3 | 0 | 0 | 3 | 0 | 0 |
| F12 AI | `audit-12-ai.md` | closed | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F13 git/FUSS | `audit-13-git.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F14 plugins | `audit-14-plugins.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F15 CI | `audit-15-ci.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

## Verdict

We are not ready to tag: F01 through F12 of Sprint 58's fifteen fronts have
closed, the invariant sweep has not run, and no campaign-wide
absence-of-findings claim has been earned. F06 adds an open High finding
(`YEW-F-005`), while closed F07 records two open Critical workspace-state
findings (`YEW-F-006`, `YEW-F-007`): normal persistence can drop future keys
and reorder a group member sequence. Its tutor-inclusive repository-pollution
session remains one explicit Sprint 59 unverified observation.

F08 adds an open High capability finding (`YEW-F-008`): a plugin granted no
capabilities can route macro source through replay and receive config
authority. The product-code baseline remains immutable; its two explicitly
unverified observations are the destructive GC-root crash request and the
parts of the literal 4-by-4 capability matrix that have no 1.0 native surface.

F09 adds two open Medium findings. `YEW-F-009` records that generator growth
invalidated the pinned recorder fault seed, so the shrinker self-test exits
before injection. `YEW-F-010` records that macro store accepts source whose
unresolved global makes first replay fail; replay rollback still preserves
document bytes. The exact cross-target emitted-source artifact comparison is
F09's one unverified observation.

F10 adds three open Medium findings. `YEW-F-011` records that matching source
size and nanosecond mtime can bypass the authoritative syntax-source hash.
`YEW-F-012` records the pending-embed mechanism's conflict with the canonical
zero-tail state law. `YEW-F-013` records that the JS/TS known-wrong fixtures
lack their required heuristic comment. The product-code baseline remains
immutable. Tutor `NO_COLOR` behavior is F10's one unverified observation
because that Sprint 59 surface does not yet exist. F13 through F15 remain
open.

F11 adds three open Medium contract/control findings. `YEW-F-014` records
that stripped `ed.lsp.complete` bypasses the exact module hard error.
`YEW-F-015` records that the snippet-policy source gate matches unrelated
core placeholders. `YEW-F-016` records the conflict between Sprint 46's
repository-wide line-number adjustment ban and Sprint 47's required 1-based
display edges. Position conversion, pre-op sync, stale-response admission,
rename rollback, and 200,000 malformed-response iterations passed; the
product-code baseline remains immutable.

F12 closes with no findings or unverified observations. Its controls prove
off-by-default transport silence, the pre-transport privacy boundary, all
required loopback spellings and redirect refusal, hostile chunked-parser
coverage, exact cancellation resource accounting, AI-specific stale-shadow
rejection, and every stripped `ed.ai.*` command surface. The product-code
baseline remains immutable. F13 through F15 remain open.
