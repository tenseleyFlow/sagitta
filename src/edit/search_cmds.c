/*
 * Sprint 21 §4: the `:s` surface.
 *
 * Sprint 18's grammar hands the whole substitution body over as ONE
 * opaque string argument, because its tokenizer must not try to
 * understand `/` inside a regex.  Splitting on the delimiter is this
 * file's job and nothing else's.
 */
#include "edit/search_cmds.h"

#include <string.h>

#include "edit/ed.h"
#include "search/replace.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "ui/message.h"
#include "util/log.h"

typedef struct SubParts {
    const char *pat;
    size_t patlen;
    const char *rep;
    size_t replen;
    u32 flags;
} SubParts;

/*
 * Any non-alphanumeric, non-backslash delimiter is allowed, so
 * `:s#a#b#` works when the pattern is full of slashes.  Excluding
 * alphanumerics is what keeps `:s` (no body at all) unambiguous, and
 * excluding backslash keeps the escape rule readable.
 */
static bool delim_ok(u8 c)
{
    if (c == '\\' || c == '\0')
        return false;
    if (c >= '0' && c <= '9')
        return false;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return false;
    return c > ' ' && c < 0x7FU;
}

/* Scans to the next unescaped delimiter, leaving escapes in place: the
 * regex and template parsers own their own escapes, and consuming them
 * here would eat the backslash out of `\/`. */
static size_t scan_field(const char *s, size_t len, size_t at, u8 delim)
{
    while (at < len) {
        if (s[at] == '\\' && at + 1U < len) {
            at += 2U;
            continue;
        }
        if ((u8)s[at] == delim)
            return at;
        at++;
    }
    return len;
}

static bool parse_flags(const char *s, size_t len, u32 *flags,
                        const char **err)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        switch (s[i]) {
        case 'g': *flags |= SAG_SUB_GLOBAL; break;
        case 'c': *flags |= SAG_SUB_CONFIRM; break;
        case 'n': *flags |= SAG_SUB_COUNT_ONLY; break;
        case 'i': *flags |= SAG_SUB_ICASE; break;
        case 'I': *flags |= SAG_SUB_CASE; break;
        case 'p': *flags |= SAG_SUB_PRESERVE; break;
        case 'e': *flags |= SAG_SUB_QUIET; break;
        case ' ': case '\t': break;
        default:
            *err = "unknown :s flag";
            return false;
        }
    }
    if ((*flags & SAG_SUB_ICASE) != 0U && (*flags & SAG_SUB_CASE) != 0U) {
        *err = ":s flags i and I contradict each other";
        return false;
    }
    return true;
}

static bool parse_sub(const char *body, size_t len, SubParts *out,
                      const char **err)
{
    u8 delim;
    size_t p_end;
    size_t r_end;

    (void)memset(out, 0, sizeof(*out));
    if (len == 0U) {
        *err = "usage: :s/pattern/replacement/flags";
        return false;
    }
    delim = (u8)body[0];
    if (!delim_ok(delim)) {
        *err = ":s delimiter must be punctuation, not a letter, digit "
               "or backslash";
        return false;
    }
    p_end = scan_field(body, len, 1U, delim);
    out->pat = body + 1;
    out->patlen = p_end - 1U;
    if (p_end >= len) {
        /* `:s/pat` — no replacement given, which means delete it. */
        out->rep = body + len;
        out->replen = 0U;
        return true;
    }
    r_end = scan_field(body, len, p_end + 1U, delim);
    out->rep = body + p_end + 1U;
    out->replen = r_end - (p_end + 1U);
    if (r_end >= len)
        return true;
    return parse_flags(body + r_end + 1U, len - (r_end + 1U), &out->flags,
                       err);
}

/* The range, defaulting to the cursor's line — `:s` with no range is
 * "this line", which is what every `:s` a user types means. */
static bool sub_range(CmdCtx *cx, LineNo *lo, LineNo *hi)
{
    const TextBuf *tb = cx->win->buf->tb;
    u64 nlines = sag_textbuf_line_count(tb);

    if (nlines == 0U)
        return false;
    switch (cx->range.kind) {
    case SAG_RANGE_BUFFER:
        *lo = LINENO(0U);
        *hi = LINENO(nlines - 1U);
        return true;
    case SAG_RANGE_LINES:
        *lo = cx->range.lo;
        *hi = cx->range.hi;
        return true;
    default: {
        const Cursor *c = sag_ed_cursor(cx->ed);

        if (c == NULL)
            return false;
        *lo = sag_textbuf_line_of(tb, c->pos);
        *hi = *lo;
        return true;
    }
    }
}

