# Sprint 24: Tab Groups

## Prerequisites

- Sprint 23 — `Tab` with stable `tab_id`, dormant `group_id` /
  `group_ordinal` / `deferred` fields, strip-layout engine, `ed.tab.goto`,
  survivor-by-id close discipline.
- Sprint 22 — region registry with the `YEW_REGION_TAB` payload sign
  convention (positive = tab index, negative = −group id) already declared.
- Sprint 13 — keymap layers (the picker is a modal layer); the event-loop
  timer heap (digit-jump window).
- Binding: `.docs/plan/00-decisions.md` Tab groups row — "Facsimile model
  verbatim". Source of truth for this sprint:
  `~/GithubOrgs/FortranGoingOnForty/facsimile/src/editor_state_module.f90`,
  `src/ui/group_picker_module.f90`, `docs/KEYBINDINGS.md` §Tab groups.

## Goals

Port the facsimile tab-group model whole: groups hold **no member lists**
(membership lives on the tab as `group_id` + compacted `group_ordinal`,
computed on demand, never cached); `last_active_member` is a **path**, not
an index; empty groups auto-dissolve; deferred tabs cost no file read until
first viewed, and residency is asked of the allocation, never a flag. On top
of the model: the one New/Edit group-picker dialog with ticks stored as a
path set; row-1 `[label (N)]` entries with the negative-payload convention;
the row-2 member strip (pinned inside a group, hover preview outside — same
renderer, so they cannot disagree); walk-through navigation that treats all
open files as one continuous line with enter-at-edge semantics; and the
digit jump's 500 ms extension window (jump immediately, then arm).
Persistence of groups is Sprint 25; drag/drop-into-group and context menus
are Sprint 27; F-mode "open directory as group" is Sprint 53.

## Deliverables

### 1. The group type — `src/ui/groups.h`

```c
typedef struct TabGroup {
    u32   id;                  /* monotonic per session, never reused */
    char *label;               /* display name; heap-owned */
    char *dir_path;            /* origin dir, canonical; a LABEL of origin,
                                  not a constraint — members may live anywhere */
    char *last_active_member;  /* PATH of the member to resume at; may dangle */
} TabGroup;
/* Ed holds Vec(TabGroup) groups; u32 next_group_id (starts 1). */
```

**Deliberately no member list.** Closing a tab compacts the tabs array and
renumbers every index above it, so any stored membership is stale one close
later. Membership is the tab's `group_id` (0 = ungrouped, authoritative)
plus `group_ordinal` (1-based position within the group, compacted when a
member leaves). A cached member count is a label that can lie; it is always
computed. `last_active_member` is a **path** because indices renumber; it is
non-authoritative and falls back to the lowest ordinal when it dangles.

### 2. Group APIs — `src/ui/groups.c`

```c
u32  yew_group_create(Ed *ed, const char *dir_path, const char *name);
void yew_group_dissolve(Ed *ed, u32 gid);      /* stragglers -> ungrouped */
int  yew_group_find(const Ed *ed, u32 gid);    /* index into groups; -1 */
int  yew_group_member_count(const Ed *ed, u32 gid);       /* computed */
int  yew_group_members(const Ed *ed, u32 gid, int *out, int cap); /* by ordinal */
void yew_group_add_member(Ed *ed, u32 gid, int tab_idx);
void yew_group_remove_member(Ed *ed, int tab_idx);
void yew_group_label(const Ed *ed, u32 gid, char *buf, size_t n); /* "src/ (4)" */
u32  yew_active_group_id(const Ed *ed);        /* derived from active tab */
void yew_group_prune_empty(Ed *ed);            /* restore path uses this */
void yew_group_set_ordinal(Ed *ed, int tab_idx, int pos);
```

Rules (each is a facsimile scar; keep the comments carrying the why):

- `create`: empty name → label = `basename(dir)/`. Groups may be created
  empty (the picker adds members after).
