#include "edit/search_cmds.h"
#include "search/searchui.h"
#include "ui/cmdline.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/motion.h"
#include "edit/option.h"
#include "edit/sel_actions.h"
#include "fl/flruntime.h"
#include "term/grid.h"
#include "text/edit.h"
#include "text/register.h"
#include "ui/compfish.h"
#include "ui/compgen.h"
#include "ui/comphelp.h"
#include "ui/compspec.h"
#include "ui/draw.h"
#include "ui/shctx.h"
#include "ui/message.h"
#include "ui/statusline.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "util/log.h"

enum {
    YEW_CMDLINE_TABWIDTH = 4,
    YEW_CMDLINE_MENU_ROWS = 5,
    /*
     * Budget for a refilter that runs inside a keystroke.  Tab passes 0
     * (unlimited) because the user asked a question and is waiting for
     * the answer; typing has to stay inside the frame.
     *
     * 1500, not invariant 4's whole 5000.  The budget bounds the SCAN
     * only, and the keystroke still has to rank what was read, pick its
     * survivors and draw them — about 0.8 ms in perf-cmdcomp's
     * 10 000-entry directory on a CI runner.  Spending the entire 5 ms
     * on the scan would leave the paint outside the budget and turn a
     * gate that measures keypress-to-paint into one that passes while
     * the frame is late.  Whatever the slice does not finish, the idle
     * tick picks up.
     */
    YEW_CMDLINE_LIVE_BUDGET_US = 1500
};

/* Sprint 57.25 §6: the prompt's line changed since the idle path last
 * considered prewarming its command's help. */
static bool comp_idle_dirty;

static bool parse_option_value(const OptDesc *desc, const char *text,
                               OptVal *out)
{
    char *end = NULL;
    long long integer;

    if (desc->type == (u8)YEW_OPT_BOOL) {
        if (strcmp(text, "true") == 0) {
            *out = (OptVal){YEW_OPT_BOOL, {.b = true}};
            return true;
        }
        if (strcmp(text, "false") == 0) {
            *out = (OptVal){YEW_OPT_BOOL, {.b = false}};
            return true;
        }
        return false;
    }
    if (desc->type == (u8)YEW_OPT_INT) {
        errno = 0;
        integer = strtoll(text, &end, 10);
        if (errno != 0 || end == text || *end != '\0')
            return false;
        *out = (OptVal){YEW_OPT_INT, {.i = (i64)integer}};
        return true;
    }
    if (desc->type == (u8)YEW_OPT_STRLIST)
        return false;
    *out = (OptVal){desc->type,
                    {.str = {text, (u32)strlen(text)}}};
    return true;
}

CmdStatus yew_opt_cmdline_set(CmdCtx *cx)
{
    const OptDesc *desc;
    const char *err = NULL;
    OptVal value;

    if (cx == NULL || cx->ed == NULL || cx->argv.n != 3U)
        return YEW_CMD_ERR_ARG;
    desc = yew_opt_desc_for(cx->ed, cx->argv.v[1],
                            (u32)strlen(cx->argv.v[1]));
    if (desc == NULL || !parse_option_value(desc, cx->argv.v[2], &value))
        return YEW_CMD_ERR_ARG;
    if (!yew_opt_set(cx->ed, YEW_OPT_SCOPE_DECLARED,
                     cx->argv.v[1], (u32)strlen(cx->argv.v[1]),
                     &value, &err)) {
        if (err != NULL)
            yew_log(YEW_LOG_ERROR, ":set: %s", err);
        return YEW_CMD_ERR_ARG;
    }
    return YEW_CMD_OK;
}

typedef struct CmdLineTarget {
    Buffer buffer;
    Win win;
} CmdLineTarget;

/* Defined with the drawing helpers; §7 needs it above them. */
static void text_copy_span(const TextBuf *tb, Span span, u8 *out);

static CmdLineTarget *cmdline_target(const CmdLine *line)
{
    return line == NULL ? NULL : line->target;
}

static char *text_string(const TextBuf *tb)
{
    TextIter it;
    u64 total = yew_textbuf_len(tb);
    u64 copied = 0U;
    char *text;

    if (total > (u64)SIZE_MAX - 1U)
        YEW_BUG("command line exceeds address space");
    text = yew_xmalloc((size_t)total + 1U);
    if (total != 0U) {
        if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
            YEW_BUG("cannot iterate command line");
        while (copied < total) {
            const u8 *bytes;
            u64 available;
            u64 take;

            if (!yew_textiter_chunk(&it, tb, &bytes, &available) ||
                available == 0U)
                YEW_BUG("command line iterator ended early");
            take = available < total - copied ? available : total - copied;
            (void)memcpy(text + (size_t)copied, bytes, (size_t)take);
            copied += take;
            if (copied < total && !yew_textiter_advance(&it, tb))
                YEW_BUG("command line iterator advance failed");
        }
    }
    text[(size_t)total] = '\0';
    return text;
}

static void sync_from_target(CmdLine *line)
{
    CmdLineTarget *target = cmdline_target(line);

    if (target != NULL && target->win.cs.curs.len != 0U &&
        target->win.cs.primary < target->win.cs.curs.len)
        line->cur = target->win.cs.curs.data[target->win.cs.primary];
}

static void sync_to_target(CmdLine *line)
{
    CmdLineTarget *target = cmdline_target(line);

    if (target != NULL && target->win.cs.curs.len != 0U &&
        target->win.cs.primary < target->win.cs.curs.len)
        target->win.cs.curs.data[target->win.cs.primary] = line->cur;
}

static void cmdline_target_free(CmdLineTarget *target)
{
    if (target == NULL)
        return;
    yew_vp_free(&target->win);
    yew_cset_free(&target->win.cs);
    yew_syn_detach(&target->buffer.syn);
    yew_marks_free(target->buffer.marks);
    yew_undo_free(target->buffer.undo);
    yew_textbuf_free(target->buffer.tb);
    yew_filemeta_dispose(&target->buffer.meta);
    yew_xfree(target);
}

static void menu_discard(Ed *ed)
{
    CmdLine *line = &ed->cmdline;
    bool was_open = line->menu.items.len != 0U || line->menu.sel >= 0;

    yew_menu_dismiss(&line->menu);
    /* The cached candidate set's strings live in this arena, so the
     * cache dies with it -- a surviving `valid` flag over freed strings
     * is a use-after-free waiting for the next keystroke. */
    yew_comp_filter_invalidate(&line->filter);
    /* The directory listing outlives individual keystrokes but not the
     * menu: a prompt opened later must see the directory as it is now. */
    yew_comp_listing_invalidate();
    arena_free_all(&line->comp_arena);
    yew_xfree(line->menu_stem);
    line->menu_stem = NULL;
    line->menu_original = (Span){0U, 0U};
    line->comp_total = 0U;
    line->comp_asked = false;
    if (was_open)
        ed->full_damage = true;
}

static void clear_error(Ed *ed)
{
    if (ed->cmdline.err.msg[0] != '\0') {
        ed->cmdline.err = (CmdErr){0};
        ed->full_damage = true;
    }
    if (ed->msg.active && ed->msg.sev == YEW_MSG_ERROR)
        yew_msg_clear(ed);
}

static void set_error(Ed *ed, const CmdErr *error)
{
    CmdLine *line = &ed->cmdline;
    u64 len = yew_textbuf_len(line->buf);
    u64 at;

    /*
     * The menu and the message share the rows above the prompt, and the
     * menu wins when both are present.  Under live filtering the menu is
     * open almost always, so an error would simply never be seen -- s18
     * §7's whole contract is that the prompt stays open WITH the message
     * and the offending token highlighted.  The error is about the line
     * as typed, so a stale candidate list underneath it is noise: it
     * goes, and the next edit brings the list back.
     */
    yew_menu_dismiss(&line->menu);
    line->err = *error;
    if (len != 0U) {
        if (line->err.tok_lo >= len)
            line->err.tok_lo = (u32)(len - 1U);
        if (line->err.tok_hi <= line->err.tok_lo)
            line->err.tok_hi = line->err.tok_lo + 1U;
        if (line->err.tok_hi > len)
            line->err.tok_hi = (u32)len;
    }
    at = len == 0U ? 0U : line->err.tok_lo;
    line->cur.pos = BYTEOFF(at);
    line->cur.anchor = line->cur.pos;
    yew_cursor_clamp(line->buf, &line->cur);
    sync_to_target(line);
    yew_msg(ed, YEW_MSG_ERROR, "E: %s", line->err.msg);
    ed->full_damage = true;
    ed->footer_dirty = true;
}

static void sanitize_bytes(const u8 *bytes, size_t len, Bytebuf *out)
{
    size_t i;
    bool newline_run = false;

    for (i = 0U; i < len; i++) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            if (!newline_run)
                bytebuf_push_u8(out, (u8)' ');
            newline_run = true;
        } else {
            bytebuf_push_u8(out, bytes[i]);
            newline_run = false;
        }
    }
}

static CmdStatus invoke_prompt_text(Ed *ed, const u8 *bytes, size_t len)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup("ed.edit.insert.text", 19U);
    CmdStatus status;

    if (len > UINT32_MAX)
        len = UINT32_MAX;
    cx.win = yew_cmdline_target(ed);
    cx.count = 1U;
    cx.sarg = (const char *)bytes;
    cx.sarg_len = (u32)len;
    cx.source = YEW_SRC_KEY;
    status = yew_ed_invoke(ed, id, &cx);
    sync_from_target(&ed->cmdline);
    return status;
}

bool yew_cmdline_clean(Ed *ed, const Win *win, const u8 *b, size_t n,
                       Bytebuf *out)
{
    if (ed == NULL || !ed->cmdline.active ||
        win != yew_cmdline_target(ed)) {
        bytebuf_append(out, b, n);
        return true;
    }
    if (n != 0U && memchr(b, '\0', n) != NULL) {
        yew_msg(ed, YEW_MSG_ERROR,
                "NUL byte is not valid in a command line");
        return false;
    }
    sanitize_bytes(b, n, out);
    return true;
}

static CmdStatus insert_sanitized(Ed *ed, const u8 *bytes, size_t len)
{
    Bytebuf clean;
    CmdStatus status;

    bytebuf_init(&clean);
    if (!yew_cmdline_clean(ed, yew_cmdline_target(ed), bytes, len,
                           &clean)) {
        bytebuf_free(&clean);
        return YEW_CMD_ERR_ARG;
    }
    status = invoke_prompt_text(ed, clean.data, clean.len);
    bytebuf_free(&clean);
    return status;
}

static bool replace_span(Ed *ed, Span span, const u8 *bytes, size_t len,
                         bool reset_history)
{
    CmdLine *line = &ed->cmdline;
    CmdLineTarget *target = cmdline_target(line);
    EditCtx ec;
    bool ok = true;

    if (target == NULL || span.lo > span.hi ||
        span.hi > yew_textbuf_len(line->buf))
        return false;
    sync_to_target(line);
    ec = yew_ed_edit_ctx_for(ed, &target->win);
    yew_undo_begin(&ec, YEW_TXN_TYPE);
    if (span.lo != span.hi)
        ok = yew_edit_delete(&ec, span);
    if (ok && len != 0U)
        ok = yew_edit_insert(&ec, BYTEOFF(span.lo), bytes, (u64)len);
    if (ok)
        yew_undo_end(&ec);
    else
        yew_undo_abort(&ec);
    yew_ed_finish_edit(ed, &ec);
    if (!ok)
        return false;
    line->cur.pos = BYTEOFF(span.lo + len);
    line->cur.anchor = line->cur.pos;
    line->cur.goal_col = (CCol){0U};
    sync_to_target(line);
    /* Sprint 57.30 §2: an edit ends the history walk; the next Up begins
     * a new one from the line as it is then. */
    if (reset_history)
        yew_hist_walk_end(&line->walk);
    clear_error(ed);
    ed->footer_dirty = true;
    return true;
}

static bool replace_all(Ed *ed, const char *text, bool reset_history)
{
    return replace_span(ed, (Span){0U, yew_textbuf_len(ed->cmdline.buf)},
                        (const u8 *)text, strlen(text), reset_history);
}

static void set_cmd_register(Ed *ed, const char *text)
{
    yew_reg_set_cmdline(&ed->regs, (const u8 *)text, strlen(text));
}

static const char *history_kind(YewPromptKind kind)
{
    switch (kind) {
    case YEW_PROMPT_CMD:
        return "cmd";
    case YEW_PROMPT_SEARCH_F:
    case YEW_PROMPT_SEARCH_B:
        return "search";
    case YEW_PROMPT_INPUT:
        return "input";
    }
    return "cmd";
}

static size_t history_slot(YewPromptKind kind)
{
    if (kind == YEW_PROMPT_SEARCH_F || kind == YEW_PROMPT_SEARCH_B)
        return 1U;
    return kind == YEW_PROMPT_INPUT ? 2U : 0U;
}

