# Completion specs

A completion spec tells yew what a command accepts, so that `:!wolf bu<Tab>`
completes `wolf build `, `:!git remote a<Tab>` offers `add`, and
`:!wolf build --emit=<Tab>` offers exactly `wir obj bin llvm-ir`.

A spec is **data**, not code: one Fletch data document (the same literal
syntax as `runtime/syntax/*.fl`), one file per command. Comments start with
`#`. The first line of every shipped spec names the tool version it was
written against, because a spec is a description of somebody else's
program and goes stale when that program changes.

## Where specs live

yew looks for the spec of a command by the command word's **basename**
(`/usr/bin/git` and `git` both find `git.fl`), first hit wins:

1. `$XDG_CONFIG_HOME/yew/completions/<command>.fl` (usually
   `~/.config/yew/completions/`) -- yours.
2. `completions/<command>.fl` in the shipped runtime (this directory).
3. A shipped spec whose `command` list names the command.

A file of yours **replaces** the shipped one whole; the two are never
merged. To extend `git`, copy `git.fl` into your directory and edit the
copy. yew re-reads your file when it changes (checked at most once per
prompt). A file that fails to validate is reported once, naming the file
and the key, and the command then completes as if it had no spec.

Specs are only read from these two places. A spec's generators run
programs, so yew never loads one from a project directory you merely
opened.

## The format

The top-level map is the command's **root node**, so it takes every node
key below as well as these:

| Key | Type | Required | Meaning |
|---|---|---|---|
| `completion` | int | yes | schema version; must be `1` |
| `command` | string or list of strings | yes | every command name this spec answers for |
| `description` | string | no | shown beside the command in the command list |
| `precommand` | map | no | this command runs another command (`sudo`, `env`, `nice`): see below |
| `generators` | map | no | name → generator, see below |

### Nodes (the root and every subcommand)

| Key | Type | Meaning |
|---|---|---|
| `name` | string | the subcommand (required on a subcommand, not allowed at the top) |
| `aliases` | list of strings | other spellings the tool itself accepts |
| `desc` | string | the pager description (keep it under 60 bytes) |
| `flags` | list of flag maps | flags valid here |
| `args` | list of arg maps | positional arguments in order; only the last may set `repeat: true` |
| `subcommands` | list of node maps | children |
| `dash_values` | arg map | values spelled like a flag, offered as `-VALUE` (`kill -KILL`) |

### Flags

| Key | Type | Meaning |
|---|---|---|
| `long` | string | without the dashes: `"release"` for `--release` |
| `short` | string | one character without the dash |
| `desc` | string | pager description |
| `arg` | arg map | present when the flag takes a value |
| `arg_optional` | bool | the value may only be attached with `=` (`--profile-gen[=<dir>]`); the next word is not consumed |
| `global` | bool | also valid under every subcommand |

A flag needs `long`, `short` or both; each spelling gets its own row.

### Arguments

| `kind` | Offers | Extra keys |
|---|---|---|
| `path` | files and directories | `ext`: extensions without the dot; directories always shown |
| `dir` | directories only | |
| `exec` | commands on `$PATH` and shell builtins | |
| `command` | the rest of the line is a new command (`xargs`) | |
| `var` | environment variable names | |
| `user` | user names | |
| `host` | hosts from `~/.ssh/config` and `~/.ssh/known_hosts` | |
| `pid` | running process ids | |
| `signal` | signal names | |
| `values` | a fixed list | `values`: strings, or `{ value, desc }` maps |
| `generator` | a named generator | `generator`: a built-in or a key of `generators` |
| `none` | free text; nothing is offered | |

Any argument may set `repeat: true` if it is the last one.

Built-in generators: `hosts`, `signals`, `users`, `make_targets` (read from
the Makefile's text -- make is never run), `pids`.

### Generators

```
generators: {
    branches: { argv: ["git", "for-each-ref", "--format=%(refname:short)",
                       "refs/heads", "refs/remotes"],
                cache_ms: 2000, pass_flags: ["-C", "--git-dir"] },
},
```

| Key | Meaning |
|---|---|
| `argv` | the program and its arguments -- run directly, never through a shell |
| `cache_ms` | how long an answer stays fresh (default 2000) |
| `pass_flags` | flags that, when already on the line before the cursor, are passed to the generator with their values, right after `argv[0]` (`git -C ../other checkout <Tab>` lists `../other`'s branches) |

A generator prints one candidate per line, optionally followed by a tab and
a description. It runs in the directory `:!` commands run in, with
`NO_COLOR=1 PAGER=cat GIT_PAGER=cat TERM=dumb`, and is given 1.5 seconds.
Completion never waits for it: its answer appears when it arrives, and it
never changes what you have typed.

### Precommands

```
precommand: { flags_with_args: ["-u", "-g"], operands: 0, assignments: true },
```

`flags_with_args` lists the flags that take the next word; `operands` is
how many words come after the flags before the command (`timeout 5 cmd`
has one); `assignments: true` lets `NAME=value` words through (`env`,
`sudo`). The first word after all of that completes as a command, and the
rest of the line completes as that command's arguments.

## A worked example (abridged `wolf.fl`)

```
# wolf 0.2.14 -- from `wolf --help` and `wolf <command> --help`.
{
    completion: 1,
    command: "wolf",
    description: "the wolf toolchain",
    flags: [
        { long: "help", short: "h", global: true, desc: "this message" },
        { long: "completions", desc: "a completion script for a shell",
          arg: { kind: "values", values: ["bash", "zsh", "fish"] } },
    ],
    subcommands: [
        { name: "build", desc: "compile an entry file to a native executable",
          flags: [
              { short: "o", desc: "the output executable",
                arg: { kind: "path" } },
              { long: "emit", desc: "what to emit",
                arg: { kind: "values",
                       values: ["wir", "obj", "bin", "llvm-ir"] } },
              { long: "profile-gen", desc: "instrument for PGO",
                arg: { kind: "dir" }, arg_optional: true },
              { long: "std-root", desc: "the standard library checkout",
                arg: { kind: "dir" } },
          ],
          args: [ { kind: "path", ext: ["lu"] } ] },
    ],
}
```

With that spec, `wolf b<Tab>` completes `build`, `wolf build <Tab>` offers
`.lu` files and directories, `wolf build --emit=<Tab>` offers the four
emit kinds, and `wolf build --std-root <Tab>` offers directories.
