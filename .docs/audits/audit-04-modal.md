# F04 MODAL — keymap, modes, units, multi-cursor

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-04
Scope: `src/edit/`, `src/ui/viewport.c`
Owners read: Sprint 13, Sprint 14, Sprint 15, Sprint 16, Sprint 17

The product-code baseline remained immutable. Audit-control changes add
command-word, composed-keymap, thousand-cursor rollback, exact rectangular
geometry, syntax-unit progress, and runtime-global coverage only.

## Q1 — command registry and CMDWORD bijection

probed, nothing found

- The dynamic registry contained 352 named core commands. The round-trip
  coverage dump classified all 154 recordable commands: 18 generated and
  136 explicitly denied from generation. Every descriptor's word mapped
  through `yew_cmd_by_word` to its original `CmdId`.
- `cmd_registry_word_roundtrip` compared every registry word against every
  other word, then added two plugin commands and repeated the proof across
  the composed registry. Plugin registrations cannot claim the recordable
  flag and expose no CMDWORD, so they cannot create an unrecordable alias.
  The focused test passed 12,102 assertions.
- `make test-roundtrip-coverage` printed the complete generate/deny dump,
  passed the bijection check, and then passed 2,000 seeds × six fixtures
  plus the 21-file corpus under properties P1–P5.

## Q2 — named-command dispatch cardinality

probed, nothing found

- `scripts/check-cmd-dispatch.sh` found three private `cmd_*` symbols and
  exactly two source occurrences of each.
- The checker appends a third occurrence of the first discovered symbol to
  its own input and requires that negative control to fail. It then verifies
  that `keys_default.c` is data plus the single
  `yew_keys_default_install` function. The final result was
  `dispatch: named-command integrity ok`.

## Q3 — composed chord ownership and Escape rejection

probed, nothing found

The exact rebuilt-keymap transcript was:

| Origin | Binding | Result after the shared `g` prefix |
|---|---|---|
| mode table | `g g` | lower fallback remained suppressed |
| `runtime/init.fl` | `g i` | retained by the composed config layer |
| user config | `g u` | dispatched the user command |
| plugin A | `g x` | dispatched plugin A's command |
| plugin B | `g`, `g p` | owned the partial and dispatched its tail |

- After the first `g`, the composed config layer remained the recorded
  owner even though the lower mode table had `g g`; completing `g g`
  invoked the composed layer's pending short command rather than falling
  through. The user and both plugin tails then dispatched exactly once.
- Attempts to add `<esc> i`, `<esc> u`, `<esc> a`, and `<esc> b` from the
  builtin, user, plugin A, and plugin B origins all returned binding ID zero.
  The focused test passed 20 assertions.

## Q4 — shipped defaults, grammar, and globals

finding `YEW-F-004`

- The exact defaults test rebuilt all 196 active configuration bindings and
  all 57 panic-map rows from `runtime/init.fl`, with zero missing or extra
  rows. Every option value matched its C default. The three runtime-default
  tests passed 1,633 assertions.
- A full-runtime program successfully called `win.current().cursors()`,
  `set`, `bind`, `on`, and `ed.msg`, proving the globals/modules used by the
  shipped configuration are registered and executable.
- The full parser rejects a bare dotted map key at its first dot:
  `set({ clipboard.sync: "none", search.smartcase: false })` reports
  `expected ':', found '.'`. Spec §2's `entry` production and the separate
  literal parser accept this shape. The shipped configuration masks the
  mismatch by quoting dotted option names.
- This is `YEW-F-004` (Medium): valid documented configuration fails loudly
  without changing document bytes, and quoting the key is a working
  recovery. Its hard-XPASS control is `tests/audit/yew_f_004.c`.

## Q5 — thousand-cursor transaction law

probed, nothing found

- The exact control created 1,000 cursors in ascending byte order and a
  command that mutated cursors 0 through 699 before failing at cursor 700.
  It observed 701 calls, last index 700, and strict ascending visitation.
- The failure restored every byte of the 2,000-byte input, all 1,000 cursor
  positions, anchors, and goal columns, and the undo tree to its root.
- The fault-shim intercept log contained exactly one successful file
  `fsync` for the journal set and zero `fdatasync` substitutes. The child
  proof passed 4,017 assertions and its intercept-log parent passed 22.

## Q6 — rectangular highlight/delete cell identity

probed, nothing found

- The control embeds the exact line `a漢\tb👨‍👩‍👧‍👦c` between two 40-cell
  ASCII ruler lines. It sweeps all 41 × 41 `CCol` boundary pairs across the
  ruler, including reversed endpoints, padding, clipping, tab interiors,
  the wide Han glyph, and the ZWJ family cluster.
- For each of the 1,681 pairs it compares the drawn selection-background
  cell set with the cells represented by the deletion spans, invokes the
  real rectangular delete, and compares all output bytes with mechanical
  span removal. The proof passed 44,131 assertions.

## Q7 — unit-engine progress after syntax installation

probed, nothing found

- A settled syntax provider was installed over strings, delimiters, nested
  braces, and a multiline comment. At every grapheme boundary, normal and
  alternate block-next moved strictly forward when in range and block-prev
  moved strictly backward when nonzero. The focused proof passed 353
  assertions.
- The existing all-engine monotonic control passed 1,125 assertions. Four
  `fuzz_units` seeds then completed 200,000 operations each with hashes
  `cc17aa9b2128f4e0`, `aa4315f0ed73252c`, `bd84c913abfa1ffc`, and
  `1a35fbafa0f3e15b`; no in-range fixed point occurred.

## Q8 — keyboard reachability with mouse disabled

probed, nothing found in implemented surfaces

All rows below were rerun with `YEW_MOUSE=0`:

| Surface/state | Keyboard proof | Result |
|---|---|---|
| pane focus | focus-right command | pass |
| pane border resize | grow command | pass |
| tab activate and reorder | goto/move commands | pass |
| tab strip overflow | previous/next commands | pass |
| group membership and entry | add-tab/enter commands | pass |
| view scrolling | three view-scroll commands | pass |
| unit selection | `H` plus word-unit route | pass |
| context menu and every row | open, navigate, invoke, close | pass |
| panel/doc/hover/signature shell | arrows, pages, Escape, redispatch | pass |
| completion menu and doc panel | arrows, pages, `C-n`, `C-p`, `C-space`, Escape | pass |
| plugin picker | `:ed.plug.list`, Enter, reopen, Enter | pass |

- The eleven s27 invariant tests passed 177 assertions with the mouse
  disabled. The focused panel controls passed 21 assertions and completion
  passed 39. The real-PTY `s54_plugin_picker` and `s54_plugin_toggle`
  transcripts also passed with the mouse disabled.
- The tutor is not an implemented Sprint 58 surface: the Sprint 58 contract
  assigns `yew tutor` to Sprint 59. It cannot be reached or audited yet and
  is recorded below rather than treated as a mouse-only state.

## Unverified observations

- Tutor keyboard reachability is unverified because the tutor does not
  exist at this baseline; Sprint 59 owns its implementation and coverage.

## Count

Raw 1 · deduped 1 · critical 0 · high 0 · medium 1 · low 0 · unverified 1.
