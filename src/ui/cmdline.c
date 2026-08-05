#include "edit/search_cmds.h"
#include "search/searchui.h"
#include "ui/cmdline.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/motion.h"
#include "term/grid.h"
#include "text/edit.h"
#include "text/register.h"
#include "ui/message.h"
#include "ui/statusline.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "util/log.h"

enum {
    SAG_CMDLINE_TABWIDTH = 4,
    SAG_CMDLINE_MENU_ROWS = 5,
    /*
     * Advisory budget for a refilter that runs inside a keystroke.  Tab
     * passes 0 (unlimited) because the user asked a question and is
     * waiting for the answer; typing has to stay inside the frame.
     */
    SAG_CMDLINE_LIVE_BUDGET_US = 5000
};

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
    u64 total = sag_textbuf_len(tb);
    u64 copied = 0U;
    char *text;

    if (total > (u64)SIZE_MAX - 1U)
        SAG_BUG("command line exceeds address space");
    text = sag_xmalloc((size_t)total + 1U);
    if (total != 0U) {
        if (!sag_textiter_begin(&it, tb, BYTEOFF(0U)))
            SAG_BUG("cannot iterate command line");
        while (copied < total) {
            const u8 *bytes;
            u64 available;
            u64 take;

            if (!sag_textiter_chunk(&it, tb, &bytes, &available) ||
                available == 0U)
                SAG_BUG("command line iterator ended early");
            take = available < total - copied ? available : total - copied;
            (void)memcpy(text + (size_t)copied, bytes, (size_t)take);
            copied += take;
            if (copied < total && !sag_textiter_advance(&it, tb))
                SAG_BUG("command line iterator advance failed");
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
    sag_vp_free(&target->win);
    sag_cset_free(&target->win.cs);
    sag_marks_free(target->buffer.marks);
    sag_undo_free(target->buffer.undo);
    sag_textbuf_free(target->buffer.tb);
    sag_filemeta_dispose(&target->buffer.meta);
    free(target);
}

static void menu_discard(Ed *ed)
{
    CmdLine *line = &ed->cmdline;
    bool was_open = line->menu.items.len != 0U || line->menu.sel >= 0;

    sag_menu_dismiss(&line->menu);
    /* The cached candidate set's strings live in this arena, so the
     * cache dies with it -- a surviving `valid` flag over freed strings
     * is a use-after-free waiting for the next keystroke. */
    sag_comp_filter_invalidate(&line->filter);
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
    if (ed->msg.active && ed->msg.sev == SAG_MSG_ERROR)
        sag_msg_clear(ed);
}

static void set_error(Ed *ed, const CmdErr *error)
{
    CmdLine *line = &ed->cmdline;
    u64 len = sag_textbuf_len(line->buf);
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
    sag_menu_dismiss(&line->menu);
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
    sag_cursor_clamp(line->buf, &line->cur);
    sync_to_target(line);
    sag_msg(ed, SAG_MSG_ERROR, "E: %s", line->err.msg);
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
    CmdId id = sag_cmd_lookup("ed.edit.insert.text", 19U);
    CmdStatus status;

    if (len > UINT32_MAX)
        len = UINT32_MAX;
    cx.win = sag_cmdline_target(ed);
    cx.count = 1U;
    cx.sarg = (const char *)bytes;
    cx.sarg_len = (u32)len;
    cx.source = SAG_SRC_KEY;
    status = sag_ed_invoke(ed, id, &cx);
    sync_from_target(&ed->cmdline);
    return status;
}

static CmdStatus insert_sanitized(Ed *ed, const u8 *bytes, size_t len)
{
    Bytebuf clean;
    CmdStatus status;

    if (len != 0U && memchr(bytes, '\0', len) != NULL) {
        sag_msg(ed, SAG_MSG_ERROR,
                "NUL byte is not valid in a command line");
        return SAG_CMD_ERR_ARG;
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
        span.hi > sag_textbuf_len(line->buf))
        return false;
    sync_to_target(line);
    ec = sag_ed_edit_ctx_for(ed, &target->win);
    sag_undo_begin(&ec, SAG_TXN_TYPE);
    if (span.lo != span.hi)
        ok = sag_edit_delete(&ec, span);
    if (ok && len != 0U)
        ok = sag_edit_insert(&ec, BYTEOFF(span.lo), bytes, (u64)len);
    if (ok)
        sag_undo_end(&ec);
    else
        sag_undo_abort(&ec);
    sag_ed_finish_edit(ed, &ec);
    if (!ok)
        return false;
    line->cur.pos = BYTEOFF(span.lo + len);
    line->cur.anchor = line->cur.pos;
    line->cur.goal_col = (GCol){0U};
    sync_to_target(line);
    if (reset_history) {
        char *draft = text_string(line->buf);

        sag_hist_cur_reset(&line->hist, draft);
        free(draft);
    }
    clear_error(ed);
    ed->footer_dirty = true;
    return true;
}

static bool replace_all(Ed *ed, const char *text, bool reset_history)
{
    return replace_span(ed, (Span){0U, sag_textbuf_len(ed->cmdline.buf)},
                        (const u8 *)text, strlen(text), reset_history);
}

static void set_cmd_register(Ed *ed, const char *text)
{
    sag_reg_set_cmdline(&ed->regs, (const u8 *)text, strlen(text));
}

static const char *history_kind(SagPromptKind kind)
{
    switch (kind) {
    case SAG_PROMPT_CMD:
        return "cmd";
    case SAG_PROMPT_SEARCH_F:
    case SAG_PROMPT_SEARCH_B:
        return "search";
    case SAG_PROMPT_INPUT:
        return "input";
    }
    return "cmd";
}

void sag_cmdline_open(Ed *ed, SagPromptKind kind, const char *seed)
{
    CmdLine *line;
    CmdLineTarget *target;
    Bytebuf clean;
    Cursor cursor;

    if (ed == NULL)
        return;
    if (ed->cmdline.active)
        sag_cmdline_close(ed, false);
    line = &ed->cmdline;
    target = sag_xcalloc(1U, sizeof(*target));
    bytebuf_init(&clean);
    if (seed != NULL)
        sanitize_bytes((const u8 *)seed, strlen(seed), &clean);
    sag_filemeta_init(&target->buffer.meta);
    target->buffer.tb = sag_textbuf_from_bytes(clean.data, clean.len);
    target->buffer.tabwidth = SAG_CMDLINE_TABWIDTH;
    target->buffer.undo = sag_undo_new(target->buffer.tb);
    target->buffer.marks = sag_marks_new();
    cursor = (Cursor){BYTEOFF(clean.len), {0U}, BYTEOFF(clean.len)};
    sag_cset_init(&target->win.cs, cursor);
    target->win.buf = &target->buffer;
    sag_vp_init(&target->win);
    bytebuf_free(&clean);

    line->kind = kind;
    line->active = true;
    line->buf = target->buffer.tb;
    line->cur = cursor;
    line->target = target;
    line->return_mode = (u8)ed->mode;
    line->history = sag_hist_open(history_kind(kind));
    {
        /* Inline, five rows, wrapping, detail at column 31 -- the
         * geometry Sprint 18's goldens pinned, now expressed as a spec
         * so Sprint 26's picker can pick a different one. */
        MenuSpec spec = {NULL, 5U, true, true, 31U};

        sag_menu_init(&line->menu, &spec);
    }
    sag_comp_filter_init(&line->filter);
    {
        char *draft = text_string(line->buf);

        sag_hist_cur_reset(&line->hist, draft);
        free(draft);
    }
    line->err = (CmdErr){0};
    line->scroll = 0U;
    sag_msg_clear(ed);
    sag_dispatch_set_mode(ed, SAG_MODE_E);
    ed->full_damage = true;
    ed->footer_dirty = true;
}

void sag_cmdline_close(Ed *ed, bool accepted)
{
    CmdLine *line;
    Mode restore;

    if (ed == NULL || !ed->cmdline.active)
        return;
    line = &ed->cmdline;
    if (line->kind == SAG_PROMPT_SEARCH_F ||
        line->kind == SAG_PROMPT_SEARCH_B) {
        /* Accept commits the pattern and the jump; cancel restores the
         * view exactly, which is why it happens BEFORE the widget tears
         * down and repaints. */
        if (accepted)
            sag_search_accept(ed, ed->win);
        else
            sag_search_cancel(ed, ed->win);
    }
    restore = line->return_mode < SAG_MODE__N ? (Mode)line->return_mode :
                                               SAG_MODE_L;
    if (restore == SAG_MODE_E)
        restore = SAG_MODE_L;
    menu_discard(ed);
    sag_menu_free(&line->menu);
    sag_comp_filter_free(&line->filter);
    sag_hist_flush(line->history);
    sag_hist_close(line->history);
    line->history = NULL;
    sag_hist_cur_dispose(&line->hist);
    cmdline_target_free(cmdline_target(line));
    line->target = NULL;
    line->buf = NULL;
    line->active = false;
    line->err = (CmdErr){0};
    line->scroll = 0U;
    sag_msg_clear(ed);
    sag_dispatch_set_mode(ed, restore);
    ed->full_damage = true;
    ed->footer_dirty = true;
    /* A `:s/../../c` started a confirm run whose question this close
     * just wiped; restate it. */
    sag_search_confirm_reprompt(ed);
}

void sag_cmdline_dispose(Ed *ed)
{
    if (ed != NULL && ed->cmdline.active)
        sag_cmdline_close(ed, false);
}

Win *sag_cmdline_target(Ed *ed)
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
    desc = sag_cmd_desc(point->command);
    entry = sag_cmd_entry(point->command);
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
    if (point->range.kind == SAG_RANGE_BUFFER)
        (void)snprintf(line->hint + at, sizeof(line->hint) - at,
                       " \xC2\xB7 whole buffer");
    else if (point->range.kind == SAG_RANGE_SELECTION)
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
    SagCompQuery query;
    Vec_CompItem items = {0};
    char *text;

    /* Only `:` completes; `/` and `?` carry a pattern, not a command. */
    if (line->kind != SAG_PROMPT_CMD) {
        sag_menu_dismiss(&line->menu);
        line->hint[0] = '\0';
        return;
    }
    text = text_string(line->buf);
    arena_init(&scratch);
    /* ONE tolerant parse per keystroke, read by both the hint and the
     * filter.  Two would drift apart. */
    if (!sag_cmd_parse_point(ed, text, (size_t)sag_textbuf_len(line->buf),
                             (size_t)line->cur.pos.v, &scratch, &point)) {
        sag_menu_dismiss(&line->menu);
        line->hint[0] = '\0';
        arena_free_all(&scratch);
        free(text);
        ed->full_damage = true;
        return;
    }
    cmdline_set_hint(ed, &point);
    if (!sag_comp_query_at(ed, &point, &query) ||
        query.replace.hi <= query.replace.lo) {
        sag_menu_dismiss(&line->menu);
        arena_free_all(&scratch);
        free(text);
        ed->full_damage = true;
        return;
    }
    line->comp_total = sag_comp_filter_run(ed, &line->filter,
                                           &line->comp_arena, &query,
                                           SAG_CMDLINE_LIVE_BUDGET_US,
                                           &items);
    if (items.len == 0U) {
        Vec_CompItem_free(&items);
        sag_menu_dismiss(&line->menu);
    } else {
        sag_menu_reset(&line->menu, items, line->comp_total, query.replace);
    }
    arena_free_all(&scratch);
    free(text);
    ed->full_damage = true;
}

