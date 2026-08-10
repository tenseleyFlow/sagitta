/*
 * Sprint 27 §7.  See glyphs.h for the »/« rule and the single-decision
 * rule this file obeys.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/glyphs.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/width.h"
#include "util/log.h"

typedef struct GlyphRow {
    const char *utf8;
    const char *ascii;
} GlyphRow;

/*
 * THE table.  Order matches YewGlyph exactly; the static assert below
 * is what keeps a future insertion from silently shifting every glyph
 * in the program by one.
 */
static const GlyphRow glyphs[] = {
    [YEW_GLYPH_EXPAND] = {"\xC2\xBB", ">"},          /* » */
    [YEW_GLYPH_COLLAPSE] = {"\xC2\xAB", "<"},        /* « */
    [YEW_GLYPH_GRIP] = {"\xE2\x87\x95", "|"},        /* ⇕ */
    [YEW_GLYPH_MODIFIED] = {"*", "*"},
    [YEW_GLYPH_DIRTY_TICK] = {"\xE2\x80\xA2", "+"},  /* • */
    [YEW_GLYPH_DISCLOSE_OPEN] = {"\xE2\x96\xBC", "v"},  /* ▼ */
    [YEW_GLYPH_DISCLOSE_SHUT] = {"\xE2\x96\xB6", ">"},  /* ▶ */
    [YEW_GLYPH_MORE_LEFT] = {"<", "<"},
    [YEW_GLYPH_MORE_RIGHT] = {">", ">"},
    [YEW_GLYPH_TICKED] = {"[\xE2\x9C\x93]", "[x]"},  /* [✓] */
    [YEW_GLYPH_UNTICKED] = {"[ ]", "[ ]"},
    [YEW_GLYPH_BORDER_V] = {"\xE2\x94\x82", "|"},    /* │ */
    [YEW_GLYPH_BORDER_H] = {"\xE2\x94\x80", "-"},    /* ─ */
    [YEW_GLYPH_BORDER_CROSS] = {"\xE2\x94\xBC", "+"},  /* ┼ */
    [YEW_GLYPH_BORDER_TEE_R] = {"\xE2\x94\x9C", "+"},  /* ├ */
    [YEW_GLYPH_BORDER_TEE_L] = {"\xE2\x94\xA4", "+"},  /* ┤ */
    [YEW_GLYPH_BORDER_TEE_D] = {"\xE2\x94\xAC", "+"},  /* ┬ */
    [YEW_GLYPH_BORDER_TEE_U] = {"\xE2\x94\xB4", "+"},  /* ┴ */
    /* Reserved for Sprint 52; nothing draws these yet. */
    [YEW_GLYPH_GIT_AHEAD] = {"\xE2\x86\x91", "^"},     /* ↑ */
    [YEW_GLYPH_GIT_CONFLICT] = {"\xE2\x9C\x97", "x"},  /* ✗ */
    [YEW_GLYPH_GIT_BEHIND] = {"\xE2\x86\x93", "v"}     /* ↓ */
};

_Static_assert(YEW_ARRAY_LEN(glyphs) == (size_t)YEW_GLYPH__N,
               "every YewGlyph needs a row");

static bool ascii_resolved;
static bool ascii_on;

/* True when `s` names a UTF-8 locale.  Case-insensitive on the tail,
 * because "UTF-8", "utf8" and "UTF8" are all in the wild. */
static bool locale_is_utf8(const char *s)
{
    size_t i;

    if (s == NULL)
        return false;
    for (i = 0U; s[i] != '\0'; i++) {
        const char *p = s + i;

        if ((p[0] == 'U' || p[0] == 'u') && (p[1] == 'T' || p[1] == 't') &&
            (p[2] == 'F' || p[2] == 'f')) {
            const char *rest = p + 3;

            if (rest[0] == '-')
                rest++;
            if (rest[0] == '8')
                return true;
        }
    }
    return false;
}

static void resolve(void)
{
    const char *forced;

    if (ascii_resolved)
        return;
    ascii_resolved = true;
    forced = getenv("YEW_ASCII");
    if (forced != NULL && forced[0] != '\0') {
        /* An explicit answer wins over the locale in both directions:
         * a UTF-8 locale on a terminal whose font lacks the box glyphs
         * is exactly the case YEW_ASCII=1 exists for. */
        ascii_on = forced[0] != '0';
        return;
    }
    /*
     * AUTO.  On when NONE of the three name UTF-8 — the same precedence
     * the C library uses, so the answer matches what the terminal is
     * actually being told to expect.
     */
    ascii_on = !locale_is_utf8(getenv("LC_ALL")) &&
               !locale_is_utf8(getenv("LC_CTYPE")) &&
               !locale_is_utf8(getenv("LANG"));
}

bool yew_glyph_ascii(void)
{
    resolve();
    return ascii_on;
}

void yew_glyph_reset(void)
{
    ascii_resolved = false;
    ascii_on = false;
}

void yew_glyph_force_ascii(bool on)
{
    ascii_resolved = true;
    ascii_on = on;
}

const char *yew_glyph(YewGlyph g)
{
    if ((u32)g >= (u32)YEW_GLYPH__N)
        YEW_BUG("yew_glyph: glyph %u is not in the table", (unsigned)g);
    resolve();
    return ascii_on ? glyphs[g].ascii : glyphs[g].utf8;
}

size_t yew_glyph_len(YewGlyph g)
{
    return strlen(yew_glyph(g));
}

u16 yew_glyph_cells(YewGlyph g)
{
    const char *s = yew_glyph(g);
    int cells = 0;

    /*
     * MEASURED, never assumed.  A two-cell glyph in a one-cell slot is
     * a layout bug the goldens catch a frame later and nobody can
     * explain; measuring at the call site is what makes it impossible.
     */
    (void)yew_str_clip((const u8 *)s, strlen(s), 1000, &cells);
    return cells < 0 ? 0U : (u16)cells;
}