CmdStatus sag_search_cmd_replace(CmdCtx *cx)
{
    SubParts parts;
    const char *err = NULL;
    SearchOpts opts;
    SagRe *re;
    SagReErr rerr;
    SagReplPlan plan;
    SagReplErr terr;
    LineNo lo;
    LineNo hi;
    Arena arena;
    CmdStatus status = SAG_CMD_OK;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->buf->tb == NULL)
        return SAG_CMD_ERR_STATE;
    if (!parse_sub(cx->sarg == NULL ? "" : cx->sarg,
                   cx->sarg == NULL ? 0U : cx->sarg_len, &parts, &err)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s", err);
        return SAG_CMD_ERR_ARG;
    }
    if (!sub_range(cx, &lo, &hi))
        return SAG_CMD_ERR_STATE;

    opts = cx->ed->search_opts;
    if ((parts.flags & SAG_SUB_ICASE) != 0U) {
        opts.ignorecase = true;
        opts.smartcase = false;
    } else if ((parts.flags & SAG_SUB_CASE) != 0U) {
        opts.ignorecase = false;
    }

    arena_init(&arena);
    /*
     * An empty pattern reuses the last search — `:s//new/` after a `/`
     * is how you replace what you just found without retyping it.
     */
    if (parts.patlen == 0U) {
        const RegVal *slash = sag_reg_get(&cx->ed->regs, (u8)'/');

        if (slash == NULL || slash->bytes.len == 0U) {
            sag_msg(cx->ed, SAG_MSG_ERROR, "no previous search pattern");
            arena_free_all(&arena);
            return SAG_CMD_ERR_STATE;
        }
        parts.pat = (const char *)slash->bytes.data;
        parts.patlen = slash->bytes.len;
    }
    (void)memset(&rerr, 0, sizeof(rerr));
    re = sag_search_compile(&arena, parts.pat, parts.patlen, &opts, &rerr);
    if (re == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s (at %u)",
                rerr.msg != NULL ? rerr.msg : "bad pattern",
                (unsigned)rerr.off);
        arena_free_all(&arena);
        return SAG_CMD_ERR_ARG;
    }
    /* Reject a bad template before touching the buffer. */
    (void)memset(&terr, 0, sizeof(terr));
    if (!sag_repl_check(parts.rep, parts.replen, sag_re_group_count(re),
                        &terr)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s (at %u)",
                terr.msg != NULL ? terr.msg : "bad replacement",
                (unsigned)terr.off);
        arena_free_all(&arena);
        return SAG_CMD_ERR_ARG;
    }

    sag_repl_plan_init(&plan);
    if (!sag_repl_plan_build(&plan, re, cx->win->buf->tb, lo, hi,
                             parts.rep, parts.replen, parts.flags,
                             &terr)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "%s",
                terr.msg != NULL ? terr.msg : "replacement failed");
        arena_free_all(&arena);
        return SAG_CMD_ERR_ARG;
    }
    if (plan.len == 0U) {
        /* `e` exists so a script can run a substitution that may not
         * apply without failing the whole run. */
        if ((parts.flags & SAG_SUB_QUIET) == 0U) {
            Bytebuf shown;

            bytebuf_init(&shown);
            bytebuf_append(&shown, parts.pat, parts.patlen);
            sag_msg(cx->ed, SAG_MSG_ERROR, "pattern not found: %.*s",
                    (int)shown.len, (const char *)shown.data);
            bytebuf_free(&shown);
            status = SAG_CMD_ERR_STATE;
        }
        sag_repl_plan_free(&plan);
        arena_free_all(&arena);
        return status;
    }
    if ((parts.flags & SAG_SUB_COUNT_ONLY) != 0U) {
        sag_msg(cx->ed, SAG_MSG_INFO, "%u matches on %u lines",
                (unsigned)plan.len, (unsigned)plan.lines);
        sag_repl_plan_free(&plan);
        arena_free_all(&arena);
        return SAG_CMD_OK;
    }
    if ((parts.flags & SAG_SUB_CONFIRM) != 0U) {
        /*
         * Confirm hands the plan to the interactive walker, which the
         * key handler drives.  The plan and its arena outlive this call
         * and are freed when the run ends.
         */
        sag_search_confirm_start(cx->ed, &plan, parts.rep, parts.replen);
        arena_free_all(&arena);
        return SAG_CMD_OK;
    }
    {
        EditCtx ec = sag_ed_edit_ctx(cx->ed);
        u32 n = sag_repl_plan_apply(&plan, &ec);

        sag_ed_finish_edit(cx->ed, &ec);
        sag_msg(cx->ed, SAG_MSG_INFO, "%u replacements on %u lines",
                (unsigned)n, (unsigned)plan.lines);
        sag_ed_damage_document(cx->ed);
    }
    sag_repl_plan_free(&plan);
    arena_free_all(&arena);
    return SAG_CMD_OK;
}