void sag_cmdline_edited(Ed *ed)
{
    char *draft;

    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    draft = text_string(ed->cmdline.buf);
    sag_hist_cur_reset(&ed->cmdline.hist, draft);
    free(draft);
    clear_error(ed);
    cmdline_refilter(ed);
    ed->footer_dirty = true;
    /* Search-as-you-type: the `/` and `?` prompts preview on every
     * edit.  This is the one place that hook belongs — the widget is
     * shared, and only these two kinds want it. */
    if (ed->cmdline.kind == SAG_PROMPT_SEARCH_F ||
        ed->cmdline.kind == SAG_PROMPT_SEARCH_B)
        sag_search_input(ed, ed->win);
}

/* The prompt's current text.  Sprint 21's search-as-you-type needs it
 * on every edit, and reaching into ed->cmdline.buf from another module
 * would make the widget's internals part of its interface. */
void sag_cmdline_text(Ed *ed, Bytebuf *out)
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

void sag_cmdline_sync(Ed *ed)
{
    if (ed == NULL || !ed->cmdline.active)
        return;
    sync_from_target(&ed->cmdline);
    ed->footer_dirty = true;
}

bool sag_cmdline_key(Ed *ed, const Key *key)
{
    const u16 command_mods = SAG_MOD_ALT | SAG_MOD_CTRL | SAG_MOD_SUPER |
                             SAG_MOD_HYPER | SAG_MOD_META;

    if (ed == NULL || key == NULL || !ed->cmdline.active)
        return false;
    if (key->ev == SAG_KEY_RELEASE)
        return true;
    if (key->code < SAG_KEY_BASE && key->ntext != 0U &&
        (key->mods & command_mods) == 0U) {
        /*
         * §6 inverts Sprint 18's rule: a printable key REFILTERS rather
         * than dismissing.  The insert runs through the registry, which
         * lands in sag_cmdline_edited, which refilters -- so there is
         * still exactly one place that reacts to a prompt edit.
         */
        (void)insert_sanitized(ed, key->text, key->ntext);
        return true;
    }
    return false;
}

