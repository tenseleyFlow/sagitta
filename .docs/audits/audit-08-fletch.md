# F08 FL — Fletch VM, stdlib, editor API

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-10
Closed: 2026-09-10
Scope: `src/fl/`, `tests/fletch/`
Owners read: Sprint 28 through Sprint 34, Sprint 36

The product-code baseline remained immutable. This front adds two ordinary
transaction controls and one hard-XPASS capability reproducer; no `src/`
file changed.

## Q1 — amendment arithmetic and reachable error kinds

probed, nothing found

- Spec §9.1 names exactly 13 closed runtime kinds: `type`, `arity`, `name`,
  `index`, `key`, `div`, `capability`, `io`, `import`, `motion`, `user`,
  `limit`, and `handle`. `tests/fletch/09-errors.fl` and the runtime-error
  controls make each kind individually catchable.
- §16 has unique amendment IDs. A1 and A2 are the two error-kind amendments;
  A3 records the runtime import path; A4 is a separately filed closure-text
  correction. The audit prompt's literal "exactly A1/A2/A3" predates that
  valid A4 row, so treating its existence as an implementation mismatch would
  create a false finding.
- `make test-fletch` passed all 38 scripts. Its coverage gate reported all
  seven checks passing, including the real 13-kind error set and all 60
  opcodes; `tests/fletch/ledger.txt` records `kinds 13/13` and
  `opcodes 60/60`.

## Q2 — origin declaration and the two zeroes

probed, nothing found

- `src/fl/origin.h` is the sole declaration site for `FlOriginKind` and
  `FlOrigin`; a full-tree search found no `YEW_ORIGIN_*` implementation
  spelling. It documents the deliberate distinction between enum value
  `FL_ORIGIN_BUILTIN = 0` and registry id
  `FL_ORIGIN_ID_CONFIG = 0`.
- `fl_origin_reg_init` seeds config in registry slot zero. The direct
  `fl_origin_registry_reserves_config_zero` control registers a plugin first
  and asserts its id is one, not zero; all non-config origins receive distinct
  nonzero ids. The focused origin suite passed 6 tests and 103 assertions.
- The plugin-helper chain control also passed: a helper first imported as
  config is separately loaded under an unprivileged plugin and still raises
  the catchable `capability` error.

## Q3 — GC roots and stress mode

probed, nothing found

- `gc.c` enumerates and marks the VM value stack, frames, open upvalues,
  globals, modules, builtins, prelude, host roots, temporary protection stack,
  compiler chain, root providers, and an in-flight error. `fl_gc_host_root_*`
  and `fl_gc_root_provider*` are the explicit host-facing forms.
- The host-root and provider controls prove the roots are load-bearing without
  dereferencing a collected C object: an unrooted witness is swept, a rooted
  witness survives, and reducing/removing its root sweeps it on the next
  collection. The hook provider control passes as well.
- `make test-fletch-gc-stress` passed all 38 scripts. Runtime accepts both
  `YEW_FL_GC_STRESS` and `FL_GC_STRESS`; older owner DoDs use the latter while
  Sprint 34 and the Makefile use the former.

## Q4 — transaction opcodes and ledger coverage

probed, nothing found

- `src/fl/opcodes.h` retains `_Static_assert(FL_OP__COUNT == 60)`. The opcode
  table, compiler, and VM all contain `EDIT_BEGIN` and `EDIT_END`.
- `fl_compile_every_opcode_is_covered` compiles a corpus row for every opcode
  (with tested synthetic rows where appropriate) and passed with 63
  assertions. The Fletch conformance ledger independently reported 60/60.

## Q5 — editor side-door scan

probed, nothing found

- The requested scan for direct edit, text-buffer, undo, register, and
  cursor-set calls under `src/fl/` finds only the private REPL prompt buffer
  in `src/fl/repl.c`; it does not mutate the editor document or its registers.
- `scripts/check-fl-choke.sh` passed. Its self-check temporarily seeds a
  prohibited direct editor mutation and confirms the gate rejects it, while
  retaining the narrow private-prompt exception.