/*
 * `:g` is a permanent non-goal, and the message says so rather than
 * pretending it is coming: a bespoke global-command mini-language is
 * exactly the thing Fletch exists instead of.
 */
CmdStatus sag_search_cmd_global(CmdCtx *cx)
{
    sag_msg(cx->ed, SAG_MSG_ERROR,
            ":g is Fletch's query API in Sprint 34 (buf.find(re) mapped "
            "over an action); a bespoke global-command mini-language is "
            "a permanent non-goal");
    /*
     * ERR_ARG, not ERR_DEFERRED: this is a rejection with a reason, and
     * the deferred path replaces the message with a generic "lands in
     * Sprint N" that says none of it.
     */
    return SAG_CMD_ERR_ARG;
}

/* ---------------------------------------------------------------- */
/* Confirm run                                                      */
/* ---------------------------------------------------------------- */

static void confirm_prompt(Ed *ed)
{
    const SagReplEdit *cur = sag_repl_confirm_current(&ed->confirm.walk);
    const Cursor *c;

    if (cur == NULL)
        return;
    /* Put the cursor on the match being asked about, so the highlight
     * and the question agree about which one it is. */
    c = sag_ed_cursor(ed);
    if (c != NULL)
        sag_ed_cursor(ed)->pos = BYTEOFF(cur->span.lo);
    sag_win_follow_cursor(ed->win);
    sag_ed_damage_document(ed);
    sag_msg(ed, SAG_MSG_INFO, "replace with '%.*s'? (y/n/a/q/l/^E/^Y)",
            (int)ed->confirm.shown.len,
            (const char *)ed->confirm.shown.data);
}

static void confirm_finish(Ed *ed)
{
    EditCtx ec = sag_ed_edit_ctx(ed);
    u32 n;

    /*
     * One transaction for the whole run, including a run the user ended
     * with `q`: their answers up to that point are one act, and undoing
     * them should be one keystroke.
     */
    n = sag_repl_plan_apply(&ed->confirm.plan, &ec);
    sag_ed_finish_edit(ed, &ec);
    sag_msg(ed, SAG_MSG_INFO, "%u replacements on %u lines", (unsigned)n,
            (unsigned)ed->confirm.plan.lines);
    sag_repl_plan_free(&ed->confirm.plan);
    bytebuf_free(&ed->confirm.shown);
    ed->confirm.active = false;
    sag_ed_damage_document(ed);
}

void sag_search_confirm_start(Ed *ed, SagReplPlan *plan, const char *rep,
                              size_t replen)
{
    if (ed == NULL || plan == NULL)
        return;
    if (ed->confirm.active)
        sag_search_confirm_cancel(ed);
    ed->confirm.plan = *plan;
    (void)memset(plan, 0, sizeof(*plan)); /* ownership moved */
    bytebuf_init(&ed->confirm.shown);
    bytebuf_append(&ed->confirm.shown, rep, replen);
    sag_repl_confirm_begin(&ed->confirm.walk, &ed->confirm.plan);
    ed->confirm.active = true;
    if (!sag_repl_confirm_pending(&ed->confirm.walk)) {
        confirm_finish(ed);
        return;
    }
    confirm_prompt(ed);
}

bool sag_search_confirm_key(Ed *ed, u8 key)
{
    if (ed == NULL || !ed->confirm.active)
        return false;
    if (!sag_repl_confirm_answer(&ed->confirm.walk, key)) {
        /* ^E/^Y scroll while deciding; anything else is ignored rather
         * than being taken as an answer, because guessing here edits
         * the buffer. */
        return true;
    }
    if (sag_repl_confirm_pending(&ed->confirm.walk))
        confirm_prompt(ed);
    else
        confirm_finish(ed);
    return true;
}

