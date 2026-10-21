# Sprint 59.8: Interactive merge resolution — implementation in progress

## Prerequisites

Sprint 59.5 owns modal Git operations and transient-tab origin tracking.
Sprint 59.7 owns the read-only file diff projection and immutable-view
command guard. Sprint 53 supplies aligned two-pane diff presentation;
Sprints 22/23 supply pane trees and stable tab/window identities. Read the
binding decisions, architecture, Fletch and campaign documents, plus
`AUTHORING.md`, before implementation.

This contract was refined on 2026-10-21 against Yew `86d727fe` and local
`~/GithubOrgs/FortranGoingOnForty/fit` at
`8bdc994a04db3ce13808f497285e8d1d38e591ac`. Fit's `src/tui_layout.f90`,
`src/pane_state.f90` and `app/main.f90` establish the reference: two source
panes above a full-width result, approximately 40/60 vertical allocation,
active-pane emphasis, independent scrolling and a conflict counter.
This was source inspection, not a live visual verification of fit.

Before product implementation, append a scoped amendment to
`00-decisions.md` superseding S59-A30 and Sprint 53's post-1.0 deferral for
the text-resolution surface below. Until then, existing refusal commands
are truthful shipped behavior. The numbered sprint remains 59. Do not
reset its independent soak or close the unreproduced 59.6 Insert-mode stall.

## Goals

Resolve ordinary textual Git conflicts using a keyboard-reachable editor
workspace with current/incoming comparison above a clearly identified result,
and per-conflict accept-current/accept-incoming/accept-both actions. Detect
conflicts after foreground Git operations without stealing focus after a
user has moved elsewhere. Preserve unsaved documents, arbitrary bytes and
normal undo/save behavior. Fit supplies the spatial idea, not its file writer,
input loop or marker-side terminology. Sub-hunk staging remains outside this
sprint. Ordinary Alt+d file comparison remains a read-only unified view;
the merge workspace is a distinct, explicitly entered surface.

## Deliverables

### 1. Snapshot acquisition and side identity

New `src/mod/git/conflict.[ch]`, using the existing asynchronous
callback/blob machinery in `git.[ch]`. Planned entry points (not landed):

```c
typedef struct GitConflictSnapshot GitConflictSnapshot;
bool yew_git_conflict_request(Ed *ed, const char *workspace_path);
void yew_git_conflict_drop(GitConflictSnapshot *snapshot);
```

Own canonical repo/worktree identity, byte-exact repo-relative path, stage
OIDs/modes, immutable source bytes, operation identity, result-buffer ID and
revision, originating tab/window IDs and a request token. Do not retain
`Win *`, `Buffer *`, tab indices or callback-borrowed bytes across a yield.
Normalize nested workspace paths through the existing Git path machinery.

- Read unmerged entries with literal-path `git ls-files --unmerged -z --
  <path>`. Parse `<mode> SP <oid> SP <stage> TAB <path> NUL`: only the
  metadata delimiters have meaning; tabs/newlines inside the path survive.
  Validate duplicate stages, modes, OID lengths and record termination.
- Fetch stages by captured OID, not mutable `HEAD:path` or `:path` after
  acquisition. Extend the existing async blob owner rather than adding a
  synchronous process path. Missing stage 1 is legitimate add/add; missing
  stage 2 or 3 is an unsupported delete-side case, not an empty blob.
- Capture operation kind and relevant ref/OID identity. Re-read stage and
  operation identity after acquisition; refuse a mixed snapshot. A status
  cache refresh alone is not a content change.
- Each blob/result scan is capped at 16 MiB and 200,000 lines. Keep only
  one acquisition/revalidation in flight per session and one apply pending;
  honor the module's four-job global cap and five-second read timeout.
  Charge all session-owned blob copies, scratch projections and row maps
  against a 128 MiB per-session allocation ceiling (the ordinary result
  buffer/undo retain their existing ownership). Check before allocating;
  oversize resolution stays available as manual editing. Do not multiply
  full-file copies per conflict or per pane. Record aggregate peak RSS as
  well; this ceiling is not a claim about total editor/process residency.

| Display position | Index stage | Merge / cherry-pick | Rebase |
| --- | --- | --- | --- |
| Top left: Current | 2 / ours | checked-out side | rebased-onto side, including earlier replayed commits |
| Top right: Incoming | 3 / theirs | merged-in / picked commit | commit being replayed |
| Optional Base | 1 | merge ancestor | merge ancestor for this replay |
| Bottom: Result | worktree document | actual editable result | actual editable result |