## Q6 — capability enforcement across call paths

finding `YEW-F-008`

- Existing controls pass the meaningful direct and delegation checks:
  `fs.read` and `fs.write` are granted and denied through `io`; config and
  plugin helper modules remain distinct; builtin frames are transparent; and
  denials are catchable. The focused capability suite passed 5 tests and 37
  assertions.
- Shell and net have no public 1.0 Fletch native surface, so their granted
  and denied bit checks run directly through `fl_cap_check`; inventing an
  `io.run` or network native just for this audit would test a non-shipping
  surface. Hook registration/lifecycle is covered, and hook closures retain
  their defining origin in the call path, but it does not supply a separate
  live capability matrix for every bit.
- The macro path fails. `tests/audit/yew_f_008.c` installs a plugin with
  `capabilities: []`. Its `init` uses `ed.run("ed.reg.set", ...)` to store
  `io.write` source in register `a`, then calls
  `ed.run("ed.macro.replay", ...)`. The isolated baseline run enables the
  plugin and creates the temporary `escaped.txt` file. The XFAIL is registered
  only in profiles that include the optional plugin module; stripped profiles
  have no plugin entry point to attack and compile the fixture's inert stub.
- The audited call chain is `fl_api_ed_run` → `yew_flapi_cmd_reg_set` →
  `yew_macro_replay` → `fl_macro_compile_cached` → `fl_compile_str`.
  `fl_compile_str` assigns its trusted config `runtime_origin`, so the
  plugin-supplied macro runs with config authority instead of its defining
  plugin origin. This violates spec §13.1's no-ambient-authority rule.
- This is High under the Sprint 58 rubric: it violates a documented security
  rule and lets an unprivileged plugin perform a protected file write. The
  reproducer is a hard-XPASS and remediation is deferred to Sprint 59; no
  product source changed in F08.

## Q7 — nested transaction scale and escaping error

probed, nothing found

- `fl_txn_audit_100k_nested_mutations_make_one_undo` performs 100,000 real
  text insertions inside two nested edit boundaries. It commits exactly one
  undo node, then one undo restores the zero-byte buffer.
- `fl_txn_audit_nested_escape_rolls_back_and_reraises` evaluates nested
  `edit` blocks whose inner block raises `error("audit escape")`. It reports
  the unchanged `!user: audit escape`, leaves zero text and only the root undo
  node, and resets both VM edit and transaction depth. The focused controls
  passed 2 tests, 100,028 assertions, and no failures.

## Q8 — formatting boundary and ban enforcement

probed, nothing found

- `src/fl/stdfmt.c` parses user templates with the bespoke formatter and
  `Bytebuf`; its `snprintf` uses are fixed-format diagnostics and labels.
- `scripts/bans.sh` passed and its `src/fl/` printf rule self-seeds nonliteral
  `printf`-family calls to prove the detector is active. `git diff --check`
  also passed.

## Q9 — handle lifetime matrix

probed, nothing found

The focused handle suite passed 8 tests and 496,129 assertions. It covers a
buffer handle after close, all dead-kind/free-and-reuse cases, a span through
1,000 randomized edits and collapse, and regex ownership through 10,000
compile cycles.

## Unverified observations

- The literal request to unregister every host root and then deliberately
  crash was not executed. A post-sweep C dereference would be undefined
  behavior, not a reliable test assertion. The ordinary liveness/drop controls
  above prove each supplied root controls reachability, and the full stress
  suite passes under both accepted environment spellings.
- The requested 4-capability by 4-call-path matrix cannot be fully live on
  this 1.0 surface: shell and net have no native Fletch API, and no exact
  cap-specific hook matrix exists. This is recorded separately from the
  confirmed macro-replay escalation; unavailable surfaces are not counted as
  passing.

## Count

Raw 1 · deduped 1 · critical 0 · high 1 · medium 0 · low 0 · unverified 2.
