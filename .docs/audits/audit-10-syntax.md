# F10 SYN — syntax engine and definitions

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/syn/`, `runtime/syntax/`, `runtime/themes/`
Owners read: Sprints 39, 40, 41, 41.5, 42, 42.5

The product-code baseline remained immutable. This front adds audit controls,
four `NO_COLOR` PTY cases, and three hard-XFAIL reproducers; no `src/` file
changed.

## Q1 — asymmetric depth-cap corruption

probed, nothing found

- `test_syn_all_48_definitions_depth_cap_and_firstbyte_sets` enumerates the
  live 48-definition catalog. For every definition it pushes 40 contexts,
  pops 40, and requires the final interned state id to equal the entry id.
- The matrix passed for all 48 definitions. Focused depth controls also passed
  2 tests and 83 assertions.

## Q2 — state and balanced-embed invariants

finding `YEW-F-012`

- The all-definition control validates every produced state's depth and
  resident-definition bounds, every live frame reference, and zero frame
  tails. Non-embedding definitions retain `ndef == 1` with the root definition
  in every live frame. `SynState.def` does not exist.
- Inline, deferred, unknown-fallback, line, inline-root, line-continuation,
  pending-loader, and pump-mediated balanced embeds now assert exact exit state
  ids. The `syn_embed_` filter passed 21 tests and 109,052 assertions; the
  broader state filters passed 12 tests and 60,937 assertions.
- The pending-loader case exposes one contract mismatch. With HTML resident
  and JavaScript requested, the state has `ndef == 1` but stores the pending
  definition in `aux[1]`. `syn_state_canon` preserves that slot even though
  Sprint 41.5 requires `aux[ndef..]` to be zero. `tests/audit/yew_f_012.c`
  records the isolated state as a hard-XFAIL. The Medium finding remains for
  Sprint 59.

## Q3 — first-byte set completeness

probed, nothing found

- The 48-definition matrix brute-forces all 256 possible leading bytes for
  every rule and checks that any matching rule's byte appears in its compiled
  first-byte set. The complete depth/first-byte control passed 1,619
  assertions.

## Q4 — cache invalidation and corruption

finding `YEW-F-011`

- Existing cache controls plus a new zero-length case passed 19 tests and 620
  assertions. Truncated, bit-flipped, bad-magic, bad-version, bad-CRC,
  structurally corrupt, and empty `.stab` inputs recompile safely.
- The source hash is not consulted when an already-loaded definition has the
  same size and nanosecond mtime. `tests/audit/yew_f_011.c` replaces an
  isolated builtin-shaped INI source with equal-size content, restores the
  exact timestamp, and observes the stale rule with zero recompiles. This is
  a Medium correctness finding for Sprint 59.

## Q5 — theme-switch isolation and attribute coverage

probed, nothing found

- The focused theme matrix passed 13 tests and 1,029 assertions. Switching
  themes calls `yew_syn_line` exactly zero times, and both shipped themes
  explicitly define every one of the 54 syntax attributes.

## Q6 — documented 256-colour tables

probed, nothing found

- The same theme matrix checks all 108 documented values against
  `yew_rgb_to_256(hex)`. Both shipped theme tables match the conversion.

## Q7 — `NO_COLOR` across post-Sprint-41 surfaces

probed, nothing found within the implemented surfaces

- The degradation unit filter passed 10 tests and 3,186 assertions, including
  empty and nonempty `NO_COLOR` values.
- Nineteen `NO_COLOR` PTYs pass. Four new cases exercise the completion
  documentation panel, shadow-index surface, diagnostics picker, and plugin
  picker. Their raw SGR transcripts contain no foreground or background colour
  selectors; only non-colour rendition controls remain.
- The tutor is not present in this baseline, so its requested surface cannot
  be executed. That evidence limit is recorded below rather than inferred as
  a pass.

## Q8 — differential edit matrix

probed, nothing found

- With `YEW_SYN_AUDIT_ALL48=1`, four deterministic seeds perform 100,000
  random edits in each of the 48 syntax modes and compare incremental state
  against from-scratch restyling after every edit. All 19.2 million edits
  passed: 1 test, 57,667,715 assertions, zero failures.

## Q9 — JS/TS known-wrong fixture annotation

finding `YEW-F-013`

- Both golden inputs retain the known-wrong `}` and `)` value-flag rows, but
  their fixture text contains no comment naming the heuristic. Identifiers
  named `knownWrong` do not satisfy Sprint 42's required fixture-local comment.
- `tests/audit/yew_f_013.c` makes that documentation requirement a
  hard-XFAIL. This Medium release-control finding remains for Sprint 59.

## Unverified observations

- Sprint 59 owns the tutor. Because no tutor surface exists at this baseline,
  F10 cannot demonstrate its `NO_COLOR` behavior; no absence claim is made for
  that future surface.

## Count

Raw 3 · deduped 3 · critical 0 · high 0 · medium 3 · low 0 · unverified 1.
