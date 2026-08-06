#ifndef SAG_UI_PICKER_H
#define SAG_UI_PICKER_H

/*
 * Sprint 26 §5: THE list picker.
 *
 * Every future list in this program is an instance of this widget — the
 * file finder, the buffer switcher, the undo branch picker, and later
 * the LSP symbol list (47), `sag pkg` (55), F-mode fuzzy jump (52) and
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
    SAG_PICK_MODIFIED = 1U << 0,
    SAG_PICK_DEFERRED = 1U << 1,
    SAG_PICK_ORPHAN = 1U << 2
};

/* How an accept was requested; the picker splits before calling. */
enum {
    SAG_PICK_ACCEPT_HERE = 0,
    SAG_PICK_ACCEPT_VSPLIT = 1,
    SAG_PICK_ACCEPT_HSPLIT = 2
};

enum {
    /* Wider than this and the eye stops tracking rows; narrower than
     * SAG_PICKER_MIN_COLS and there is no honest way to draw a list, so
     * the picker refuses rather than drawing a 6-cell box. */
    SAG_PICKER_MAX_W = 80,
    SAG_PICKER_MAX_H = 20,
    SAG_PICKER_MIN_COLS = 24,
    SAG_PICKER_MIN_ROWS = 6,
    /* Below this the `detail` column is dropped: two columns in 40
     * cells means neither is readable. */
    SAG_PICKER_DETAIL_MIN_W = 40
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
    bool path_mode; /* §2's two-pass basename rule */
    void *ctx;
} PickerSpec;

/*
 * Opens the picker.  Refuses, with a message, when the terminal is too
 * small — a dialog that cannot show its list is worse than none.
 */
void sag_picker_open(Ed *ed, const PickerSpec *s);
/* False when the key was not ours, which only happens when the picker
 * is closed: an open picker swallows everything. */
bool sag_picker_key(Ed *ed, const Key *k);
void sag_picker_draw(Ed *ed, Rect area);
void sag_picker_close(Ed *ed, bool accepted);
bool sag_picker_active(const Ed *ed);

/* Rows currently shown and candidates considered — what the footer
 * prints, and what the tests assert against. */
u32 sag_picker_shown(const Ed *ed);
u32 sag_picker_total(const Ed *ed);
/* The payload under the cursor; 0 when the list is empty. */
i32 sag_picker_selected(const Ed *ed);

/* Test seam: re-rank against the current filter text.  The editor calls
 * this from the cmdline's edited hook. */
void sag_picker_refilter(Ed *ed);

#endif