void sag_cmdline_paste(Ed *ed, const u8 *bytes, size_t len)
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
        return SAG_CMD_ERR_STATE;
    found = previous ? sag_hist_prev(cx->ed->cmdline.history,
                                     &cx->ed->cmdline.hist) :
                       sag_hist_next(cx->ed->cmdline.history,
                                     &cx->ed->cmdline.hist);
    if (found == NULL)
        return SAG_CMD_OK;
    if (!replace_all(cx->ed, found, false))
        return SAG_CMD_ERR_IO;
    /* A history jump rewrites the whole line without going through the
     * edit hook, so the menu is refiltered here rather than left stale. */
    cmdline_refilter(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_hist_prev(CmdCtx *cx)
{
    return history_move(cx, true);
}

CmdStatus sag_cmdline_cmd_hist_next(CmdCtx *cx)
{
    return history_move(cx, false);
}

static char *heap_slice(const char *text, Span span)
{
    size_t len = (size_t)(span.hi - span.lo);
    char *copy = sag_xmalloc(len + 1U);

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

    if (!sag_menu_move(&line->menu, previous ? -1 : 1, false))
        return SAG_CMD_OK;
    item = sag_menu_selected(&line->menu);
    if (item == NULL)
        return SAG_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return SAG_CMD_ERR_IO;
    ed->full_damage = true;
    return SAG_CMD_OK;
}

static CmdStatus complete(Ed *ed, bool previous)
{
    CmdLine *line = &ed->cmdline;
    Arena scratch;
    SagCompQuery query;
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
    if (!sag_comp_query(ed, text, (size_t)sag_textbuf_len(line->buf),
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
    line->comp_total = sag_comp_filter_run(ed, &line->filter,
                                           &line->comp_arena, &query, 0,
                                           &items);
    if (items.len == 0U) {
        sag_msg(ed, SAG_MSG_INFO, "no completions");
        ed->full_damage = true;
        ed->footer_dirty = true;
        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        free(text);
        return SAG_CMD_OK;
    }
    if (items.len == 1U) {
        bool ok = insert_completion(ed, query.replace, &items.data[0], true);

        Vec_CompItem_free(&items);
        arena_free_all(&scratch);
        free(text);
        if (!ok)
            return SAG_CMD_ERR_IO;
        /*
         * The line just changed under the live menu, which is still
         * holding rows for the OLD token.  Completion insertion does not
         * run through the edit hook, so refilter explicitly rather than
         * leaving stale rows on screen.
         */
        cmdline_refilter(ed);
        return SAG_CMD_OK;
    }
    free(line->menu_stem);
    line->menu_stem = heap_slice(text, query.replace);
    line->menu_original = query.replace;
    sag_menu_reset(&line->menu, items, line->comp_total, query.replace);
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
            if (line->menu.items.data[i].score >= SAG_FZ_BASENAME_TIER)
                Vec_CompItem_push(&tiered, line->menu.items.data[i]);
        }
        lcp = sag_comp_lcp(&scratch, &tiered);
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
            return SAG_CMD_ERR_IO;
        }
        line->menu.replace = (Span){query.replace.lo,
                                    query.replace.lo + strlen(lcp)};
    } else {
        /* Nothing left to insert unambiguously, so this Tab is a choice:
         * enter the list (from the far end for S-Tab). */
        (void)sag_menu_move(&line->menu, previous ? -1 : 1, false);
        {
            const CompItem *item = sag_menu_selected(&line->menu);

            if (item != NULL &&
                !insert_completion(ed, line->menu.replace, item, false)) {
                arena_free_all(&scratch);
                free(text);
                return SAG_CMD_ERR_IO;
            }
        }
    }
    ed->full_damage = true;
    ed->footer_dirty = true;
    arena_free_all(&scratch);
    free(text);
    return SAG_CMD_OK;
}