static CmdHist *history_open(Ed *ed, YewPromptKind kind)
{
    CmdHist **cached = &ed->cmdline.memory_history[history_slot(kind)];

    if (*cached != NULL) {
        CmdHist *history = *cached;

        *cached = NULL;
        return history;
    }
    /* Sprint 25 §8.  A stateless session (--clean, --batch, an unusable
     * state home) keeps the global history it has always had; there is no
     * workspace directory to scope to and inventing one would put state
     * where the user asked for none. */
    if (ed->clean)
        return yew_hist_open_memory();
    if (ed->state.ready) {
        const char *scope =
            yew_state_option_str(ed, "history.scope", "workspace");

        return yew_hist_open_scoped(history_kind(kind), ed->state.key.dir,
                                    strcmp(scope, "global") != 0);
    }
    return yew_hist_open(history_kind(kind));
}

static void history_release(Ed *ed, YewPromptKind kind,
                            CmdHist *history)
{
    CmdHist **cached;

    if (history == NULL)
        return;
    yew_hist_flush(history);
    if (!yew_hist_is_memory(history)) {
        yew_hist_close(history);
        return;
    }
    cached = &ed->cmdline.memory_history[history_slot(kind)];
    if (*cached != NULL)
        YEW_BUG("two live in-memory histories for one prompt kind");
    *cached = history;
}

static void history_add_closed_prompt(Ed *ed, YewPromptKind kind,
                                      const char *text)
{
    CmdHist *history;

    /* A command may replace itself with another prompt in the same history
     * family.  That successor already owns the displaced in-memory history,
     * so add there instead of manufacturing a second owner. */
    if (ed->cmdline.active &&
        history_slot(ed->cmdline.kind) == history_slot(kind)) {
        yew_hist_add(ed->cmdline.history, text);
        return;
    }
    history = history_open(ed, kind);

    yew_hist_add(history, text);
    history_release(ed, kind, history);
}

