/*
 * Sprint 26 §8.  See typejump.h for the four rules.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/typejump.h"

#include <stdlib.h>
#include <string.h>

#include "ws/finder.h"

void yew_typejump_clear(TypeJump *tj)
{
    if (tj == NULL)
        return;
    tj->len = 0U;
    tj->pat[0] = '\0';
    tj->deadline_ms = 0;
}

bool yew_typejump_active(const TypeJump *tj, i64 now_ms)
{
    return tj != NULL && tj->len > 0U && now_ms < tj->deadline_ms;
}

/*
 * A key that types a character.
 *
 * Modified keys are excluded: `C-n` is a binding, not the letter `n`,
 * and swallowing it here would take the host list's navigation away.
 */
static bool printable(const Key *k)
{
    if (k->ntext != 1U)
        return false;
    if ((k->mods & (YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SUPER |
                    YEW_MOD_HYPER | YEW_MOD_META)) != 0U)
        return false;
    return k->text[0] >= 0x20U && k->text[0] != 0x7FU;
}

bool yew_typejump_key(TypeJump *tj, const Key *k, i64 now_ms,
                      const PickItem *items, u32 n, u32 *sel)
{
    const char **labels;
    FzRanked *ranked;
    u32 matched;
    u32 i;

    if (tj == NULL || k == NULL)
        return false;
    if (!printable(k)) {
        /*
         * Rule 4: cleared FIRST, then NOT consumed.  An arrow key ends
         * the sequence and still moves the list; a letter typed after
         * it starts a new pattern rather than continuing the old one.
         */
        yew_typejump_clear(tj);
        return false;
    }
    if (items == NULL || n == 0U || sel == NULL)
        return false;

    /*
     * Rule 1.  Outside the window the key REPLACES rather than appends,
     * so a pause makes `s` mean "the next thing starting with s".
     */
    if (tj->len == 0U || now_ms >= tj->deadline_ms)
        tj->len = 0U;
    if (tj->len + 1U < (u32)YEW_TYPEJUMP_PAT_MAX) {
        tj->pat[tj->len++] = (char)k->text[0];
        tj->pat[tj->len] = '\0';
    }
    tj->deadline_ms = now_ms + (i64)YEW_TYPEJUMP_RESET_MS;

    /*
     * Rule 3, checked BEFORE ranking: if what is already selected
     * scores exact, nothing moves.  fuss's rule — an exact match under
     * the cursor is the answer, and jumping off it to another row that
     * merely also matched is the behaviour this exists to prevent.
     */
    if (*sel < n && items[*sel].label != NULL) {
        i32 here = yew_fz_score(tj->pat, tj->len, items[*sel].label,
                                (u32)strlen(items[*sel].label), NULL);

        if (here >= 10000)
            return true;
    }

    labels = yew_xreallocarray(NULL, n, sizeof(*labels));
    ranked = yew_xreallocarray(NULL, n, sizeof(*ranked));
    for (i = 0U; i < n; i++)
        labels[i] = items[i].label;
    /*
     * path_mode false: these are list labels, and scoring their last
     * `/`-or-`.` segment would rank a directory listing on fragments.
     */
    matched = yew_fz_rank(tj->pat, tj->len, labels, n, false, ranked);
    if (matched > 0U)
        *sel = ranked[0].idx;
    yew_xfree(labels);
    yew_xfree(ranked);
    return true;
}