/*
 * Sprint 18.5 §10.  Every menu behaviour is a registered command, so it
 * is rebindable, recordable, and reachable from Fletch (Sprint 34)
 * rather than being a keystroke handled inside a switch.  They all carry
 * SAG_CMD_INTERNAL: they are keymap plumbing, not commands a user types.
 */
static CmdStatus menu_page(Ed *ed, bool previous)
{
    CmdLine *line = &ed->cmdline;
    const CompItem *item;

    if (!sag_menu_move(&line->menu, previous ? -1 : 1, true))
        return SAG_CMD_OK;
    item = sag_menu_selected(&line->menu);
    if (item == NULL)
        return SAG_CMD_OK;
    if (!insert_completion(ed, line->menu.replace, item, false))
        return SAG_CMD_ERR_IO;
    ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_menu_page_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    return menu_page(cx->ed, false);
}

CmdStatus sag_cmdline_cmd_menu_page_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    return menu_page(cx->ed, true);
}

/* No default binding: this is how §8's click and Fletch commit a row. */
CmdStatus sag_cmdline_cmd_menu_accept(CmdCtx *cx)
{
    Ed *ed;
    const CompItem *item;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    item = sag_menu_selected(&ed->cmdline.menu);
    if (item == NULL)
        return SAG_CMD_OK;
    if (!insert_completion(ed, ed->cmdline.menu.replace, item, true))
        return SAG_CMD_ERR_IO;
    menu_discard(ed);
    cmdline_refilter(ed);
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_menu_dismiss(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    menu_discard(cx->ed);
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_complete_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    return complete(cx->ed, false);
}

CmdStatus sag_cmdline_cmd_complete_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    return complete(cx->ed, true);
}

