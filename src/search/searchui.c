/*
 * Sprint 21 §1/§2.  See searchui.h for what this layer owns.
 */
#include "search/searchui.h"

#include <string.h>

void sag_search_opts_init(SearchOpts *o)
{
    if (o == NULL)
        return;
    o->ignorecase = false;
    o->smartcase = true;
    o->wrapscan = true;
    o->hlsearch = true;
}

bool sag_search_wants_icase(const SagRe *probe, const SearchOpts *o)
{
    bool ignorecase = o != NULL && o->ignorecase;
    bool smartcase = o == NULL || o->smartcase;

    /*
     * The two escapes win outright, and \C wins over \c when a pattern
     * somehow contains both: asking for exact case is the narrower
     * request, and a search that silently widens is the one that
     * surprises.
     */
    if (sag_re_forces_case(probe))
        return false;
    if (sag_re_forces_icase(probe))
        return true;
    if (!ignorecase)
        return false;
    if (!smartcase)
        return true;
    return !sag_re_has_upper_literal(probe);
}

SagRe *sag_search_compile(Arena *a, const char *pat, size_t len,
                          const SearchOpts *o, SagReErr *err)
{
    SagRe *probe;

    if (a == NULL || pat == NULL)
        return sag_re_compile(a, pat, len, 0U, err);
    /*
     * Two passes, because the answer to "should this be
     * case-insensitive" is inside the pattern.  The first pass is the
     * cheap one — it is the same work a failed compile would do anyway,
     * and Sprint 20's compiler is microseconds on a prompt-sized
     * pattern, which is what makes recompiling per keystroke viable at
     * all.
     */
    probe = sag_re_compile(a, pat, len, 0U, err);
    if (probe == NULL)
        return NULL;
    if (!sag_search_wants_icase(probe, o))
        return probe;
    return sag_re_compile(a, pat, len, SAG_RE_ICASE, err);
}
