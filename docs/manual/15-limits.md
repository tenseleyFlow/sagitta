# Limits, deliberate non-goals, and workarounds

This page distinguishes permanent 1.0 design boundaries from bugs. Audit
deferrals, when any exist, are copied here in the same user-facing words as
the release-notes ledger. The current audit ledger has no deferred or wontfix
finding.

## Text and terminal

- yew performs no Unicode normalization. Canonically equivalent byte strings
  remain distinct. Use an external normalization tool when a project requires
  one.
- Bidirectional/right-to-left layout is not implemented. Bytes remain safe,
  but visual order is not a bidi editor. Use a bidi-capable editor for those
  documents.
- Ambiguous-width characters follow the configured terminal-width policy;
  terminals that disagree can still display a shifted cell. Set
  `ambiguous_wide` to match the terminal.
- OSC 52 clipboard integration is write-only. Reading uses a platform
  clipboard subprocess; configure one explicitly when auto-detection fails.

## Search and rendering

- Regex lookaround, backreferences, recursion, and canonical-equivalence
  matching are absent so matching remains bounded and linear-time. Restructure
  the search or use an external specialized tool.
- `&` in a replacement is a literal ampersand, not the whole match. Use a
  capture such as `\1` explicitly.
- Rendered Markdown is not an editor mode. Markdown receives syntax
  highlighting and embedded-language support; preview through an external
  renderer.
- There is no interactive terminal emulator buffer. `:!!` hands the real
  terminal to a child and repaints afterward; `:!` captures output.

## Completion and workspace

- LSP snippets are inserted as plain text. There is no snippet-expansion or
  placeholder-jump mode.
- There is no filesystem watcher. Explicit refresh, save, focus return, and
  existing idle discovery paths update state; externally churned trees may
  require refresh.
- Fuzzy matching folds ASCII case only. Non-ASCII names must be typed with
  matching case.
- `*` is an alias of `+` in address arithmetic rather than a separate
  multiplication-like address operator.

## Extensibility and automation

- Fletch plugins share the editor process and VM. Capability prompts limit
  four I/O surfaces; they are not a memory, CPU, or buffer-read sandbox.
- Package installation has no central registry, signing, dependency
  resolution, build hooks, recursive submodules, or automatic updates. Pin a
  reviewed Git revision and use `doctor` to detect drift.
- `--batch-strict` is reserved and not implemented. Batch already reports
  compile/runtime failures and dirty-buffer warnings; write explicit
  assertions for stricter automation.

Resource caps—buffer size, undo bytes, regex program size, subprocess output,
JSON nodes, syntax depth, job count, and similar bounds—fail with a named
diagnostic rather than silently truncating mutable data. Consult `yew(1)` and
the relevant command error for the exact cap reached.
