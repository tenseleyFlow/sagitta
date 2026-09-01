# Release checklist

This checklist records release evidence that headless CI cannot provide. It
does not replace the unit, PTY, sanitizer, performance, or packaging gates.

## Sprint 60: real macOS terminals

Run the candidate Apple-silicon binary in each of these terminals on a real
interactive session:

- Terminal.app
- iTerm2
- kitty
- Ghostty
- WezTerm

For every terminal, record the date, Yew commit, macOS version, terminal
name/version, `TERM`, `COLORTERM`, and Yew's detected capability tier. Then
verify:

1. Cold start and first paint are complete, stable, and free of stray escape
   bytes.
2. Open, edit, save, quit, and reopen preserve the document byte-for-byte.
3. Arrow keys, Escape, bracketed paste, focus changes, mouse input, and live
   resize remain responsive and deterministic.
4. Unicode width, combining marks, wide glyphs, selection, and cursor
   placement render correctly in both shipped themes.
5. Supported truecolor, synchronized-output, and extended-key capabilities
   activate; unsupported capabilities take the documented fallback path.
6. Normal quit and the intentional crash-path check both restore the terminal
   modes, cursor, mouse tracking, paste mode, and alternate screen.

Terminal.app is expected to exercise fallback behavior rather than truecolor,
kitty keyboard protocol, or synchronized output. A passing CI PTY run is not
evidence that any terminal in this list was exercised; attach the completed
matrix to the Sprint 60 release record.