void yew_cmdline_open(Ed *ed, YewPromptKind kind, const char *seed)
{
    CmdLine *line;
    CmdLineTarget *target;
    Bytebuf clean;
    Cursor cursor;
    Mode old;

    if (ed == NULL)
        return;
    if (ed->cmdline.active)
        yew_cmdline_close(ed, false);
    old = ed->mode;
    line = &ed->cmdline;
    line->generation++;
    if (line->generation == 0U)
        line->generation = 1U;
    target = yew_xcalloc(1U, sizeof(*target));
    bytebuf_init(&clean);
    if (seed != NULL)
        sanitize_bytes((const u8 *)seed, strlen(seed), &clean);
    yew_filemeta_init(&target->buffer.meta);
    yew_syn_buf_init(&target->buffer.syn);
    target->buffer.tb = yew_textbuf_from_bytes(clean.data, clean.len);
    target->buffer.tabwidth = YEW_CMDLINE_TABWIDTH;
    target->buffer.undo = yew_undo_new(target->buffer.tb);
    target->buffer.marks = yew_marks_new();
    cursor = (Cursor){BYTEOFF(clean.len), {0U}, BYTEOFF(clean.len)};
    yew_cset_init(&target->win.cs, cursor);
    target->win.buf = &target->buffer;
    yew_vp_init(&target->win);
    bytebuf_free(&clean);

    line->kind = kind;
    line->active = true;
    line->buf = target->buffer.tb;
    line->cur = cursor;
    line->target = target;
    line->return_mode = (u8)ed->mode;
    line->history = history_open(ed, kind);
    {
        /* Inline, five rows, wrapping, detail at column 31 -- the
         * geometry Sprint 18's goldens pinned, now expressed as a spec
         * so Sprint 26's picker can pick a different one. */
        MenuSpec spec = {NULL, 5U, true, true, 31U};

        yew_menu_init(&line->menu, &spec);
    }
    yew_comp_filter_init(&line->filter);
    /* -1, not 0: a zeroed field would make the very first click on row 0
     * read as the SECOND click and accept it outright. */
    line->click_row = -1;
    yew_hist_walk_end(&line->walk);
    line->walk_prefix = 0U;
    line->walk_bang = false;
    line->err = (CmdErr){0};
    line->scroll = 0U;
    yew_hist_suggest_init(&line->suggest);
    line->suggest_loaded = false;
    comp_idle_dirty = true;
    yew_msg_clear(ed);
    if (old != YEW_MODE_E) {
        yew_fl_hook_mode(ed, FL_EV_MODE_LEAVE, yew_modes[old].name);
        yew_dispatch_set_mode(ed, YEW_MODE_E);
        yew_fl_hook_mode(ed, FL_EV_MODE_ENTER, yew_modes[YEW_MODE_E].name);
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
}

void yew_cmdline_open_input(Ed *ed, const char *seed,
                            YewCmdlineInputDone done, void *ctx)
{
    if (ed == NULL)
        return;
    yew_cmdline_open(ed, YEW_PROMPT_INPUT, seed);
    if (ed->cmdline.active) {
        ed->cmdline.input_done = done;
        ed->cmdline.input_ctx = ctx;
    }
}

void yew_cmdline_close(Ed *ed, bool accepted)
{
    CmdLine *line;
    YewCmdlineInputDone input_done;
    void *input_ctx;
    char *input_text = NULL;
    size_t input_len = 0U;
    Mode restore;
    bool keep_message;

    if (ed == NULL || !ed->cmdline.active)
        return;
    line = &ed->cmdline;
    input_done = line->input_done;
    input_ctx = line->input_ctx;
    line->input_done = NULL;
    line->input_ctx = NULL;
    if (input_done != NULL) {
        input_len = (size_t)yew_textbuf_len(line->buf);
        input_text = text_string(line->buf);
    }
    /* A successful command may have produced the message the user needs to
     * see.  Opening the prompt already cleared older messages, so an active
     * message here belongs to the command that was just accepted. */
    keep_message = accepted && ed->msg.active &&
                   (line->kind == YEW_PROMPT_CMD ||
                    ((line->kind == YEW_PROMPT_SEARCH_F ||
                      line->kind == YEW_PROMPT_SEARCH_B) &&
                     ed->search.preview_pending));
    if (line->kind == YEW_PROMPT_SEARCH_F ||
        line->kind == YEW_PROMPT_SEARCH_B) {
        /* Accept commits the pattern and the jump; cancel restores the
         * view exactly, which is why it happens BEFORE the widget tears
         * down and repaints. */
        if (accepted)
            yew_search_accept(ed, ed->win);
        else
            yew_search_cancel(ed, ed->win);
    }
    restore = line->return_mode < YEW_MODE__N ? (Mode)line->return_mode :
                                               YEW_MODE_L;
    if (restore == YEW_MODE_E)
        restore = YEW_MODE_L;
    menu_discard(ed);
    yew_menu_free(&line->menu);
    yew_comp_filter_free(&line->filter);
    /* Sprint 57.24: generator answers and user spec files are held for
     * one prompt -- the next one must see branches made in between. */
    yew_compgen_cache_clear();
    yew_compspec_prompt_closed();
    /* Sprint 57.25: requests this prompt made and never spawned go with
     * it; learned trees stay (they are keyed by the executable). */
    yew_comphelp_prompt_closed();
    /* Sprint 57.26: likewise fish's; its answers stay fresh 5 s. */
    yew_compfish_prompt_closed();
    comp_idle_dirty = false;
    yew_hist_suggest_free(&line->suggest);
    line->suggest_loaded = false;
    history_release(ed, line->kind, line->history);
    line->history = NULL;
    yew_hist_walk_end(&line->walk);
    cmdline_target_free(cmdline_target(line));
    line->target = NULL;
    line->buf = NULL;
    line->active = false;
    line->err = (CmdErr){0};
    line->scroll = 0U;
    if (!keep_message)
        yew_msg_clear(ed);
    if (ed->mode != restore) {
        Mode old = ed->mode;

        yew_fl_hook_mode(ed, FL_EV_MODE_LEAVE, yew_modes[old].name);
        yew_dispatch_set_mode(ed, restore);
        yew_fl_hook_mode(ed, FL_EV_MODE_ENTER, yew_modes[restore].name);
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
    /* A `:s/../../c` started a confirm run whose question this close
     * just wiped; restate it. */
    yew_search_confirm_reprompt(ed);
    if (input_done != NULL) {
        input_done(ed, accepted, (const u8 *)input_text, input_len,
                   input_ctx);
        yew_xfree(input_text);
    }
}

void yew_cmdline_dispose(Ed *ed)
{
    size_t i;

    if (ed == NULL)
        return;
    if (ed->cmdline.active)
        yew_cmdline_close(ed, false);
    for (i = 0U; i < YEW_ARRAY_LEN(ed->cmdline.memory_history); i++) {
        yew_hist_close(ed->cmdline.memory_history[i]);
        ed->cmdline.memory_history[i] = NULL;
    }
}

Win *yew_cmdline_target(Ed *ed)
{
    CmdLineTarget *target;

    if (ed == NULL || !ed->cmdline.active)
        return ed == NULL ? NULL : ed->win;
    target = cmdline_target(&ed->cmdline);
    return target == NULL ? NULL : &target->win;
}

/*
 * Sprint 18.5 §6: refilter for the prompt's current text.
 *
 * The menu opens when there is a TOKEN to filter on, and closes when
 * there is not.  An empty token means a bare `:` or a fresh argument
 * position, where "every command in the registry" is noise rather than
 * an answer -- Tab still asks that question explicitly.
 *
 * A live filter that finds nothing says NOTHING.  Tab is a question and
 * deserves `no completions`; a keystroke is not, and answering every
 * unmatched character with an error makes the message line flash through
 * a word being typed.  That is Sprint 21's doctrine -- a half-typed line
 * is the normal state of a prompt -- applied to the menu.
 */
/*
 * Sprint 18.5 §9: what the parser already understands, said out loud.
 *
 * Only ever reports what it KNOWS.  An unknown command produces no hint
 * at all rather than an "unknown command" line: while the user is still
 * typing, an empty menu is already the signal, and styling the normal
 * state of a half-typed line as a failure is the flashing-message-line
 * behaviour Sprint 21's doctrine forbids.
 */
static const char *hint_arg_name(const CmdEntry *entry, u32 token_index,
                                 bool *repeats)
{
    const char *spec;
    size_t len;
    size_t at;
    char code;

    *repeats = false;
    if (entry == NULL || entry->argspec == NULL)
        return NULL;
    spec = entry->argspec;
    len = strlen(spec);
    if (len != 0U && spec[len - 1U] == '*') {
        *repeats = true;
        len--;
    }
    if (len == 0U)
        return NULL;
    /* Token 0 is the name itself, so the argument being ASKED for is the
     * first; inside argument N it is that argument's own slot. */
    at = token_index == 0U ? 0U : (size_t)token_index - 1U;
    if (at >= len) {
        if (!*repeats)
            return NULL;
        at = len - 1U;
    }
    code = spec[at];
    switch (code) {
    case 'f':
        return "<file>";
    case 'b':
        return "<buffer>";
    case 'o':
        return "<option>";
    case 'v':
        return "<value>";
    case 'p':
        return "<plugin>";
    case 's':
        return "<text>";
    default:
        break;
    }
    return NULL;
}

static void cmdline_set_hint(Ed *ed, const CmdParsePoint *point)
{
    CmdLine *line = &ed->cmdline;
    const CmdDesc *desc;
    const CmdEntry *entry;
    const char *shown;
    const char *arg;
    bool repeats = false;
    size_t at = 0U;

    line->hint[0] = '\0';
    if (!point->command_known)
        return;
    desc = yew_cmd_desc(point->command);
    entry = yew_cmd_entry(point->command);
    if (desc == NULL || entry == NULL)
        return;
    shown = strncmp(desc->name, "ed.", 3U) == 0 ? desc->name + 3U
                                                : desc->name;
    /* An abbreviation resolved to something else is the one case where
     * the user cannot see what will run, so it is spelled out. */
    if (point->stem != NULL && point->stem[0] != '\0' &&
        strcmp(point->stem, shown) != 0 && point->token_index == 0U)
        at += (size_t)snprintf(line->hint + at, sizeof(line->hint) - at,
                               "%s \xE2\x86\x92 %s", point->stem, shown);
    else
        at += (size_t)snprintf(line->hint + at, sizeof(line->hint) - at,
                               "%s", shown);
    if (at >= sizeof(line->hint))
        return;
    arg = hint_arg_name(entry, point->token_index, &repeats);
    if (arg != NULL)
        at += (size_t)snprintf(line->hint + at, sizeof(line->hint) - at,
                               " \xC2\xB7 %s%s", arg, repeats ? "\xE2\x80\xA6"
                                                             : "");
    if (at >= sizeof(line->hint) || !point->range.given)
        return;
    /*
     * The user typed the line numbers, so echoing them says nothing; how
     * many lines they resolve to is the part they cannot see.
     */
    if (point->range.kind == YEW_RANGE_BUFFER)
        (void)snprintf(line->hint + at, sizeof(line->hint) - at,
                       " \xC2\xB7 whole buffer");
    else if (point->range.kind == YEW_RANGE_SELECTION)
        (void)snprintf(line->hint + at, sizeof(line->hint) - at,
                       " \xC2\xB7 selection");
    else {
        u64 lines = point->range.hi.v - point->range.lo.v + 1U;

        (void)snprintf(line->hint + at, sizeof(line->hint) - at,
                       " \xC2\xB7 %llu line%s", (unsigned long long)lines,
                       lines == 1U ? "" : "s");
    }
}

static void cmdline_refilter_as(Ed *ed, bool asked);

/* Sprint 57.32 §4: the filter's `in ch7/` note, onto the menu. */
static void menu_where(CmdLine *line)
{
    (void)memcpy(line->menu.where, line->filter.where,
                 sizeof(line->menu.where));
    line->menu.where[sizeof(line->menu.where) - 1U] = '\0';
}

static void cmdline_refilter(Ed *ed)
{
    cmdline_refilter_as(ed, false);
}

/*
 * `asked`: a Tab (or the arrival of what a Tab asked for) is answering,
 * so an empty word still gets rows.  The live path passes false: an
 * empty token completes nothing while the user is only typing.
 */
static void cmdline_refilter_as(Ed *ed, bool asked)
{
    CmdLine *line = &ed->cmdline;
    Arena scratch;
    CmdParsePoint point;
    YewCompQuery query;
    Vec_CompItem items = {0};
    char *text;

    /* Only `:` completes; `/` and `?` carry a pattern, not a command. */
    if (line->kind != YEW_PROMPT_CMD) {
        yew_menu_dismiss(&line->menu);
        line->hint[0] = '\0';
        return;
    }
    text = text_string(line->buf);
    arena_init(&scratch);
    /* ONE tolerant parse per keystroke, read by both the hint and the
     * filter.  Two would drift apart. */
    if (!yew_cmd_parse_point(ed, text, (size_t)yew_textbuf_len(line->buf),
                             (size_t)line->cur.pos.v, &scratch, &point)) {
        yew_menu_dismiss(&line->menu);
        line->hint[0] = '\0';
        arena_free_all(&scratch);
        yew_xfree(text);
        ed->full_damage = true;
        return;
    }
    cmdline_set_hint(ed, &point);
    /*
     * Sprint 57.30 §1: a walked history entry on the line has no table
     * -- the user is reading history, not completing a word (fish does
     * the same).  The hint above still reads the entry.  The next edit
     * ends the walk and the live table comes back.
     */
    if (line->walk.on && line->walk.n_seen != 0U) {
        menu_discard(ed);
        arena_free_all(&scratch);
        yew_xfree(text);
        ed->full_damage = true;
        return;
    }
    if (!yew_comp_query_at(ed, &point, &query) ||
        (!asked && query.replace.hi <= query.replace.lo)) {
        yew_menu_dismiss(&line->menu);
        arena_free_all(&scratch);
        yew_xfree(text);
        ed->full_damage = true;
        return;
    }
    line->comp_total = yew_comp_filter_run(ed, &line->filter,
                                           &line->comp_arena, &query,
                                           YEW_CMDLINE_LIVE_BUDGET_US,
                                           &items);
    if (query.kind == YEW_COMP_SHELL &&
        (yew_compspec_notice(ed) || yew_comphelp_notice(ed)))
        ed->footer_dirty = true;
    if (items.len == 0U) {
        Vec_CompItem_free(&items);
        yew_menu_dismiss(&line->menu);
    } else {
        yew_menu_reset(&line->menu, items, line->comp_total, query.replace);
        line->menu.pending = line->filter.gen_pending;
        menu_where(line);
    }
    arena_free_all(&scratch);
    yew_xfree(text);
    ed->full_damage = true;
}

/*
 * Sprint 57.24 §5.4: a completion generator answered.  If the prompt is
 * still open AND its filter asked for exactly this key, re-rank and
 * repaint; otherwise the answer only lands in the cache (compgen did
 * that already).
 *
 * An arrival NEVER edits the line: no sole-survivor insertion, no LCP.
 * cmdline_refilter only replaces the menu's rows, and the prompt's text
 * changing under the user's fingers because a subprocess finished is
 * not acceptable.
 */
void yew_cmdline_compgen_arrived(Ed *ed, const char *key)
{
    CmdLine *line;

    if (ed == NULL || key == NULL || !ed->cmdline.active ||
        ed->cmdline.kind != YEW_PROMPT_CMD)
        return;
    line = &ed->cmdline;
    if (line->filter.gen_key == NULL || strcmp(line->filter.gen_key, key) != 0)
        return;
    /* The cached set was ranked without the answer; drop it so the
     * refilter re-enumerates, now served from the generator cache.  A
     * menu Tab opened (or a Tab still waiting) keeps its empty-word
     * answer; otherwise this is exactly the live keystroke's refilter. */
    yew_comp_filter_invalidate(&line->filter);
    cmdline_refilter_as(ed, line->comp_asked || line->menu.items.len != 0U);
    line->comp_asked = false;
    ed->footer_dirty = true;
}

/*
 * Is a sliced completion scan waiting for the idle path?
 *
 * ONE predicate, because yew_loop_deadline has to stop sleeping on
 * exactly the condition yew_cmdline_comp_tick will act on.  Two
 * spellings that disagree give either a menu that stops filling in (the
 * loop sleeps through work the tick would do) or a busy loop (the
 * deadline says "work pending" for a state the tick declines to touch).
 */
bool yew_cmdline_comp_scanning(const Ed *ed)
{
    return ed != NULL && ed->cmdline.active &&
           ed->cmdline.kind == YEW_PROMPT_CMD && yew_comp_listing_pending();
}

/*
 * Sprint 26 §7.2's idle-path pattern, applied to the completion scan.
 *
 * Called after input is drained, so a keystroke always wins the race for
 * the iteration (invariant 4) and the scan finishes behind it.
 *
 * ONE repaint, at the END of the scan -- not one per slice.  Re-ranking
 * after every slice makes the number of frames a function of how many
 * slices the FILESYSTEM needed, and a pty golden records that count:
 * s19_badge_while_running went unstable the moment this repainted per
 * slice, and every case with a path completion in it was next.  The
 * intermediate slices have nothing to show anyway -- the menu keeps the
 * first slice's rows until the set it was ranked from is complete, and
 * then updates once.
 *
 * Returns true while more remains, matching yew_picker_tick.
 */
bool yew_cmdline_comp_tick(Ed *ed)
{
    if (!yew_cmdline_comp_scanning(ed))
        return false;
    if (yew_comp_listing_advance(YEW_CMDLINE_LIVE_BUDGET_US))
        return true;
    /*
     * Complete.  The filter caches per (kind, head, pattern) and would
     * hand back the partial slice's answer unchanged; the listing it was
     * computed from has grown underneath it, so that entry is stale by
     * definition.
     */
    yew_comp_filter_invalidate(&ed->cmdline.filter);
    cmdline_refilter(ed);
    return false;
}

/*
 * Sprint 57.25 §6: the line changed since the idle path last looked at
 * it, so the prewarm has a new caret to consider.  One flag, read by the
 * loop's deadline and acted on by yew_cmdline_comp_idle -- the pair
 * share yew_cmdline_comp_idle_pending, as the scan tick's pair does.
 */
bool yew_cmdline_comp_idle_pending(const Ed *ed)
{
    if (ed == NULL || !ed->cmdline.active ||
        ed->cmdline.kind != YEW_PROMPT_CMD)
        return false;
    /* Sprint 57.26: the history snapshot is taken on the first idle turn
     * after the prompt opens, so no keystroke pays for it. */
    return comp_idle_dirty || !ed->cmdline.suggest_loaded ||
           yew_compfish_idle_ready() || yew_comphelp_idle_ready();
}

static void suggest_ensure(Ed *ed);

u32 yew_cmdline_comp_idle(Ed *ed)
{
    if (!yew_cmdline_comp_idle_pending(ed))
        return 0U;
    /* Sprint 57.26 §3: load the `:!` history snapshot now, on a turn with
     * no input.  (A line typed faster than the first idle turn loads it
     * on demand instead -- the same snapshot either way.) */
    if (!ed->cmdline.suggest_loaded)
        suggest_ensure(ed);
    if (comp_idle_dirty) {
        CmdLine *line = &ed->cmdline;
        YewCompQuery query;
        Arena scratch;
        char *text;

        comp_idle_dirty = false;
        text = text_string(line->buf);
        arena_init(&scratch);
        if (yew_comp_query(ed, text, (size_t)yew_textbuf_len(line->buf),
                           (size_t)line->cur.pos.v, &scratch, &query) &&
            query.kind == YEW_COMP_SHELL && query.shell != NULL)
            yew_comp_shell_prewarm(ed, query.shell);
        arena_free_all(&scratch);
        yew_xfree(text);
    }
    /* Sprint 57.26: fish is the rung above help, so its request goes
     * first; one spawn per idle turn, and the next turn takes the
     * other (the predicate keeps the loop from sleeping between). */
    if (yew_compfish_idle(ed) != 0U)
        return 1U;
    return yew_comphelp_idle(ed);
}

void yew_cmdline_edited(Ed *ed)
{
    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    yew_hist_walk_end(&ed->cmdline.walk);
    clear_error(ed);
    /*
     * Sprint 57.30 §1: typing leaves the table.  The SELECTION survives
     * (it is held by identity across the refilter), so Tab goes on from
     * the row it was on; the arrows are history until Tab enters the
     * table again.
     */
    yew_menu_blur(&ed->cmdline.menu);
    ed->cmdline.comp_asked = false;
    comp_idle_dirty = true;
    cmdline_refilter(ed);
    ed->footer_dirty = true;
    /* Search-as-you-type: the `/` and `?` prompts preview on every
     * edit.  This is the one place that hook belongs — the widget is
     * shared, and only these two kinds want it. */
    if (ed->cmdline.kind == YEW_PROMPT_SEARCH_F ||
        ed->cmdline.kind == YEW_PROMPT_SEARCH_B)
        yew_search_input(ed, ed->win);
}

/* The prompt's current text.  Sprint 21's search-as-you-type needs it
 * on every edit, and reaching into ed->cmdline.buf from another module
 * would make the widget's internals part of its interface. */
void yew_cmdline_text(Ed *ed, Bytebuf *out)
{
    char *text;

    if (out == NULL)
        return;
    if (ed == NULL || !ed->cmdline.active || ed->cmdline.buf == NULL)
        return;
    sync_from_target(&ed->cmdline);
    text = text_string(ed->cmdline.buf);
    if (text == NULL)
        return;
    bytebuf_append(out, text, strlen(text));
    yew_xfree(text);
}

void yew_cmdline_sync(Ed *ed)
{
    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    ed->footer_dirty = true;
}

/*
 * Sprint 57.29 §1: the prompt's selection and its collapse table.
 *
 * ONE table, applied by the dispatcher around every command it runs on
 * the prompt Win, so no command has to know a selection exists.  The
 * cursor motions keep an anchor that differs from the caret (that is
 * how H extends), so without this a plain motion would silently grow
 * the selection instead of collapsing it.
 */
typedef enum PromptSel {
    PSEL_COLLAPSE, /* collapse at the caret, then act (the default) */
    PSEL_EXTEND,   /* Shift+motion: the anchor stays */
    PSEL_KEEP,     /* reads the selection itself, or hands it on to the
                    * insert it runs (C-c, C-x, A-., C-r, C-q) */
    PSEL_START,    /* <left>, C-b: collapse to the start, no motion */
    PSEL_END,      /* <right>, C-f: collapse to the end, no accept */
    PSEL_REPLACE,  /* typing, paste, yank: the text replaces it */
    PSEL_DELETE    /* <bs>, <del>, C-d, C-h: delete it, nothing more */
} PromptSel;

typedef struct PromptSelRow {
    const char *command;
    u8 rule;
} PromptSelRow;

static const PromptSelRow prompt_sel_rows[] = {
    {"ed.clip.copy", PSEL_KEEP},
    {"ed.clip.cut", PSEL_KEEP},
    {"ed.cmdline.copy_or_cancel", PSEL_KEEP},
    {"ed.cmdline.last_arg", PSEL_KEEP},
    {"ed.cmdline.insert_register", PSEL_KEEP},
    {"ed.cmdline.literal_next", PSEL_KEEP},
    {"ed.move.char.prev", PSEL_START},
    {"ed.move.char.left", PSEL_START},
    {"ed.move.char.next", PSEL_END},
    {"ed.move.char.right", PSEL_END},
    {"ed.cmdline.ghost.accept", PSEL_END},
    {"ed.edit.insert.text", PSEL_REPLACE},
    {"ed.clip.paste", PSEL_REPLACE},
    {"ed.edit.kill.yank", PSEL_REPLACE},
    {"ed.edit.delete.grapheme_left", PSEL_DELETE},
    {"ed.edit.delete.grapheme", PSEL_DELETE},
    /* Sprint 57.30: history and the table's rows replace the line or a
     * word of it -- collapse at the caret first, like any command. */
    {"ed.cmdline.up", PSEL_COLLAPSE},
    {"ed.cmdline.down", PSEL_COLLAPSE},
    {"ed.cmdline.hist_prev", PSEL_COLLAPSE},
    {"ed.cmdline.hist_next", PSEL_COLLAPSE},
};

static PromptSel prompt_sel_rule(const char *command)
{
    size_t i;

    if (strncmp(command, "ed.sel.extend.", 14U) == 0)
        return PSEL_EXTEND;
    for (i = 0U; i < YEW_ARRAY_LEN(prompt_sel_rows); i++) {
        if (strcmp(command, prompt_sel_rows[i].command) == 0)
            return (PromptSel)prompt_sel_rows[i].rule;
    }
    return PSEL_COLLAPSE;
}

static Cursor *prompt_cursor(Ed *ed)
{
    CmdLineTarget *target;

    if (ed == NULL || !ed->cmdline.active)
        return NULL;
    target = cmdline_target(&ed->cmdline);
    if (target == NULL || target->win.cs.primary >= target->win.cs.curs.len)
        return NULL;
    return &target->win.cs.curs.data[target->win.cs.primary];
}

bool yew_cmdline_selection(Ed *ed, Span *out)
{
    const Cursor *c = prompt_cursor(ed);

    if (c == NULL || c->anchor.v == c->pos.v)
        return false;
    if (out != NULL)
        *out = c->pos.v < c->anchor.v ? (Span){c->pos.v, c->anchor.v}
                                      : (Span){c->anchor.v, c->pos.v};
    return true;
}

static void prompt_sel_collapse(Ed *ed, ByteOff at)
{
    Cursor *c = prompt_cursor(ed);

    if (c == NULL)
        return;
    c->pos = at;
    c->anchor = at;
    c->goal_col = (CCol){YEW_CCOL_HERE};
    sync_from_target(&ed->cmdline);
    ed->footer_dirty = true;
}

/* Joins the transaction the dispatcher opened for the command, so a
 * replacement or a deletion over a selection is ONE undo step. */
static bool prompt_sel_delete(Ed *ed, Span span)
{
    CmdLineTarget *target = cmdline_target(&ed->cmdline);
    EditCtx ec = yew_ed_edit_ctx_for(ed, &target->win);
    bool ok = yew_edit_delete(&ec, span);

    yew_ed_finish_edit(ed, &ec);
    if (ok)
        prompt_sel_collapse(ed, BYTEOFF(span.lo));
    return ok;
}

bool yew_cmdline_sel(Ed *ed, const char *command, bool after)
{
    Cursor *c = prompt_cursor(ed);
    PromptSel rule;
    Span span;

    if (c == NULL || command == NULL)
        return false;
    rule = prompt_sel_rule(command);
    if (after) {
        if (rule != PSEL_EXTEND && c->anchor.v != c->pos.v)
            prompt_sel_collapse(ed, c->pos);
        return false;
    }
    if (!yew_cmdline_selection(ed, &span))
        return false;
    switch (rule) {
    case PSEL_EXTEND:
    case PSEL_KEEP:
        return false;
    case PSEL_START:
        prompt_sel_collapse(ed, BYTEOFF(span.lo));
        return true;
    case PSEL_END:
        prompt_sel_collapse(ed, BYTEOFF(span.hi));
        return true;
    case PSEL_REPLACE:
        /* A failed delete must not leave the insert landing beside the
         * text it was meant to replace. */
        return !prompt_sel_delete(ed, span);
    case PSEL_DELETE:
        (void)prompt_sel_delete(ed, span);
        return true;
    case PSEL_COLLAPSE:
        break;
    }
    prompt_sel_collapse(ed, c->pos);
    return false;
}

CmdStatus yew_cmdline_cmd_copy_or_cancel(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    if (!yew_cmdline_selection(cx->ed, NULL))
        return yew_cmdline_cmd_cancel(cx);
    /* The dispatcher collapses it afterwards; the prompt stays open. */
    cx->win = yew_cmdline_target(cx->ed);
    return yew_sel_cmd_clip_copy(cx);
}

bool yew_cmdline_key(Ed *ed, const Key *key)
{
    const u16 command_mods = YEW_MOD_ALT | YEW_MOD_CTRL | YEW_MOD_SUPER |
                             YEW_MOD_HYPER | YEW_MOD_META;

    if (ed == NULL || key == NULL || !ed->cmdline.active)
        return false;
    if (key->ev == YEW_KEY_RELEASE)
        return true;
    /*
     * A register name or a literal-next key is ARMED (A-r, C-r, C-q):
     * the next key is that command's argument, printable or not, so it
     * belongs to the dispatcher's capture rather than to the text.
     */
    if (ed->capture_cmd.v != 0U)
        return false;
    if (key->code < YEW_KEY_BASE && key->ntext != 0U &&
        (key->mods & command_mods) == 0U) {
        /*
         * §6 inverts Sprint 18's rule: a printable key REFILTERS rather
         * than dismissing.  The insert runs through the registry, which
         * lands in yew_cmdline_edited, which refilters -- so there is
         * still exactly one place that reacts to a prompt edit.
         */
        (void)insert_sanitized(ed, key->text, key->ntext);
        return true;
    }
    return false;
}

void yew_cmdline_paste(Ed *ed, const u8 *bytes, size_t len)
{
    if (ed == NULL || !ed->cmdline.active || bytes == NULL || len == 0U)
        return;
    /* Like a printable key: the insert refilters through the one hook. */
    (void)insert_sanitized(ed, bytes, len);
}

/*
 * Sprint 57.30 §2: fish's "smart" history.
 *
 * SOURCE: a bang line walks the 57.26 snapshot -- yew's own bang bodies
 * first, then the shells' files the option allows -- so Up and the
 * ghost read the same entries; any other line walks its prompt's own
 * history.  TERM: the line's text when the walk begins (a bang line's
 * body, leading blanks dropped, as the snapshot drops them), frozen
 * until an edit ends the walk.  An entry is shown when the term is a
 * substring of it (yew_hist_find) and it is not equal to one this walk
 * already showed; newest first.
 */
static void walk_begin(Ed *ed, const char *text)
{
    CmdLine *line = &ed->cmdline;
    size_t len = strlen(text);
    size_t body = 0U;
    size_t term;

    line->walk_bang = line->kind == YEW_PROMPT_CMD &&
                      yew_cmd_bang_body(ed, text, len, &body) && body <= len;
    if (!line->walk_bang)
        body = 0U;
    line->walk_prefix = body;
    term = body;
    if (line->walk_bang) {
        while (term < len && (text[term] == ' ' || text[term] == '\t'))
            term++;
    }
    yew_hist_walk_begin(&line->walk, text, text + term, len - term);
}

static YewHistView walk_view(Ed *ed)
{
    YewHistView v = {NULL, NULL};

    if (ed->cmdline.walk_bang) {
        suggest_ensure(ed);
        v.suggest = &ed->cmdline.suggest;
    } else {
        v.hist = ed->cmdline.history;
    }
    return v;
}

static bool walk_showing(const CmdLine *line)
{
    return line->walk.on && line->walk.n_seen != 0U;
}

/* The line becomes the draft's prefix (a bang line's `!`) and `entry`,
 * without ending the walk. */
static CmdStatus walk_show(Ed *ed, const char *entry)
{
    CmdLine *line = &ed->cmdline;
    size_t prefix = line->walk_prefix;
    Bytebuf bytes;
    bool ok;

    bytebuf_init(&bytes);
    if (prefix != 0U && line->walk.draft != NULL)
        bytebuf_append(&bytes, line->walk.draft, prefix);
    sanitize_bytes((const u8 *)entry, strlen(entry), &bytes);
    ok = replace_span(ed, (Span){0U, yew_textbuf_len(line->buf)},
                      bytes.data, bytes.len, false);
    bytebuf_free(&bytes);
    if (!ok)
        return YEW_CMD_ERR_IO;
    /* A history jump rewrites the whole line without going through the
     * edit hook: refilter for the hint (the table stays closed while a
     * walked entry is shown). */
    cmdline_refilter(ed);
    return YEW_CMD_OK;
}

static CmdStatus history_move(CmdCtx *cx, bool previous)
{
    Ed *ed;
    CmdLine *line;
    YewHistView view;
    const char *found;
    bool at_draft = false;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    line = &ed->cmdline;
    sync_from_target(line);
    if (!line->walk.on) {
        char *text = text_string(line->buf);

        walk_begin(ed, text);
        yew_xfree(text);
    }
    view = walk_view(ed);
    if (previous) {
        found = yew_hist_walk_older(&line->walk, &view);
        /* Past the oldest match: stay put. */
        return found == NULL ? YEW_CMD_OK : walk_show(ed, found);
    }
    found = yew_hist_walk_newer(&line->walk, &view, &at_draft);
    if (found != NULL)
        return walk_show(ed, found);
    if (!at_draft)
        return YEW_CMD_OK;
    /* Down past the newest match: the text the walk began with. */
    if (!replace_all(ed, line->walk.draft, false))
        return YEW_CMD_ERR_IO;
    cmdline_refilter(ed);
    return YEW_CMD_OK;
}

bool yew_cmdline_hist_match(Ed *ed, Span *out)
{
    CmdLine *line;
    char *text;
    size_t len;
    size_t at;
    bool hit;

    if (ed == NULL || !ed->cmdline.active || ed->cmdline.buf == NULL)
        return false;
    line = &ed->cmdline;
    if (!walk_showing(line) || line->walk.term_len == 0U)
        return false;
    text = text_string(line->buf);
    len = strlen(text);
    hit = line->walk_prefix <= len &&
          yew_hist_find(text + line->walk_prefix, len - line->walk_prefix,
                        line->walk.term, line->walk.term_len, &at);
    yew_xfree(text);
    if (hit && out != NULL)
        *out = (Span){line->walk_prefix + at,
                      line->walk_prefix + at + line->walk.term_len};
    return hit;
}

CmdStatus yew_cmdline_cmd_hist_prev(CmdCtx *cx)
{
    return history_move(cx, true);
}

CmdStatus yew_cmdline_cmd_hist_next(CmdCtx *cx)
{
    return history_move(cx, false);
}

static char *heap_slice(const char *text, Span span)
{
    size_t len = (size_t)(span.hi - span.lo);
    char *copy = yew_xmalloc(len + 1U);

    (void)memcpy(copy, text + span.lo, len);
    copy[len] = '\0';
    return copy;
}

static bool insert_completion(Ed *ed, Span replace, const CompItem *item,
                              bool trailing_space)
{
    Bytebuf bytes;
    bool ok;

    bytebuf_init(&bytes);
    sanitize_bytes((const u8 *)item->text, strlen(item->text), &bytes);
    if (trailing_space && !item->is_dir) {
        /*
         * Sprint 57.23 §6: a COMMITTED shell word closes what it opened
         * -- the `"` of a word typed inside quotes, the `}` of `${NAME`
         * -- and then gets its space.  A directory gets neither: the
         * user keeps typing into it.
         */
        if (item->suffix != NULL)
            sanitize_bytes((const u8 *)item->suffix, strlen(item->suffix),
                           &bytes);
        bytebuf_push_u8(&bytes, (u8)' ');
    }
    ok = replace_span(ed, replace, bytes.data, bytes.len, true);
    if (ok)
        ed->cmdline.menu.replace = (Span){replace.lo,
                                          replace.lo + bytes.len};
    bytebuf_free(&bytes);
    return ok;
}

static CmdStatus completion_cycle(Ed *ed, bool previous)
{
    CmdLine *line = &ed->cmdline;
    const CompItem *item;

    if (!yew_menu_move(&line->menu, previous ? -1 : 1, false))
        return YEW_CMD_OK;
    /* Sprint 57.30 §1: Tab is how the user enters the table. */
    (void)yew_menu_focus(&line->menu);
    item = yew_menu_selected(&line->menu);
    if (item == NULL)
        return YEW_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return YEW_CMD_ERR_IO;
    ed->full_damage = true;
    return YEW_CMD_OK;
}

/*
 * Sprint 57.17 §2: move the pager's selection WITHOUT touching the
 * prompt.
 *
 * The whole difference from completion_cycle, which inserts the newly
 * selected candidate on every move: this is a LOOK.  DoD 3 pins that
 * looking cannot change what Enter would run, which is what makes
 * arrowing up into the list to read it before choosing possible at all.
 *
 * `previous` off the FIRST row leaves the pager instead of wrapping:
 * the rows stay on screen and the choice is dropped so §6's Enter rule
 * sees none.  No longer bound (57.30 §1 gave the arrows to history and
 * the table's rows); Fletch still reaches it.
 */
static CmdStatus menu_preview(Ed *ed, bool previous)
{
    Menu *menu = &ed->cmdline.menu;

    if (menu->items.len == 0U)
        return YEW_CMD_OK;
    if (!yew_menu_focused(menu)) {
        if (!yew_menu_focus(menu))
            return YEW_CMD_OK;
    } else if (previous && menu->sel == 0) {
        yew_menu_unselect(menu);
    } else {
        (void)yew_menu_move(menu, previous ? -1 : 1, false);
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
    return YEW_CMD_OK;
}

static CmdStatus complete(Ed *ed, bool previous)
{
    CmdLine *line = &ed->cmdline;
    Arena scratch;
    YewCompQuery query;
    Vec_CompItem items = {0};
    char *text;
    char *lcp;
    size_t stem_len;

    /*
     * Already walking the list: Tab and S-Tab just move.  A LIVE menu
     * with nothing chosen is not "already cycling" -- it falls through
     * so the first Tab can still offer the longest common prefix.
     */
    /* Sprint 57.30: Tab is a completion question, not a history step. */
    yew_hist_walk_end(&line->walk);
    if (line->menu.explicit_sel)
        return completion_cycle(ed, previous);
    text = text_string(line->buf);
    arena_init(&scratch);
    if (!yew_comp_query(ed, text, (size_t)yew_textbuf_len(line->buf),
                        (size_t)line->cur.pos.v,
                        &scratch, &query)) {
        arena_free_all(&scratch);
        yew_xfree(text);
        return insert_sanitized(ed, (const u8 *)"\t", 1U);
    }
    /*
     * Tab is an explicit question, so the budget is 0 (unlimited): the
     * user is waiting for the answer.  §6's live path passes a real
     * budget.  The filter owns comp_arena and resets it only when it
     * actually re-enumerates.
     */
    line->comp_total = yew_comp_filter_run(ed, &line->filter,
                                           &line->comp_arena, &query, 0,
                                           &items);
    if (query.kind == YEW_COMP_SHELL &&
        (yew_compspec_notice(ed) || yew_comphelp_notice(ed)))
        ed->footer_dirty = true;
    if (items.len == 0U && line->filter.gen_pending) {
        /* §5.5: the answer is still coming.  Say nothing; the arrival
         * opens the menu (and edits nothing). */
        line->comp_asked = true;
        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        yew_xfree(text);
        return YEW_CMD_OK;
    }
    if (items.len == 0U) {
        /* Sprint 57.32 §3: say why when the directory is the reason. */
        if (query.kind == YEW_COMP_SHELL && line->filter.where[0] != '\0')
            yew_msg(ed, YEW_MSG_INFO, "no completions (%s)",
                    line->filter.where);
        else
            yew_msg(ed, YEW_MSG_INFO, "no completions");
        ed->full_damage = true;
        ed->footer_dirty = true;
        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        yew_xfree(text);
        return YEW_CMD_OK;
    }
    /*
     * Sprint 57.24 §5: while a generator's answer is still coming the set
     * is INCOMPLETE -- a lone static row is not a sole survivor, and the
     * static rows' common prefix is not the answer's.  Show the rows,
     * insert nothing; the arrival refills the menu and Tab again decides.
     */
    if (line->filter.gen_pending) {
        yew_xfree(line->menu_stem);
        line->menu_stem = heap_slice(text, query.replace);
        line->menu_original = query.replace;
        yew_menu_reset(&line->menu, items, line->comp_total, query.replace);
        line->menu.pending = true;
        menu_where(line);
        line->comp_asked = true;
        ed->full_damage = true;
        ed->footer_dirty = true;
        arena_free_all(&scratch);
        yew_xfree(text);
        return YEW_CMD_OK;
    }
    /* §1's predicate, not a second spelling of it: one survivor of the
     * ranked set is the same question here and at Enter. */
    if (yew_comp_sole(&items, YEW_COMP_KIND__N) != NULL) {
        bool ok = insert_completion(ed, query.replace, &items.data[0], true);

        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        yew_xfree(text);
        if (!ok)
            return YEW_CMD_ERR_IO;
        /*
         * The line just changed under the live menu, which is still
         * holding rows for the OLD token.  Completion insertion does not
         * run through the edit hook, so refilter explicitly rather than
         * leaving stale rows on screen.
         */
        cmdline_refilter(ed);
        return YEW_CMD_OK;
    }
    yew_xfree(line->menu_stem);
    line->menu_stem = heap_slice(text, query.replace);
    line->menu_original = query.replace;
    yew_menu_reset(&line->menu, items, line->comp_total, query.replace);
    line->menu.pending = line->filter.gen_pending;
    menu_where(line);
    /*
     * The common prefix is taken over the TIERED rows only -- the ones
     * that matched as an exact or prefix match.  A fuzzy match shares no
     * meaningful prefix with them (`f` matches `move.line.first_nonblank`
     * somewhere in the middle), so including it drags the LCP to empty
     * and Tab stops being able to complete `file.` at all.
     */
    {
        Vec_CompItem tiered = {0};
        size_t i;

        for (i = 0U; i < line->menu.items.len; i++) {
            if (line->menu.items.data[i].score >= YEW_FZ_BASENAME_TIER)
                Vec_CompItem_push(&tiered, line->menu.items.data[i]);
        }
        lcp = yew_comp_lcp(&scratch, &tiered);
        Vec_CompItem_free(&tiered);
    }
    stem_len = strlen(query.stem);
    /*
     * Sprint 57.23: a SHELL row is ENCODED for the caret's quote state
     * (`"my dir/`, `my\ dir/`), so it is measured against the bytes the
     * user typed, not the decoded stem -- or an LCP equal to what is
     * already there would count as progress and Tab would never enter
     * the list.
     */
    if (query.kind == YEW_COMP_SHELL)
        stem_len = (size_t)(query.replace.hi - query.replace.lo);
    if (strlen(lcp) > stem_len) {
        /* The prefix every candidate shares is unambiguous, so insert it
         * and leave the list open with nothing selected -- the user has
         * still not chosen a row. */
        if (!replace_span(ed, query.replace, (const u8 *)lcp, strlen(lcp),
                          true)) {
            menu_discard(ed);
            arena_free_all(&scratch);
            yew_xfree(text);
            return YEW_CMD_ERR_IO;
        }
        line->menu.replace = (Span){query.replace.lo,
                                    query.replace.lo + strlen(lcp)};
    } else {
        /* Nothing left to insert unambiguously, so this Tab is a choice:
         * enter the list (from the far end for S-Tab). */
        (void)yew_menu_move(&line->menu, previous ? -1 : 1, false);
        (void)yew_menu_focus(&line->menu);
        {
            const CompItem *item = yew_menu_selected(&line->menu);

            if (item != NULL &&
                !insert_completion(ed, line->menu.replace, item, false)) {
                arena_free_all(&scratch);
                yew_xfree(text);
                return YEW_CMD_ERR_IO;
            }
        }
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
    arena_free_all(&scratch);
    yew_xfree(text);
    return YEW_CMD_OK;
}

/*
 * Sprint 18.5 §10.  Every menu behaviour is a registered command, so it
 * is rebindable, recordable, and reachable from Fletch (Sprint 34)
 * rather than being a keystroke handled inside a switch.  They all carry
 * YEW_CMD_INTERNAL: they are keymap plumbing, not commands a user types.
 */
static CmdStatus menu_page(Ed *ed, bool previous)
{
    CmdLine *line = &ed->cmdline;
    const CompItem *item;

    if (!yew_menu_move(&line->menu, previous ? -1 : 1, true))
        return YEW_CMD_OK;
    /* The page keys move inside the table, so they are in it. */
    (void)yew_menu_focus(&line->menu);
    item = yew_menu_selected(&line->menu);
    if (item == NULL)
        return YEW_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return YEW_CMD_ERR_IO;
    ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_menu_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return menu_preview(cx->ed, false);
}

CmdStatus yew_cmdline_cmd_menu_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return menu_preview(cx->ed, true);
}

/*
 * Sprint 57.30 §1: `<up>` and `<down>` (and C-p / C-n, bound to the same
 * commands) are HISTORY unless the user has ENTERED the table -- and
 * only Tab / S-Tab (and the page keys, which move there the same way)
 * enter it.  The live table, open the whole time a token is typed,
 * never takes an arrow; that was 57.17 §2's rule, and dogfooding found
 * it backwards for a shell prompt.  ONE rule:
 *
 *   not in the table          Up: history older   Down: history newer
 *   in it, not the top row    Up: row up (the row written to the line)
 *   in it, the top row        Up: leave it, close it, and walk history
 *                                 from the text the user TYPED
 *   in it, not the last row   Down: row down; off the bottom VISIBLE
 *                                 row the window scrolls by one
 *   in it, the true last row  Down: leave it, keeping the candidate in
 *                                 the line; the table stays open but
 *                                 unfocused, so Up is history again and
 *                                 Tab re-enters it.
 */
static CmdStatus table_row(Ed *ed, i32 delta)
{
    Menu *menu = &ed->cmdline.menu;
    const CompItem *item;

    if (!yew_menu_move(menu, delta, false))
        return YEW_CMD_OK;
    item = yew_menu_selected(menu);
    if (item != NULL && !insert_completion(ed, menu->replace, item, false))
        return YEW_CMD_ERR_IO;
    ed->full_damage = true;
    ed->footer_dirty = true;
    return YEW_CMD_OK;
}

/*
 * The pitfall: row navigation wrote each candidate into the line, so the
 * line now holds a word the user never typed.  What they TYPED is the
 * line with the table's span put back to its stem -- the same text Esc
 * restores -- and that is what the history walk searches with.
 */
static char *typed_text(const CmdLine *line)
{
    char *text = text_string(line->buf);
    size_t len = strlen(text);
    Span r = line->menu.replace;
    Bytebuf out;
    char *typed;

    if (line->menu_stem == NULL || r.lo > r.hi || r.hi > len)
        return text;
    bytebuf_init(&out);
    bytebuf_append(&out, text, (size_t)r.lo);
    bytebuf_append(&out, line->menu_stem, strlen(line->menu_stem));
    bytebuf_append(&out, text + r.hi, len - (size_t)r.hi);
    bytebuf_push_u8(&out, 0U);
    typed = (char *)out.data;
    yew_xfree(text);
    return typed;
}

static CmdStatus table_top_to_history(CmdCtx *cx)
{
    Ed *ed = cx->ed;
    char *typed = typed_text(&ed->cmdline);
    CmdStatus status;

    menu_discard(ed);
    /* An edit, so it ends any walk; the new one begins from `typed`. */
    if (!replace_all(ed, typed, true)) {
        yew_xfree(typed);
        return YEW_CMD_ERR_IO;
    }
    walk_begin(ed, typed);
    yew_xfree(typed);
    status = history_move(cx, true);
    /* Nothing older matched: the typed text stays, with its live table. */
    if (status == YEW_CMD_OK && !walk_showing(&ed->cmdline))
        cmdline_refilter(ed);
    return status;
}

CmdStatus yew_cmdline_cmd_up(CmdCtx *cx)
{
    Menu *menu;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    menu = &cx->ed->cmdline.menu;
    if (!yew_menu_focused(menu))
        return history_move(cx, true);
    if (menu->sel > 0)
        return table_row(cx->ed, -1);
    return table_top_to_history(cx);
}

CmdStatus yew_cmdline_cmd_down(CmdCtx *cx)
{
    Menu *menu;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    menu = &cx->ed->cmdline.menu;
    if (!yew_menu_focused(menu))
        return history_move(cx, false);
    if ((size_t)menu->sel + 1U < menu->items.len)
        return table_row(cx->ed, 1);
    yew_menu_blur(menu);
    cx->ed->full_damage = true;
    cx->ed->footer_dirty = true;
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_menu_page_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return menu_page(cx->ed, false);
}

CmdStatus yew_cmdline_cmd_menu_page_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return menu_page(cx->ed, true);
}

/* No default binding: this is how §8's click and Fletch commit a row. */
CmdStatus yew_cmdline_cmd_menu_accept(CmdCtx *cx)
{
    Ed *ed;
    const CompItem *item;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    item = yew_menu_selected(&ed->cmdline.menu);
    if (item == NULL)
        return YEW_CMD_OK;
    if (!insert_completion(ed, ed->cmdline.menu.replace, item, true))
        return YEW_CMD_ERR_IO;
    menu_discard(ed);
    cmdline_refilter(ed);
    return YEW_CMD_OK;
}

/*
 * Sprint 18.5 §8: what a click on a menu row does.
 *
 * First click SELECTS, a second click on the same row ACCEPTS.  The pair
 * is deliberately state-based rather than timed: a double-click window
 * would make the pty goldens depend on a clock, and "click to choose,
 * click again to confirm" is legible without one.
 *
 * Selection by click is EXPLICIT, exactly as Tab is -- which is what
 * makes §6's Enter rule treat a clicked row as a choice.
 */
bool yew_cmdline_menu_click(Ed *ed, i32 row)
{
    CmdLine *line;
    CmdCtx cx = {0};

    if (ed == NULL || !ed->cmdline.active)
        return false;
    line = &ed->cmdline;
    if (!yew_menu_select(&line->menu, row))
        return false;
    if (line->click_row == row) {
        line->click_row = -1;
        cx.ed = ed;
        cx.win = yew_cmdline_target(ed);
        cx.count = 1U;
        cx.source = YEW_SRC_MOUSE;
        (void)yew_cmdline_cmd_menu_accept(&cx);
        ed->full_damage = true;
        return true;
    }
    line->click_row = row;
    /* Show the choice in the line, the same as Tab does. */
    {
        const CompItem *item = yew_menu_selected(&line->menu);

        if (item != NULL)
            (void)insert_completion(ed, line->menu.replace, item, false);
    }
    ed->full_damage = true;
    return true;
}

bool yew_cmdline_menu_scroll(Ed *ed, i32 delta)
{
    if (ed == NULL || !ed->cmdline.active || ed->footer_rect.h == 0U)
        return false;
    /* The menu may use every row above the prompt. */
    if (!yew_menu_scroll(&ed->cmdline.menu, delta, ed->footer_rect.y))
        return false;
    ed->full_damage = true;
    return true;
}

CmdStatus yew_cmdline_cmd_menu_dismiss(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    menu_discard(cx->ed);
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_complete_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return complete(cx->ed, false);
}

CmdStatus yew_cmdline_cmd_complete_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    return complete(cx->ed, true);
}

CmdStatus yew_cmdline_cmd_insert_register(CmdCtx *cx)
{
    RegVal *value;
    u8 name;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active ||
        cx->sarg == NULL || cx->sarg_len == 0U)
        return YEW_CMD_ERR_ARG;
    name = (u8)cx->sarg[0];
    value = yew_reg_get(&cx->ed->regs, name);
    if (value == NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "unknown register '%c'", name);
        return YEW_CMD_ERR_ARG;
    }
    return insert_sanitized(cx->ed, value->bytes.data, value->bytes.len);
}