CmdStatus sag_cmdline_cmd_insert_register(CmdCtx *cx)
{
    RegVal *value;
    u8 name;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active ||
        cx->sarg == NULL || cx->sarg_len == 0U)
        return SAG_CMD_ERR_ARG;
    name = (u8)cx->sarg[0];
    value = sag_reg_get(&cx->ed->regs, name);
    if (value == NULL) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "unknown register '%c'", name);
        return SAG_CMD_ERR_ARG;
    }
    return insert_sanitized(cx->ed, value->bytes.data, value->bytes.len);
}

CmdStatus sag_cmdline_cmd_literal_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active ||
        cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    return insert_sanitized(cx->ed, (const u8 *)cx->sarg, cx->sarg_len);
}

static CmdStatus delete_range(CmdCtx *cx, Span span)
{
    EditCtx ec;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL)
        return SAG_CMD_ERR_STATE;
    ec = sag_ed_edit_ctx_for(cx->ed, cx->win);
    if (!sag_edit_delete(&ec, span)) {
        sag_ed_finish_edit(cx->ed, &ec);
        return SAG_CMD_ERR_IO;
    }
    sag_ed_finish_edit(cx->ed, &ec);
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_delete_word_prev(CmdCtx *cx)
{
    Cursor *cursor;
    UnitCtx unit;
    ByteOff previous;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->cs.curs.len == 0U)
        return SAG_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    unit = (UnitCtx){cx->win->buf->tb, cx->win->buf, cx->win};
    previous = sag_unit_word.prev(&unit, cursor->pos, false);
    return delete_range(cx, (Span){previous.v, cursor->pos.v});
}

CmdStatus sag_cmdline_cmd_delete_to_home(CmdCtx *cx)
{
    Cursor *cursor;

    if (cx == NULL || cx->win == NULL || cx->win->cs.curs.len == 0U)
        return SAG_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    return delete_range(cx, (Span){0U, cursor->pos.v});
}

CmdStatus sag_cmdline_cmd_delete_to_end(CmdCtx *cx)
{
    Cursor *cursor;
    u64 len;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->cs.curs.len == 0U)
        return SAG_CMD_ERR_STATE;
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    len = sag_textbuf_len(cx->win->buf->tb);
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
 * never typed, and make sag_cmdline_text() -- which Sprint 21's search
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
    buf_len = sag_textbuf_len(line->buf);
    /* Only at end of line: a suggestion in the middle of a line has no
     * coherent place to go. */
    if (line->cur.pos.v != buf_len || line->menu.replace.hi != buf_len)
        return NULL;
    item = sag_menu_selected(&line->menu);
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

