# Sprint 59.9: Live theme picker and coherent modal palettes

## Prerequisites

Sprints 41 (pure-data themes and precompiled rendition tables), 26 (shared
list picker), 24 (group picker) and 59.5 (input modals) landed. The binding
decisions, architecture, Fletch and campaign index apply. This user-directed
detour neither restarts the numbered Sprint 59 soak nor closes Sprint 59.8.

## Goals

Make theme selection discoverable and reversible through a centered modal:
navigation previews the whole editor, Enter/Space/click applies, and Escape
restores the exact opening palette. Ship familiar dark and light palettes
and make modal surfaces follow them. Preserve configuration semantics:
applying changes this session; persistent choices remain in init.fl.

## Deliverables

1. **Bounded deterministic discovery**, `src/syn/theme.[ch]`:

   ```c
   typedef struct ThemeNames ThemeNames;
   void yew_theme_names(ThemeNames *names);
   ```

   Enumerate valid filename stems from user themes, runtime directories and
   the embedded image, following the existing loader's precedence and
   exclusive YEW_RUNTIME_DIR semantics. Deduplicate and sort bytewise with
   bounded storage (256 names, 127 bytes each). Report truncation. Never
   execute theme data. Invalid theme files remain selectable with an honest
   load error; a failed preview leaves the last successful view untouched.

2. **Live transaction**, `src/edit/theme_cmds.[ch]`, `src/ui/picker.[ch]`:

   ```c
   CmdStatus yew_theme_cmd_pick(CmdCtx *cx);
   ```

   Register `ed.theme.pick` / `:theme.pick`, and Line-mode `t h`. Repeating
   that opener uses the existing modal cancellation route. Shared picker
   gains optional selection and closed callbacks, and Space-accept/no-split
   policies; existing consumers retain their behavior. Selection callbacks
   run after navigation/filtering, never in draw. Mouse press previews,
   same-row release accepts. Preserve payload identity across filtering.

   Hold the original compiled Theme by ownership transfer, not a pathname
   reload. Preview only replaces resolved rendering tables: no option,
   dark/light history, document, undo, syntax, or file changes. Accept uses
   the selected successfully loaded palette and updates the theme option
   and appropriate toggle history once. Cancel/replacement/shutdown restores
   the original even if its file disappeared. Release all owned memory.

   **Pitfall:** close runs before accept; teardown must not invalidate the
   context needed by accept or clear a successor modal. Do not split panes
   for Ctrl-v/Ctrl-s in this preference picker. Empty/invalid selections
   must not commit some other preview under the wrong name.

3. **Palettes**, `runtime/themes/*.fl`: retain Quiver Dark/Light, add
   Dracula, Nord, Gruvbox Dark/Light, Catppuccin Mocha/Latte, Solarized
   Dark/Light. Adapt upstream palette values to all 54 semantic attrs and
   all existing UI roles. Preserve notices with installed runtime assets.
   Default Quiver input modals use neutral surfaces and accent ink instead
   of the previous orange fill. This explicitly supersedes Sprint 59.5's
   orange styling, not its geometry or interaction model.

4. **Consistent chrome**, input, shared-list and group modals: use theme
   modal surface/title/detail/key/selection roles, with editor fg/bg as the
   fallback for partial user themes. Selected rows retain non-color cues;
   16/256/truecolor, NO_COLOR, dumb and ASCII modes remain supported. Clip
   labels within the modal. Theme previews repaint existing tabs, FUSS,
   syntax, status, menus and modal surfaces without rehighlighting buffers.

5. **Docs and scope**: update `.docs/themes.md`, the syntax manual, command
   reference and handoff with controls, presets and session-only behavior.
   No automatic config rewrite, third-party package installer, theme editor
   or new dependency. Merge workspace work remains Sprint 59.8's scope.

## Testing Strategy

- Units: all ten palettes parse, all 54 attrs explicit, all rendition
  tables; sorted/deduplicated discovery, explicit runtime precedence;
  preview/apply/cancel/replacement, toggle history, option consistency,
  source removal, invalid/empty selections, small terminals and teardown.
- Shared-picker regression: selection callbacks on keys/wheel/filter/mouse,
  Space acceptance, split suppression and opener cancellation; no document
  mutation. Existing picker, group, modal, mouse and recorder tests.
- PTY: theme live preview, cancel and accept plus dark/light input modal
  goldens. Inspect intentional changes, narrow/ASCII/NO_COLOR cases.
- Warning-clean Clang/GCC and focused ASan/UBSan; empty-module build.
- Bounded single-worker theme fuzz smoke; measure repeat preview work;
  no unbounded fuzz campaign, no new soak or hosted CI claim.

## Definition of Done

- [x] Ten shipped palettes load with all 54 attrs and no diagnostics.
- [x] Picker discovers shipped and custom names deterministically.
- [x] Arrows/wheel/filter preview; Enter/Space/same-row click apply.
- [x] Escape/repeated opener/replacement restore exact original tables.
- [x] Preview never mutates options, toggle history, or document bytes.
- [x] Failed/empty choices cannot commit an unrelated preview or split.
- [x] Input, list and group modals follow the active palette.
- [x] Focused unit/PTY/compiler/sanitizer checks and timings recorded.
- [x] Docs identify session-only selection and persistent init.fl setting.

## Completion evidence — 2026-10-21

Warning-clean Clang/GCC 16 and ASan/UBSan builds: theme units 23 tests /
3,643 assertions; empty-module theme units 22 / 3,541. Focused picker/group,
prompt, mouse, modal routing, recorder, registry and default-keymap suites
pass. All 43 focused PTYs pass their normal two-run comparison, including
six new theme variants and dark/light/ANSI/ASCII/monochrome commit dialogs.
Man/docs synchronization checks and selftests pass (groff unavailable).
Bounded plain/sanitized theme fuzz: 300 iterations, seed 5909, matching hash
`e0401bd93adf2fd1`.

Theme switching at 200x50: p99 0.324 ms, zero syntax calls, versus 0.327 ms
in clean pre-change `ffb03f4f`. Thirty picker open/preview/cancel cycles plus
setup: 0.01 s wall, about 5.1 MiB peak RSS. Broader syntax performance gates
are not green: the two absolute-budget misses reproduce in the pre-change
control, and no gate was relaxed. The incomplete broad PTY run also is not
a full-regression signoff. `.docs/HANDOFF.md` records exact scope, remaining
unrelated checks and log paths. No push, install or new soak was performed.
