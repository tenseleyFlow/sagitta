/*
 * Sprint 23 §3.  See strip.h for why there is exactly one of these.
 */
#include "ui/strip.h"

#include <string.h>

#include "unicode/width.h"
#include "util/log.h"

u16 sag_strip_label_cells(const char *label)
{
    int cells = 0;
    size_t len;

    if (label == NULL)
        return 0U;
    len = strlen(label);
    /*
     * Measured through the Sprint 2 clipper, which is the only thing
     * that knows a character can be two cells wide.  Using strlen here
     * is the multibyte click-shift bug in one line.
     */
    (void)sag_str_clip((const u8 *)label, len, SAG_STRIP_LABEL_CELLS,
                       &cells);
    return (u16)(cells < 0 ? 0 : cells);
}

void sag_strip_layout(const StripEntry *entries, int n, u16 width,
                      int active, int *scroll, StripSpan *spans,
                      int *n_spans, bool *more_left, bool *more_right)
{
    int first;
    int i;
    u16 at;
    int count = 0;

    if (n_spans != NULL)
        *n_spans = 0;
    if (more_left != NULL)
        *more_left = false;
    if (more_right != NULL)
        *more_right = false;
    if (entries == NULL || spans == NULL || n <= 0 || width == 0U)
        return;

    first = scroll != NULL ? *scroll : 0;
    if (first < 0)
        first = 0;
    if (first >= n)
        first = n - 1;

    /*
     * Keep the active entry visible, adjusting as little as possible.
     *
     * Scrolling left is trivial.  Scrolling right walks the first
     * visible entry forward one at a time until the active entry fits,
     * rather than jumping to "active - something": the minimal answer
     * is the one that does not move entries the user is looking at.
     */
    if (active >= 0 && active < n) {
        if (active < first) {
            first = active;
        } else {
            for (;;) {
                u16 used = 0U;
                bool fits = false;

                for (i = first; i < n; i++) {
                    u16 w = sag_strip_label_cells(entries[i].label);

                    if ((u32)used + w > width)
                        break;
                    used = (u16)(used + w);
                    if (i == active) {
                        fits = true;
                        break;
                    }
                }
                if (fits || first >= active)
                    break;
                first++;
            }
        }
    }

    at = 0U;
    for (i = first; i < n; i++) {
        u16 w = sag_strip_label_cells(entries[i].label);

        if ((u32)at + w > width)
            break;
        spans[count].idx = i;
        spans[count].col0 = at;
        spans[count].col1 = (u16)(at + w);
        count++;
        at = (u16)(at + w);
    }
    /*
     * An entry too wide for the whole strip would otherwise place
     * nothing and loop forever in the caller; place it clipped so the
     * strip always shows something.
     */
    if (count == 0 && first < n) {
        spans[0].idx = first;
        spans[0].col0 = 0U;
        spans[0].col1 = width;
        count = 1;
    }

    if (scroll != NULL)
        *scroll = first;
    if (n_spans != NULL)
        *n_spans = count;
    if (more_left != NULL)
        *more_left = first > 0;
    if (more_right != NULL)
        *more_right = first + count < n;
}