Headers include operation, stage and actual ref/short OID; detailed identity
is available in the action/help picker. Unknown refs display the OID, never
a guessed branch name. Fit calls the first `<<<<<<<` arm incoming and the
second local; do **not** copy that convention. In Git's ordinary markers
the first arm is stage 2 and the second stage 3. Rebase labels must say
`Current / onto` and `Incoming / replayed` explicitly.

**Pitfall:** an index OID is a blob identity, not a commit identity. Name
which is displayed. Linked worktrees must use their own Git metadata.

### 2. Conflict ranges, byte policy and undoable application

Keep parsing/range validation in `conflict.[ch]`, separate from drawing.
Parse the real result document, not the unified patch or terminal output.
Store half-open byte spans for marker envelope, current arm, optional base
arm and incoming arm; each actionable conflict gets a session-stable ID.

| Input | Required behavior |
| --- | --- |
| Ordinary markers | coherent start / separator / end at line starts |
| diff3 / zdiff3 | optional base section excluded from accepted result; retain surrounding common text |
| Custom marker width | consistent width from attributes/validated markers; never assume seven |
| LF / CRLF / BOM | preserve actual result representation and untouched bytes |
| Missing final newline | preserve source termination when mapping is unambiguous |
| Nested, unmatched, literal-looking or repeated ambiguous markers | refuse that automated choice with location/reason |
| Empty arm / adjacent conflicts | valid zero-byte replacement and distinct conflict IDs |
| Binary, symlink, submodule, rename, delete-side or custom-driver ambiguity | inspect/manual-edit route; no automated choice |

Verify monotonic correspondence of marker arms with captured stages; never
select the first equal substring when repeated text gives multiple answers.
Account explicitly for Git's ordinary EOL conversion when comparing blob
bytes to worktree bytes. Custom filters/working-tree encodings that cannot
be mapped losslessly are an explanatory refusal, not silent normalization.
Only a verified conflict range is eligible; hand-edited ambiguous arms stay
editable but lose automated choices until revalidated. Missing markers do
not by themselves prove the index is resolved.

Acceptance replaces **only** the selected marker envelope using the mapped
worktree-form arm bytes, with `yew_ed_edit_ctx_for`, `yew_edit_delete` and
`yew_edit_insert`. One `yew_undo_begin` / `yew_undo_end` transaction per
choice; failure uses `yew_undo_abort`. Preserve all journal, cursor, mark,
syntax, change notification and normal durability-failure behavior.

- Current = stage-2 arm; incoming = stage-3 arm.
- Both = current then incoming, explicitly labelled in the action UI.
  Preserve both byte sequences. If both are nonempty and the current arm
  lacks a terminal newline, insert exactly one boundary newline using the
  result's EOL policy; announce this in the action detail. Preserve the
  incoming arm's final-newline state. Empty sides add no separator. Refuse
  an ambiguous boundary rather than guessing or joining source tokens.
- Keep focus on the accepted result for review; no automatic next/accept.
  Repeating accept on an already-resolved range is a visible refusal.
  Undo restores markers and unresolved state; redo reapplies the choice.
  Manual edits invalidate/recompute affected ranges, not the whole document
  on every arrow key. Never offer an old choice against a changed range.

An action first schedules asynchronous identity revalidation. Capture its
result revision/conflict ID; if either changes while validation runs, cancel
that action rather than applying at a shifted offset. Compare stage/operation
identity and a bounded worktree content identity, not just timestamps.
External modification marks the session stale. An explicit refresh may
rebuild source snapshots, but must never reload a dirty result document.
The ordinary save path retains its own external-change checks: preflight is
not a filesystem lock or a promise that another process cannot race later.

**Pitfall:** the async request command is not itself the text transaction.
Do not leave an undo transaction open across poll-loop turns. On completion,
reacquire the stable result view and apply through the normal edit/durability
boundary. Do not bypass the immutable guard for source panes, or dispatch a
CHANGES_BUFFER command against whichever unrelated pane now has focus.
Do not mark asynchronous choices recordable/repeatable until replay ordering
is actually supported; batch/macro invocation must fail explicitly, not
report a completed edit while merely queuing work.

### 3. Fit-inspired comparison/result workspace

New `src/mod/git/mergeview.[ch]` owns session/view state. Integrate through
`src/mod/git/editor.[ch]`, `src/ui/layout.[ch]`, `draw.c`, `gutter.c`,
`region.[ch]`, `mouse.c`, `tabs.[ch]` and `src/edit/ed.c` lifecycle hooks.
Reuse pane trees, Unicode measurement, diff row maps, region dispatch and
theme roles; no second terminal renderer or input loop.

