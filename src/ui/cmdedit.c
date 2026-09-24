/*
 * Sprint 57.31 §2: A-e, the prompt's line in a `*command-line*` buffer.
 * The contract lives in cmdedit.h; this file keeps its two promises --
 * the return hangs off the buffer's release, and no text is ever lost.
 */
#include "ui/cmdedit.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/cmdline.h"
#include "ui/cmdparse.h"
#include "ui/layout.h"
#include "ui/message.h"
#include "ui/shctx.h"
#include "ui/tabs.h"
#include "util/arena.h"

static const char cmdedit_name[] = "*command-line*";

/* ------------------------------------------------------ the one-line rule */

static bool cmdedit_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* Only newlines and blanks from `at` to the end: what is left is a
 * trailing newline, dropped rather than joined. */
static bool cmdedit_rest_blank(const char *s, size_t n, size_t at)
{
    for (; at < n; at++) {
        if (s[at] != '\n' && !cmdedit_blank(s[at]))
            return false;
    }
    return true;
}

/*
 * A command word after which the shell wants ANOTHER command, so a
 * newline there is a blank and `; ` would be a syntax error
 * (`if x; then; echo`).  The words that END a construct (`fi`, `done`,
 * `}`) are complete commands and take `; ` like any other.
 */
static bool cmdedit_opens_list(const char *w)
{
    static const char *const words[] = {"if", "then", "else", "elif",
                                        "while", "until", "do", "{", "!",
                                        "time"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(words); i++) {
        if (strcmp(w, words[i]) == 0)
            return true;
    }
    return false;
}

/* The shell's reading of the newline at `at`, as the join it needs:
 * 1 `; `, 0 a blank, -1 refused (with `why`). */
static int cmdedit_join(const char *s, size_t n, size_t at, Arena *a,
                        const char **why)
{
    YewShCtx ctx;

    if (!yew_shctx_at(s, n, at, a, &ctx) || ctx.pos == YEW_SH_POS_NONE) {
        *why = "inside a comment or an unfinished construct";
        return -1;
    }
    if (ctx.quote != YEW_SH_Q_NONE) {
        *why = "inside quotes";
        return -1;
    }
    if (ctx.pos != YEW_SH_POS_COMMAND)
        return 1;
    /* Command position with no word yet: after `;`, `&`, `|`, `&&`,
     * `(` or an empty line -- nothing is pending. */
    if (ctx.replace.lo == ctx.replace.hi)
        return 0;
    return ctx.stem != NULL && cmdedit_opens_list(ctx.stem) ? 0 : 1;
}

bool yew_cmdedit_oneline(const char *src, size_t n, bool shell,
                         Bytebuf *out, u32 *line, const char **why)
{
    Bytebuf norm;
    Arena a;
    const char *s;
    size_t i;
    u32 lines = 1U;
    bool ok = true;

    *line = 0U;
    *why = NULL;
    if (n > (size_t)YEW_CMDEDIT_BYTES_MAX) {
        *why = "the text is too long for a one-line prompt";
        return false;
    }
    /* CRLF is one newline; a lone CR stays a byte of the line. */
    bytebuf_init(&norm);
    for (i = 0U; i < n; i++) {
        if (src[i] == '\r' && i + 1U < n && src[i + 1U] == '\n')
            continue;
        if (src[i] == '\0') {
            *line = lines;
            *why = "a prompt cannot hold a NUL byte";
            bytebuf_free(&norm);
            return false;
        }
        if (src[i] == '\n')
            lines++;
        bytebuf_push_u8(&norm, (u8)src[i]);
    }
    if (lines > (u32)YEW_CMDEDIT_LINES_MAX) {
        *why = "the text is too long for a one-line prompt";
        bytebuf_free(&norm);
        return false;
    }
    s = (const char *)norm.data;
    n = norm.len;
    lines = 1U;
    arena_init(&a);
    i = 0U;
    while (i < n) {
        size_t run = 0U;
        int join;
        bool rest;

        if (s[i] != '\n') {
            bytebuf_push_u8(out, (u8)s[i]);
            i++;
            continue;
        }
        if (!shell) {
            if (cmdedit_rest_blank(s, n, i))
                break;
            *line = lines;
            *why = NULL;
            ok = false;
            break;
        }
        while (run < i && s[i - 1U - run] == '\\')
            run++;
        /* The lexer's answer first: a `\` inside quotes or a comment is
         * no continuation, and a newline there is never ours to join --
         * except the trailing one of a comment, which just goes. */
        join = cmdedit_join(s, n, i, &a, why);
        arena_free_all(&a);
        arena_init(&a);
        rest = cmdedit_rest_blank(s, n, i);
        if (join < 0 && !(rest && strcmp(*why, "inside quotes") != 0)) {
            *line = lines;
            ok = false;
            break;
        }
        *why = NULL;
        if (rest && (run & 1U) == 0U)
            break;
        lines++;
        i++;
        if ((run & 1U) != 0U) {
            /* `\`-newline: both go, as the shell drops them.  The
             * indent after it folds to one blank -- or to none, when
             * neither side had one (`a\<nl>b` is the word `ab`). */
            bool had = out->len > 1U &&
                       cmdedit_blank((char)out->data[out->len - 2U]);
            size_t j = i;

            out->len--;
            while (j < n && cmdedit_blank(s[j]))
                j++;
            if (!had && j > i && j < n && s[j] != '\n')
                bytebuf_push_u8(out, (u8)' ');
            i = j;
            continue;
        }
        /* Unescaped trailing blanks go; the join supplies its own. */
        while (out->len != 0U &&
               cmdedit_blank((char)out->data[out->len - 1U]) &&
               !(out->len > 1U && out->data[out->len - 2U] == '\\'))
            out->len--;
        if (join == 1)
            bytebuf_append(out, "; ", 2U);
        else if (out->len != 0U &&
                 !cmdedit_blank((char)out->data[out->len - 1U]))
            bytebuf_push_u8(out, (u8)' ');
        while (i < n && cmdedit_blank(s[i]))
            i++;
    }
    arena_free_all(&a);
    bytebuf_free(&norm);
    return ok;
}

