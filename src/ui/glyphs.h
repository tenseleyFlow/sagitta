#ifndef YEW_UI_GLYPHS_H
#define YEW_UI_GLYPHS_H

/*
 * Sprint 27 §7: ONE table, every chrome glyph in the program.
 *
 * Not tidiness — consistency the eye can trust.  Chrome glyphs invented
 * at their call sites drift: two widgets end up spelling "collapsed"
 * differently, and nobody ever files a bug about it because there is
 * nothing to point at.  They just find the UI slightly untrustworthy.
 *
 * THE » / « RULE, pinned: THE ARROW POINTS THE WAY THE THING WILL MOVE,
 * NOT THE WAY IT CURRENTLY IS.  A collapsed node shows `»` because
 * activating it expands rightward; an expanded one shows `«` because
 * activating it collapses leftward.  `▶` / `▼` follow the same rule for
 * vertical disclosure.  Half of all terminal trees get this
 * inconsistent between two widgets.
 *
 * ASCII fallbacks are selected by `ui.ascii`, default AUTO: on when
 * neither LC_ALL nor LC_CTYPE nor LANG contains UTF-8/utf8, off
 * otherwise.  YEW_ASCII=1 forces on, YEW_ASCII=0 forces off.  The
 * decision is made ONCE, at startup (Sprint 0's single-decision rule),
 * because a glyph set that could change mid-session would make two
 * frames of one screen disagree.
 *
 * EVERY GLYPH IS MEASURED WITH yew_cluster_width AT ITS CALL SITE.  A
 * two-cell glyph in a one-cell slot is a layout bug, and `⇕` and `✓`
 * are exactly the risky ones — both have a dedicated width assertion in
 * tests/unit/test_glyphs.c.
 */

#include "util/base.h"

typedef enum {
    /* Collapsed node / expands rightward. */
    YEW_GLYPH_EXPAND = 0,
    /* Expanded node / collapses leftward. */
    YEW_GLYPH_COLLAPSE,
    /* Draggable separator grip, at a pane border's midpoint. */
    YEW_GLYPH_GRIP,
    /* Modified — DERIVED from the undo tree (s23), never a stored
     * flag. */
    YEW_GLYPH_MODIFIED,
    /* A ticked row with unsaved changes (s24's picker). */
    YEW_GLYPH_DIRTY_TICK,
    /* Vertical disclosure, expanded and collapsed. */
    YEW_GLYPH_DISCLOSE_OPEN,
    YEW_GLYPH_DISCLOSE_SHUT,
    /* Strip overflow: more entries to the left / to the right (s23).
     * The right-hand one is followed by a count at its call site. */
    YEW_GLYPH_MORE_LEFT,
    YEW_GLYPH_MORE_RIGHT,
    /* Ticked and unticked rows (s24). */
    YEW_GLYPH_TICKED,
    YEW_GLYPH_UNTICKED,
    /* Pane borders and joints (s22). */
    YEW_GLYPH_BORDER_V,
    YEW_GLYPH_BORDER_H,
    YEW_GLYPH_BORDER_CROSS,
    YEW_GLYPH_BORDER_TEE_R,
    YEW_GLYPH_BORDER_TEE_L,
    YEW_GLYPH_BORDER_TEE_D,
    YEW_GLYPH_BORDER_TEE_U,
    /*
     * Git status — RESERVED for Sprint 52.  They live in the table now
     * so that sprint does not invent a second vocabulary, and nothing
     * draws them yet.
     */
    YEW_GLYPH_GIT_AHEAD,
    YEW_GLYPH_GIT_CONFLICT,
    YEW_GLYPH_GIT_BEHIND,
    YEW_GLYPH__N
} YewGlyph;

/*
 * The glyph's bytes, in whichever vocabulary is in force.  Always
 * NUL-terminated, so a caller may use strlen — but the length is
 * available without one.
 */
const char *yew_glyph(YewGlyph g);
size_t yew_glyph_len(YewGlyph g);
/* Cells the glyph occupies, measured with yew_cluster_width. */
u16 yew_glyph_cells(YewGlyph g);

/*
 * Which vocabulary is in force.  Resolved once, on the first ask, from
 * YEW_ASCII then the locale variables — and cached, because two frames
 * of one screen must not disagree.
 */
bool yew_glyph_ascii(void);
/*
 * Test seam ONLY: re-resolves against `getenv`.  The running editor
 * never calls this; the single-decision rule is what it exists to make
 * testable, not to weaken.
 */
void yew_glyph_reset(void);
/* Test seam: forces the vocabulary, bypassing the environment. */
void yew_glyph_force_ascii(bool on);

#endif