```text
 [existing tabs] [Merge: path *]
 Merge / path                         2 unresolved / 3 conflicts
 Current / stage 2 / ref@oid          | Incoming / stage 3 / ref@oid
 read-only source                    | read-only source
 ... active conflict ...             | ... active conflict ...
 ------------------------------------+--------------------------
 Result / path *       [Current] [Incoming] [Both: current first]
 actual editable file, including unresolved markers
 ... normal cursor, selection, syntax and undo ...
 [mode]                        previous / next / actions / leave
```

- Source band starts at 40% of available content height; full-width result
  gets 60%, after tab/footer/chrome reservations. Use existing split ratios
  and resize mechanics, with equal source widths. Compute rectangles once;
  drawing and hit regions consume the same clipped cells.
- Wide layout requires at least 81 content columns and 16 content rows
  after global chrome: two 40-cell source panes plus divider. Below 81
  columns, show one full-width source above result, with a source selector.
  Below 16 content rows, show one focus-selected view with a compact
  Current / Incoming / Result selector; initially Result. These are layout
  breakpoints, not minimum terminal requirements. Resize never drops a
  buffer, selection, cursor, scroll position or pending action.
- Each pane has its own viewport. Previous/next conflict aligns the three
  views to the same conflict; ordinary scrolling affects only the focused
  pane. No lockstep pixel/line scrolling of the result: accepted arms have
  different lengths. Reuse alignment maps only for read-only comparisons;
  never insert filler lines into the editable result.
- Source panes show captured full-file stage content, not a composite with
  accepted choices substituted into other conflicts. Source selection/copy
  yields source bytes only, excluding filler, labels and action chrome.
  Base inspection temporarily replaces the selected source, not a fourth
  permanent pane; an absent base is clearly indicated.
- Subtle distinct source accents, one consistent header surface, thin full
  separators, active-pane emphasis and a conspicuous `Result` label.
  Unresolved ranges use the conflict role. Actual additions/deletions retain
  green/red diff roles; do not call an entire side a deletion merely because
  it is Current. Labels/markers carry meaning in monochrome too. Use shared
  glyph fallbacks and existing dark/light/16/256/truecolor theme facilities.
- A fixed action strip above Result names the selected conflict and offers
  current/incoming/both. It is chrome, never ghost text, document bytes or
  copyable content. On narrow screens collapse actions to a labelled picker
  button; hidden rectangles are never clickable. Headers/buttons are cell-
  clipped through shared Unicode APIs, including hostile path/ref bytes.

**Document ownership:** the merge workspace is a dedicated, non-transient
tab labelled `Merge: path`; it references the existing result buffer, or
opens that file normally if absent. There is exactly one editable buffer
for that canonical path. Opening does not restructure an existing tab/group
or duplicate its undo tree. Sources are read-only scratch snapshots with no
save target, LSP or completion-provider attachment.

Retain origin tab ID/FUSS return intent separately from transient-output
flags. Leaving the workspace releases private source views and leaves the
result in an ordinary file tab (reuse an existing one, or convert the merge
tab if it was the sole document view). Escape never discards result edits:
first dismiss overlays/leave Insert/Highlight as usual, then L-mode Escape
leaves comparison chrome. Standard tab close uses ordinary dirty-document
rules. Dirty result buffers remain journaled and recoverable; workspace
serialization retains the real file, not ephemeral source blobs or stale
merge sessions. Reopening reacquires Git identity.

Closing/reordering source panes, tab drag/split, group close, Save As and
workspace change must invalidate or rebind by stable identity. Refuse a
merge choice after Save As changes its result path. No late callback may
recreate closed chrome or steal focus.

### 4. Commands and keyboard/mouse reachability

Add commands in `src/edit/cmd.c`; handlers live in `mergeview.[ch]` and
delegate byte operations to `conflict.[ch]`. Retain existing ours/theirs
command names for compatibility, but label them Current/Incoming in UI.
All new functions below return `CmdStatus` and take `CmdCtx *cx`:

```c
CmdStatus yew_git_cmd_conflict_open(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_next(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_prev(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_ours(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_theirs(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_both(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_actions(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_source(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_refresh(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_leave(CmdCtx *cx);
```

Default binding proposal, checked against `runtime/init.fl` and
`src/edit/keys_default.c` at the planning anchor:

