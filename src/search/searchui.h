#ifndef YEW_SEARCH_SEARCHUI_H
#define YEW_SEARCH_SEARCHUI_H

/*
 * Sprint 21 §1/§2: the search surface over Sprint 20's engine.
 *
 * No pattern logic lives here — every match goes through yew_re_*.
 * What this file owns is what the user means: which direction, which
 * case rule, and what to restore when they change their mind.
 */

#include "search/regex.h"
#include "text/coords.h"
#include "edit/loop.h"
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

void yew_search_opts_init(SearchOpts *o);

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
bool yew_search_wants_icase(const YewRe *probe, const SearchOpts *o);

/*
 * Compiles `pat` under the smartcase rule.  Compiles once to learn what
 * the pattern contains, then recompiles with YEW_RE_ICASE if the table
 * says so — the second pass is what folds literals into classes, and it
 * cannot be decided before the first.  Returns NULL with `err` set.
 */
YewRe *yew_search_compile(Arena *a, const char *pat, size_t len,
                          const SearchOpts *o, YewReErr *err);

/*
 * Live search state.  `save_*` is the restore point captured when the
 * prompt opens: cancelling must put the view back exactly, INCLUDING
 * the viewport's top line and the cursor's goal column.  Restoring only
 * the cursor leaves the window scrolled somewhere else — the user
 * pressed `/`, looked, changed their mind, and lost their place anyway.
 */
typedef struct SearchState {
    Arena arena;      /* owns `re` and `pat` */
    YewRe *re;        /* last pattern that COMPILED, for highlighting */
    char *pat;
    size_t patlen;
    bool reverse;     /* the direction the search was started with */
    u32 pat_gen;      /* bumped per recompile; the overlay's cache key */

    Cursor save_cur;
    LineNo save_top;
    u32 save_top_sub;
    ByteOff origin;   /* where the search started from */

    bool wrapped;
    bool active;      /* a prompt is open */
    TimerId count_timer;
    u32 count_win_id; /* stable identity; timer callbacks hold no Win * */
    TimerId preview_timer; /* chunked whole-literal continuation */
    u32 preview_win_id;
    u64 preview_at;
    u64 preview_stop;
    u64 preview_origin;
    u64 preview_buf_gen;
    u32 preview_gen;
    u32 preview_ui_seq;
    bool preview_pending;
    bool preview_backward;
    bool preview_wrapped;
    bool preview_wrapscan;
    u32 preview_remaining;
    i64 wrap_until_ms; /* the 2 s indicator's deadline; 0 = not shown */
    YewReErr err;     /* live compile error; err.msg NULL means ok */
} SearchState;

void yew_search_state_init(SearchState *st);
void yew_search_state_free(SearchState *st);

void yew_search_open(Ed *ed, Win *w, bool reverse);
/* A preview continuation belongs to the key that started it.  Call before
 * handling a later key so stale work cannot move the cursor afterward. */
void yew_search_preview_cancel(Ed *ed);
/* Raw bytes must stop the timer before terminal escape disambiguation, but
 * Enter still has to accept the search that timer was completing.  Preempt
 * therefore retires only the scheduled callback and preserves its state;
 * decoded non-Enter keys call cancel before they take effect. */
void yew_search_preview_preempt(Ed *ed);
void yew_search_input(Ed *ed, Win *w);
void yew_search_accept(Ed *ed, Win *w);
void yew_search_cancel(Ed *ed, Win *w);
/* n / N.  `forward` is relative to the search's own direction, so after
 * `?foo` an `n` goes backwards. */
bool yew_search_step(Ed *ed, Win *w, bool forward, u32 count);
/* `*` and `#`: the word under the cursor, quoted and \b-wrapped. */
bool yew_search_word(Ed *ed, Win *w, bool forward);
/* Drops the highlight without disturbing the pattern register. */
void yew_search_clear_highlight(Ed *ed, Win *w);

/*
 * Schedules the bounded match count and, when a step wrapped, the
 * statusline's wrap indicator.  Both run on the Sprint 15 timer heap
 * rather than on the keystroke: counting every match IS a whole-file
 * scan, and a keystroke is not the place to do one.
 */
void yew_search_schedule_count(Ed *ed, Win *w);
/* Idle deadline for the 2 s wrap indicator; 0 when not showing. */
i64 yew_search_wrap_until(const Ed *ed);

#endif
