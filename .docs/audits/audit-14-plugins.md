# F14 PLUG — plugins and `yew pkg`

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/mod/plug/`
Owners read: Sprints 54, 55

The product-code baseline remained immutable. This front adds containment,
capability-provenance, lifecycle, trust-corruption, save-containment, and
package-verification controls plus three hard-XFAIL reproducers; no `src/`
file changed.

## Q1 — manifest containment after resolution

probed, nothing found

- A manifest entry of `src/../../evil.fl` is rejected and reports the
  resolved path outside the plugin root. A symlink entry resolving to
  `/etc/passwd` is rejected by the same post-resolution containment check.
- An embedded NUL in the literal entry is rejected before pathname use, and
  a manifest whose `name` differs from the containing directory basename is
  rejected with both names in its diagnostic.
- The complete manifest filter passes 12 tests and 299 assertions. Four
  independent five-second manifest-parser fuzz streams pass 1,691,759 total
  iterations with stable final hashes `a6cd401d5be1d154`,
  `5a3f8550294b9483`, `89b844b1481bcb97`, and `ea68884acb68c7de`.

## Q2 — undeclared callback capability provenance

probed, nothing found

- A real plugin declaring only `fs`, with `fs` session-granted, registers one
  named command, one `ed.idle` hook, and one bound-key closure. Each callback
  tries `ed.run("ed.shell.run", ...)`; all three return an undeclared-`shell`
  error immediately, leave `YEW_PROMPT_NONE`, and retain the plugin's
  principal. The plugin remains enabled below the five-error limit.
- Plugin API 1.0 exposes no callable `net` native: networking is intentionally
  pinned for post-1.0. The shared capability seam nevertheless has the closed
  four-capability by three-decision matrix, including direct undeclared-`net`
  denial. No absent API is represented as a successful runtime probe.

## Q3 — 20 by 20 teardown residue

finding `YEW-F-021`

- Twenty plugins each register a command, hook, key binding, option, attribute,
  and overlay. Across 20 enable/disable cycles—400 plugin enables and 400
  disables—every live registry count returns to baseline and every captured
  closure disappears from the GC object list.
- Command and plugin-value raw lengths return to baseline. Hook and
  registration-ledger raw lengths instead retain 20 and 120 inactive
  tombstones after the first cycle. They plateau rather than growing, but do
  not meet the literal zero-residue contract. `tests/audit/yew_f_021.c` pins
  this bounded Medium finding.
- A `plug.disable` observer that throws increments only its own error count;
  it cannot abort the target plugin's reverse teardown or leave the target's
  command, hook, or binding active.

## Q4 — trust preservation, order, atomicity, and corruption

probed, nothing found

- Schema-2 input carrying unknown root and per-plugin keys migrates to schema
  3 without losing either. Plugin names and the complete known-plus-unknown
  member set emit in bytewise order; a second write is byte-identical.
- The shared trust writer's atomic replacement controls remain green. Package
  integration also proves a failed trust save preserves the sentinel and that
  install/remove crash recovery restores exact policy bytes.
- Every proper byte-length prefix of a valid 169-byte trust document is loaded
  through normal editor/config initialization. All corruptions start with an
  empty policy rather than aborting startup. The trust filter passes 3 tests
  and 679 assertions, and capability allow-always persistence passes its
  restart PTY.

## Q5 — package verification and failed publication

probed, nothing found

- An update whose tag moves to a non-descendant commit is refused without
  changing the installed checkout or lock. A checkout whose HEAD disagrees
  with the lock is reported by `doctor` as `rev-mismatch` with status 1.
- A fake transport that reports clone success but leaves a truncated,
  non-repository tree fails the mandatory `rev-parse` verification with
  status 3. No plugin directory, lock row, or staging residue is published.
  Sprint 55 defines Git-only distribution, so this exercises the requested
  truncated-transfer failure at the product's actual transport boundary;
  there is no tarball installation path to pretend was tested.
- Package Git operations use argv vectors with option terminators. Static
  scans find no Git-module linkage, `system(`, or `popen(` under
  `src/mod/plug/`. Publication uses staged paths, intent recovery, rename,
  file and parent-directory synchronization, and explicit rollback errors.
- `make test-pkg` passes all 53 integration cases in both the default build
  and an isolated warning-clean `MODULES=plugins` build.

## Q6 — honest security wording

finding `YEW-F-022`

- `plug.h` plainly states that capability checks provide no memory isolation,
  no resource limits, and do not create a sandbox. The author guide quotes
  that warning verbatim, as Sprint 54 separately requires.
- F14 also requires `sandbox` to be absent from all user-facing strings. The
  honest required quotation necessarily violates that literal grep gate.
  `tests/audit/yew_f_022.c` records the contradictory contract as a Medium
  finding; the truthful warning is retained unchanged.

## Q7 — error-limit teardown and save-hook containment

probed, nothing found

- Hook, plugin-command, and bound-key callback errors all enter the shared
  five-error accounting path. On the fifth error normal reverse teardown runs,
  the plugin becomes disabled, and live registrations return to baseline. Two
  plugins reaching the limit in one event are both drained.
- A `buf.save` hook inserts bytes and throws inside one Fletch `edit {}`
  transaction. The transaction rolls back, the hook error is recorded, save
  continues, and both the resident buffer and bytes read back from disk equal
  the pre-hook `stable\n` content.

## Q8 — recorder and CMDWORD integration

finding `YEW-F-023`

- Plugin commands execute, but their descriptors omit `YEW_CMD_RECORDABLE`
  and carry a NULL CMDWORD. The recorder therefore cannot serialize or replay
  them.
- With no plugin word entered in the global CMDWORD map, a plugin local command
  named `up` is accepted beside core's `up` rather than reaching the required
  collision check. `tests/audit/yew_f_023.c` pins both halves of this Medium
  finding for Sprint 59.

## Supporting evidence

- The complete plugin unit slice passes 64 tests and 26,994 assertions.
- Nine focused plugin PTYs pass: picker/toggle behavior and every capability
  allow/deny persistence path.
- Plugin performance remains within every budget: discovery/parse p99 779 us,
  ten no-op enables p99 602 us, example enable p99 313 us, overlay p99 54 us,
  20-file package tree hash p99 1.019 ms, and 100-row lock load/save p99
  402 us.
- `make test-audit` retains all three findings as hard XFAILs; an accidental
  fix becomes a hard XPASS rather than disappearing from the campaign.

## Unverified observations

None.

## Count

Raw 3 · deduped 3 · critical 0 · high 0 · medium 3 · low 0 · unverified 0.
