#ifndef YEW_SEARCH_REPLACE_H
#define YEW_SEARCH_REPLACE_H

/*
 * Sprint 21 §4: replacement templates and the substitute driver.
 *
 * Two halves that are deliberately separable:
 *
 *   yew_repl_expand   pure — a match plus a template in, bytes out.
 *   plan/apply        finds every match in a range, then applies the
 *                     whole set back-to-front inside ONE transaction.
 *
 * The split is what makes confirm mode safe.  A confirm run presents
 * matches front-to-back but records decisions rather than editing as it
 * goes, so no answer can invalidate a later match's offsets mid-run —
 * and the whole run, including one the user ends with `q`, collapses to
 * a single undo step.  One undo step per match means undoing a
 * 400-match replace takes 400 keystrokes, which is how people stop
 * trusting undo.
 */

#include "search/regex.h"
#include "text/coords.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct TextBuf TextBuf;
typedef struct EditCtx EditCtx;

enum {
    YEW_SUB_GLOBAL = 1U << 0,   /* g: every match per line          */
    YEW_SUB_CONFIRM = 1U << 1,  /* c                                 */
    YEW_SUB_COUNT_ONLY = 1U << 2, /* n: report, change nothing      */
    YEW_SUB_ICASE = 1U << 3,    /* i                                 */
    YEW_SUB_CASE = 1U << 4,     /* I                                 */
    YEW_SUB_PRESERVE = 1U << 5, /* p: infer case from the match      */
    YEW_SUB_QUIET = 1U << 6     /* e: no error when nothing matches  */
};

typedef struct YewReplErr {
    u32 off; /* byte offset into the template */
    const char *msg;
} YewReplErr;

/*
 * Expands `tpl` for one match, appending to `out`.
 *
 * `&` is a LITERAL ampersand, deliberately unlike vim and sed: `&`
 * appears in real text far more often than it is wanted as a
 * backreference, and `\0` already covers the need.  Documented in the
 * manual as a known difference.
 *
 * With `preserve_case`, the shape of the matched text is imposed on the
 * result: all-uppercase (two or more cased characters) uppercases,
 * Titlecase titlecases, anything else is verbatim.
 */
bool yew_repl_expand(Bytebuf *out, const char *tpl, size_t tlen,
                     const YewReInput *in, const YewReMatch *m,
                     bool preserve_case, YewReplErr *err);

/* Validates a template without a match to expand against, so the
 * command line can reject `\q` before touching the buffer. */
bool yew_repl_check(const char *tpl, size_t tlen, u32 ngroups,
                    YewReplErr *err);

/* One planned edit: replace `span` with `text`. */
typedef struct YewReplEdit {
    Span span;
    Bytebuf text;
    LineNo line;
    bool accepted;
} YewReplEdit;

typedef struct YewReplPlan {
    YewReplEdit *v;
    u32 len;
    u32 cap;
    u32 lines; /* distinct lines touched */
} YewReplPlan;

void yew_repl_plan_init(YewReplPlan *p);
void yew_repl_plan_free(YewReplPlan *p);

/*
 * Finds every match of `re` in lines [lo, hi] and expands the template
 * for each.  Without YEW_SUB_GLOBAL only the first match on each line
 * is planned.  Every entry starts accepted; confirm mode clears the
 * ones the user declines.
 */
bool yew_repl_plan_build(YewReplPlan *p, const YewRe *re, const TextBuf *tb,
                         LineNo lo, LineNo hi, const char *tpl,
                         size_t tlen, u32 flags, YewReplErr *err);

/*
 * Applies the accepted entries back-to-front inside one transaction, so
 * earlier offsets stay valid without adjustment.  Returns the number
 * applied.
 */
u32 yew_repl_plan_apply(YewReplPlan *p, EditCtx *ec);

/*
 * Confirm mode (`c`).  Walks the plan FRONT to back asking about each
 * match, recording answers into the plan without touching the buffer;
 * the apply pass then runs back-to-front as usual.  Presenting forward
 * and applying backward is the pinned choice — it means no answer can
 * invalidate a later match's offsets mid-run.
 *
 * Answers: y accept, n skip, a accept all remaining, q stop keeping
 * what was already approved, l accept this one and stop.  A run ended
 * with q is still exactly one transaction.
 */
typedef struct YewReplConfirm {
    YewReplPlan *plan;
    u32 at;
    bool done;
} YewReplConfirm;

void yew_repl_confirm_begin(YewReplConfirm *c, YewReplPlan *p);
bool yew_repl_confirm_pending(const YewReplConfirm *c);
const YewReplEdit *yew_repl_confirm_current(const YewReplConfirm *c);
/* Returns false when the key is not an answer (^E/^Y scroll while
 * deciding and deliberately do not advance). */
bool yew_repl_confirm_answer(YewReplConfirm *c, u8 key);

#endif