/* ------------------------------------------------------------- helpers */

static char *cmdedit_dup(const char *s, size_t n)
{
    char *d = yew_xmalloc(n + 1U);

    if (n != 0U)
        (void)memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void cmdedit_bytes(const Buffer *b, Bytebuf *out)
{
    TextIter it;
    u64 left;

    if (b == NULL || b->tb == NULL)
        return;
    left = yew_textbuf_len(b->tb);
    if (left == 0U || !yew_textiter_begin(&it, b->tb, BYTEOFF(0U)))
        return;
    while (left != 0U) {
        const u8 *p;
        u64 got;

        if (!yew_textiter_chunk(&it, b->tb, &p, &got) || got == 0U)
            break;
        if (got > left)
            got = left;
        bytebuf_append(out, p, (size_t)got);
        left -= got;
        if (left != 0U && !yew_textiter_advance(&it, b->tb))
            break;
    }
}

static void cmdedit_forget(YewCmdEdit *st)
{
    yew_xfree(st->prefix);
    yew_xfree(st->original);
    yew_xfree(st->text);
    (void)memset(st, 0, sizeof(*st));
}

static bool cmdedit_shell(const YewCmdEdit *st)
{
    return st->prefix != NULL && st->prefix[0] != '\0';
}

/* The whole line the prompt would hold for `body`, or false with the
 * refusal on the message line. */
static bool cmdedit_line(Ed *ed, const YewCmdEdit *st, const char *body,
                         size_t n, Bytebuf *out)
{
    const char *why = NULL;
    u32 line = 0U;

    bytebuf_append(out, st->prefix, strlen(st->prefix));
    if (yew_cmdedit_oneline(body, n, cmdedit_shell(st), out, &line, &why))
        return true;
    if (line == 0U)
        yew_msg(ed, YEW_MSG_ERROR, "%s", why);
    else if (why == NULL)
        yew_msg(ed, YEW_MSG_ERROR,
                "line %u: a one-line prompt cannot hold a newline",
                (unsigned)line);
    else if (strncmp(why, "inside", 6U) == 0)
        yew_msg(ed, YEW_MSG_ERROR,
                "line %u: a one-line prompt cannot hold a newline %s",
                (unsigned)line, why);
    else
        yew_msg(ed, YEW_MSG_ERROR, "line %u: %s", (unsigned)line, why);
    return false;
}

static bool cmdedit_pane_shows(Pane *root, const Buffer *b)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return false;
    yew_pane_collect_leaves(root, leaves, YEW_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++) {
        if (leaves[i]->win != NULL && leaves[i]->win->buf == b)
            return true;
    }
    return false;
}