CmdStatus sag_cmdline_cmd_ghost_accept(CmdCtx *cx)
{
    Ed *ed;
    const CompItem *item;
    const char *ghost;
    size_t len;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
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

        move.win = sag_cmdline_target(ed);
        move.count = 1U;
        move.source = cx->source;
        return sag_ed_invoke(ed, sag_cmd_lookup("ed.move.char.next", 17U),
                             &move);
    }
    item = sag_menu_selected(&ed->cmdline.menu);
    if (item == NULL)
        item = &ed->cmdline.menu.items.data[0];
    /* One accept path, shared with the menu's: a ghost accepted and a
     * row accepted must land byte-identical text. */
    if (!insert_completion(ed, ed->cmdline.menu.replace, item, true))
        return SAG_CMD_ERR_IO;
    menu_discard(ed);
    cmdline_refilter(ed);
    return SAG_CMD_OK;
}

static void deferred_dispatch_error(Ed *ed, const CmdParse *parsed)
{
    const CmdDesc *desc = sag_cmd_desc(parsed->command);
    CmdErr error = {0};
    const char *sprint;
    const char *label;

    error.tok_lo = (u32)parsed->name_tok.lo;
    error.tok_hi = (u32)parsed->name_tok.hi;
    label = desc == NULL ? "command" :
            strncmp(desc->name, "ed.", 3U) == 0 ? desc->name + 3U :
                                                   desc->name;
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
        (void)snprintf(error.msg, sizeof(error.msg),
                       ":%s lands in Sprint %s", label, number);
    } else {
        (void)snprintf(error.msg, sizeof(error.msg), ":%s failed", label);
    }
    set_error(ed, &error);
}

CmdStatus sag_cmdline_cmd_accept(CmdCtx *cx)
{
    Ed *ed;
    CmdLine *line;
    char *text;
    Arena arena;
    CmdParse parsed;
    SagCmdInvoke invoke;
    CmdStatus status;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    line = &ed->cmdline;
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
        const CompItem *item = sag_menu_selected(&line->menu);

        if (item != NULL && !insert_completion(ed, line->menu.replace, item,
                                               true))
            return SAG_CMD_ERR_IO;
        menu_discard(ed);
        return SAG_CMD_OK;
    }
    text = text_string(line->buf);
    if (sag_textbuf_len(line->buf) == 0U) {
        free(text);
        sag_cmdline_close(ed, true);
        return SAG_CMD_OK;
    }
    /*
     * A search prompt's text is a PATTERN, not a command line.  The
     * preview has already applied it and sag_search_accept commits it;
     * handing it to the command parser instead reports the pattern as
     * an unknown command, which is what `/needle` did before this
     * check existed.
     */
    if (line->kind == SAG_PROMPT_SEARCH_F ||
        line->kind == SAG_PROMPT_SEARCH_B) {
        sag_hist_add(line->history, text);
        free(text);
        sag_cmdline_close(ed, true);
        return SAG_CMD_OK;
    }
    arena_init(&arena);
    if (!sag_cmd_parse(ed, text, (size_t)sag_textbuf_len(line->buf),
                       &arena, &parsed)) {
        set_error(ed, &parsed.err);
        arena_free_all(&arena);
        free(text);
        return SAG_CMD_ERR_ARG;
    }
    invoke = (SagCmdInvoke){parsed.range, parsed.argv, 0, parsed.bang,
                            ed->win};
    status = sag_ed_invoke_parsed(ed, parsed.command, &invoke);
    if (status != SAG_CMD_OK) {
        if (status == SAG_CMD_ERR_DEFERRED)
            deferred_dispatch_error(ed, &parsed);
        else {
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
    sag_hist_add(line->history, text);
    set_cmd_register(ed, text);
    arena_free_all(&arena);
    free(text);
    sag_cmdline_close(ed, true);
    return SAG_CMD_OK;
}

CmdStatus sag_cmdline_cmd_cancel(CmdCtx *cx)
{
    CmdLine *line;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return SAG_CMD_ERR_STATE;
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
                     strcpy(sag_xmalloc(strlen(line->menu_stem) + 1U),
                            line->menu_stem);
        Span replace = line->menu.replace;

        menu_discard(cx->ed);
        if (stem != NULL) {
            bool ok = replace_span(cx->ed, replace, (const u8 *)stem,
                                   strlen(stem), true);

            free(stem);
            return ok ? SAG_CMD_OK : SAG_CMD_ERR_IO;
        }
        return SAG_CMD_OK;
    }
    sag_cmdline_close(cx->ed, false);
    return SAG_CMD_OK;
}

