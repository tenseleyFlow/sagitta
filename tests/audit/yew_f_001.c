/*
 * YEW-F-001 — ambiguous-wide doubles fixed-cell chrome glyphs.
 *
 * Correct behavior: changing `ambiguous_wide` may change document glyph
 * width, but every chrome glyph keeps the cells its layout slot reserved.
 * The UTF-8 and ASCII vocabularies must therefore have identical widths
 * under both option values.
 *
 * Baseline failure: `•`, box-drawing borders, disclosure arrows, and other
 * fixed-slot glyphs widen from one to two cells when `ambiguous_wide=true`.
 */
#include "audit.h"

#include <stdio.h>

#include "ui/glyphs.h"
#include "unicode/width.h"

bool test_yew_f_001(char *why, size_t why_cap)
{
    YewWidthOpts narrow = {false};
    YewWidthOpts wide = {true};
    u16 expected[YEW_GLYPH__N];
    u32 i;
    bool ok = true;

    yew_glyph_force_ascii(false);
    yew_width_set_opts(&narrow);
    for (i = 0U; i < (u32)YEW_GLYPH__N; i++)
        expected[i] = yew_glyph_cells((YewGlyph)i);

    yew_width_set_opts(&wide);
    for (i = 0U; i < (u32)YEW_GLYPH__N; i++) {
        u16 got = yew_glyph_cells((YewGlyph)i);

        if (got != expected[i]) {
            (void)snprintf(why, why_cap,
                           "glyph %u widens from %u to %u cells",
                           (unsigned)i, (unsigned)expected[i],
                           (unsigned)got);
            ok = false;
            break;
        }
    }
    yew_width_set_opts(NULL);
    yew_glyph_reset();
    return ok;
}