static bool cmdedit_shown(Ed *ed, const Buffer *b)
{
    size_t i;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (cmdedit_pane_shows(ed->tabs.v.data[i].root, b))
            return true;
    }
    if (cmdedit_pane_shows(ed->pane_root, b))
        return true;
    return ed->win != NULL && ed->win->buf == b;
}

/* Every view of `b` shows `to` instead, and a tab that named `b` as its
 * buffer names `to`. */
static void cmdedit_unshow(Ed *ed, const Buffer *b, Buffer *to)
{
    size_t t;

    if (ed->win != NULL && ed->win->buf == b)
        (void)yew_ed_show_buffer(ed, to);
    for (t = 0U; t < ed->tabs.v.len; t++) {
        Tab *tab = &ed->tabs.v.data[t];
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        if (tab->buffer_id == b->id)
            tab->buffer_id = to->id;
        if (tab->root == NULL)
            continue;
        yew_pane_collect_leaves(tab->root, leaves, YEW_ARRAY_LEN(leaves),
                                &n);
        for (i = 0U; i < n; i++) {
            if (leaves[i]->win != NULL && leaves[i]->win->buf == b)
                yew_ed_win_set_buffer(ed, leaves[i]->win, to);
        }
    }
}

/* The buffer to put back where the scratch was: what the window showed
 * before A-e, else the document. */
static Buffer *cmdedit_fallback(Ed *ed, const Buffer *b)
{
    Buffer *to = yew_ws_buf_by_id(ed, ed->cmdedit.origin_buf_id);

    return to != NULL && to != b && to->tb != NULL ? to : &ed->buffer;
}

static void cmdedit_back_to_origin(Ed *ed)
{
    int idx = yew_tab_index_of_id(ed, ed->cmdedit.origin_tab_id);

    if (idx >= 0 && idx != ed->tabs.active)
        yew_tab_switch(ed, idx);
}

/*
 * Show `b` in a tab of its own and put the caret at `caret`, in Insert.
 * Without a tab to spare it takes the focused window, and the close puts
 * the window's own buffer back.
 */
static bool cmdedit_show(Ed *ed, Buffer *b, size_t caret)
{
    int idx = yew_tab_open_buffer(ed, b);
    Cursor *c;

    if (idx >= 0)
        yew_tab_switch(ed, idx);
    else if (!yew_ed_show_buffer(ed, b))
        return false;
    if (ed->win == NULL || ed->win->buf != b)
        return false;
    /* The new view has no size until it is laid out, and following the
     * caret in a zero-width view scrolls the line away. */
    yew_ed_layout(ed);
    c = yew_ed_cursor(ed);
    if (c != NULL) {
        c->pos = BYTEOFF(caret);
        c->anchor = c->pos;
        c->goal_col = (CCol){0U};
        yew_cursor_clamp(b->tb, c);
    }
    yew_win_follow_cursor(ed->win);
    (void)yew_mode_enter(ed, YEW_MODE_I);
    ed->layout_dirty = true;
    ed->full_damage = true;
    return true;
}

/* A fresh *command-line* buffer holding `text`, shown; its id is the
 * one A-e now watches.  NULL when none could be made. */
static Buffer *cmdedit_open(Ed *ed, const char *text, size_t n,
                            size_t caret)
{
    Buffer *b = yew_ws_scratch_new(ed, cmdedit_name, 0U);

    if (b == NULL)
        return NULL;
    if (n != 0U)
        yew_textbuf_insert(b->tb, BYTEOFF(0U), (const u8 *)text, (u64)n);
    yew_undo_mark_saved(b->undo);
    if (!cmdedit_show(ed, b, caret)) {
        yew_ws_scratch_drop(ed, b);
        return NULL;
    }
    ed->cmdedit.buf_id = b->id;
    return b;
}

/* ------------------------------------------------------------ the command */

bool yew_cmdedit_owns(const Ed *ed, const Buffer *b)
{
    return ed != NULL && b != NULL && ed->cmdedit.buf_id != 0U &&
           b->id == ed->cmdedit.buf_id;
}