static Cell styled_blank(const SagUiStyle *style)
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
    if (!sag_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        SAG_BUG("cannot draw command line span");
    while (copied < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!sag_textiter_chunk(&it, tb, &bytes, &available))
            SAG_BUG("command line draw iterator ended early");
        take = available < len - copied ? available : len - copied;
        (void)memcpy(out + copied, bytes, (size_t)take);
        copied += take;
        if (copied < len && !sag_textiter_advance(&it, tb))
            SAG_BUG("command line draw iterator advance failed");
    }
}

static void draw_menu(Ed *ed, u16 footer, const SagUiStyle *style)
{
    /* Everything above the prompt row is the menu's to use; the widget
     * bottom-aligns itself inside it and registers its own rows. */
    Rect area = {0U, 0U, ed->grid.cols, footer};

    if (footer == 0U)
        return;
    sag_menu_draw(ed, &ed->cmdline.menu, area, style);
}

static void draw_prompt_message(Ed *ed, u16 footer,
                                const SagUiStyle *style)
{
    char message[sizeof(ed->msg.text) + 4U];
    SagUiStyle message_style = *style;

    if (footer == 0U || ed->cmdline.menu.items.len != 0U)
        return;
    if (ed->cmdline.err.msg[0] != '\0') {
        message_style.row_fg = (SagColor){SAG_COLOR_INDEXED, 196U, 0U, 0U};
        message_style.attrs |= SAG_ATTR_BOLD;
        (void)snprintf(message, sizeof(message), "E: %s",
                       ed->cmdline.err.msg);
    } else if (ed->msg.active) {
        message_style = sag_message_style(ed);
        (void)snprintf(message, sizeof(message),
                       ed->msg.sev == SAG_MSG_ERROR ? "E: %s" : "%s",
                       ed->msg.text);
    } else if (ed->cmdline.hint[0] != '\0') {
        /*
         * §9: a hint, not a diagnostic.  It draws in the ORDINARY footer
         * style -- dimmed, unadorned -- because styling the normal state
         * of a half-typed line as a failure is what makes a message line
         * flash through a word being typed.
         */
        message_style.attrs |= SAG_ATTR_DIM;
        (void)snprintf(message, sizeof(message), "%s", ed->cmdline.hint);
    } else {
        return;
    }
    sag_grid_fill(&ed->grid, (u16)(footer - 1U), 0U, ed->grid.cols,
                  styled_blank(&message_style));
    (void)sag_grid_puts(&ed->grid, (u16)(footer - 1U), 0U,
                        (const u8 *)message, strlen(message),
                        message_style.row_fg, message_style.row_bg,
                        message_style.attrs);
}

