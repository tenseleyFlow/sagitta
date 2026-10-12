# Files, bytes, saves, and recovery

yew edits bytes and never silently changes their representation. Text files
retain LF or CRLF endings, a UTF-8 BOM is stripped for editing and restored on
save, and invalid UTF-8 bytes remain round-trippable. A NUL in the binary
probe window marks a buffer binary; binary buffers keep byte-safe rendering
and do not start syntax, symbol, LSP, or AI work.

This batch example reads the current buffer without normalizing it:

```batch
import buf
import io

io.print(buf.text(buf.current()))
```
```output

```

The documentation runner supplies an empty scratch buffer here, so `io.print`
emits one newline. The same call over an opened file returns its exact editor
bytes.

## Opening and buffers

`:edit PATH` opens a path. `:new` creates an untitled scratch tab, as does the
`+` button in the tab strip. A path outside the workspace is valid: it appears
as a normal tab but is absent from workspace-relative FUSS and does not gain
workspace discovery benefits.

Every pane owns a window and viewport; buffers are shared objects. Opening an
already-open path focuses the existing buffer instead of manufacturing a
second copy with divergent bytes.

## Saving without data loss

`:write` or `s` uses the buffer's current path. `:write PATH` is Save As.
The default `save.strategy = "auto"` uses replacement-style atomic saving
for ordinary single-link files, but preserves symlink and hard-link identity
with the in-place path. Both routes sync data and directory metadata in their
required order. A failure leaves either the old complete file or the new
complete file; it does not report success for a partial write.

`save.check_disk = "mtime"` refuses to overwrite a file whose on-disk
identity changed after load. `content` compares bytes (up to
`save.check_disk_max`, 8 MiB by default), and `off` deliberately disables the
check. Resolve the conflict or use the explicit forced-save command; do not
train yourself to bypass it routinely.

## Journal and backups

Edits append recovery records under `$XDG_STATE_HOME/yew/journal/`. Records
carry checksums and replay only against the pinned base identity. On a clean
save or deliberate discard, obsolete journal state is retired. A crash leaves
enough information for the next open to offer recovery without modifying the
file before the user chooses.

In-place saves rotate backups when `save.backup_keep` is nonzero. The default
keeps one. `save.backup_dir = ""` selects yew's state-directory location;
setting a directory moves backups there.

## Command output is not your file

`:!command` opens job output in an editor-owned output buffer. It does not
replace or save the originating file. Escape or `:q` dismisses that output
and returns to the origin. A filter command is different: when a range is
supplied, successful stdout replaces that range as one undoable edit; failure
leaves the buffer unchanged. See [executing commands](06-execute.md).
