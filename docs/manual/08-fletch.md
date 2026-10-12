# Configuring yew with Fletch

Fletch is yew's configuration, macro, syntax-definition, plugin, and batch
language. It has seven forms—literals, lists, maps, calls, blocks,
function literals, and motion literals—and one parser/compiler/VM. Recordings
are Fletch source rather than opaque key bytes.

The normal configuration order is shipped `runtime/init.fl`, user
`$XDG_CONFIG_HOME/yew/init.fl`, then a trusted workspace `.yew.fl`. Later
origins layer above earlier ones. Reload is transactional: a parse or runtime
error tears down only the failed source and leaves the previous live layer in
place.

```fletch
set({
    tabwidth: 4,
    expandtab: true,
    wrap: false,
})

bind("L", "g h", "ed.move.buf.home")
on("buf.saved", fn(ev) { log.info("saved") })
```

`set()` uses the same typed descriptor table as command-line option changes.
`bind()` targets a registered command or closure. `on()` subscribes to the
closed event inventory. Unknown names and wrong types are errors, never
ignored configuration.

## Workspace trust

A workspace `.yew.fl` is code and requires content-based trust. Trust binds
the canonical path, file identity, and content hash; moving through a symlink,
replacing the inode, or changing bytes prompts again. Batch mode cannot
prompt: use `--trust-workspace` deliberately or `--no-workspace-config`.
Workspace configuration cannot weaken the user's AI redaction policy.

## Built-in options and defaults

Scopes are `buffer`, `window`, and `global`. Module-owned rows are present in
the descriptor inventory even when their module is omitted; setting one then
fails with a module diagnostic.

| Option | Default | Scope |
|---|---:|---|
| `tabwidth` | `4` | buffer |
| `expandtab` | `false` | buffer |
| `autoindent` | `true` | buffer |
| `autopair` | `true` | buffer |
| `wrap` | `false` | window |
| `scrolloff` | `3` | window |
| `number` | `"both"` | window |
| `statusline.column` | `"gcol"` | global |
| `errorbells` | `false` | global |
| `ambiguous_wide` | `false` | global |
| `subword` | `false` | buffer |
| `fortran_form` | `"auto"` | buffer |
| `chord_timeout_ms` | `500` | global |
| `undo.break_on_newline` | `true` | buffer |
| `undo.bytes_max` | `67108864` | buffer |
| `undo.min_nodes` | `200` | buffer |
| `undo.persist_bytes_max` | `16777216` | global |
| `registers.ring_depth` | `32` | global |
| `registers.ring_bytes_max` | `8388608` | global |
| `registers.clip_read_max` | `67108864` | global |
| `clipboard.sync` | `"yank"` | global |
| `search.ignorecase` | `false` | global |
| `search.smartcase` | `true` | global |
| `hooks.error_limit` | `5` | global |
| `save.strategy` | `"auto"` | buffer |
| `save.check_disk` | `"mtime"` | buffer |
| `save.check_disk_max` | `8388608` | global |
| `save.backup_keep` | `1` | global |
| `save.backup_dir` | `""` | global |
| `plug.error_limit` | `5` | global |
| `plug.verify_on_load` | `true` | global |
| `theme` | `"quiver-dark"` | global |
| `theme_auto` | `false` | global |
| `macro.dir` | `""` | global |
| `shell.complete_help` | `"native"` | global |
| `shell.complete_fish` | `"auto"` | global |
| `shell.suggest_history` | `"all"` | global |
| `shell.session` | `"persistent"` | global |
| `shadow.enable` | `true` | global |
| `shadow.providers` | `"index lsp ai"` | global |
| `shadow.max_lines` | `8` | global |
| `shadow.midline` | `false` | global |
| `shadow.lsp_debounce_ms` | `120` | global |
| `shadow.ai_debounce_ms` | `350` | global |
| `compl.auto_trigger` | `false` | global |
| `compl.trigger_chars` | `". -> ::"` | global |
| `lsp.open_in` | `"here"` | global |
| `git.ascii_glyphs` | `false` | global |
| `ai.enable` | `false` | global |
| `ai.backend` | `""` | global |
| `ai.default_workspace` | `"ask"` | global |
| `ai.context_bytes` | `4096` | global |
| `ai.context_prefix_pct` | `75` | global |
| `ai.max_tokens` | `256` | global |
| `ai.max_lines` | `8` | global |
| `ai.temperature` | `10` | global |
| `ai.frame_ms` | `33` | global |
| `ai.fim` | `"auto"` | global |
| `ai.on_redact` | `"block"` | global |
| `ai.deny_replace` | `false` | global |
| `ai.exclude_replace` | `false` | global |
| `ai.exclude_paths` | `[]` | global |
| `ai.badge` | `"on"` | global |
| `ai.badge_host_max` | `20` | global |
| `ai.debug_bodies` | `false` | global |
| `ai.allow_plain_remote` | `false` | global |
| `ai.key_cache` | `true` | global |
| `ai.connect_timeout_ms` | `2000` | global |
| `ai.first_byte_timeout_ms` | `10000` | global |
| `ai.stream_idle_timeout_ms` | `20000` | global |
| `ai.total_timeout_ms` | `120000` | global |
| `ai.keepalive_ms` | `30000` | global |
| `ai.backoff_max_ms` | `60000` | global |

The command-line interpreter is `yew fl`; see `yew-fl(1)` for modules,
capabilities, REPL commands, bytecode inspection, and exit status. Headless
editing uses `yew --batch`, documented in [the batch guide](../batch.md).
