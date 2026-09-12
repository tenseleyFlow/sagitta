# yew

yew is a speed-first, bespoke-first modal terminal text editor with an
arrow-key focus. Its scripting and macro language, **Fletch** (`*.fl`), is
designed so recorded edits are readable programs and programs can drive the
editor. The implementation is C11 using only the C standard library and POSIX.

yew is currently **pre-1.0**. Its terminal stack, deterministic renderer,
PTY acceptance harness, piece-tree text engine, byte-exact file loading,
durable save paths, crash journal, modal editing, Fletch configuration and
macros, batch interface, and lexical syntax runtime are in place. Remaining
future surfaces fail explicitly with a message naming the sprint that owns
them.

## Your first Fletch program

```sh
printf 'import io\nio.print("hello, world")\n' > hello.fl
yew fl hello.fl                              # hello, world
yew fl -e 'import io; io.print("hello, world")'
yew fl                                       # the prompt; :quit to leave
```

`import io` is not optional: Fletch's builtins are imported rather than
ambient (spec §11), so a program says what it reaches for. `yew fl --help`
lists the rest of the surface — `-c` to compile only, `--dump-ast`,
`--dump-bytecode`, `--list-natives`, and `--caps`/`--origin` to run a script
with fewer grants than the shell has.

## Native syntax highlighting

Yew ships exactly 48 lexical syntax modes. List the built-in pack without
loading user configuration or definitions:

```sh
yew --clean syn list
```

Built-ins are pure Fletch data under `runtime/syntax/*.fl`; installed builds
place them under `share/yew/runtime/syntax`. Add personal definitions as
`$XDG_CONFIG_HOME/yew/syntax/*.fl`. Lexical highlighting, including the
supported Markdown, HTML, Make, shell, and template embedding, needs no
plugin, language server, external process, or rebuild. Symbol intelligence
and LSP features are a separate campaign and are not required for native
coloring.

## Git-aware editing

With the `fuss` module enabled, yew shows buffer-vs-index hunk signs, supports
undoable hunk discard and dirty-buffer staging, provides inline blame and a
side-by-side diff, and opens F-mode directories as tab groups. See the
[Git-aware editing guide](.docs/git-editor.md) for the commands, statusline
badges, refresh policy, and byte-honest filter behavior.

## Mouse

The mouse is an accelerator, never a requirement: every row of every menu
below is a registry command with a keyboard route, and `t m`
(`ed.ui.context_menu`) opens the same menu for whatever the keyboard is
focused on. `ed.mouse.disable` turns reporting off entirely.

**Right-click — or ctrl+left-click, for hardware with one button — opens a
context menu at the pointer, on every surface:**

| Under the pointer | Rows |
|---|---|
| Document text or gutter | Cut, Copy, Paste, Delete, Select All · Undo, Redo · Split Right, Split Below, Close Pane · Save, Reload · *(language server, when one is attached to that buffer)* Go to Definition, Find References, Rename, Hover · *(git, inside a repository)* Toggle Blame, Diff · Command Palette, Find File, Go to Line, Toggle Wrap |
| Pane border | Close Pane, Grow, Shrink · Focus Next |
| Tab entry | Close Tab, Close Other Tabs · Copy Path, Remove from Group · New Tab, Open in Split Right, Open in Split Below |
| Group entry | Edit Group, Rename Group · Close Group, Dissolve Group |
| Tab strip tail, or the bare backdrop | New Tab, Open File, New Group · Command Palette, Find Buffer |
| Statusline / message row | Command Palette, Go to Line · Toggle Wrap, Line Numbers (cycles none → abs → rel → hybrid) · Disable Mouse |
| F-mode file row | Open, Open in Split Right, Open in Split Below, Preview · Stage, Unstage, Discard · Diff, Blame · Rename, Delete |
| F-mode directory row | Open as Group, Expand/Collapse · Stage All Below, Unstage All Below |
| F-mode header, blank drawer or backdrop | Refresh, Commit · Push, Pull, Fetch · Status, History · Leave FUSS |
| Picker row | Open, Open in Split Right, Open in Split Below · Close |
| Completion row | Accept, Toggle Docs · Cancel |
| Hover/signature panel, group picker | Close / Cancel |

