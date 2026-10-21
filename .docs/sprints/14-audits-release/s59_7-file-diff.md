# Sprint 59.7: FUSS file diff view

## Prerequisites

Sprints 53 (Git row styles, grapheme intraline differ, hunk navigation)
and 59.5 (read-only transient tabs, safe Escape/close ownership) landed.
Binding: `00-decisions.md`, `01-architecture.md`, `02-fletch.md` and
`sprints/index.md`. This user-directed detour does not reset the frozen
Sprint 59 soak or change the numbered campaign.

## Goals

Replace FUSS Alt+d's raw patch transcript with a full-context, single-pane
file comparison: deleted rows red, inserted rows green, unchanged source
neutral, old/new line numbers and non-color +/- markers. Preserve source
documents and tab groups. Interactive conflict resolution remains a
separate follow-up, Sprint 59.8; this sprint does not accept, save or stage
merge choices. The existing document side-by-side command stays available.

## Deliverables

1. **Bounded projection**, `src/mod/git/diffview.[ch]`:

   ```c
   typedef struct DiffUnified DiffUnified;
   void yew_diff_unified_drop(DiffUnified *view);
   DiffUnifiedResult yew_diff_unified_build(DiffUnified *view,
                                           const u8 *patch, size_t len);
   ```

   A zero-initialized model owns source bytes and two source-row vectors
   (zero-based; -1 means absent). Parse Git's full-context single-file
   unified patch, not arbitrary terminal output. Strip patch framing only;
   source bytes, including Unicode, CR and leading +/- characters, survive.
   Return explicit empty, binary, metadata-only, invalid and limit results.
   Preserve no-final-newline metadata. Bound input to 16 MiB and projection
   to 200,000 rows. Validate range/count arithmetic and complete hunks.

   | Patch row | Before | After | Style |
   | --- | --- | --- | --- |
   | context | next | next | neutral |
   | `-` | next | absent | deletion |
   | `+` | absent | next | addition |
   | no-newline marker | no row | no row | header notice |

   **Pitfall:** an incomplete or multi-file patch must never masquerade as
   a complete file. Failure leaves the previous model untouched. Git is
   the differ; do not add a dependency or implement another diff algorithm.

2. **Asynchronous FUSS ownership**, `src/mod/git/fussmode.c` and
   `src/mod/git/git.c`: open a named loading transient tab immediately;
   use the existing callback subprocess collector with stable window and
   buffer IDs. Update only a still-live matching view. Closing while the
   command runs never reopens it or steals focus. No document buffers are
   replaced. Failures become explicit text in that same temporary tab.

   Request no external diff/textconv, no color, no renames, fixed patch
   indicators and full context. Tracked files compare HEAD to saved disk;
   incoming files retain merge-base-to-upstream semantics. Untracked and
   unborn-repository files compare empty to saved disk via `--no-index`;
   exit 1 is successful only for that mode. Paths remain literal argv
   after `--`, relative to the workspace root. Conflicted paths use a
   read-only HEAD comparison, not combined-patch parsing or merge editing.

   **Pitfall:** stdout and stderr are separate, capped, noninteractive
   streams. Never parse a capped, timed-out or failed collector as a diff.
   Label saved-worktree semantics; do not imply unsaved edits are included.

3. **File presentation**, `src/ui/win.h`, `src/ui/gutter.c`,
   `src/ui/draw.c`, `src/edit/ed.c`: view-owned old/new row metadata,
   source gutters independent of relative-number preferences, and existing
   `git.diff.add` / `git.diff.del` roles. +/- remains readable without
   color. Free metadata on buffer replacement/window destruction and clone
   it when splitting the same view. Ordinary gutters remain unchanged.
   Use existing editor motion/search, and existing hunk commands to move
   between contiguous change runs in the unified projection.

4. **Documentation and evidence**: update `docs/manual/12-git.md` if
   present (otherwise the existing Git chapter), and `.docs/HANDOFF.md`.
   Record exact focused test results; do not claim a new hosted CI pass.

5. **Defer:** interactive merge editing to Sprint 59.8. Required safety
   contract: index stages 1/2/3 and explicit rebase-aware side labels;
   per-conflict choices as undoable edits in a result document; stale
   index/worktree detection; cancellation and separate explicit save/stage;
   conflict detection must not steal focus or modify files automatically.
   Existing unsupported resolution commands continue to error explicitly.

## Testing Strategy

- Pure units: add/delete/replace/context, Unicode and CRLF, absent final
  LF, binary/mode-only/empty, malformed numbers/ranges, incomplete/multiple
  hunks/files, input/row limits and atomic failure.
- View units: clone/release and gutter state; hunk traversal, first/last and
  wrapping. Existing side-by-side row-map/intraline/scroll tests stay green.
- PTY: real Git fixtures, source rows and colors, untracked/deleted/binary,
  read-only behavior, Escape and normal-close restoration, loading-close
  safety, light/monochrome variants. Update reviewed affected goldens only.
- Bounded parser mutation/property coverage in `tests/fuzz/fuzz_git_diff.c`;
  run a short fixed-seed smoke, not another soak. ASan/UBSan focused units.
- Resource/performance: at most two compiler jobs; parsing linear in input,
  renderer linear in visible rows, no subprocess or allocation per painted
  row. Large input rejects before allocation. Independent soaks untouched.
- Manual/doc-sync checks, warning-clean C11 build, focused Git/FUSS
  regression tests. No install, push or unrelated worktree changes.

## Definition of Done

1. Alt+d opens a separately titled, read-only full-context comparison.
2. Old/new source numbers and +/- markers agree with projection rows.
3. Red/green roles distinguish removals/additions in dark and light themes;
   monochrome still distinguishes them without color.
4. Tracked/untracked/deleted/incoming comparisons have explicit semantics;
   binary and unsupported output give an honest explanatory view.
5. Escape and normal tab close return to the original documents unchanged.
6. A late callback after dismissal cannot reopen or overwrite another tab.
7. Existing document diff and Git modal workflows retain their tests.
8. Focused units, PTYs, bounded fuzz smoke and sanitizer checks pass.
9. Manual and handoff name the implemented surface and merge follow-up.
10. Commit in coherent chunks. No feature removal, gate relaxation or soak
    restart to deliver this UI change.