| Context | Keys | Command / behavior |
| --- | --- | --- |
| FUSS | Alt+e | `ed.git.conflict.open` selected conflicted path; otherwise conflict chooser |
| L/W/B | Alt+e, then o | `ed.git.conflict.open` active path or chooser |
| L/W/B | Alt+e, then n / p | `ed.git.conflict.next` / `.prev` |
| L/W/B | Alt+e, then c / i / b | `.ours` / `.theirs` / `.both` |
| L/W/B | Alt+e, then a | `.actions` effective-binding-derived picker |
| L/W/B | Alt+e, then s | `.source` Current / Incoming / Base selector |
| L/W/B | Alt+e, then r / q | `.refresh` / `.leave` |
| L | Ctrl+w, then arrows / w | existing spatial / next-pane navigation |
| Any command context | `:git.conflict.*` | same registry commands; no duplicate action path |

Two-key chords release Alt before the second key. Add the FUSS row to its
panic table so it appears in Ctrl+Shift+/ help; document-mode rows belong
in the runtime config and remain rebindable. No new bare-letter actions in
FUSS or Insert; no override of Alt+m commit, Alt+d diff, Alt+g groups,
ordinary arrows, shadow acceptance or E-mode Alt+e prompt editing. In compact
layout, pane focus must also select/reveal hidden logical sources/results.

Accept buttons/picker choices target the session's result, not the hovered
source buffer. Keyboard, mouse and command-bar invocations share validation
and application. Cancel any pending chord when tab/session context changes.
Outside a merge session, actions other than open error with a precise reason.

**Pitfall:** do not globally relax `YEW_BUF_READONLY` to make acceptance
work while a source is focused. Source typing, paste, cut, undo and redo
remain refused. Ordinary result editing/clipboard bindings remain unchanged.

### 5. Foreground integration and explicit completion

`src/mod/git/fussmode.c`: after a user-initiated merge, pull, rebase or
cherry-pick completes/stops, refresh status and detect actual unmerged paths.
An exit code or stderr substring alone is insufficient. One supported
conflicted file opens directly; several open a path chooser with operation
context. Unsupported files remain listed with their reason/manual route.

Capture originating tab/window and an interaction token when the operation
starts. Open automatically only if the user has not moved elsewhere or
started another interaction, and no newer modal owns input. Otherwise post
a notification with explicit `git.conflict.open`. Background status discovery
never opens a workspace. Do not reopen FUSS over the result after completion.
Other conflict sources (external Git, revert, stash pop) remain explicitly
openable when their text/stage identity is supported; do not claim automatic
flows that have not been tested.

The completion indicator says `No unresolved markers; not staged`, not
`Merge complete`. Save, stage, commit and rebase-continue remain distinct
explicit operations. Stage consumes saved worktree bytes, not the live
buffer. Any contextual Stage action must check unsaved changes and offer
Save or Cancel first; it must not quietly stage an older disk version.
Accepting the last conflict never saves, stages, commits or continues.

### 6. Delivery sequence and exclusions

Implement as independently reviewable/tested commits, in this order:

1. Scoped decision amendment, pure marker/index parser and fixtures.
2. Async snapshot/revalidation, byte-exact acceptance, undo and ownership
   tests; registered actions only replace their refusal when complete.
3. Dedicated three-pane/compact layouts, actions, close/recovery behavior,
   themes and keyboard/mouse PTYs.
4. Foreground operation integration, real-Git merge/rebase/pull/cherry-pick
   scenarios, manual and handoff evidence; then full relevant regression.

Deferred: sub-hunk staging; automated binary, rename, delete-side, symlink,
submodule or ambiguous custom-driver resolution; accept-all; reorder-both;
automatic save/stage/continue; a second Git merge algorithm. Unsupported
variants fail with an honest scope reason; manual editing remains available.
Do not change ordinary 59.7 file diff or 53 index/live diff semantics.
Update `docs/manual/13-git.md` only with shipped behavior and limitations.

## Testing Strategy

- **Unit:** new `tests/unit/test_git_conflict.c`, `test_git_mergeview.c`,
  registered in `tests.h` and `registry.c`. Exercise every input/label row,
  current-first separator policy, failure-atomic rejection, exact bytes outside
  each envelope, one undo node per choice, undo/redo after manual edits,
  generation changes, same-mtime external replacement, journal failure,
  readonly-source refusal, byte/line/allocation-limit boundaries and
  full/stripped module boundaries.
- **Real Git:** new `tests/script/git_conflicts.c` and Makefile target
  `test-git-conflicts`, following `fuss_commands.c`'s isolated repositories
  and identity/signing/hook policy. Cover merge, local-remote pull, rebase and
  cherry-pick; stage 2/3 identity, add/add, missing sides, nested/linked
  workspaces, spaces/tabs/newlines/leading dashes in paths, dirty result
  reuse, stale index/worktree, save/stage/continue separation and cancellation.
  Never run these operations in the user's real repositories.