- `add_member`: a tab belongs to exactly one group — if grouped, call
  `remove_member` **first** so both groups' ordinals stay consistent; then
  ordinal = max ordinal in target group + 1.
- `remove_member`: zero the tab's fields, decrement every ordinal above the
  vacated one, then **auto-dissolve if the count hit 0**. Tab close calls
  this *before* the array compaction (Sprint 23's close order).
- `dissolve`: any tab still pointing at the gid becomes ungrouped rather
  than orphaned against an id that no longer resolves.
- `members`: ordered by ordinal; do not assume ordinals contiguous
  mid-removal — sort, don't index.
- `active_group_id`: derived (`tabs[active].group_id`), never stored — the
  active index is assigned raw in several places and a cached copy drifts.
- `set_ordinal(pos)`: `pos` counts in the **final** list the user sees.
  Pitfall: "skip the moved member, insert at pos" is off by one whenever
  the member moves right (its vacated slot is still counted). Build the
  final order, then renumber 1..n.
- `yew_group_reorder_block(Ed*, u32 gid, int to_idx)` (row-1 drag lands in
  Sprint 27, command lands now): a group is ONE row-1 entry but several
  array entries; moving it moves every member, made contiguous at the
  destination. Pitfall: compute the entire destination order first **as
  tab_ids**, then place position by position — moving members one at a
  time drags already-placed ones back down and interleaves the group with
  the tabs it passed; recording indices instead of ids goes stale after
  the first move.

### 3. Lazy hydration — deferred tabs

```c
bool yew_tab_is_resident(const Ed *ed, int tab_idx);
int  yew_tab_hydrate(Ed *ed, int tab_idx);     /* 0 ok; loads the file */
void yew_tab_defer(Ed *ed, int tab_idx);       /* frees text; modified off */
```

- **Residency is asked of the allocation, never a flag**: `is_resident`
  checks whether the tab's buffer holds loaded text (BufferList handle has
  a live TextBuf). A flag can drift; the allocation is what every save path
  ultimately asks about, so a stale answer structurally cannot exist. The
  Sprint 23 `Tab.deferred` bool is **deleted this sprint** in favor of the
  structural question.
- `defer` releases the TextBuf and clears modified — a tab read from
  nowhere cannot be modified.
- `hydrate` is called at the top of every switch-to-tab; resident tabs
  return immediately. Opening a 40-file group costs one file read (the
  viewed member); the other 39 are tabs with a path and no buffer.