The document menu's `Cut`, `Copy` and `Paste` are the *system* clipboard —
the same `ed.clip.cut` / `ed.clip.copy` / `ed.clip.paste` that ctrl+X /
ctrl+C / ctrl+V run — so what you copy is there for the next window over.
`Cut` and `Copy` need a highlight; `Paste` says so when the clipboard turns
out to be empty.

Rows that do not apply right now are greyed rather than removed, so a menu
keeps its shape between two right-clicks on the same thing. A whole *section*
does disappear when its feature is unavailable — no language server attached
to that buffer, no git repository, or a build without the module — rather than
standing as rows that can never be used. On a short terminal the menu sheds
its lowest-priority rows instead of refusing to open.

The pointer highlights rows as it moves over an open menu; arrow keys, Home,
End, Enter and Esc drive the same menu from the keyboard. While a menu is
open yew switches from button-event reporting (DEC private mode 1002) to
any-motion reporting (1003), then restores 1002 on close. Suspend and exit
disable both protocols.

**Other gestures:** click to place the cursor, drag to select, double-click
for a word and triple-click for a line (Alt for whitespace-delimited words
and whole display lines); click a tab to switch and drag it to reorder;
drag a pane border to resize; wheel to scroll the pane, the picker, or the
F-mode tree; single click selects an F-mode row and double-click opens it.

## Build

```sh
make
make test
make torture
make MODULES="lsp ai"
```

The default build enables the `lsp`, `ai`, `fuss`, and `plugins` compile-time
modules. Build products are written under `build/`; the editor executable is
`build/yew`.

Unicode behavior is generated from the checked-in Unicode 16.0.0 data under
`ucd/`. The generated `src/unicode/tables.c` is also committed deliberately:
a fresh clone builds offline with only a C compiler, while `make
unicode-tables` reproducibly regenerates the file for review and CI drift
checks. No network access, Python, locale data, or platform `wcwidth` is part
of the build or rendering contract.

## Data-safety contract

Files are loaded with ordinary full-read POSIX I/O—never `mmap`—and remain
byte-exact in memory apart from a detected UTF-8 BOM, which is restored on
save. CRLF, mixed line endings, invalid UTF-8, and binary bytes round-trip
without normalization.

Ordinary saves write and `fsync` a same-directory temporary before rename and
directory `fsync`. Symlinks, hardlinks, and unwritable-directory cases use a
fsynced state-directory backup plus in-place preservation. Per-buffer crash
journals are versioned, checksummed, append-only recovery logs. `make torture`
faults every save syscall boundary and runs external `SIGKILL` campaigns to
prove the destination is always old or new—or recoverable from journal and
backup—never silently corrupted.

## Terminal environment

- `YEW_TTY_PROBE=0` disables live capability queries.
- `YEW_PROBE_TIMEOUT_MS` changes the default 50 ms probe deadline for
  high-latency links. Probing races startup and never delays first paint.
- `YEW_TRUECOLOR=0` or `1` overrides truecolor environment detection.
- `YEW_CLIPBOARD=auto|osc52|wl|xclip|xsel|pb|none` selects the system
  clipboard backend. `cmd:<write-argv>[|<read-argv>]` runs a custom command
  directly without a shell.
- `YEW_OSC52=off|plain|tmux|screen` controls OSC 52 wrapping. `plain` is the
  escape hatch when a multiplexer consumes OSC 52 itself. OSC 52 is
  write-only; clipboard reads use a local subprocess.
- For tmux passthrough, use `set -g allow-passthrough on` on tmux 3.3 or
  newer. If tmux owns clipboard forwarding through `set -g set-clipboard on`
  instead, use `YEW_OSC52=plain` so tmux consumes the unwrapped sequence.
- `YEW_CLIPBOARD_TARGET=c|p|cp` selects the OSC 52 target,
  `YEW_OSC52_MAX` caps encoded payload size at 100,000 bytes by default, and
  `YEW_CLIPBOARD_TIMEOUT_MS` changes the 1,000 ms subprocess deadline.

The terminal lifecycle is terminfo-free. Raw mode restoration is armed for
normal close, `atexit`, internal-error reports, and fatal signals.

## Sister projects

- [Cgfried](https://github.com/tenseleyFlow/Cgfried)
- [fuss](https://github.com/FortranGoingOnForty/fuss)
- [facsimile](https://github.com/FortranGoingOnForty/facsimile)

yew is licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`).
