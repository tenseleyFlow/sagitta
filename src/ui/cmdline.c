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
#include "fl/flruntime.h"
#include "term/grid.h"
#include "text/edit.h"
#include "text/register.h"
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
    free(target);
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
    free(line->menu_stem);
    line->menu_stem = NULL;
    line->menu_original = (Span){0U, 0U};
    line->comp_total = 0U;
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

static CmdStatus insert_sanitized(Ed *ed, const u8 *bytes, size_t len)
{
    Bytebuf clean;
    CmdStatus status;

    if (len != 0U && memchr(bytes, '\0', len) != NULL) {
        yew_msg(ed, YEW_MSG_ERROR,
                "NUL byte is not valid in a command line");
        return YEW_CMD_ERR_ARG;
    }
    bytebuf_init(&clean);
    sanitize_bytes(bytes, len, &clean);
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
    line->cur.goal_col = (GCol){0U};
    sync_to_target(line);
    if (reset_history) {
        char *draft = text_string(line->buf);

        yew_hist_cur_reset(&line->hist, draft);
        free(draft);
    }
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
    {
        char *draft = text_string(line->buf);

        yew_hist_cur_reset(&line->hist, draft);
        free(draft);
    }
    line->err = (CmdErr){0};
    line->scroll = 0U;
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
    history_release(ed, line->kind, line->history);
    line->history = NULL;
    yew_hist_cur_dispose(&line->hist);
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
        free(input_text);
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

static void cmdline_refilter(Ed *ed)
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
        free(text);
        ed->full_damage = true;
        return;
    }
    cmdline_set_hint(ed, &point);
    if (!yew_comp_query_at(ed, &point, &query) ||
        query.replace.hi <= query.replace.lo) {
        yew_menu_dismiss(&line->menu);
        arena_free_all(&scratch);
        free(text);
        ed->full_damage = true;
        return;
    }
    line->comp_total = yew_comp_filter_run(ed, &line->filter,
                                           &line->comp_arena, &query,
                                           YEW_CMDLINE_LIVE_BUDGET_US,
                                           &items);
    if (items.len == 0U) {
        Vec_CompItem_free(&items);
        yew_menu_dismiss(&line->menu);
    } else {
        yew_menu_reset(&line->menu, items, line->comp_total, query.replace);
    }
    arena_free_all(&scratch);
    free(text);
    ed->full_damage = true;
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

void yew_cmdline_edited(Ed *ed)
{
    char *draft;

    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    draft = text_string(ed->cmdline.buf);
    yew_hist_cur_reset(&ed->cmdline.hist, draft);
    free(draft);
    clear_error(ed);
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
    free(text);
}

void yew_cmdline_sync(Ed *ed)
{
    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    ed->footer_dirty = true;
}