CmdStatus yew_cmdline_cmd_literal_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active ||
        cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    return insert_sanitized(cx->ed, (const u8 *)cx->sarg, cx->sarg_len);
}

/*
 * Sprint 18.5 §7: the part of a candidate that has not been typed yet.
 *
 * Computed at DRAW time and never stored, so there is no lifetime to get
 * wrong -- the string it points into belongs to the completion arena and
 * is valid exactly between two refilters, which is exactly when a frame
 * is drawn.
 *
 * The ghost is NEVER inserted into the prompt's TextBuf.  Putting it
 * there would poison the history draft, hand the parser text the user
 * never typed, and make yew_cmdline_text() -- which Sprint 21's search
 * reads on every keystroke -- return a pattern with a suggestion glued
 * to it.
 */
static const char *token_ghost(Ed *ed, size_t *len)
{
    const CmdLine *line = &ed->cmdline;
    const CompItem *item;
    u64 buf_len;
    size_t stem_len;
    size_t text_len;
    u8 typed[256];

    *len = 0U;
    if (line->buf == NULL || line->menu.items.len == 0U)
        return NULL;
    buf_len = yew_textbuf_len(line->buf);
    /* Only at end of line: a suggestion in the middle of a line has no
     * coherent place to go. */
    if (line->cur.pos.v != buf_len || line->menu.replace.hi != buf_len)
        return NULL;
    item = yew_menu_selected(&line->menu);
    if (item == NULL)
        item = &line->menu.items.data[0];
    if (item == NULL || item->text == NULL)
        return NULL;
    stem_len = (size_t)(line->menu.replace.hi - line->menu.replace.lo);
    text_len = strlen(item->text);
    if (stem_len == 0U || stem_len >= text_len || stem_len > sizeof(typed))
        return NULL;
    text_copy_span(line->buf, line->menu.replace, typed);
    /*
     * Only a PREFIX match has a "rest of it" to show.  A fuzzy match
     * shares no head with what was typed, so it shows nothing and the
     * menu row's highlighting carries the information instead.
     */
    if (memcmp(typed, item->text, stem_len) != 0)
        return NULL;
    *len = text_len - stem_len;
    return item->text + stem_len;
}

