#ifndef SAG_SEARCH_REPLACE_H
#define SAG_SEARCH_REPLACE_H

/*
 * Sprint 21 §4: replacement templates and the substitute driver.
 *
 * Two halves that are deliberately separable:
 *
 *   sag_repl_expand   pure — a match plus a template in, bytes out.
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
    SAG_SUB_GLOBAL = 1U << 0,   /* g: every match per line          */
    SAG_SUB_CONFIRM = 1U << 1,  /* c                                 */
    SAG_SUB_COUNT_ONLY = 1U << 2, /* n: report, change nothing      */
    SAG_SUB_ICASE = 1U << 3,    /* i                                 */
    SAG_SUB_CASE = 1U << 4,     /* I                                 */
    SAG_SUB_PRESERVE = 1U << 5, /* p: infer case from the match      */
    SAG_SUB_QUIET = 1U << 6     /* e: no error when nothing matches  */
};

typedef struct SagReplErr {
    u32 off; /* byte offset into the template */
    const char *msg;
} SagReplErr;

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
bool sag_repl_expand(Bytebuf *out, const char *tpl, size_t tlen,
                     const SagReInput *in, const SagReMatch *m,
                     bool preserve_case, SagReplErr *err);

/* Validates a template without a match to expand against, so the
 * command line can reject `\q` before touching the buffer. */
bool sag_repl_check(const char *tpl, size_t tlen, u32 ngroups,
                    SagReplErr *err);

/* One planned edit: replace `span` with `text`. */
typedef struct SagReplEdit {
    Span span;
    Bytebuf text;
    LineNo line;
    bool accepted;
} SagReplEdit;

typedef struct SagReplPlan {
    SagReplEdit *v;
    u32 len;
    u32 cap;
    u32 lines; /* distinct lines touched */
} SagReplPlan;

void sag_repl_plan_init(SagReplPlan *p);
void sag_repl_plan_free(SagReplPlan *p);

/*
 * Finds every match of `re` in lines [lo, hi] and expands the template
 * for each.  Without SAG_SUB_GLOBAL only the first match on each line
 * is planned.  Every entry starts accepted; confirm mode clears the
 * ones the user declines.
 */
bool sag_repl_plan_build(SagReplPlan *p, const SagRe *re, const TextBuf *tb,
                         LineNo lo, LineNo hi, const char *tpl,
                         size_t tlen, u32 flags, SagReplErr *err);

/*
 * Applies the accepted entries back-to-front inside one transaction, so
 * earlier offsets stay valid without adjustment.  Returns the number
 * applied.
 */
u32 sag_repl_plan_apply(SagReplPlan *p, EditCtx *ec);

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
typedef struct SagReplConfirm {
    SagReplPlan *plan;
    u32 at;
    bool done;
} SagReplConfirm;

void sag_repl_confirm_begin(SagReplConfirm *c, SagReplPlan *p);
bool sag_repl_confirm_pending(const SagReplConfirm *c);
const SagReplEdit *sag_repl_confirm_current(const SagReplConfirm *c);
/* Returns false when the key is not an answer (^E/^Y scroll while
 * deciding and deliberately do not advance). */
bool sag_repl_confirm_answer(SagReplConfirm *c, u8 key);

#endif
