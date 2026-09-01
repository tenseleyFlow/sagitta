# Git-aware Editing

Yew compares the live buffer with the file's stage-0 index blob. It does not
compare the buffer with the worktree file or `HEAD`. This keeps gutter signs
accurate before a save and after staging part of an unsaved edit. An untracked
file has an empty base, so every line is an addition. During a conflict, where
stage 0 does not exist, yew compares with `HEAD` and uses conflict-colored
signs.

The Git editor surface requires the `fuss` module and a `git` executable. The
Git statusline segment is absent, without an error badge, when the current
workspace is not a Git repository.

## FUSS tree controls

From L mode, `f` opens the FUSS workspace drawer. Arrows navigate the visible
tree, Space expands or collapses a directory, Enter opens the selected file,
and Escape returns to the editor. Page Up, Page Down, Home, End, and `Ctrl-R`
remain direct navigation and refresh keys.

Typing an unmodified printable filename character immediately starts fuzzy
type-to-jump; there is no separate arm key. Characters typed within 500 ms
accumulate, Backspace shortens the pattern, and the footer shows the live
pattern. Search candidates are only the rows currently visible in the
flattened tree, so a descendant cannot match until its directory is expanded.

Printable actions use a `Ctrl-G` prefix so bare letters remain available for
search:

| Chord | Action | Chord | Action |
|---|---|---|---|
| `C-g a` / `C-g u` | stage / unstage path | `C-g S` / `C-g U` | stage / unstage all |
| `C-g m` / `C-g M` | commit / amend | `C-g p` / `C-g l` / `C-g f` | push / pull / fetch |
| `C-g d` / `C-g D` | textual / side-by-side diff | `C-g s` / `C-g w` | status / blame |
| `C-g h` / `C-g L` | history / reflog | `C-g c` | view selected path |
| `C-g b` / `C-g n` / `C-g R` | switch / create / delete branch | `C-g G` / `C-g O` | merge / reset |
| `C-g I` | interactive rebase | `C-g y` / `C-g v` | cherry-pick / revert |
| `C-g z` / `C-g Z` | stash push / pop | `C-g t` | tag |
| `C-g x` | discard path | `C-g r` / `C-g N` | delete / rename path |
| `C-g g` | open directory as a tab group | `C-g T` / `C-g .` | all-files / hidden toggles |
| `C-g q` | leave F mode | `C-g ?` | expand the current message |

`Ctrl-W s` and `Ctrl-W v` open the selected file in a horizontal or vertical
split. Escape is always the shortest leave action.

## Gutter signs and hunk movement

The two-cell sign column shows changes relative to the index:

| Change | Sign | Meaning |
|---|---|---|
| Addition | `▎ ` | Buffer lines added after the indexed version. |
| Modification | `▎ ` | Indexed lines changed in the buffer. |
| Deletion | `▔ ` | Indexed lines deleted above this buffer line. |
| Deletion at end of file | `▁ ` | Indexed lines deleted after the final buffer line. |
| Conflict | `▎ ` | The comparison fell back to `HEAD`. |
| Truncated diff | `~ ` | The diff exceeded its bounded edit-distance budget. |

An LSP diagnostic wins the first sign cell when a diagnostic and Git change
share a line; the Git sign moves to the second cell.

In L, W, and B modes, `Alt-Up` runs `ed.git.hunk.prev` and `Alt-Down` runs
`ed.git.hunk.next`. The motions wrap with a message and accept a repeat count.
The named commands `ed.git.hunk.first` and `ed.git.hunk.last` jump to the ends
of the hunk list.

Files over 200,000 lines or 16 MiB do not receive signs. The statusline reports
`git: file too large for signs` instead of showing a partial result.

## Stage and discard

`ed.git.hunk.stage` stages the current hunk directly from the buffer. It sends
a byte-exact patch with three lines of context to `git apply --cached -`.
Staging changes the index only: it does not save or modify the worktree file.
With a selection active, the command stages every intersecting whole hunk as
one patch.

Staging from an unsaved buffer is supported. The first such stage in a session
shows:

```text
staged from an unsaved buffer — the file on disk is still the old version
```