bool yew_cmdline_key(Ed *ed, const Key *key)
{
    const u16 command_mods = YEW_MOD_ALT | YEW_MOD_CTRL | YEW_MOD_SUPER |
                             YEW_MOD_HYPER | YEW_MOD_META;

    if (ed == NULL || key == NULL || !ed->cmdline.active)
        return false;
    if (key->ev == YEW_KEY_RELEASE)
        return true;
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

static CmdStatus history_move(CmdCtx *cx, bool previous)
{
    const char *found;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    found = previous ? yew_hist_prev(cx->ed->cmdline.history,
                                     &cx->ed->cmdline.hist) :
                       yew_hist_next(cx->ed->cmdline.history,
                                     &cx->ed->cmdline.hist);
    if (found == NULL)
        return YEW_CMD_OK;
    if (!replace_all(cx->ed, found, false))
        return YEW_CMD_ERR_IO;
    /* A history jump rewrites the whole line without going through the
     * edit hook, so the menu is refiltered here rather than left stale. */
    cmdline_refilter(cx->ed);
    return YEW_CMD_OK;
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
    if (trailing_space && !item->is_dir)
        bytebuf_push_u8(&bytes, (u8)' ');
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
    item = yew_menu_selected(&line->menu);
    if (item == NULL)
        return YEW_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return YEW_CMD_ERR_IO;
    ed->full_damage = true;
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
    if (line->menu.explicit_sel)
        return completion_cycle(ed, previous);
    text = text_string(line->buf);
    arena_init(&scratch);
    if (!yew_comp_query(ed, text, (size_t)yew_textbuf_len(line->buf),
                        (size_t)line->cur.pos.v,
                        &scratch, &query)) {
        arena_free_all(&scratch);
        free(text);
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
    if (items.len == 0U) {
        yew_msg(ed, YEW_MSG_INFO, "no completions");
        ed->full_damage = true;
        ed->footer_dirty = true;
        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        free(text);
        return YEW_CMD_OK;
    }
    if (items.len == 1U) {
        bool ok = insert_completion(ed, query.replace, &items.data[0], true);

        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        free(text);
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
    free(line->menu_stem);
    line->menu_stem = heap_slice(text, query.replace);
    line->menu_original = query.replace;
    yew_menu_reset(&line->menu, items, line->comp_total, query.replace);
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
    if (strlen(lcp) > stem_len) {
        /* The prefix every candidate shares is unambiguous, so insert it
         * and leave the list open with nothing selected -- the user has
         * still not chosen a row. */
        if (!replace_span(ed, query.replace, (const u8 *)lcp, strlen(lcp),
                          true)) {
            menu_discard(ed);
            arena_free_all(&scratch);
            free(text);
            return YEW_CMD_ERR_IO;
        }
        line->menu.replace = (Span){query.replace.lo,
                                    query.replace.lo + strlen(lcp)};
    } else {
        /* Nothing left to insert unambiguously, so this Tab is a choice:
         * enter the list (from the far end for S-Tab). */
        (void)yew_menu_move(&line->menu, previous ? -1 : 1, false);
        {
            const CompItem *item = yew_menu_selected(&line->menu);

            if (item != NULL &&
                !insert_completion(ed, line->menu.replace, item, false)) {
                arena_free_all(&scratch);
                free(text);
                return YEW_CMD_ERR_IO;
            }
        }
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
    arena_free_all(&scratch);
    free(text);
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
    item = yew_menu_selected(&line->menu);
    if (item == NULL)
        return YEW_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return YEW_CMD_ERR_IO;
    ed->full_damage = true;
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

static CmdStatus delete_range(CmdCtx *cx, Span span)
{
    EditCtx ec;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL)
        return YEW_CMD_ERR_STATE;
    ec = yew_ed_edit_ctx_for(cx->ed, cx->win);
    if (!yew_edit_delete(&ec, span)) {
        yew_ed_finish_edit(cx->ed, &ec);
        return YEW_CMD_ERR_IO;
    }
    yew_ed_finish_edit(cx->ed, &ec);
    return YEW_CMD_OK;
}

CmdStatus yew_cmdline_cmd_delete_word_prev(CmdCtx *cx)
{
    Cursor *cursor;
    UnitCtx unit;
    ByteOff previous;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->cs.curs.len == 0U)
        return YEW_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    unit = (UnitCtx){cx->win->buf->tb, cx->win->buf, cx->win};
    previous = yew_unit_word.prev(&unit, cursor->pos, false);
    return delete_range(cx, (Span){previous.v, cursor->pos.v});
}

CmdStatus yew_cmdline_cmd_delete_to_home(CmdCtx *cx)
{
    Cursor *cursor;

    if (cx == NULL || cx->win == NULL || cx->win->cs.curs.len == 0U)
        return YEW_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    return delete_range(cx, (Span){0U, cursor->pos.v});
}

CmdStatus yew_cmdline_cmd_delete_to_end(CmdCtx *cx)
{
    Cursor *cursor;
    u64 len;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->cs.curs.len == 0U)
        return YEW_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    len = yew_textbuf_len(cx->win->buf->tb);
    return delete_range(cx, (Span){cursor->pos.v, len});
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
static const char *cmdline_ghost(Ed *ed, size_t *len)
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

CmdStatus yew_cmdline_cmd_ghost_accept(CmdCtx *cx)
{
    Ed *ed;
    const CompItem *item;
    const char *ghost;
    size_t len;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    ghost = cmdline_ghost(ed, &len);
    if (ghost == NULL) {
        /*
         * No suggestion under the caret, so this is just a motion.  The
         * fallback is one grapheme right rather than end-of-line, which
         * is why this is bound to Right and not to C-e: at end of line
         * the two agree, but anywhere else they do not.
         */
        CmdCtx move = {0};

        move.win = yew_cmdline_target(ed);
        move.count = 1U;
        move.source = cx->source;
        return yew_ed_invoke(ed, yew_cmd_lookup("ed.move.char.next", 17U),
                             &move);
    }
    item = yew_menu_selected(&ed->cmdline.menu);
    if (item == NULL)
        item = &ed->cmdline.menu.items.data[0];
    /* One accept path, shared with the menu's: a ghost accepted and a
     * row accepted must land byte-identical text. */
    if (!insert_completion(ed, ed->cmdline.menu.replace, item, true))
        return YEW_CMD_ERR_IO;
    menu_discard(ed);
    cmdline_refilter(ed);
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
        free(text);
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
        free(text);
        yew_cmdline_close(ed, true);
        return YEW_CMD_OK;
    }
    if (line->kind == YEW_PROMPT_INPUT) {
        yew_hist_add(line->history, text);
        free(text);
        yew_cmdline_close(ed, true);
        return YEW_CMD_OK;
    }
    arena_init(&arena);
    if (!yew_cmd_parse(ed, text, (size_t)yew_textbuf_len(line->buf),
                       &arena, &parsed)) {
        set_error(ed, &parsed.err);
        arena_free_all(&arena);
        free(text);
        return YEW_CMD_ERR_ARG;
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
        free(text);
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
    free(text);
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

            free(stem);
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
            free(bytes);
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
