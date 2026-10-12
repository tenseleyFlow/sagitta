# Languages, themes, and syntax definitions

Syntax highlighting is incremental, deterministic, and bounded. Definitions
compile into immutable tables; buffers cache one state identity per line and
rescan forward only until the old and new states settle. Budgets turn an
unfinished tail into a visible degraded state and resume it cooperatively.

`yew syn list` prints the built-in language inventory. Detection proceeds by
exact filename, glob, longest compound extension, shebang, then first-line
regex, with priority and language name as stable ties. A user definition can
add a new name but cannot replace a built-in language silently.

```sh
yew syn list >/dev/null
```
```output
```

## Definition files

Definitions are pure Fletch literal data under `runtime/syntax/` or the user
syntax directory. They declare metadata, named contexts, ordered regex rules,
attributes, and state operations. Validation rejects unknown keys, invalid
attributes, nonregular patterns, unresolved contexts, nonreducing cycles,
excessive push depth, and incompatible embedded-language bridges.

Use the CLI before loading a definition interactively:

```text
yew syn check path/to/language.fl --strict
yew syn dump path/to/language.fl --tables
yew syn dump path/to/language.fl --spans sample.ext
```

`--strict` promotes warnings to errors. `check --coverage` proves that fixture
inputs reach every context/rule/embed site claimed by the definition. The
binary syntax cache is an optimization only: source identity, content, CRC,
version, and structural checks all must pass, otherwise yew recompiles.

## Embedded languages

An embed rule names a guest definition and an exit condition. The host owns
the opener and bridge, the guest owns its body, and the exit returns to the
exact host context. Depth and resident-definition caps are fixed. Unknown
guests fall back visibly without corrupting the host state.

## Themes

Themes map semantic attributes—comment, keyword, selection, warning, and so
on—to terminal styles. The built-in `quiver-dark` and `quiver-light` themes
cover the full closed attribute set. Runtime switching repaints without
rehighlighting because syntax stores meanings, not colors.

`:theme NAME` selects one. `theme_auto = true` asks the terminal background
query and chooses a light or dark theme; malformed or absent replies retain
the configured theme. Color degrades deterministically from truecolor through
256 and 16 colors to no-color styles.
