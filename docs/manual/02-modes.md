# Modes: unit, action, and escape

yew's modes answer two separate questions. Unit modes choose what an arrow
means; action modes choose what you are doing. The statusline always carries
the current one-letter chip.

| Mode | Kind | Arrow unit or purpose | Escape |
|---|---|---|---|
| `L` | unit | lines | cancels a count/chord, then remains in L |
| `W` | unit | Unicode words (or subwords) | returns to L |
| `B` | unit | structural blocks and enclosing bodies | returns to L |
| `H` | action | extends a character/line/word/block selection | collapses and returns to the borrowed unit |
| `I` | action | inserts text | commits the typing undo group and returns to L |
| `E` | action | `:` commands and `!` shell commands | closes completion/prompt, then returns |
| `F` | action | FUSS workspace and Git tree | closes FUSS and returns to the prior unit mode |

The complete default binding inventory lives in `yew(1)` and is mechanically
cross-checked against `runtime/init.fl`. This chapter explains the model
behind that inventory.

## Unit modes

In L mode, Up and Down move by lines while Left and Right move to the current
line's boundaries. In W mode, horizontal arrows move by word boundaries. In B
mode, arrows navigate structural landmarks: language-aware bodies where the
syntax engine can prove them, and conservative paragraph/indent structure as
the fallback. Block motion deliberately does not treat every assignment or
match arm as a block.

Switch unit with `l`, `w`, or `b`. A count prefixes a repeatable command:
`12G` goes to line 12 and `4x` deletes four grapheme clusters as one undo
step. Escape cancels a partial count or key chord before it changes mode.

## Action modes

`i` enters Insert. `h` enters Highlight and borrows the current unit; this is
why selecting in H after W grows by words while H after B grows through block
containment. `e` or `:` enters the command line, `!` enters it already seeded
for a shell command, and `f` opens FUSS.

Shift plus an arrow starts or extends a character selection directly from
any editing mode. Ctrl-C copies a selection, Ctrl-X cuts it, and Ctrl-V pastes
the system clipboard; the register-oriented commands remain available too.

## Escape is layered

Escape closes the most local transient state first: an incomplete chord,
count, completion menu, prompt, selection, or action mode. Pressing it again
continues outward. This keeps cancellation predictable and prevents a key
intended for a popup from leaking into the document.

A keyboard-opened dialog also cancels when you repeat its activation
shortcut: for example, Alt-m opens the commit modal and Alt-m again discards
it. This applies to input dialogs, list/group pickers, context menus and
confirmations, including remapped multi-key shortcuts. Workflow steps inherit
their opener. Holding a key does not toggle the dialog. Mouse- or command-line-
opened dialogs without an activation shortcut still use Escape.

The same rule applies to pointer menus and panels. Keyboard reachability is
not a fallback path: every pointer action resolves to a registered editor
command, and the keyboard invokes that same command.