/* `shell.suggest_history`: `all` (the default) or `yew`. */
static bool suggest_all_shells(Ed *ed)
{
    OptVal v;

    if (!yew_opt_get(ed, NULL, NULL, "shell.suggest_history", 21U, &v) ||
        (v.type != (u8)YEW_OPT_ENUM && v.type != (u8)YEW_OPT_STR))
        return true;
    return !(v.as.str.len == 3U && memcmp(v.as.str.s, "yew", 3U) == 0);
}

/*
 * Sprint 57.26 §3: the snapshot, taken once per prompt -- on the first
 * idle turn after the prompt opens, or on demand if a bang body is typed
 * before one comes.  yew's own
 * E-mode history first (its bang entries' BODIES, so `:!git st` finds
 * `:%!git status` too), newest first; then, under `all`, the shells'
 * files.  Nothing here runs again until the prompt closes, so a history
 * another process writes mid-prompt cannot change a frame.
 */
static void suggest_ensure(Ed *ed)
{
    CmdLine *line = &ed->cmdline;
    size_t i;

    if (line->suggest_loaded)
        return;
    line->suggest_loaded = true;
    yew_hist_suggest_init(&line->suggest);
    if (line->history != NULL) {
        for (i = yew_hist_len(line->history); i > 0U; i--) {
            const char *entry = yew_hist_at(line->history, i - 1U);
            size_t n = entry == NULL ? 0U : strlen(entry);
            size_t body;

            if (entry != NULL && yew_cmd_bang_body(ed, entry, n, &body))
                (void)yew_hist_suggest_add(&line->suggest, entry + body,
                                           n - body);
        }
    }
    if (suggest_all_shells(ed))
        yew_hist_suggest_read_shells(&line->suggest);
}