- Pitfalls (all real facsimile bugs — encode as tests):
  1. Never copy any buffer into a non-resident tab: the copy *allocates*,
     making it look resident, after which the real file is never read and
     the fabricated content is what gets saved.
  2. Save paths refuse non-resident tabs with a distinct status ("never
     loaded, nothing written") rather than an I/O error.
  3. Any future sync-to-instances path skips non-resident tabs — marking
     one modified means a save writes an empty file.

### 4. Group picker dialog — `src/ui/grouppicker.c`

One dialog, two modes. Creating and editing a group are the same act —
choose files, name the result — so Edit is the same list opened with the
current members already ticked. The mode reaches no further than the title
("New Tab Group" / "Edit Tab Group") and the footer hint (create/save).
The picker knows nothing about groups: it is a file picker with a name
field; the meaning of a confirmed result lives in the command layer.

```c
bool yew_gp_show(Ed *ed, const char *dir);                 /* New  */
bool yew_gp_show_edit(Ed *ed, const char *dir, const char *name); /* Edit */
void yew_gp_preselect(const char *path);   /* tick without listing it */
void yew_gp_mark_dirty(const char *path);  /* row shows the • bullet */
/* results after confirm: */
int         yew_gp_result(void);           /* PENDING/CANCELLED/CONFIRMED */
const char *yew_gp_name(void);
int         yew_gp_count(void);
const char *yew_gp_path(int i);
```

- **Ticks are a SET of canonical paths (`Strmap`), never a flag on listed
  rows.** Walking to `../` re-lists a different directory; per-item flags
  would drop every tick the moment you moved — defeating the entire point
  of the `../` row, which is assembling a group that spans directories.
  `preselect` exists because an edit-mode member may live outside `dir`
  and cannot be expressed as an index into the current listing.
- Footer: `N selected` count always visible — ticks in other directories
  have scrolled out of sight, and without the count the `../` row looks
  like it loses them. Second footer line: mode-appropriate hints
  (`tab focus · space tick · enter create|save · esc cancel`) or a
  transient note (`tick at least one file (space)` / `the group needs a
  name`).
- Listing: `../` row pinned on top (absent at `/`), directories first then
  files, each case-insensitively sorted. Directories are **walked, not
  ticked** (space on a dir enters it). Space on a file toggles and
  advances one row (fast run-ticking); Enter on a dir walks in, on a file
  confirms; `left` walks to parent; `up` off the top returns focus to the
  name field; Tab toggles name/list focus.
- Name field pre-filled `basename(dir)/` in New mode so the happy path is
  Enter-Enter; Edit mode pre-fills the group's label.
- Rows: `[ ]` unticked (the box is the affordance), `[✓]` ticked in the
  accent color, `•` suffix on ticked rows with unsaved changes — in Edit
  mode unticking closes that member's tab, so the warning must be readable
  *before* Enter, not in a prompt after.
- Confirm applies in the command layer: New → `yew_group_create` + open
  each ticked path (deferred; `find_by_path` first — already-open tabs are
  adopted, not duplicated) + `add_member` each. Edit → diff ticked set vs
  members: added paths open deferred + join; removed members leave the
  group; their tabs close (dirty ones run the Sprint 23 prompt, cancel
  re-ticks). Empty result set can't happen (confirm refuses 0 ticks).
- Chrome: centered box (62 cells wide max, 12 visible rows), drop shadow,
  `YEW_REGION_BLOCK` over its rectangle, `YEW_REGION_GP_NAME` and
  `YEW_REGION_GP_ROW` (payload = listed index) added per drawn row —
  hit-test rows come from the same layout arithmetic as drawing (one
  function computes row→index; renderer and click share it).
- Commands: `ed.group.new` (picker at the workspace dir or the active
  tab's dir), `ed.group.edit` (active group; error if none),
  `ed.group.remove_tab`, `ed.group.dissolve`.

### 5. Tab bar: row-1 entries and the row-2 member strip

Row 1 (extends Sprint 23's renderer): a group occupies **one entry placed
where its first member sits**; members never appear individually on row 1.
Ungrouped tabs render as themselves. Entry label `[label (N)]` via
`yew_group_label` (count computed live). Payload = **−gid** — the sign is
how the click router tells groups from tabs without a second region kind.

```c
/* row-1 entry construction (mirrors facsimile render_tab_bar) */
for each tab i in array order:
    gid = tabs[i].group_id
    if gid != 0:
        if gid not in seen: emit { "[label (N)]", payload=-gid }; seen+=gid
        continue                      /* members are row 2's business */
    emit { "[i: name*]", payload=i }
```

Row 2: the active group's members, one entry each, label ` name*`, payload
= **global tab index** — so the existing `YEW_REGION_TAB` click case
handles them unchanged. Active member reversed; other members render dim
(secondary). Same `yew_strip_layout` engine, same overflow `<` / `>N`
(scroll regions carry ±2 payload to name the row). **Same renderer for the
pinned row and the hover preview** — pinned while the active tab is in a
group (the layout reserves the row: tab-bar height is 2 inside a group),
preview when hovering a group's row-1 entry while outside it. The preview
persists while the pointer is anywhere that belongs to the group (its
row-1 entry or the strip itself) so you can move down into it; anywhere
else hides it. Option `tabs.group_hover_preview` (default on; the off
switch matters to users who don't want motion tracking). Hover arrives
with Sprint 27's mouse routing — this sprint lands the renderer +
pin path; preview is called by S27.

- Click router additions: `YEW_REGION_TAB` payload > 0 → switch to tab;
  payload < 0 → enter group `-payload` (resume semantics, D6).
- Why one renderer is a law, not taste: facsimile drew row 2 with the same
  layout engine specifically so the two rows cannot disagree about where a
  click landed — placement split out of drawing is the whole point.

### 6. Navigation: the continuous line

Commands (registry) and default bindings (keymap data; kitty-protocol
chords where available, with universal fallbacks per facsimile's lesson —
treat `super+ctrl` as the enhancement, never the only path):

| Command | Keys (default) | Behavior |
|---|---|---|
| `ed.file.next/prev` | `ctrl+pgdn/pgup`, `super+ctrl+right/left` | walk-through step |
| `ed.group.enter` | `super+ctrl+down` | into a group, resume member |
| `ed.group.leave` | `super+ctrl+up` | out in one press, from any member |
| `ed.tab.goto` (S23) | `alt+1..9,0` | now arms the extension window (D7) |

Walk-through (`step(delta)`), the facsimile algorithm exactly:

1. If inside a group: step through `members` by ordinal. Target in range →
   switch to that member, done. Off either end → **fall through** to the
   row-1 walk (the group's entry is `here`; `here+delta` is the entry
   beside it). This is what makes left/right one continuous line through
   every open file — and a guaranteed exit that survives a window manager
   eating `super+ctrl+up`.
2. Row-1 entry list: mirror of the renderer's construction — a group once,
   at its first member's position, as `-gid`; ungrouped tabs as `+idx`.
   `target = here + delta`, wrapping at both ends.
3. Record `last_active_member` (path) for the group being left
   (`note_group_position`), then: target ≥ 0 → switch to tab; target < 0 →
   `enter_group_at_edge(-target, delta)`.

```c
/* Enter from the side the walk arrived from: moving right lands on the
 * first member, moving left on the last. That makes the walk reversible —
 * retrace your steps, visit the same files in reverse. Deliberately NOT
 * the resume-at-last-active entry: right for an explicit ed.group.enter,
 * wrong mid-walk, where it would skip every member between the edge and
 * wherever you last happened to be. */
static void enter_group_at_edge(Ed *ed, u32 gid, int delta);
```

- `ed.group.enter` (explicit): resume at `last_active_member` — resolve
  the stored path against current members; dangling → first member.
- `ed.group.leave`: note position, then prefer the nearest tab after the
  group in array order, else the nearest before; all tabs grouped here →
  status message, stay.
- Every switch passes through hydrate (D3).

### 7. Digit jump: 500 ms extension window

**Jump immediately, then arm.** Waiting to disambiguate would put half a
second of lag on the overwhelmingly common single-digit case; superseding
an already-made jump costs nothing.

State (module-local in `tabs.c`): `jump_value`, `jump_deadline_ms`,
`jump_group` (gid of the tab the last digit landed on, 0 if ungrouped).
`YEW_JUMP_WINDOW_MS = 500`, driven by the event-loop timer heap (the
deadline is a timer entry so the status hint clears even with no keys).

- `ed.tab.goto N`: switch now; `jump_value = N`; deadline = now+500 ms;
  `jump_group = tabs[N].group_id`; status line announces what a further
  digit would do (the window must never feel like a lost keystroke).
- Next key inside the window, if it carries a digit (bare `5`, and also
  `alt+5` / `ctrl+5` — holding the modifier is the natural way to type
  `alt+1` `5`; accepting only the bare form sent the second digit to the
  main dispatch as its own jump: tab 1 then tab 5, not 15):
  - `jump_group != 0` → the digit picks that group's **Nth member by
    ordinal** (what row 2 shows is what the user is counting). Out of
    range → status `this group has N members`, window cleared, first jump
    stands.
  - else `target = jump_value*10 + digit`; in range → switch, **re-arm**
    (value=target, new deadline, new jump_group) so three digits work; out
    of range → status, window cleared, jump stands, and the digit is NOT
    inserted into the document — it was part of a chord, and a surprise
    edit while navigating is worse than a dropped key.
- Any non-digit key or an expired deadline clears the state first, then
  dispatches normally (clearing before returning matters: a digit typed
  much later must not read as a continuation).

### 8. Defer

- Persisting groups + ordinals + `last_active_member` + deferred residency:
  Sprint 25 (this sprint exposes `yew_group_prune_empty` and the id-remap
  needs no new API — ids are session-local).
- Hover preview trigger, drag tab onto a group entry (dwell drop-in),
  group/tab context menus: Sprint 27.
- F-mode "open directory as a group": Sprint 53 — `ed.group.from_dir`
  hard-errors naming it.

## Testing Strategy

- **Unit** (`tests/unit/test_groups.c`, `test_grouppicker.c`,
  `test_groupnav.c`): membership computed vs oracle through 5k random
  add/remove/close/reorder ops (counts, ordinal compaction, contiguity
  after reorder_block, auto-dissolve, dissolve-ungroups-stragglers);
  set_ordinal off-by-one case (move right by 1 in a 3-group);
  reorder_block interleave regression (2-member group past 3 tabs);
  residency: defer → !resident, hydrate → resident, save on non-resident
  refuses with the distinct status; picker driven headless (accessors, no
  terminal): walk into subdir, tick two, walk `../`, tick one, count == 3
  and all paths survive; preselect out-of-dir path counted; confirm with 0
  ticks refused; edit-diff produces exact open/close/join/leave sets;
  walk-through: fixture of [t1, G{a,b,c}, t2] — 6 rights from t1 visit
  t1,a,b,c,t2,t1; 6 lefts visit them in exact reverse (reversibility);
  enter_at_edge left lands on c; explicit enter resumes last_active_member
  and falls back when the path dangles; leave from middle member lands on
  t2; digit jump: 15 via alt+1,5; group-member pick via alt+N,digit;
  out-of-range leaves prior jump; expiry via mocked clock.
- **Pty** (`tests/pty/groups_*.golden`): row-1 `[src/ (3)]` entry; row-2
  pinned strip with active member reversed, others dim; row-2 overflow
  `>N`; picker full chrome (New and Edit titles, ticks, footer count,
  dirty bullet); click on a negative-payload entry enters the group.
- **Fuzz**: the 5k-op membership storm above, plus a picker key-sequence
  fuzzer (random keys never crash, ticked set only ever contains listed or
  preselected canonical paths); ASan/UBSan lanes.

## Definition of Done

1. ≥ 90 unit assertions across the three test files; gcc+clang clean;
   ASan/UBSan/valgrind lanes green.
2. `grep -n "member" src/ui/groups.h` shows no member array/list/count
   field on `TabGroup`; `grep -rn "is_resident\|resident" src/ui | grep
   -v "("` shows no residency flag — both structural questions.
3. Membership fuzz (5k ops, pinned seed + random lane) green.
4. Opening a 40-file group performs exactly 1 file read (counted via a
   test hook in file.c); switching to member 2 performs the 2nd.
5. Walk-through reversibility test green: N rights then N lefts returns to
   start visiting the same sequence reversed.
6. `enter_group_at_edge` vs resume: both unit-tested; the mid-walk path
   never consults `last_active_member` (asserted via test hook).
7. Digit jump: single digit switches in the same frame (no 500 ms lag —
   pty test asserts the grid changed before the window expires); `alt+1`
   `5` reaches tab 15; digit after landing in a group picks by ordinal.
8. Picker ticks survive directory navigation (unit + pty); footer count
   always equals set size.
9. Row-1/row-2 click agreement: pty test clicks every span of both rows
   with multibyte labels present; each lands on its payload.
10. Empty groups cannot exist after any op sequence (fuzz asserts
    count > 0 for every live group id).
11. All new commands dispatch via the registry; `ed.group.from_dir`
    hard-errors naming Sprint 53.
12. Two consecutive full pty runs byte-identical (invariant 5).