Afterward, the buffer, worktree file, and index may all contain different
bytes. Save only when the worktree should receive the buffer contents.

`ed.git.hunk.discard` replaces the current hunk in the buffer with its indexed
version. It is one editor undo transaction and never runs `git checkout` or
modifies the worktree. `ed.edit.undo` restores the discarded buffer text. With
a selection active, all intersecting whole hunks are discarded in that one
transaction.

## Inline blame

`ed.git.blame.toggle` places blame text after each visible logical line:

```text
  ▏ jane, 3 weeks ago · fix the CRLF case
```

The annotation is virtual text: it cannot be selected, clicked, or addressed
by the cursor, and it does not change the buffer. Yew omits it when fewer than
24 cells remain on a row and draws it only on the final display row of a
wrapped logical line.

Blame is requested lazily for the viewport and includes unsaved buffer
contents. Unsaved lines appear as `(uncommitted)`. Editing invalidates the
cache; stale text remains dim while the replacement request is in flight so
scrolling and typing do not flicker.

## Side-by-side diff

In F mode, press `Ctrl-G D` on a file, or run `ed.git.diff.view`, to open a
vertical side-by-side diff. The indexed version is on the left and the live
buffer is on the right. Both panes are read-only scratch buffers.

Filler rows marked `~` keep equal content aligned after additions and
deletions. Scrolling either pane scrolls its partner to the corresponding diff
row; cursor movement remains independent. Modified line pairs receive
grapheme-safe intraline highlighting when they are short enough. Return to the
real buffer to edit, stage, or discard.

## Statusline badges

The statusline reads the asynchronous Git cache and never starts a process
while drawing. It keeps the last known value visible during a refresh.

| Repository state | Example |
|---|---|
| Branch | `⎇ trunk` |
| Ahead or behind | `⎇ trunk ↑2 ↓1` |
| Detached HEAD | `⎇ (a1b2c3d)` |
| No commits | `⎇ (no commits)` |
| Merge | `⎇ trunk\|MERGING` |
| Rebase | `⎇ trunk\|REBASE 3/7` |
| Cherry-pick, revert, or bisect | `\|CHERRY-PICKING`, `\|REVERTING`, or `\|BISECTING` |
| Conflicts | `⚑N` appended to the current badge |

Zero ahead and behind counts are omitted. A repository without an upstream
shows only the branch. Set `git.ascii_glyphs` to replace `⎇` with `git:`.

## Open a directory as a group

In F mode, press `Ctrl-G g` on a directory, or run `ed.group.from_dir` with a
path. Yew walks recursively by default, skips `.git`, respects ignore rules,
and orders members deterministically. An already-open tab is adopted rather
than duplicated. New tabs remain deferred; yew reads only the first focused
file.

A directory with more than 200 candidate members opens the group picker
instead of silently truncating the group or opening thousands of tabs. An
empty directory creates no group.

## Byte-honest filter behavior

Yew compares raw buffer bytes with raw index-blob bytes. It does not apply
`core.autocrlf`, `.gitattributes` clean or smudge filters, or textconv before
diffing. A repository that transforms content with any of these mechanisms
may therefore show a whole-file difference when the buffer and index bytes
really differ. Yew does not normalize that difference away because doing so
would make signs and patches describe bytes other than those in the buffer or
index.

## Refresh policy

Yew uses no filesystem-watcher backend for Git state. It refreshes on demand
through the 500 ms status-cache TTL, invalidates immediately when the terminal
regains focus, invalidates after every completed job and buffer save, and
refreshes explicitly with F-mode `Ctrl-R` or `ed.git.refresh`. The policy
covers commands run through yew and changes made in another terminal without
adding platform-specific watcher behavior.

## Post-1.0 scope

Selections operate on whole intersecting hunks. Staging a selection finer
than a hunk, like Git's `add -p` edit mode, is post-1.0.

Conflict-marker navigation and three-way take-ours/take-theirs editing are
also outside 1.0. Their registered commands report:

```text
conflict resolution is not a 1.0 feature (F mode's diff and your editor are)
```

F mode still shows conflicted files, and the editor gutter shows their changes
against `HEAD`.