/*
 * The history ghost: the caret at the end of a non-empty bang body, and
 * the newest snapshot entry that begins with that body and is longer
 * supplies its remainder.  A pure function of (snapshot, line).
 */
static const char *history_ghost(Ed *ed, size_t *len)
{
    CmdLine *line = &ed->cmdline;
    const char *ghost = NULL;
    u64 buf_len;
    size_t body;
    char *text;

    *len = 0U;
    if (line->kind != YEW_PROMPT_CMD || line->buf == NULL)
        return NULL;
    buf_len = yew_textbuf_len(line->buf);
    if (buf_len == 0U || line->cur.pos.v != buf_len)
        return NULL;
    text = text_string(line->buf);
    if (yew_cmd_bang_body(ed, text, (size_t)buf_len, &body) &&
        body < (size_t)buf_len) {
        suggest_ensure(ed);
        ghost = yew_hist_suggest_match(&line->suggest, text + body,
                                       (size_t)buf_len - body, len);
    }
    yew_xfree(text);
    return ghost;
}

typedef enum GhostKind {
    GHOST_NONE,
    GHOST_TOKEN,
    GHOST_HISTORY
} GhostKind;

/*
 * Two providers behind the one function the draw and the accept both
 * call.  An EXPLICIT menu selection means the user is navigating the
 * menu: its row's rest.  Otherwise the history ghost when there is one,
 * else the top row's rest.
 */
static const char *cmdline_ghost_of(Ed *ed, size_t *len, GhostKind *kind)
{
    const char *ghost;

    *kind = GHOST_NONE;
    /* Sprint 57.30 §2: a walked history entry is the whole suggestion;
     * a ghost continuing it would be a second one. */
    if (walk_showing(&ed->cmdline))
        return NULL;
    if (yew_menu_selected(&ed->cmdline.menu) == NULL) {
        ghost = history_ghost(ed, len);
        if (ghost != NULL && *len != 0U) {
            *kind = GHOST_HISTORY;
            return ghost;
        }
    }
    ghost = token_ghost(ed, len);
    if (ghost != NULL)
        *kind = GHOST_TOKEN;
    return ghost;
}

static const char *cmdline_ghost(Ed *ed, size_t *len)
{
    GhostKind kind;

    return cmdline_ghost_of(ed, len, &kind);
}

const char *yew_cmdline_ghost(Ed *ed, size_t *len)
{
    size_t n = 0U;
    const char *ghost = NULL;

    if (ed != NULL && ed->cmdline.active)
        ghost = cmdline_ghost(ed, &n);
    if (len != NULL)
        *len = ghost == NULL ? 0U : n;
    return ghost;
}

/*
 * How much of `ghost` one word is: any blanks it starts with, the next
 * word, and the run of UNQUOTED whitespace after it -- or all of it when
 * there is none.  Quoting is read from `typed` (the line before the
 * caret) onwards, so a ghost that continues an open `"a b` does not stop
 * at the blank inside the quotes.
 */
static size_t ghost_word_len(const char *typed, size_t tn,
                             const char *ghost, size_t gn)
{
    char q = 0;
    size_t i;
    bool word = false;

    for (i = 0U; i < tn; i++) {
        char c = typed[i];

        if (q != '\'' && c == '\\' && i + 1U < tn)
            i++;
        else if (q == 0 && (c == '\'' || c == '"'))
            q = c;
        else if (q != 0 && c == q)
            q = 0;
    }
    for (i = 0U; i < gn; i++) {
        char c = ghost[i];

        if (q == 0 && (c == ' ' || c == '\t')) {
            if (word)
                break;
            continue;
        }
        word = true;
        if (q != '\'' && c == '\\' && i + 1U < gn)
            i++;
        else if (q == 0 && (c == '\'' || c == '"'))
            q = c;
        else if (q != 0 && c == q)
            q = 0;
    }
    while (i < gn && (ghost[i] == ' ' || ghost[i] == '\t'))
        i++;
    return i;
}

static CmdStatus ghost_motion(CmdCtx *cx, const char *command)
{
    CmdCtx move = {0};

    move.win = yew_cmdline_target(cx->ed);
    move.count = 1U;
    move.source = cx->source;
    return yew_ed_invoke(cx->ed, yew_cmd_lookup(command,
                                                (u32)strlen(command)),
                         &move);
}

