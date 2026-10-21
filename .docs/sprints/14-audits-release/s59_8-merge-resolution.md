# Sprint 59.8: Interactive merge resolution — queued

## Prerequisites

Sprint 59.5 owns modal Git operations and transient-tab origin tracking.
Sprint 59.7 owns the read-only file diff projection and immutable-view
command guard. Sprint 53 supplies aligned two-pane diff presentation.
Read the binding decisions, architecture, Fletch and campaign documents
before implementation. This is a queued follow-up, **not shipped behavior**.
Before implementation, amend decision S59-A30's post-1.0 resolution deferral
with this user-authorized scope. Do not reset Sprint 59's independent soak.

## Goals

Resolve ordinary textual Git conflicts using a keyboard-reachable editor
workspace with current/incoming comparison, a clearly identified result,
and per-conflict accept-current/accept-incoming/accept-both actions. Detect
conflicts after foreground Git operations without stealing focus after a
user has moved elsewhere. Preserve unsaved documents, arbitrary bytes and
normal undo/save behavior. Sub-hunk staging remains outside this sprint.

## Deliverables

1. **Conflict snapshot**, new `src/mod/git/conflict.[ch]`, using the
   existing asynchronous callback/blob machinery in `git.[ch]`:

   ```c
   typedef struct GitConflictSnapshot GitConflictSnapshot;
   bool yew_git_conflict_request(Ed *ed, const char *workspace_path);
   void yew_git_conflict_drop(GitConflictSnapshot *snapshot);
   ```

   Own byte-exact path, stage OIDs/modes, source buffers, operation kind,
   original result-buffer ID/generation and worktree identity. Consume
   NUL-delimited index records, never split paths on whitespace/newlines.
   Separate status refresh generations from identity of the conflicting
   content. Bound each blob to the established 16 MiB Git limit.

   | Index stage | Normal merge | Rebase |
   | --- | --- | --- |
   | 1 | common ancestor | common ancestor |
   | 2 / ours | checked-out side | checked-out/rebased-onto side |
   | 3 / theirs | merged-in side | replayed commit |

   **Pitfall:** “ours” during rebase does not mean the user's topic branch.
   Display stage labels plus actual refs/OIDs and operation context. Missing
   stages are explicit add/delete conflicts, not empty successful reads.

2. **Conflict ranges and result ownership**, `conflict.[ch]`,
   `src/mod/git/editor.c`, `src/text/edit.[ch]` consumers. Identify ordinary
   text conflicts from the actual merge result, preserving marker width,
   newline style, source bytes and diff3/zdiff3 base sections. Require
   coherent markers and matching stage identity before offering choices.
   Refuse ambiguous/malformed/binary/rename/submodule conflicts with an
   explicit explanation; leave manual file editing available.

   Result edits belong to a normal editable document, not a read-only
   projection. Reuse the already-open file buffer when possible. Never
   reload or replace unsaved content to construct a merge view. Source
   panes are immutable snapshots. The result's dirty state and close prompt
   follow existing document rules, not transient-output auto-dismissal.

3. **Two-pane workspace and actions**, `src/mod/git/editor.c`,
   `src/ui/draw.c`, `src/ui/region.c`, command registry/runtime bindings:
   show a read-only source comparison alongside a clearly labelled editable
   result; make both current and incoming source sides selectable. Keep
   base context discoverable. Per-conflict action chrome must not become
   document bytes or use completion/shadow acceptance semantics.

   ```c
   CmdStatus yew_git_cmd_conflict_open(CmdCtx *cx);
   CmdStatus yew_git_cmd_conflict_next(CmdCtx *cx);
   CmdStatus yew_git_cmd_conflict_prev(CmdCtx *cx);
   CmdStatus yew_git_cmd_conflict_ours(CmdCtx *cx);
   CmdStatus yew_git_cmd_conflict_theirs(CmdCtx *cx);
   CmdStatus yew_git_cmd_conflict_both(CmdCtx *cx);
   ```

   Define key bindings in the sprint before landing them; expose actions
   through the discoverable action picker and command bar. Mouse buttons
   invoke these same commands. Keep all source sides reachable at narrow
   widths without invisible buttons or terminal-width assumptions.

   **Pitfall:** each acceptance is one undo transaction through EditCtx.
   Accept-both preserves current-then-incoming bytes and a documented
   separator policy, never concatenating tokens across a missing newline.
   Recompute/revalidate conflict ranges after edits or undo. Refuse stale
   actions after external index/worktree changes rather than guessing.

4. **Detection and completion**, `src/mod/git/fussmode.c`: after a Git
   operation initiated here ends in conflicts, open the conflict chooser
   only if its stable originating tab/window still owns focus and no newer
   modal is active. Otherwise post a notification with an explicit open
   action. Background status discovery never steals focus. A sole conflict
   can open directly from that foreground flow. Save and stage remain
   separate, explicit actions; accepting the last hunk does not stage,
   commit, continue a rebase or complete a merge automatically.

5. **Defer:** sub-hunk staging and non-text conflict resolution. Existing
   conflict commands continue to error until their complete actions and
   tests land; unsupported variants name their scope rather than acting
   as successful no-ops. Update `docs/manual/13-git.md` and `.docs/HANDOFF.md`
   with implemented versus unsupported cases and precise evidence.

## Testing Strategy

- Pure parser/range tests: normal/diff3/zdiff3, custom marker widths,
  missing newline, CRLF, Unicode/invalid UTF-8, nested/ambiguous markers,
  absent stages, repeated identical text, and bounded malformed input.
- Real temporary Git fixtures: merge, pull, rebase and cherry-pick;
  current/incoming labels, stage identity, unsaved-buffer preservation,
  independent external changes, accept/undo/redo and explicit save/stage.
- PTY: keyboard and mouse reach the same actions; action chrome never
  enters result bytes; selection/copy, scrolling, resizing, Escape,
  dirty-close prompts, and focus changes while snapshots load.
- ASan/UBSan and short fixed-seed fuzz smoke; no additional unbounded
  campaign. Keep two build jobs maximum. Test before integrating branchwork.
- Existing diff, FUSS, command-output and Git modal suites stay green.

## Definition of Done

1. Every supported textual conflict has accurate side labels and ranges.
2. Current/incoming/both acceptance is byte-checked and individually undoable.
3. Dirty documents survive opening, cancellation, stale callbacks and close.
4. Stale index/worktree data causes a visible refusal, not a wrong edit.
5. Normal merge and rebase tests verify their differing stage semantics.
6. Foreground automatic opening and background non-stealing are tested.
7. No implicit stage/commit/continue or destructive fallback exists.
8. Narrow, Unicode, ASCII and monochrome terminal cases remain usable.
9. Focused unit/script/PTY/sanitizer checks and bounded fuzz smoke pass.
10. User manual and handoff report actual shipped scope and limitations.