CmdStatus yew_cmdedit_cmd_edit_in_buffer(CmdCtx *cx)
{
    Ed *ed;
    YewCmdEdit *st;
    Bytebuf text;
    Buffer *open;
    size_t body = 0U;
    size_t caret;
    bool bang;
    YewPromptKind kind;

    if (cx == NULL || cx->ed == NULL || !cx->ed->cmdline.active)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    st = &ed->cmdedit;
    kind = ed->cmdline.kind;
    if (kind == YEW_PROMPT_INPUT) {
        /* A picker's filter, a rename, a Git question: its owner is
         * waiting on THIS prompt's answer and cannot be handed a buffer. */
        yew_msg(ed, YEW_MSG_WARN, "A-e: this prompt cannot become a buffer");
        return YEW_CMD_OK;
    }
    yew_cmdline_sync(ed);
    bytebuf_init(&text);
    yew_cmdline_text(ed, &text);
    caret = (size_t)ed->cmdline.cur.pos.v;
    if (caret > text.len)
        caret = text.len;
    open = st->buf_id == 0U ? NULL : yew_ws_buf_by_id(ed, st->buf_id);
    if (open != NULL) {
        /* §2.5: one at a time.  This prompt's line is not dropped on the
         * way: it goes where Up finds it. */
        if (text.len != 0U && ed->cmdline.history != NULL) {
            char *line = cmdedit_dup((const char *)text.data, text.len);

            yew_hist_add(ed->cmdline.history, line);
            yew_xfree(line);
        }
        bytebuf_free(&text);
        yew_cmdline_close(ed, false);
        {
            size_t t;

            for (t = 0U; t < ed->tabs.v.len; t++) {
                if (cmdedit_pane_shows(ed->tabs.v.data[t].root, open)) {
                    yew_tab_switch(ed, (int)t);
                    break;
                }
            }
        }
        if (ed->win == NULL || ed->win->buf != open)
            (void)yew_ed_show_buffer(ed, open);
        (void)yew_mode_enter(ed, YEW_MODE_I);
        yew_msg(ed, YEW_MSG_INFO, "%s is already open", cmdedit_name);
        return YEW_CMD_OK;
    }
    cmdedit_forget(st);
    bang = kind == YEW_PROMPT_CMD &&
           yew_cmd_bang_body(ed, (const char *)text.data, text.len, &body) &&
           body <= text.len;
    if (!bang)
        body = 0U;
    st->kind = (u8)kind;
    st->return_mode = ed->cmdline.return_mode;
    st->prefix = cmdedit_dup((const char *)text.data, body);
    st->original = cmdedit_dup((const char *)text.data, text.len);
    st->original_caret = caret;
    st->origin_tab_id = ed->tabs.active >= 0
                            ? ed->tabs.v.data[ed->tabs.active].tab_id
                            : 0U;
    yew_cmdline_close(ed, false);
    st->origin_buf_id = ed->win != NULL && ed->win->buf != NULL
                            ? ed->win->buf->id
                            : 0U;
    if (cmdedit_open(ed, (const char *)text.data + body, text.len - body,
                     caret > body ? caret - body : 0U) == NULL) {
        /* Nothing opened: the prompt comes straight back. */
        yew_cmdline_restore(ed, kind, st->original, st->original_caret,
                            st->return_mode);
        cmdedit_forget(st);
        bytebuf_free(&text);
        yew_msg(ed, YEW_MSG_ERROR, "A-e: cannot open %s", cmdedit_name);
        return YEW_CMD_OK;
    }
    bytebuf_free(&text);
    yew_msg(ed, YEW_MSG_INFO,
            "%s: :q returns the line to the prompt, :q! discards the edit",
            cmdedit_name);
    return YEW_CMD_OK;
}

/* ------------------------------------------------------- close and return */

CmdStatus yew_cmdedit_close(Ed *ed, bool discard)
{
    YewCmdEdit *st;
    Buffer *b;
    Buffer *to;
    size_t t;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    st = &ed->cmdedit;
    b = st->buf_id == 0U ? NULL : yew_ws_buf_by_id(ed, st->buf_id);
    if (b == NULL)
        return YEW_CMD_ERR_STATE;
    if (!discard) {
        Bytebuf body;
        Bytebuf line;
        bool ok;

        bytebuf_init(&body);
        bytebuf_init(&line);
        cmdedit_bytes(b, &body);
        ok = cmdedit_line(ed, st, (const char *)body.data, body.len, &line);
        bytebuf_free(&body);
        bytebuf_free(&line);
        if (!ok)
            return YEW_CMD_ERR_STATE;
    }
    st->discard = discard;
    /* Its own tab goes, unless it is the last one standing. */
    for (t = ed->tabs.v.len; t-- > 0U;) {
        if (ed->tabs.v.len > 1U && ed->tabs.v.data[t].buffer_id == b->id)
            (void)yew_tab_close(ed, (int)t);
    }
    to = cmdedit_fallback(ed, b);
    cmdedit_unshow(ed, b, to);
    cmdedit_back_to_origin(ed);
    if (ed->mode == YEW_MODE_I)
        (void)yew_mode_enter(ed, YEW_MODE_L);
    /* The release is what returns the line (cmdedit.h). */
    yew_ws_scratch_drop(ed, b);
    return YEW_CMD_OK;
}