static CmdStatus ghost_take(CmdCtx *cx, bool one_word)
{
    Ed *ed;
    const CompItem *item;
    const char *ghost;
    GhostKind kind;
    size_t len;
    size_t take;
    u64 end;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    ghost = cmdline_ghost_of(ed, &len, &kind);
    if (ghost == NULL) {
        /*
         * No suggestion under the caret, so this is just a motion.  The
         * fallback is one grapheme right rather than end-of-line, which
         * is why this is bound to Right and not to C-e: at end of line
         * the two agree, but anywhere else they do not.  A-f is a word.
         */
        return ghost_motion(cx, one_word ? "ed.move.word.next"
                                         : "ed.move.char.next");
    }
    take = len;
    end = yew_textbuf_len(ed->cmdline.buf);
    if (one_word) {
        char *text = text_string(ed->cmdline.buf);
        size_t body = 0U;

        if (!yew_cmd_bang_body(ed, text, (size_t)end, &body))
            body = 0U;
        take = ghost_word_len(text + body, (size_t)end - body, ghost, len);
        yew_xfree(text);
    }
    if (kind == GHOST_TOKEN && take == len) {
        item = yew_menu_selected(&ed->cmdline.menu);
        if (item == NULL)
            item = &ed->cmdline.menu.items.data[0];
        /* One accept path, shared with the menu's: a ghost accepted and
         * a row accepted must land byte-identical text. */
        if (!insert_completion(ed, ed->cmdline.menu.replace, item, true))
            return YEW_CMD_ERR_IO;
        menu_discard(ed);
        cmdline_refilter(ed);
        return YEW_CMD_OK;
    }
    /* A history remainder (whole or a word of it), or a word of a row's:
     * the bytes on screen, appended at the caret, which is the end. */
    {
        Bytebuf bytes;
        bool ok;

        bytebuf_init(&bytes);
        sanitize_bytes((const u8 *)ghost, take, &bytes);
        ok = replace_span(ed, (Span){end, end}, bytes.data, bytes.len, true);
        bytebuf_free(&bytes);
        if (!ok)
            return YEW_CMD_ERR_IO;
    }
    menu_discard(ed);
    yew_cmdline_edited(ed);
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_ghost_accept(CmdCtx *cx)
{
    return ghost_take(cx, false);
}

CmdStatus yew_cmdline_cmd_ghost_accept_word(CmdCtx *cx)
{
    return ghost_take(cx, true);
}

/* Sprint 57.28 §3: fish's C-e -- the whole ghost when there is one (the
 * caret is then at the end already), else the line end. */
CmdStatus yew_cmdline_cmd_ghost_accept_line(CmdCtx *cx)
{
    size_t len = 0U;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    if (cmdline_ghost(cx->ed, &len) != NULL && len != 0U)
        return ghost_take(cx, false);
    return ghost_motion(cx, "ed.move.line.end");
}

/*
 * The last word of a history entry.  A bang entry's is the shell word
 * yew_shctx_at finds at the entry's end -- quotes and escapes honoured,
 * the RAW bytes as typed -- so `!cp a "my file"` gives `"my file"`.
 * Anything else: the last blank-delimited token.  Empty when the entry
 * ends in a blank.
 */
static Span last_word(Ed *ed, const char *entry, size_t n)
{
    size_t body;
    size_t lo = n;

    if (yew_cmd_bang_body(ed, entry, n, &body)) {
        Arena a;
        YewShCtx ctx;
        Span word = {n, n};

        arena_init(&a);
        if (yew_shctx_at(entry + body, n - body, n - body, &a, &ctx) &&
            ctx.replace.lo < ctx.replace.hi)
            word = (Span){ctx.replace.lo + body, ctx.replace.hi + body};
        arena_free_all(&a);
        return word;
    }
    while (lo > 0U && entry[lo - 1U] != ' ' && entry[lo - 1U] != '\t')
        lo--;
    return (Span){lo, n};
}

/*
 * Sprint 57.28 §4: A-. inserts the newest history entry's last word at
 * the caret; each further A-. -- consecutive by the dispatcher's
 * sequence number -- replaces what the last one inserted with the last
 * word of the next OLDER entry.  Entries with no last word are skipped.
 * Past the oldest it stops where it is: there is nothing older to show,
 * and wrapping back to the newest would read as a different command.
 */
CmdStatus yew_cmdline_cmd_last_arg(CmdCtx *cx)
{
    Ed *ed;
    CmdLine *line;
    Span replace;
    size_t i;
    bool again;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    line = &ed->cmdline;
    if (line->history == NULL)
        return YEW_CMD_OK;
    sync_from_target(line);
    again = line->last_arg_seq != 0U &&
            line->last_arg_gen == line->generation &&
            ed->invoke_seq == line->last_arg_seq + 1U;
    i = again ? line->last_arg_entry : yew_hist_len(line->history);
    /* Sprint 57.29: the first A-. replaces a selection, in the same one
     * edit, as any insert over one does. */
    replace = (Span){line->cur.pos.v, line->cur.pos.v};
    if (again)
        replace = line->last_arg_span;
    else
        (void)yew_cmdline_selection(ed, &replace);
    while (i > 0U) {
        const char *entry = yew_hist_at(line->history, --i);
        size_t n = entry == NULL ? 0U : strlen(entry);
        Span word;

        if (n == 0U)
            continue;
        word = last_word(ed, entry, n);
        if (word.lo >= word.hi)
            continue;
        if (!replace_span(ed, replace, (const u8 *)entry + word.lo,
                          (size_t)(word.hi - word.lo), true))
            return YEW_CMD_ERR_IO;
        line->last_arg_entry = i;
        line->last_arg_span = (Span){replace.lo,
                                     replace.lo + (word.hi - word.lo)};
        line->last_arg_seq = ed->invoke_seq;
        line->last_arg_gen = line->generation;
        menu_discard(ed);
        yew_cmdline_edited(ed);
        return YEW_CMD_OK;
    }
    /* Nothing older: keep the chain, so a further A-. stops here too. */
    if (again)
        line->last_arg_seq = ed->invoke_seq;
    return YEW_CMD_OK;
}

static void deferred_dispatch_error(Ed *ed, const CmdParse *parsed)
{
    const CmdDesc *desc = yew_cmd_desc(parsed->command);
    CmdErr error = {0};
    bool preserve_message;
    const char *sprint;
    const char *label;

    error.tok_lo = (u32)parsed->name_tok.lo;
    error.tok_hi = (u32)parsed->name_tok.hi;
    label = desc == NULL ? "command" :
            strncmp(desc->name, "ed.", 3U) == 0 ? desc->name + 3U :
                                                   desc->name;
    preserve_message = desc != NULL &&
                       strncmp(desc->name, "ed.ai.", 6U) == 0 &&
                       ed->msg.active && ed->msg.text[0] != '\0';
    sprint = desc == NULL ? NULL : strstr(desc->help, "Sprint ");
    if (sprint != NULL) {
        char number[16];
        size_t n = 0U;

        sprint += 7U;
        while (sprint[n] >= '0' && sprint[n] <= '9' &&
               n + 1U < sizeof(number)) {
            number[n] = sprint[n];
            n++;
        }
        number[n] = '\0';
        if (preserve_message)
            (void)snprintf(error.msg, sizeof(error.msg),
                           "%.64s; :%.24s lands in Sprint %.15s",
                           ed->msg.text, label, number);
        else
            (void)snprintf(error.msg, sizeof(error.msg),
                           ":%s lands in Sprint %s", label, number);
    } else {
        (void)snprintf(error.msg, sizeof(error.msg), ":%s failed", label);
    }
    set_error(ed, &error);
}

/*
 * Sprint 57.17 §1: resolve a command name through the FILTER when the
 * parser could not resolve it.
 *
 * `resolve_name` resolves by unique PREFIX.  When that fails the menu
 * may still be showing exactly one command -- `fwq` is a prefix of
 * nothing but a unique fuzzy match for `file.write_quit` -- and the
 * user can see the answer without being able to take it.  Ranking the
 * command source against the same stem the live menu ranked, and
 * accepting only a SOLE survivor (yew_comp_sole, the one definition of
 * that predicate), is the answer they can see.
 *
 * The question is asked of TOKEN 0 wherever the caret happens to be:
 * `:fwq somefile` leaves the caret on an argument, and the arguments
 * are exactly what §1 requires to survive.  `CmdParsePoint.name` is the
 * name token from the same loose scan, so there is no second lexical
 * rule here to drift from the parser's.
 *
 * Returns a heap command line with the name token replaced and every
 * other byte -- range, bang, arguments -- preserved, or NULL when the
 * filter did not leave exactly one command.  It reads NOTHING of
 * `line->menu`: the caller runs it before invoking anything, and the
 * command it goes on to run may replace the whole prompt.
 */
static char *cmdline_fuzzy_line(Ed *ed, const char *text, size_t len)
{
    Arena scratch;
    CmdParsePoint point;
    Vec_CompItem items = {0};
    const CompItem *sole;
    char *out = NULL;

    arena_init(&scratch);
    if (yew_cmd_parse_point(ed, text, len, len, &scratch, &point) &&
        !point.command_known && point.name != NULL &&
        point.name[0] != '\0' &&
        (size_t)point.name_tok.hi <= len &&
        point.name_tok.hi > point.name_tok.lo) {
        CompReq req;

        (void)memset(&req, 0, sizeof(req));
        req.kind = YEW_COMP_CMD;
        req.stem = point.name;
        req.ed = ed;
        req.arena = &scratch;
        /* Enter is an explicit question, like Tab: the user is waiting
         * for the answer, so there is no budget to respect. */
        req.budget_us = 0;
        req.allow_cache = false;
        (void)yew_comp_request(&req, &items);
        sole = yew_comp_sole(&items, YEW_COMP_CMD);
        if (sole != NULL && sole->text != NULL) {
            Bytebuf rewritten;

            bytebuf_init(&rewritten);
            bytebuf_append(&rewritten, text, (size_t)point.name_tok.lo);
            bytebuf_append(&rewritten, sole->text, strlen(sole->text));
            bytebuf_append(&rewritten, text + point.name_tok.hi,
                           len - (size_t)point.name_tok.hi);
            bytebuf_push_u8(&rewritten, 0U);
            out = yew_xmalloc(rewritten.len);
            (void)memcpy(out, rewritten.data, rewritten.len);
            bytebuf_free(&rewritten);
        }
    }
    Vec_CompItem_free(&items);
    arena_free_all(&scratch);
    return out;
}

CmdStatus yew_cmdline_cmd_accept(CmdCtx *cx)
{
    Ed *ed;
    CmdLine *line;
    char *text;
    Arena arena;
    CmdParse parsed;
    YewCmdInvoke invoke;
    CmdStatus status;
    YewPromptKind accepted_kind;
    u64 accepted_generation;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    line = &ed->cmdline;
    accepted_kind = line->kind;
    accepted_generation = line->generation;
    /*
     * §6: Enter is governed by whether the user CHOSE a row, not by
     * whether a menu happens to be open.
     *
     * Sprint 18's rule -- Enter with a menu open accepts instead of
     * executing -- exists so `:w /etc/pas` cannot run when the user
     * meant to pick `passwd`.  Under a live menu the list is open for
     * the whole time a command name is being typed, so that rule taken
     * literally would mean the prompt can never be executed with one
     * Enter.  Keying on `explicit_sel` preserves exactly the property
     * s18 was protecting: a selection only becomes explicit through Tab,
     * S-Tab, C-n, C-p or a click, and filtering alone never selects.  So
     * the dangerous case -- the user was looking at a highlighted row --
     * still accepts, and the ordinary case -- a complete command typed
     * out -- still executes.
     */
    if (line->menu.explicit_sel && line->menu.sel >= 0) {
        const CompItem *item = yew_menu_selected(&line->menu);

        if (item != NULL && !insert_completion(ed, line->menu.replace, item,
                                               true))
            return YEW_CMD_ERR_IO;
        menu_discard(ed);
        return YEW_CMD_OK;
    }
    text = text_string(line->buf);
    if (yew_textbuf_len(line->buf) == 0U) {
        yew_xfree(text);
        yew_cmdline_close(ed, true);
        return YEW_CMD_OK;
    }
    /*
     * A search prompt's text is a PATTERN, not a command line.  The
     * preview has already applied it and yew_search_accept commits it;
     * handing it to the command parser instead reports the pattern as
     * an unknown command, which is what `/needle` did before this
     * check existed.
     */
    if (line->kind == YEW_PROMPT_SEARCH_F ||
        line->kind == YEW_PROMPT_SEARCH_B) {
        yew_hist_add(line->history, text);
        yew_xfree(text);
        yew_cmdline_close(ed, true);
        return YEW_CMD_OK;
    }
    if (line->kind == YEW_PROMPT_INPUT) {
        yew_hist_add(line->history, text);
        yew_xfree(text);
        yew_cmdline_close(ed, true);
        return YEW_CMD_OK;
    }
    arena_init(&arena);
    if (!yew_cmd_parse(ed, text, strlen(text), &arena, &parsed)) {
        /*
         * §1: ordinary resolution failed, so ask the filter.  The
         * user's OWN error is kept aside -- when the fallback declines,
         * or when the row it found does not parse either, the caret has
         * to point at the line they typed and not at a rewrite they
         * never saw.
         */
        CmdErr typed_err = parsed.err;
        char *fuzzy = cmdline_fuzzy_line(ed, text, strlen(text));
        bool resolved = false;

        arena_free_all(&arena);
        arena_init(&arena);
        if (fuzzy != NULL) {
            resolved = yew_cmd_parse(ed, fuzzy, strlen(fuzzy), &arena,
                                     &parsed);
            if (resolved) {
                /* The RESOLVED line is the one that runs, so it is also
                 * the one that enters history and the `:` register. */
                yew_xfree(text);
                text = fuzzy;
            } else {
                yew_xfree(fuzzy);
            }
        }
        if (!resolved) {
            set_error(ed, &typed_err);
            arena_free_all(&arena);
            yew_xfree(text);
            return YEW_CMD_ERR_ARG;
        }
    }
    invoke = (YewCmdInvoke){parsed.range, parsed.argv, 0, parsed.bang,
                            ed->win};
    status = yew_ed_invoke_parsed(ed, parsed.command, &invoke);
    if (status != YEW_CMD_OK) {
        bool same_prompt = ed->cmdline.active &&
                           ed->cmdline.generation == accepted_generation &&
                           ed->cmdline.kind == accepted_kind;

        if (same_prompt && status == YEW_CMD_ERR_DEFERRED)
            deferred_dispatch_error(ed, &parsed);
        else if (same_prompt) {
            CmdErr error = {0};

            error.tok_lo = (u32)parsed.name_tok.lo;
            error.tok_hi = (u32)parsed.name_tok.hi;
            if (ed->msg.active && ed->msg.text[0] != '\0') {
                size_t n = strlen(ed->msg.text);

                if (n >= sizeof(error.msg))
                    n = sizeof(error.msg) - 1U;
                (void)memcpy(error.msg, ed->msg.text, n);
                error.msg[n] = '\0';
            } else
                (void)snprintf(error.msg, sizeof(error.msg),
                               "command failed");
            set_error(ed, &error);
        }
        arena_free_all(&arena);
        yew_xfree(text);
        return status;
    }
    if (ed->cmdline.active &&
        ed->cmdline.generation == accepted_generation &&
        ed->cmdline.kind == accepted_kind)
        yew_hist_add(ed->cmdline.history, text);
    else
        history_add_closed_prompt(ed, accepted_kind, text);
    set_cmd_register(ed, text);
    arena_free_all(&arena);
    yew_xfree(text);
    if (ed->cmdline.active &&
        ed->cmdline.generation == accepted_generation &&
        ed->cmdline.kind == accepted_kind)
        yew_cmdline_close(ed, true);
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_cancel(CmdCtx *cx)
{
    CmdLine *line;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    line = &cx->ed->cmdline;
    /*
     * Esc dismisses the MENU only when the user opened or entered it --
     * Tab left a stem to restore, or a row was chosen.  A menu that
     * merely filtered itself open while typing is not something the user
     * asked for, so Esc goes past it and closes the prompt; otherwise a
     * live menu would make leaving the prompt take two presses.  Same
     * reasoning as the Enter rule above.
     */
    if (line->menu.items.len != 0U &&
        (line->menu.explicit_sel || line->menu_stem != NULL)) {
        char *stem = line->menu_stem == NULL ? NULL :
                     strcpy(yew_xmalloc(strlen(line->menu_stem) + 1U),
                            line->menu_stem);
        Span replace = line->menu.replace;

        menu_discard(cx->ed);
        if (stem != NULL) {
            bool ok = replace_span(cx->ed, replace, (const u8 *)stem,
                                   strlen(stem), true);

            yew_xfree(stem);
            return ok ? YEW_CMD_OK : YEW_CMD_ERR_IO;
        }
        return YEW_CMD_OK;
    }
    yew_cmdline_close(cx->ed, false);
    return YEW_CMD_OK;
}

static Cell styled_blank(const YewUiStyle *style)
{
    Cell cell = {0};

    cell.fg = style->row_fg;
    cell.bg = style->row_bg;
    cell.attrs = style->attrs;
    cell.w = 1U;
    return cell;
}

static void text_copy_span(const TextBuf *tb, Span span, u8 *out)
{
    TextIter it;
    u64 copied = 0U;
    u64 len = span.hi - span.lo;

    if (len == 0U)
        return;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        YEW_BUG("cannot draw command line span");
    while (copied < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &available))
            YEW_BUG("command line draw iterator ended early");
        take = available < len - copied ? available : len - copied;
        (void)memcpy(out + copied, bytes, (size_t)take);
        copied += take;
        if (copied < len && !yew_textiter_advance(&it, tb))
            YEW_BUG("command line draw iterator advance failed");
    }
}

static void draw_menu(Ed *ed, u16 footer, const YewUiStyle *style)
{
    /* Everything above the prompt row is the menu's to use; the widget
     * bottom-aligns itself inside it and registers its own rows. */
    Rect area = {0U, 0U, ed->grid.cols, footer};

    if (footer == 0U)
        return;
    yew_menu_draw(ed, &ed->cmdline.menu, area, style);
}

