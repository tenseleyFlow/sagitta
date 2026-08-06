#ifndef SAG_UI_GLYPHS_H
#define SAG_UI_GLYPHS_H

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
 * otherwise.  SAG_ASCII=1 forces on, SAG_ASCII=0 forces off.  The
 * decision is made ONCE, at startup (Sprint 0's single-decision rule),
 * because a glyph set that could change mid-session would make two
 * frames of one screen disagree.
 *
 * EVERY GLYPH IS MEASURED WITH sag_cluster_width AT ITS CALL SITE.  A
 * two-cell glyph in a one-cell slot is a layout bug, and `⇕` and `✓`
 * are exactly the risky ones — both have a dedicated width assertion in
 * tests/unit/test_glyphs.c.
 */

#include "util/base.h"

typedef enum {
    /* Collapsed node / expands rightward. */
    SAG_GLYPH_EXPAND = 0,
    /* Expanded node / collapses leftward. */
    SAG_GLYPH_COLLAPSE,
    /* Draggable separator grip, at a pane border's midpoint. */
    SAG_GLYPH_GRIP,
    /* Modified — DERIVED from the undo tree (s23), never a stored
     * flag. */
    SAG_GLYPH_MODIFIED,
    /* A ticked row with unsaved changes (s24's picker). */
    SAG_GLYPH_DIRTY_TICK,
    /* Vertical disclosure, expanded and collapsed. */
    SAG_GLYPH_DISCLOSE_OPEN,
    SAG_GLYPH_DISCLOSE_SHUT,
    /* Strip overflow: more entries to the left / to the right (s23).
     * The right-hand one is followed by a count at its call site. */
    SAG_GLYPH_MORE_LEFT,
    SAG_GLYPH_MORE_RIGHT,
    /* Ticked and unticked rows (s24). */
    SAG_GLYPH_TICKED,
    SAG_GLYPH_UNTICKED,
    /* Pane borders and joints (s22). */
    SAG_GLYPH_BORDER_V,
    SAG_GLYPH_BORDER_H,
    SAG_GLYPH_BORDER_CROSS,
    SAG_GLYPH_BORDER_TEE_R,
    SAG_GLYPH_BORDER_TEE_L,
    SAG_GLYPH_BORDER_TEE_D,
    SAG_GLYPH_BORDER_TEE_U,
    /*
     * Git status — RESERVED for Sprint 52.  They live in the table now
     * so that sprint does not invent a second vocabulary, and nothing
     * draws them yet.
     */
    SAG_GLYPH_GIT_AHEAD,
    SAG_GLYPH_GIT_CONFLICT,
    SAG_GLYPH_GIT_BEHIND,
    SAG_GLYPH__N
} SagGlyph;

/*
 * The glyph's bytes, in whichever vocabulary is in force.  Always
 * NUL-terminated, so a caller may use strlen — but the length is
 * available without one.
 */
const char *sag_glyph(SagGlyph g);
size_t sag_glyph_len(SagGlyph g);
/* Cells the glyph occupies, measured with sag_cluster_width. */
u16 sag_glyph_cells(SagGlyph g);

/*
 * Which vocabulary is in force.  Resolved once, on the first ask, from
 * SAG_ASCII then the locale variables — and cached, because two frames
 * of one screen must not disagree.
 */
bool sag_glyph_ascii(void);
/*
 * Test seam ONLY: re-resolves against `getenv`.  The running editor
 * never calls this; the single-decision rule is what it exists to make
 * testable, not to weaken.
 */
void sag_glyph_reset(void);
/* Test seam: forces the vocabulary, bypassing the environment. */
void sag_glyph_force_ascii(bool on);

#endif