void sag_search_confirm_reprompt(Ed *ed)
{
    if (ed == NULL || !ed->confirm.active)
        return;
    if (sag_repl_confirm_pending(&ed->confirm.walk))
        confirm_prompt(ed);
}

void sag_search_confirm_cancel(Ed *ed)
{
    if (ed == NULL || !ed->confirm.active)
        return;
    sag_repl_plan_free(&ed->confirm.plan);
    bytebuf_free(&ed->confirm.shown);
    ed->confirm.active = false;
}

/* ---------------------------------------------------------------- */
/* Search commands                                                  */
/* ---------------------------------------------------------------- */

CmdStatus sag_search_cmd_open(CmdCtx *cx)
{
    /* `:search pat` seeds and accepts in one step; `/` opens the
     * prompt.  Both go through the same state. */
    sag_search_open(cx->ed, cx->win, false);
    return SAG_CMD_OK;
}

CmdStatus sag_search_cmd_open_back(CmdCtx *cx)
{
    sag_search_open(cx->ed, cx->win, true);
    return SAG_CMD_OK;
}

static u32 step_count(const CmdCtx *cx)
{
    return cx->count_given && cx->count != 0U ? cx->count : 1U;
}

CmdStatus sag_search_cmd_next(CmdCtx *cx)
{
    (void)sag_search_step(cx->ed, cx->win, true, step_count(cx));
    return SAG_CMD_OK;
}

CmdStatus sag_search_cmd_prev(CmdCtx *cx)
{
    (void)sag_search_step(cx->ed, cx->win, false, step_count(cx));
    return SAG_CMD_OK;
}

CmdStatus sag_search_cmd_word_next(CmdCtx *cx)
{
    (void)sag_search_word(cx->ed, cx->win, true);
    return SAG_CMD_OK;
}

CmdStatus sag_search_cmd_word_prev(CmdCtx *cx)
{
    (void)sag_search_word(cx->ed, cx->win, false);
    return SAG_CMD_OK;
}

CmdStatus sag_search_cmd_clear_highlight(CmdCtx *cx)
{
    sag_search_clear_highlight(cx->ed, cx->win);
    return SAG_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* Named marks (Sprint 21 §7, closing the Sprint 18 deferral)       */
/* ---------------------------------------------------------------- */

/* The name comes from the captured key (`ma`) or from the argument
 * (`:mark a`), so both front doors reach the same table. */
static u8 mark_name_arg(const CmdCtx *cx)
{
    if (cx->sarg != NULL && cx->sarg_len == 1U)
        return (u8)cx->sarg[0];
    return 0U;
}

CmdStatus sag_mark_cmd_set(CmdCtx *cx)
{
    u8 name = mark_name_arg(cx);
    const Cursor *c;

    if (cx->win == NULL || cx->win->buf == NULL)
        return SAG_CMD_ERR_STATE;
    c = sag_ed_cursor(cx->ed);
    if (c == NULL)
        return SAG_CMD_ERR_STATE;
    if (!sag_ed_mark_set(cx->ed, cx->win->buf, name, c->pos)) {
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "mark names are a single letter a-z");
        return SAG_CMD_ERR_ARG;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_mark_cmd_jump(CmdCtx *cx)
{
    u8 name = mark_name_arg(cx);
    ByteOff at;
    Cursor *c;

    if (cx->win == NULL || cx->win->buf == NULL)
        return SAG_CMD_ERR_STATE;
    if (!sag_ed_mark_get(cx->ed, cx->win->buf, name, &at)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "mark not set");
        return SAG_CMD_ERR_STATE;
    }
    c = sag_ed_cursor(cx->ed);
    if (c == NULL)
        return SAG_CMD_ERR_STATE;
    /*
     * A mark jump does NOT push the jumplist — §5's push table pins
     * that, on the grounds that the list is navigation history and a
     * mark jump is how you navigate it.
     */
    c->pos = at;
    c->goal_col = (GCol){0U};
    sag_win_follow_cursor(cx->win);
    sag_ed_damage_document(cx->ed);
    return SAG_CMD_OK;
}