void yew_cmdedit_released(Ed *ed, Buffer *b)
{
    YewCmdEdit *st;
    Bytebuf bytes;

    if (!yew_cmdedit_owns(ed, b))
        return;
    st = &ed->cmdedit;
    bytebuf_init(&bytes);
    cmdedit_bytes(b, &bytes);
    yew_xfree(st->text);
    st->text = cmdedit_dup((const char *)bytes.data, bytes.len);
    st->text_len = bytes.len;
    bytebuf_free(&bytes);
    st->released = true;
    st->buf_id = 0U;
}

void yew_cmdedit_settle(Ed *ed)
{
    YewCmdEdit *st;
    Bytebuf line;
    const char *text;
    size_t caret;

    if (ed == NULL || !ed->model_ready)
        return;
    st = &ed->cmdedit;
    if (st->settling || (st->buf_id == 0U && !st->released))
        return;
    st->settling = true;
    if (st->buf_id != 0U) {
        Buffer *b = yew_ws_buf_by_id(ed, st->buf_id);

        if (b != NULL && !cmdedit_shown(ed, b))
            yew_ws_scratch_drop(ed, b);
        else if (b == NULL) {
            /* Gone without the hook: nothing of the edit survives, so
             * the original line is what can still be given back. */
            st->buf_id = 0U;
            st->released = true;
            st->discard = true;
        }
    }
    /* Wait for a clear stage: another prompt still open (the `:q` that
     * closed the buffer is closed only after its command returns), a
     * modal question, or an editor on its way out. */
    if (!st->released || ed->quit || ed->cmdline.active ||
        ed->prompt != YEW_PROMPT_NONE) {
        st->settling = false;
        return;
    }
    bytebuf_init(&line);
    if (st->discard || st->text == NULL) {
        text = st->original;
        caret = st->original_caret;
    } else if (cmdedit_line(ed, st, st->text, st->text_len, &line)) {
        bytebuf_push_u8(&line, 0U);
        text = (const char *)line.data;
        caret = line.len - 1U;
    } else {
        /*
         * A route that did not ask first closed it, and the text cannot
         * be one line: it goes back into a buffer, with the message
         * cmdedit_line just put up (invariant 1).
         */
        char *keep = st->text;
        size_t n = st->text_len;
        Msg said = ed->msg;

        st->text = NULL;
        st->released = false;
        if (cmdedit_open(ed, keep, n, n) == NULL) {
            /* No buffer to be had either: the edit goes into the prompt
             * with its newlines folded to blanks, as a paste into it
             * would be -- mangled, but not gone. */
            bytebuf_free(&line);
            bytebuf_init(&line);
            bytebuf_append(&line, st->prefix, strlen(st->prefix));
            bytebuf_append(&line, keep, n);
            bytebuf_push_u8(&line, 0U);
            yew_xfree(keep);
            cmdedit_back_to_origin(ed);
            yew_cmdline_restore(ed, (YewPromptKind)st->kind,
                                (const char *)line.data, line.len - 1U,
                                st->return_mode);
            bytebuf_free(&line);
            cmdedit_forget(st);
            return;
        }
        yew_xfree(keep);
        ed->msg = said;
        st->settling = false;
        bytebuf_free(&line);
        return;
    }
    cmdedit_back_to_origin(ed);
    if (ed->mode == YEW_MODE_I)
        (void)yew_mode_enter(ed, YEW_MODE_L);
    yew_cmdline_restore(ed, (YewPromptKind)st->kind, text, caret,
                        st->return_mode);
    bytebuf_free(&line);
    cmdedit_forget(st);
}

void yew_cmdedit_free(Ed *ed)
{
    if (ed != NULL)
        cmdedit_forget(&ed->cmdedit);
}
