#ifndef YEW_UI_PICKER_H
#define YEW_UI_PICKER_H

/*
 * Sprint 26 §5: THE list picker.
 *
 * Every future list in this program is an instance of this widget — the
 * file finder, the buffer switcher, the undo branch picker, and later
 * the LSP symbol list (47), `yew pkg` (55), F-mode fuzzy jump (52) and
 * the command palette (38).  None of those need new machinery; they
 * need a PickerSpec.
 *
 * THREE LAWS, and each one is a bug that has bitten every hand-rolled
 * picker ever written:
 *
 * 1. SELECTION IS HELD BY PAYLOAD, NEVER BY ROW INDEX.  One more
 *    character reorders the list; a row-index selection then slides
 *    onto a different file a fraction of a second before Enter.  The
 *    user opens the wrong thing and blames themselves, because nothing
 *    looked wrong.  If the held payload leaves the filtered set,
 *    selection falls to row 0 — visibly, at the top.
 *
 * 2. THE FILTER LINE IS THE SPRINT 18 CmdLine.  There is exactly one
 *    text editor in this program.  Its motions and edits are the same
 *    ed.move.* / ed.ins.* / ed.del.* commands L and I modes use, so
 *    every binding and every register works here for free.  A
 *    `char buf[256]` with hand-rolled backspace is the pitfall s18
 *    exists to forbid, and DoD 9 greps for it.
 *
 * 3. THE PICKER IS A KEYMAP LAYER.  Pushed on open, popped on close, so
 *    a key it does not handle is SWALLOWED rather than leaking into the
 *    document underneath.  A picker that lets `d` through deletes a
 *    line behind a dialog.
 *
 * The group picker (s24) is deliberately NOT retrofitted onto this: it
 * walks directories and ticks a path set, which is a different
 * interaction with a different model.  Both draw their chrome through
 * the same helpers, and that is the reuse that matters.
 */

#include "edit/cmd.h"
#include "term/input.h"
#include "ui/layout.h"
#include "util/base.h"
#include "ws/finder.h"

typedef struct Ed Ed;

enum {
    YEW_PICK_MODIFIED = 1U << 0,
    YEW_PICK_DEFERRED = 1U << 1,
    YEW_PICK_ORPHAN = 1U << 2
};

/* How an accept was requested; the picker splits before calling. */
enum {
    YEW_PICK_ACCEPT_HERE = 0,
    YEW_PICK_ACCEPT_VSPLIT = 1,
    YEW_PICK_ACCEPT_HSPLIT = 2
};

enum {
    /* Wider than this and the eye stops tracking rows; narrower than
     * YEW_PICKER_MIN_COLS and there is no honest way to draw a list, so
     * the picker refuses rather than drawing a 6-cell box. */
    YEW_PICKER_MAX_W = 80,
    /* Preview pickers may widen enough to give both columns useful space;
     * ordinary pickers retain YEW_PICKER_MAX_W's established cap. */
    YEW_PICKER_PREVIEW_MAX_W = 160,
    YEW_PICKER_MAX_H = 20,
    YEW_PICKER_MIN_COLS = 24,
    YEW_PICKER_MIN_ROWS = 6,
    /* Below this the `detail` column is dropped: two columns in 40
     * cells means neither is readable. */
    YEW_PICKER_DETAIL_MIN_W = 40,
    /* §7.2: how much of a full rescan happens per frame.  2 ms leaves
     * the rest of the 5 ms keypress budget for drawing. */
    YEW_PICKER_SLICE_US = 2000,
    /*
     * Below this the preview slot is dropped entirely.  Half of a
     * 60-cell box is 30 cells of list and 30 of file contents, and
     * neither is worth reading — the list is what the picker is FOR, so
     * it keeps the space.
     */
    YEW_PICKER_PREVIEW_MIN_W = 100
};

typedef struct PickItem {
    const char *label;  /* what is ranked and drawn                  */
    const char *detail; /* dim right column; may be NULL             */
    /*
     * IDENTITY.  A tab id, a buffer id, an undo node id — anything
     * stable across a refilter.  Never a row index (the s23 law).
     */
    i32 payload;
    u8 flags;
} PickItem;

/*
 * The candidate source is a CALLBACK rather than an array so a picker
 * over 100 000 files does not copy them, and so the buffer switcher can
 * reflect a tab closed while it is open.
 */
typedef struct PickerSpec {
    const char *title;
    const PickItem *(*items)(void *ctx, u32 *n);
    /* May be NULL.  Must not create a Buffer or a TextBuf — DoD 11
     * counts reads and allocations. */
    void (*preview)(Ed *ed, void *ctx, i32 payload, Rect r);
    bool (*accept)(Ed *ed, void *ctx, i32 payload, u8 how);
    /* Optional picker-specific keys.  Called before printable text is
     * offered to the shared filter line.  Returning true consumes the key. */
    bool (*action)(Ed *ed, void *ctx, i32 payload, const Key *key);
    /* Optional borrowed search text for candidates whose searchable text is
     * larger or different from the visible label.  Parts are concatenated
     * with one normalized space and scored incrementally under the picker's
     * slice budget.  Return false when `part` is past the last part. */
    bool (*search_part)(void *ctx, i32 payload, u32 part,
                        const u8 **text, size_t *len);
    /* NULL keeps the standard picker footer. */
    const char *footer;
    bool path_mode; /* §2's two-pass basename rule */
    /* False preserves the normal type-to-filter picker behavior.  True
     * leaves printable keys available to `action` until `/` opens the
     * shared filter line. */
    bool filter_requires_slash;
    void *ctx;
} PickerSpec;

/*
 * Opens the picker.  Refuses, with a message, when the terminal is too
 * small — a dialog that cannot show its list is worse than none.
 */
void yew_picker_open(Ed *ed, const PickerSpec *s);
/* False when the key was not ours, which only happens when the picker
 * is closed: an open picker swallows everything. */
bool yew_picker_key(Ed *ed, const Key *k);
void yew_picker_draw(Ed *ed, Rect area);
void yew_picker_close(Ed *ed, bool accepted);
bool yew_picker_active(const Ed *ed);

/* Rows currently shown and candidates considered — what the footer
 * prints, and what the tests assert against. */
u32 yew_picker_shown(const Ed *ed);
u32 yew_picker_total(const Ed *ed);
/* The payload under the cursor; 0 when the list is empty. */
i32 yew_picker_selected(const Ed *ed);

/*
 * Sprint 27 §2: the mouse seams.
 *
 * By PAYLOAD, not by row, for law 1's reason — a click is resolved
 * against the region the renderer registered, and the list may have
 * been re-ranked between the paint and the press.
 */
void yew_picker_select_payload(Ed *ed, i32 payload);
/* Accepts what is under the cursor, exactly as Enter does. */
bool yew_picker_accept(Ed *ed);
/* Wheel.  Moves the SELECTION, because that is what the list's scroll
 * is derived from — there is no independent scroll offset to desync. */
void yew_picker_scroll(Ed *ed, i32 rows);

/* Test seam: re-rank against the current filter text.  The editor calls
 * this from the cmdline's edited hook. */
void yew_picker_refilter(Ed *ed);

/*
 * §7.2: continues a sliced rescan.  True while more remains, so the
 * event loop keeps calling it from the idle timer.
 */
bool yew_picker_tick(Ed *ed);
/* True while a rescan is in flight — the footer shows ` scanning…`. */
bool yew_picker_scanning(const Ed *ed);

#endif
