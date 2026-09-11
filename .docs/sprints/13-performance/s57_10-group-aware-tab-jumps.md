# Sprint 57.10: Group-Aware Numbered Tab Jumps

## Prerequisites

- Sprint 23 — the tab model, `ed.tab.goto`, row-1 labels.
- Sprint 24 §5/§6/§7 — tab groups, the two-row bar, the continuous walk,
  and the 500 ms digit-extension window.
- Sprint 57.8 — modern padded labels (` N basenameM ` / ` group-label `).
- Binding plans and invariants 5 (deterministic render) and 9 (keyboard
  reach). Nothing here may weaken tab identity or the one construction of
  the row-1 entry list.

## Goals

Numbered jumps must address what the user is looking at. Row 1 numbers every
entry left to right — groups included — and row 2 numbers a group's members
1..n. `alt+N` addresses the row the active tab lives on: member N inside a
group, row-1 entry N otherwise. `ctrl+N` always addresses row 1, so it walks
between groups (entering one at its last-active member) and falls through to
the ungrouped tab that carries that number. The digit-extension window keeps
"jump immediately, then arm" and extends in the mode of the jump it follows.

Kitty-protocol terminals deliver `ctrl+digit`; legacy terminals do not, and
this sprint adds no modifyOtherKeys negotiation (Sprint 58 audit item).

## Deliverables

### 1. Numbered labels — `src/ui/tabs.c`

| Entry | Label |
|---|---|
| row-1 ungrouped tab | ` N basenameM ` — N is the ROW-1 POSITION, 1-based |
| row-1 group | ` N label (count) ` — same numbering, one entry per group |
| row-2 member | ` K basenameM ` — K is the 1-based ordinal position |

The number is assigned in `yew_tab_row1_entries` as entries are emitted, so
the renderer, the click router and the jump share one numbering. When no
group exists row-1 positions equal flat indices, so the plain workspace is
byte-identical to Sprint 57.8. `yew_group_label` is unchanged — other
consumers (picker, context menu) never show the position.

### 2. Row-aware `ed.tab.goto` — `src/ui/tabs.c`

`CmdStatus yew_tab_cmd_goto(CmdCtx *cx)`: with the active tab in group G,
`N` switches to G's Nth member by ordinal. Otherwise `N` addresses row-1
entry N: an ungrouped entry switches; a group entry calls
`yew_group_enter` (last-active member, dangling path falls back to the first).
`0` means 10. Count overrides `iarg`. Out of range is `YEW_CMD_ERR_ARG` with
a message naming the row ("no member 9" / "no tab 9").
`yew_group_note_position` runs before every jump so returning to a group
resumes where it was left.

### 3. `ed.tab.goto_bar` — `src/ui/tabs.c`, `src/edit/cmd.c`, `runtime/init.fl`

`CmdStatus yew_tab_cmd_goto_bar(CmdCtx *cx)`: row-1 entry N regardless of
the active group. Registered as `ed.tab.goto_bar` (`YEW_ARITY_OPT_INT`,
`YEW_CMD_TAKES_COUNT`). Bound in L mode as `C-1`..`C-9`, `C-0`.
`tests/unit/test_runtime_defaults.c`'s frozen table gains the ten rows.

### 4. Digit window in two modes — `src/ui/tabs.c`

`jump_arm` records the mode (`JUMP_ROW1` / `JUMP_MEMBER`) and the value just
jumped to. A following digit extends decimally in THAT mode: `ctrl+1` `2` is
row-1 entry 12; `alt+1` `5` inside a group is member 15. Modifier discipline
in `yew_tab_jump_key`: a `ctrl` digit continues only a ROW1 window; an `alt`
digit continues only when the window's mode is what `alt` would pick now
(MEMBER inside a group, ROW1 outside) — otherwise the window clears and the
key dispatches as a fresh jump. A bare digit always continues. The old
"landing in a group makes the next digit pick a member" rule is removed:
`alt+N` now does that directly.

**Pitfall:** the arm hint must name the row ("member 1 — a digit extends to
1_") so the status line never promises a tab when a member is meant.

## Testing Strategy

- **Unit:** `tests/unit/test_groupnav.c` — row-1 positional numbering with a
  group in the middle, row-2 member numbering, `ed.tab.goto` inside/outside
  a group (member, ungrouped entry, group entry resumes last member, 0 → 10,
  out of range), `ed.tab.goto_bar` from inside a group, both window modes
  extend decimally, modifier discipline, out-of-range swallow. Existing
  window tests updated to the new contract.
- **PTY:** goldens that show a group on row 1 or a pinned row 2 are
  regenerated (`YEW_PTY_UPDATE=1`) and eyeballed for the new numbers only.
- **Regression:** every pre-existing tab/groupnav/mouse/drag unit test stays
  green unchanged except the one whose contract §4 rewrites.

## Definition of Done

1. Row 1 shows `1..n` left to right including groups; row 2 shows `1..k`.
2. `alt+N` inside a group selects member N; outside, row-1 entry N.
3. `ctrl+N` selects row-1 entry N from anywhere; a group entry resumes its
   last-active member.
4. Digit extension works in both modes; modifier discipline as §4.
5. gcc and clang warning-free; `make test-unit` and `make test-pty` green;
   ASan/UBSan unit lane green.