- **PTY:** register `git_conflict_` cases in `tests/pty/registry.c`, with
  reviewed goldens under `tests/pty/goldens/`. Compare document bytes as well
  as grids. Cover 120x40 and 80x24, each width/height breakpoint +/-1,
  40x12 and tiny-terminal degradation, dark/light/NO_COLOR/ASCII, long and
  wide paths, pane scroll/focus, all choices by keys and mouse, copy without
  chrome/filler, FUSS type-jump and Insert text retention, Escape sequencing,
  dirty close, group/tab reorder, Save As, session restore and late callbacks.
  Every selected PTY runs twice without golden-update mode.
- **Bounded dynamic checks:** ASan/UBSan focused units and real-Git scripts.
  Add parser cases to `tests/fuzz/fuzz_git_diff.c`; run one process, fixed
  seed 598, 5,000 iterations with bounded/reset inputs and RSS observation.
  Do not start another soak or unbounded text-buffer fuzz run.
- **Performance:** instrument no-spawn/no-rescan cursor navigation and
  unchanged-frame zero-output checks in an active merge session. Add a
  bounded merge fixture to `tests/perf/git_gutter.c`; verify warm keypress
  p99 against the existing 5 ms reference gate. Parsing/alignment and identity
  work use resumable, cancellable slices of at most 4 ms through the loop,
  not on arrow/draw paths. Measure loading, apply latency and peak RSS
  separately; perform 100 open/leave cycles and
  assert no live session/job/blob/region growth. Shared-host observations
  are diagnostic, not designated-hardware release signoff.
- **Regression:** warning-clean Clang/GCC full builds and `MODULES=` smoke;
  existing diffview, git editor, FUSS, cmd registry, panes, tabs, workspace,
  command-output and modal tests; `test-fuss-commands`, `test-git-script`,
  `test-git-hunks`, manual/docs-sync gates. At most two build jobs. Use a
  writable disjoint BUILD; never clean/chown the user's root-owned build.

## Definition of Done

1. The scoped decision amendment lands; unrelated 1.0 deferrals stay intact.
2. Every supported marker/index/label table row has positive and refusal
   tests; first-arm stage 2 and rebase-onto/replayed semantics are pinned.
3. Current/incoming/both each change exactly one verified envelope, produce
   one undo node and round-trip through undo/redo byte-for-byte.
4. Dirty result bytes/undo/journal survive open, cancellation, leave,
   stale callbacks, dirty close and workspace restore. Sources never save.
5. Stale identity/revision tests visibly refuse with zero result edits;
   status-refresh-only tests continue to permit valid choices.
6. Three-pane, one-source and compact layouts pass both sides of each
   breakpoint; action regions exactly match visible chrome.
7. Every action is keyboard/command-bar reachable and mouse-equivalent;
   FUSS search, existing bindings and Insert typing remain unchanged.
8. All four named foreground Git workflows pass real-repository fixtures;
   background completion and moved-focus cases never steal focus.
9. No implicit save/stage/commit/continue; dirty-result Stage cannot stage
   an older disk version without an explicit user decision.
10. Clang/GCC full and stripped builds, focused unit/script/PTY/sanitizer,
    5,000-iteration bounded fuzz and existing relevant regressions pass.
11. Performance evidence separates warm navigation from loading/apply;
    100 lifecycle cycles show no retained session resources, and no new
    synchronous Git/full-file scan occurs on cursor/draw paths.
12. Manual and handoff distinguish implemented, unsupported and outstanding
    evidence. No current-code CI/soak success is inferred from an older SHA.

## Implementation checkpoint — 2026-10-21

Delivery step 1 is implemented: S59.8-A1, pure index/marker parsers,
byte-preserving choice extraction, resumable unique stage-arm matching and
an immutable-OID entry point on the existing asynchronous blob transport.
Clang/GCC/stripped builds and focused unit/sanitizer/fuzz evidence are in
`HANDOFF.md`. Extraction is not editor application: undo, stale identity,
snapshot ownership, all workspace UI and foreground integration remain
outstanding. Existing refusal commands are still the shipped behavior.

User follow-up order: complete this sprint detour, investigate basic FUSS
commit lag, then evaluate ordinary file-diff layout against VS Code and
other open-source editors. Do not silently change the ordinary diff as
part of this merge workspace implementation.
