#ifndef SAG_SEARCH_SEARCHUI_H
#define SAG_SEARCH_SEARCHUI_H

/*
 * Sprint 21 §1/§2: the search surface over Sprint 20's engine.
 *
 * No pattern logic lives here — every match goes through sag_re_*.
 * What this file owns is what the user means: which direction, which
 * case rule, and what to restore when they change their mind.
 */

#include "search/regex.h"
#include "text/coords.h"
#include "text/cursor.h"
#include "unicode/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

/*
 * The four search options.  Sprint 36 owns the option TABLE (`:set`,
 * Fletch's opt.set, the resolution chain); these are named to match the
 * entries it will declare, so wiring them there is a lookup change and
 * not a rename.  Defaults are the sprint's: exact case unless asked,
 * wrap on, highlight on.
 */
typedef struct SearchOpts {
    bool ignorecase; /* default off */
    bool smartcase;  /* default on  */
    bool wrapscan;   /* default on  */
    bool hlsearch;   /* default on  */
} SearchOpts;

void sag_search_opts_init(SearchOpts *o);

/*
 * The §2 table, in one place.  Six rows:
 *
 *   \c in pattern            -> ignore case   (outranks everything)
 *   \C in pattern            -> match case    (outranks everything)
 *   ignorecase off           -> match case
 *   ignorecase on, sc off    -> ignore case
 *   ignorecase on, sc on, no uppercase literal -> ignore case
 *   ignorecase on, sc on, an uppercase literal -> match case
 *
 * Takes the COMPILED pattern because "has an uppercase literal" is a
 * question only the parser can answer; deciding it by scanning the
 * pattern text is the bug this signature exists to prevent.
 */
bool sag_search_wants_icase(const SagRe *probe, const SearchOpts *o);

/*
 * Compiles `pat` under the smartcase rule.  Compiles once to learn what
 * the pattern contains, then recompiles with SAG_RE_ICASE if the table
 * says so — the second pass is what folds literals into classes, and it
 * cannot be decided before the first.  Returns NULL with `err` set.
 */
SagRe *sag_search_compile(Arena *a, const char *pat, size_t len,
                          const SearchOpts *o, SagReErr *err);

#endif