void sag_cmdline_draw(Ed *ed, Rect rect)
{
    CmdLine *line;
    Span span;
    CCol caret;
    u64 visible;
    ByteOff at;
    CCol logical;
    u16 col;
    u16 right;
    SagUiStyle style;
    char prefix;

    if (ed == NULL || !ed->cmdline.active || rect.h == 0U ||
        rect.y >= ed->grid.rows)
        return;
    line = &ed->cmdline;
    sync_from_target(line);
    style = sag_statusline_mode_style(SAG_MODE_E);
    sag_grid_fill(&ed->grid, rect.y, rect.x,
                  (u16)(rect.x + rect.w), styled_blank(&style));
    prefix = line->kind == SAG_PROMPT_SEARCH_F ? '/' :
             line->kind == SAG_PROMPT_SEARCH_B ? '?' : ':';
    col = sag_grid_put(&ed->grid, rect.y, rect.x, (const u8 *)&prefix, 1U,
                       style.chip_fg, style.chip_bg, style.attrs);
    right = (u32)rect.x + rect.w > ed->grid.cols ? ed->grid.cols :
                                                   (u16)(rect.x + rect.w);
    span = (Span){0U, sag_textbuf_len(line->buf)};
    caret = sag_off_to_ccol(line->buf, span, line->cur.pos,
                            SAG_CMDLINE_TABWIDTH);
    visible = right > col ? (u64)(right - col) : 0U;
    if (caret.v < line->scroll)
        line->scroll = caret.v > UINT16_MAX ? UINT16_MAX : (u16)caret.v;
    else if (visible != 0U && caret.v >= (u64)line->scroll + visible) {
        u64 next = caret.v - visible + 1U;

        line->scroll = next > UINT16_MAX ? UINT16_MAX : (u16)next;
    }
    at = sag_ccol_to_off(line->buf, span, (CCol){line->scroll},
                         SAG_CMDLINE_TABWIDTH);
    logical = sag_off_to_ccol(line->buf, span, at, SAG_CMDLINE_TABWIDTH);
    if (logical.v > line->scroll && col < right) {
        u64 gap = logical.v - line->scroll;
        u16 take = gap > (u64)(right - col) ? (u16)(right - col) :
                                              (u16)gap;

        sag_grid_fill(&ed->grid, rect.y, col, (u16)(col + take),
                      styled_blank(&style));
        col = (u16)(col + take);
    }
    while (at.v < span.hi && col < right) {
        SagTextCluster cluster;
        u64 n;
        u8 local[64];
        u8 *bytes = local;

        if (!sag_text_cluster_next(line->buf, span, at, &cluster))
            SAG_BUG("cannot decode command line cluster");
        n = cluster.bytes.hi - cluster.bytes.lo;
        if (n > sizeof(local))
            bytes = sag_xmalloc((size_t)n);
        text_copy_span(line->buf, cluster.bytes, bytes);
        if (cluster.tab) {
            u32 cells = sag_tab_cells(logical, SAG_CMDLINE_TABWIDTH);
            u16 take = cells > (u32)(right - col) ? (u16)(right - col) :
                                                    (u16)cells;

            sag_grid_fill(&ed->grid, rect.y, col, (u16)(col + take),
                          styled_blank(&style));
            col = (u16)(col + take);
        } else {
            col = sag_grid_put(&ed->grid, rect.y, col, bytes, (size_t)n,
                               style.row_fg, style.row_bg, 0U);
        }
        if (bytes != local)
            free(bytes);
        logical.v += cluster.cells;
        at = BYTEOFF(cluster.bytes.hi);
    }
    if (line->err.tok_hi > line->err.tok_lo && span.hi != 0U) {
        CCol lo = sag_off_to_ccol(line->buf, span,
                                  BYTEOFF(line->err.tok_lo),
                                  SAG_CMDLINE_TABWIDTH);
        CCol hi = sag_off_to_ccol(line->buf, span,
                                  BYTEOFF(line->err.tok_hi),
                                  SAG_CMDLINE_TABWIDTH);
        u64 x0v = lo.v > line->scroll ?
                   (u64)(rect.x + 1U) + lo.v - line->scroll :
                   (u64)(rect.x + 1U);
        u64 x1v = hi.v > line->scroll ?
                   (u64)(rect.x + 1U) + hi.v - line->scroll : x0v + 1U;
        Cell error_cell = styled_blank(&style);
        u16 x0 = x0v > right ? right : (u16)x0v;
        u16 x1 = x1v > right ? right : (u16)x1v;

        error_cell.bg = (SagColor){SAG_COLOR_INDEXED, 196U, 0U, 0U};
        error_cell.attrs |= SAG_ATTR_UNDERLINE;
        if (x1 <= x0 && x0 < right)
            x1 = (u16)(x0 + 1U);
        sag_grid_overlay(&ed->grid, rect.y, x0, x1, &error_cell,
                         SAG_OVERLAY_BG | SAG_OVERLAY_ATTRS);
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
            SagUiStyle ghost_style = style;
            size_t keep = sag_str_clip((const u8 *)ghost, ghost_len,
                                       (int)(right - col), NULL);

            ghost_style.attrs |= SAG_ATTR_DIM;
            (void)sag_grid_puts(&ed->grid, rect.y, col,
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

        sag_grid_cursor_shape(&ed->grid, SAG_CURSOR_BAR);
        sag_grid_cursor(&ed->grid, rect.y, x, right != 0U);
    }
}
