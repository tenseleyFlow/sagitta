#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 27 §7: the glyph vocabulary.
 *
 * Two things are being defended.
 *
 * WIDTH.  Every chrome glyph goes into a slot the layout sized in
 * cells.  A two-cell glyph in a one-cell slot writes over whatever the
 * layout placed next, and the symptom — a strip that is one cell wrong
 * only when a particular file is open — is nobody's idea of a clue.
 * `⇕` and `✓` are the risky ones (both are in the ambiguous-width
 * range), and both get a dedicated assertion here.
 *
 * COMPLETENESS.  Every row of the table has both vocabularies, and the
 * ASCII one is pure ASCII — a "fallback" containing a multibyte
 * character is not a fallback at all.
 */
#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "ui/glyphs.h"
#include "unicode/width.h"

void test_glyphs_every_row_has_both_vocabularies(void)
{
    u32 i;

    for (i = 0U; i < (u32)YEW_GLYPH__N; i++) {
        const char *utf8;
        const char *ascii;
        size_t k;

        yew_glyph_force_ascii(false);
        utf8 = yew_glyph((YewGlyph)i);
        yew_glyph_force_ascii(true);
        ascii = yew_glyph((YewGlyph)i);
        YEW_ASSERT_NOT_NULL(utf8);
        YEW_ASSERT_NOT_NULL(ascii);
        YEW_ASSERT(utf8[0] != '\0');
        YEW_ASSERT(ascii[0] != '\0');
        /* A fallback with a multibyte character in it is not a
         * fallback. */
        for (k = 0U; ascii[k] != '\0'; k++)
            YEW_ASSERT((unsigned char)ascii[k] < 0x80U);
    }
    yew_glyph_reset();
}

/*
 * DoD-adjacent, and the reason §7 singles these two out: a glyph that
 * measures two cells in a one-cell slot is a layout bug.
 */
void test_glyphs_grip_and_tick_are_one_cell(void)
{
    yew_glyph_force_ascii(false);
    /* ⇕ — the draggable separator grip. */
    YEW_ASSERT_EQ_U64(yew_glyph_cells(YEW_GLYPH_GRIP), 1U);
    /* [✓] — the ticked box, three cells: bracket, check, bracket. */
    YEW_ASSERT_EQ_U64(yew_glyph_cells(YEW_GLYPH_TICKED), 3U);
    /* And its unticked twin is the SAME width, or the rows below it
     * would shift by a cell as the user ticks. */
    YEW_ASSERT_EQ_U64(yew_glyph_cells(YEW_GLYPH_UNTICKED),
                      yew_glyph_cells(YEW_GLYPH_TICKED));
    yew_glyph_reset();
}

/*
 * Every glyph fits the one-cell slots the chrome gives it, and the two
 * vocabularies agree on width — otherwise YEW_ASCII=1 would reflow
 * every strip in the program.
 */
void test_glyphs_are_the_same_width_in_both_vocabularies(void)
{
    u32 i;

    for (i = 0U; i < (u32)YEW_GLYPH__N; i++) {
        u16 wide;
        u16 narrow;

        yew_glyph_force_ascii(false);
        wide = yew_glyph_cells((YewGlyph)i);
        yew_glyph_force_ascii(true);
        narrow = yew_glyph_cells((YewGlyph)i);
        YEW_ASSERT_EQ_U64(wide, narrow);
        YEW_ASSERT(wide >= 1U);
    }
    yew_glyph_reset();
}

/*
 * The »/« rule: the arrow points the way the thing will MOVE, not the
 * way it currently is.  Asserted because half of all terminal trees get
 * this inconsistent between two widgets and nobody ever files a bug —
 * they just find the UI slightly untrustworthy.
 */
void test_glyphs_arrows_point_the_way_the_thing_moves(void)
{
    yew_glyph_force_ascii(false);
    /* Collapsed expands RIGHTWARD. */
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_EXPAND), "\xC2\xBB");
    /* Expanded collapses LEFTWARD. */
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_COLLAPSE), "\xC2\xAB");
    /* Vertical disclosure follows the same rule. */
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_DISCLOSE_SHUT), "\xE2\x96\xB6");
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_DISCLOSE_OPEN), "\xE2\x96\xBC");
    yew_glyph_force_ascii(true);
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_EXPAND), ">");
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_COLLAPSE), "<");
    yew_glyph_reset();
}

/*
 * ui.ascii is AUTO: on when none of LC_ALL / LC_CTYPE / LANG names
 * UTF-8, off when any does.  YEW_ASCII wins over all three, in both
 * directions — a UTF-8 locale on a terminal whose font lacks the box
 * glyphs is exactly what YEW_ASCII=1 is for.
 */
void test_glyphs_ascii_is_auto_from_the_locale(void)
{
    (void)unsetenv("YEW_ASCII");
    (void)unsetenv("LC_ALL");
    (void)unsetenv("LC_CTYPE");
    (void)unsetenv("LANG");

    yew_glyph_reset();
    YEW_ASSERT(yew_glyph_ascii()); /* nothing says UTF-8 */

    (void)setenv("LANG", "en_US.UTF-8", 1);
    yew_glyph_reset();
    YEW_ASSERT(!yew_glyph_ascii());

    (void)setenv("LANG", "en_US.utf8", 1);
    yew_glyph_reset();
    YEW_ASSERT(!yew_glyph_ascii());

    (void)setenv("LANG", "C", 1);
    (void)setenv("LC_CTYPE", "de_DE.UTF-8", 1);
    yew_glyph_reset();
    YEW_ASSERT(!yew_glyph_ascii());

    (void)setenv("LC_ALL", "POSIX", 1);
    yew_glyph_reset();
    /* LC_ALL does not veto the others here — ANY of the three naming
     * UTF-8 is enough, because the terminal is being told to expect it
     * by whichever one the library ends up honouring. */
    YEW_ASSERT(!yew_glyph_ascii());

    (void)unsetenv("LC_CTYPE");
    yew_glyph_reset();
    YEW_ASSERT(yew_glyph_ascii());

    /* The explicit answer wins in both directions. */
    (void)setenv("YEW_ASCII", "0", 1);
    yew_glyph_reset();
    YEW_ASSERT(!yew_glyph_ascii());
    (void)setenv("LANG", "en_US.UTF-8", 1);
    (void)setenv("YEW_ASCII", "1", 1);
    yew_glyph_reset();
    YEW_ASSERT(yew_glyph_ascii());

    (void)unsetenv("YEW_ASCII");
    (void)unsetenv("LC_ALL");
    (void)unsetenv("LC_CTYPE");
    (void)unsetenv("LANG");
    yew_glyph_reset();
}

/*
 * The decision is made ONCE (Sprint 0's single-decision rule): a glyph
 * set that could change mid-session would let two frames of one screen
 * disagree, and the goldens would be unreproducible.
 */
void test_glyphs_the_vocabulary_is_decided_once(void)
{
    (void)unsetenv("YEW_ASCII");
    (void)setenv("LANG", "en_US.UTF-8", 1);
    yew_glyph_reset();
    YEW_ASSERT(!yew_glyph_ascii());

    /* The environment changes under a running editor... */
    (void)setenv("YEW_ASCII", "1", 1);
    /* ...and the answer does NOT. */
    YEW_ASSERT(!yew_glyph_ascii());
    YEW_ASSERT_EQ_STR(yew_glyph(YEW_GLYPH_BORDER_V), "\xE2\x94\x82");

    (void)unsetenv("YEW_ASCII");
    (void)unsetenv("LANG");
    yew_glyph_reset();
}