static size_t prompt_message_row(const char *text, size_t len, u16 cells,
                                 size_t *advance)
{
    const char *newline = memchr(text, '\n', len);
    size_t line_len = newline == NULL ? len : (size_t)(newline - text);
    size_t take;

    if (line_len == 0U) {
        *advance = newline == NULL ? 0U : 1U;
        return 0U;
    }
    take = yew_str_clip((const u8 *)text, line_len, (int)cells, NULL);
    if (take == 0U) {
        /* A wide grapheme cannot fit a one-cell terminal, but it still
         * has to advance or redraw would loop forever. */
        *advance = yew_gb_next_bytes((const u8 *)text, line_len, 0U);
        return 0U;
    }
    *advance = take;
    if (take == line_len && newline != NULL)
        (*advance)++;
    return take;
}

static void draw_prompt_message_full(Ed *ed, u16 footer,
                                     const YewUiStyle *style,
                                     const char *text, size_t len)
{
    size_t pos = 0U;
    size_t rows = 0U;
    size_t skip;
    size_t seen = 0U;
    u16 first;
    u16 row;

    while (pos < len) {
        size_t advance;

        (void)prompt_message_row(text + pos, len - pos, ed->grid.cols,
                                 &advance);
        if (advance == 0U)
            break;
        pos += advance;
        rows++;
    }
    if (rows == 0U)
        return;
    /* If a terminal is too short for the disclosure, retain its tail so
     * the question and safe default remain visible and keyboard-reachable. */
    skip = rows > footer ? rows - footer : 0U;
    first = rows < footer ? (u16)(footer - rows) : 0U;
    row = first;
    pos = 0U;
    while (pos < len && row < footer) {
        size_t advance;
        size_t take = prompt_message_row(text + pos, len - pos,
                                         ed->grid.cols, &advance);

        if (advance == 0U)
            break;
        if (seen++ >= skip) {
            yew_grid_fill(&ed->grid, row, 0U, ed->grid.cols,
                          styled_blank(style));
            if (take != 0U) {
                (void)yew_grid_puts(&ed->grid, row, 0U,
                                    (const u8 *)text + pos, take,
                                    style->row_fg, style->row_bg,
                                    style->attrs);
            }
            row++;
        }
        pos += advance;
    }
}

static void draw_prompt_message(Ed *ed, u16 footer,
                                const YewUiStyle *style)
{
    char message[sizeof(ed->msg.text) + 4U];
    YewUiStyle message_style = *style;

    if (footer == 0U || ed->cmdline.menu.items.len != 0U)
        return;
    if (ed->cmdline.err.msg[0] != '\0') {
        message_style.row_fg = (YewColor){YEW_COLOR_INDEXED, 196U, 0U, 0U};
        message_style.attrs |= YEW_ATTR_BOLD;
        (void)snprintf(message, sizeof(message), "E: %s",
                       ed->cmdline.err.msg);
    } else if (ed->msg.active) {
        const char *full = ed->msg.full == NULL ? ed->msg.text :
                                                  ed->msg.full;

        message_style = yew_message_style(ed);
        if (ed->cmdline.kind == YEW_PROMPT_INPUT &&
            (ed->msg.full != NULL ||
             memchr(full, '\n', ed->msg.len) != NULL)) {
            draw_prompt_message_full(ed, footer, &message_style, full,
                                     ed->msg.len);
            return;
        }
        (void)snprintf(message, sizeof(message),
                       ed->msg.sev == YEW_MSG_ERROR ? "E: %s" : "%s",
                       ed->msg.text);
    } else if (ed->cmdline.hint[0] != '\0') {
        /*
         * §9: a hint, not a diagnostic.  It draws in the ORDINARY footer
         * style -- dimmed, unadorned -- because styling the normal state
         * of a half-typed line as a failure is what makes a message line
         * flash through a word being typed.
         */
        message_style.attrs |= YEW_ATTR_DIM;
        (void)snprintf(message, sizeof(message), "%s", ed->cmdline.hint);
    } else {
        return;
    }
    yew_grid_fill(&ed->grid, (u16)(footer - 1U), 0U, ed->grid.cols,
                  styled_blank(&message_style));
    (void)yew_grid_puts(&ed->grid, (u16)(footer - 1U), 0U,
                        (const u8 *)message, strlen(message),
                        message_style.row_fg, message_style.row_bg,
                        message_style.attrs);
}

void yew_cmdline_draw(Ed *ed, Rect rect)
{
    CmdLine *line;
    Span span;
    CCol caret;
    u64 visible;
    ByteOff at;
    CCol logical;
    u16 col;
    u16 right;
    YewUiStyle style;
    char prefix;

    if (ed == NULL || !ed->cmdline.active || rect.h == 0U ||
        rect.y >= ed->grid.rows)
        return;
    line = &ed->cmdline;
    sync_from_target(line);
    style = yew_statusline_mode_style(YEW_MODE_E);
    yew_grid_fill(&ed->grid, rect.y, rect.x,
                  (u16)(rect.x + rect.w), styled_blank(&style));
    prefix = line->kind == YEW_PROMPT_SEARCH_F ? '/' :
             line->kind == YEW_PROMPT_SEARCH_B ? '?' : ':';
    col = yew_grid_put(&ed->grid, rect.y, rect.x, (const u8 *)&prefix, 1U,
                       style.chip_fg, style.chip_bg, style.attrs);
    right = (u32)rect.x + rect.w > ed->grid.cols ? ed->grid.cols :
                                                   (u16)(rect.x + rect.w);
    span = (Span){0U, yew_textbuf_len(line->buf)};
    caret = yew_off_to_ccol(line->buf, span, line->cur.pos,
                            YEW_CMDLINE_TABWIDTH);
    visible = right > col ? (u64)(right - col) : 0U;
    if (caret.v < line->scroll)
        line->scroll = caret.v > UINT16_MAX ? UINT16_MAX : (u16)caret.v;
    else if (visible != 0U && caret.v >= (u64)line->scroll + visible) {
        u64 next = caret.v - visible + 1U;

        line->scroll = next > UINT16_MAX ? UINT16_MAX : (u16)next;
    }
    at = yew_ccol_to_off(line->buf, span, (CCol){line->scroll},
                         YEW_CMDLINE_TABWIDTH);
    logical = yew_off_to_ccol(line->buf, span, at, YEW_CMDLINE_TABWIDTH);
    if (logical.v > line->scroll && col < right) {
        u64 gap = logical.v - line->scroll;
        u16 take = gap > (u64)(right - col) ? (u16)(right - col) :
                                              (u16)gap;

        yew_grid_fill(&ed->grid, rect.y, col, (u16)(col + take),
                      styled_blank(&style));
        col = (u16)(col + take);
    }
    while (at.v < span.hi && col < right) {
        YewTextCluster cluster;
        u64 n;
        u8 local[64];
        u8 *bytes = local;

        if (!yew_text_cluster_next(line->buf, span, at, &cluster))
            YEW_BUG("cannot decode command line cluster");
        n = cluster.bytes.hi - cluster.bytes.lo;
        if (n > sizeof(local))
            bytes = yew_xmalloc((size_t)n);
        text_copy_span(line->buf, cluster.bytes, bytes);
        if (cluster.tab) {
            u32 cells = yew_tab_cells(logical, YEW_CMDLINE_TABWIDTH);
            u16 take = cells > (u32)(right - col) ? (u16)(right - col) :
                                                    (u16)cells;

            yew_grid_fill(&ed->grid, rect.y, col, (u16)(col + take),
                          styled_blank(&style));
            col = (u16)(col + take);
        } else {
            col = yew_grid_put(&ed->grid, rect.y, col, bytes, (size_t)n,
                               style.row_fg, style.row_bg, 0U);
        }
        if (bytes != local)
            yew_xfree(bytes);
        logical.v += cluster.cells;
        at = BYTEOFF(cluster.bytes.hi);
    }
    if (line->err.tok_hi > line->err.tok_lo && span.hi != 0U) {
        CCol lo = yew_off_to_ccol(line->buf, span,
                                  BYTEOFF(line->err.tok_lo),
                                  YEW_CMDLINE_TABWIDTH);
        CCol hi = yew_off_to_ccol(line->buf, span,
                                  BYTEOFF(line->err.tok_hi),
                                  YEW_CMDLINE_TABWIDTH);
        u64 x0v = lo.v > line->scroll ?
                   (u64)(rect.x + 1U) + lo.v - line->scroll :
                   (u64)(rect.x + 1U);
        u64 x1v = hi.v > line->scroll ?
                   (u64)(rect.x + 1U) + hi.v - line->scroll : x0v + 1U;
        Cell error_cell = styled_blank(&style);
        u16 x0 = x0v > right ? right : (u16)x0v;
        u16 x1 = x1v > right ? right : (u16)x1v;

        error_cell.bg = (YewColor){YEW_COLOR_INDEXED, 196U, 0U, 0U};
        error_cell.attrs |= YEW_ATTR_UNDERLINE;
        if (x1 <= x0 && x0 < right)
            x1 = (u16)(x0 + 1U);
        yew_grid_overlay(&ed->grid, rect.y, x0, x1, &error_cell,
                         YEW_OVERLAY_BG | YEW_OVERLAY_ATTRS);
    }
    /*
     * Sprint 57.30 §2: the walked entry's first occurrence of the term,
     * in the style the document gives a `/` match.  Drawn before the
     * selection, which wins where they meet; like it, clamped to the
     * text drawn.
     */
    {
        Span hit;

        if (yew_cmdline_hist_match(ed, &hit)) {
            CCol lo = yew_off_to_ccol(line->buf, span, BYTEOFF(hit.lo),
                                      YEW_CMDLINE_TABWIDTH);
            CCol hi = yew_off_to_ccol(line->buf, span, BYTEOFF(hit.hi),
                                      YEW_CMDLINE_TABWIDTH);
            u64 base = (u64)rect.x + 1U;
            u64 x0v = lo.v > line->scroll ? base + lo.v - line->scroll : base;
            u64 x1v = hi.v > line->scroll ? base + hi.v - line->scroll : base;
            u16 x0 = x0v > col ? col : (u16)x0v;
            u16 x1 = x1v > col ? col : (u16)x1v;
            Cell match_style;
            u8 fields = yew_draw_search_style(ed, false, &match_style);

            if (x0 < x1)
                yew_grid_overlay(&ed->grid, rect.y, x0, x1, &match_style,
                                 fields);
        }
    }
    /*
     * Sprint 57.29 §3: the selection, in the document's selection style,
     * over the error token so the user sees what the next key replaces.
     * Its edges are grapheme boundaries, so a wide cluster is in or out
     * whole (and yew_grid_overlay styles both cells of a pair it
     * touches).  Either edge may lie past the scroll, so both clamp to
     * the text drawn: `col` is where it ended, and the ghost after it is
     * never selected.
     */
    {
        Span sel;

        if (yew_cmdline_selection(ed, &sel)) {
            CCol lo = yew_off_to_ccol(line->buf, span, BYTEOFF(sel.lo),
                                      YEW_CMDLINE_TABWIDTH);
            CCol hi = yew_off_to_ccol(line->buf, span, BYTEOFF(sel.hi),
                                      YEW_CMDLINE_TABWIDTH);
            u64 base = (u64)rect.x + 1U;
            u64 x0v = lo.v > line->scroll ? base + lo.v - line->scroll : base;
            u64 x1v = hi.v > line->scroll ? base + hi.v - line->scroll : base;
            u16 x0 = x0v > col ? col : (u16)x0v;
            u16 x1 = x1v > col ? col : (u16)x1v;
            Cell sel_style;
            u8 fields = yew_draw_sel_style(ed, &sel_style);

            if (x0 < x1)
                yew_grid_overlay(&ed->grid, rect.y, x0, x1, &sel_style,
                                 fields);
        }
    }
    /*
     * §7: the suggestion trails the caret, dim, and is drawn AFTER the
     * line text so it can only ever occupy cells the text did not.  It
     * never scrolls the prompt: horizontal scroll follows the caret, and
     * the caret sits before the ghost, so a long suggestion cannot push
     * what the user is reading off the left edge.
     */
    if (col < right) {
        size_t ghost_len;
        const char *ghost = cmdline_ghost(ed, &ghost_len);

        if (ghost != NULL && ghost_len != 0U) {
            YewUiStyle ghost_style = style;
            size_t keep = yew_str_clip((const u8 *)ghost, ghost_len,
                                       (int)(right - col), NULL);

            ghost_style.attrs |= YEW_ATTR_DIM;
            (void)yew_grid_puts(&ed->grid, rect.y, col,
                                (const u8 *)ghost, keep,
                                ghost_style.row_fg, ghost_style.row_bg,
                                ghost_style.attrs);
        }
    }
    draw_menu(ed, rect.y, &style);
    draw_prompt_message(ed, rect.y, &style);
    {
        u64 cursor_x = (u64)(rect.x + 1U) +
                       (caret.v > line->scroll ?
                            caret.v - line->scroll : 0U);
        u16 x = cursor_x >= right ? (right == 0U ? 0U :
                                      (u16)(right - 1U)) : (u16)cursor_x;

        yew_grid_cursor_shape(&ed->grid, YEW_CURSOR_BAR);
        yew_grid_cursor(&ed->grid, rect.y, x, right != 0U);
    }
}
