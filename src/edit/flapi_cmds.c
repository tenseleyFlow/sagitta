#include "edit/flapi_cmds.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "text/register.h"
#include "ui/tabs.h"
#include "ui/win.h"
#include "util/log.h"

static bool arg_is(const CmdCtx *cx, const char *want)
{
    size_t n = strlen(want);

    return cx->sarg != NULL && cx->sarg_len == (u32)n &&
           memcmp(cx->sarg, want, n) == 0;
}

CmdStatus yew_flapi_cmd_win_split(CmdCtx *cx)
{
    Pane *before;
    CmdStatus status;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->ed->win != cx->win)
        return YEW_CMD_ERR_STATE;
    before = cx->ed->focus;
    if (arg_is(cx, "h"))
        status = yew_pane_cmd_split_h(cx);
    else if (arg_is(cx, "v"))
        status = yew_pane_cmd_split_v(cx);
    else
        return YEW_CMD_ERR_ARG;
    if (status == YEW_CMD_OK && cx->ed->focus == before)
        return YEW_CMD_ERR_STATE;
    return status;
}

static Pane *find_window(Ed *ed, Win *want, int *tab_index)
{
    size_t i;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 k;

        yew_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++) {
            if (leaves[k]->win == want) {
                *tab_index = (int)i;
                return leaves[k];
            }
        }
    }
    if (ed->focus != NULL && ed->focus->win == want) {
        *tab_index = ed->tabs.active;
        return ed->focus;
    }
    return NULL;
}

CmdStatus yew_flapi_cmd_win_focus(CmdCtx *cx)
{
    Ed *ed;
    Pane *leaf;
    Tab *tab;
    Win *before;
    int tab_index = -1;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    leaf = find_window(ed, cx->win, &tab_index);
    if (leaf == NULL)
        return YEW_CMD_ERR_STATE;
    before = ed->win;
    tab = yew_tab_at(ed, tab_index);
    if (tab != NULL)
        tab->focus = leaf;
    if (tab_index >= 0 && tab_index != ed->tabs.active) {
        yew_tab_switch(ed, tab_index);
    } else {
        if (before != NULL && leaf->win != before)
            yew_shadow_dismiss(ed, before);
        ed->focus = leaf;
        ed->win = leaf->win;
        ed->layout_dirty = true;
        ed->full_damage = true;
        yew_state_mark_dirty(ed);
        if (ed->win != before)
            yew_fl_hook_window(ed, FL_EV_WIN_FOCUS, ed->win);
    }
    return YEW_CMD_OK;
}

CmdStatus yew_flapi_cmd_cursor_set_many(CmdCtx *cx)
{
    CursorSet replacement;
    Cursor first;
    Cursor *rest = NULL;
    size_t i;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->buf->tb == NULL || cx->cursor_args == NULL ||
        cx->cursor_args_len == 0U || cx->cursor_args_len > YEW_MC_MAX)
        return YEW_CMD_ERR_ARG;
    first.pos = cx->cursor_args[0].pos;
    first.anchor = cx->cursor_args[0].anchor;
    first.goal_col = (GCol){cx->cursor_args[0].goal_col};
    yew_cset_init(&replacement, first);
    if (cx->cursor_args_len > 1U) {
        rest = yew_xmalloc((size_t)(cx->cursor_args_len - 1U) *
                           sizeof(*rest));
        for (i = 1U; i < cx->cursor_args_len; i++) {
            rest[i - 1U].pos = cx->cursor_args[i].pos;
            rest[i - 1U].anchor = cx->cursor_args[i].anchor;
            rest[i - 1U].goal_col = (GCol){cx->cursor_args[i].goal_col};
        }
        if (!yew_cset_add_many(&replacement, rest,
                               cx->cursor_args_len - 1U)) {
            yew_xfree(rest);
            yew_cset_free(&replacement);
            return YEW_CMD_ERR_ARG;
        }
        yew_xfree(rest);
    }
    yew_cset_normalize(cx->win->buf->tb, &replacement);
    yew_cset_free(&cx->win->cs);
    cx->win->cs = replacement;
    yew_win_follow_cursor(cx->win);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
}

static const UnitOps *named_unit(const char *s, u32 n)
{
    if (n == 4U && memcmp(s, "char", 4U) == 0)
        return &yew_unit_char;
    if (n == 4U && memcmp(s, "line", 4U) == 0)
        return &yew_unit_line;
    if (n == 4U && memcmp(s, "word", 4U) == 0)
        return &yew_unit_word;
    if (n == 5U && memcmp(s, "block", 5U) == 0)
        return &yew_unit_block;
    return NULL;
}

CmdStatus yew_flapi_cmd_cursor_move(CmdCtx *cx)
{
    const char *sep;
    const UnitOps *ops;
    UnitCtx unit;
    Cursor *cursor;
    ByteOff next;
    u32 unit_len;
    u32 dir_len;

    if (cx == NULL || cx->win == NULL || cx->win->buf == NULL ||
        cx->win->buf->tb == NULL || !cx->cursor_given ||
        (size_t)cx->cursor_index >= cx->win->cs.curs.len ||
        cx->sarg == NULL)
        return YEW_CMD_ERR_STATE;
    sep = memchr(cx->sarg, ':', cx->sarg_len);
    if (sep == NULL)
        return YEW_CMD_ERR_ARG;
    unit_len = (u32)(sep - cx->sarg);
    dir_len = cx->sarg_len - unit_len - 1U;
    ops = named_unit(cx->sarg, unit_len);
    if (ops == NULL)
        return YEW_CMD_ERR_ARG;
    unit = (UnitCtx){cx->win->buf->tb, cx->win->buf, cx->win};
    cursor = &cx->win->cs.curs.data[cx->cursor_index];
    if (dir_len == 4U && memcmp(sep + 1, "next", 4U) == 0)
        next = ops->next(&unit, cursor->pos, false);
    else if (dir_len == 4U && memcmp(sep + 1, "prev", 4U) == 0)
        next = ops->prev(&unit, cursor->pos, false);
    else if (dir_len == 4U && memcmp(sep + 1, "home", 4U) == 0)
        next = ops->home(&unit, cursor->pos, false);
    else if (dir_len == 3U && memcmp(sep + 1, "end", 3U) == 0)
        next = ops->end(&unit, cursor->pos, false);
    else
        return YEW_CMD_ERR_ARG;
    cursor->pos = next;
    cursor->anchor = next;
    cursor->goal_col = yew_off_to_gcol(
        cx->win->buf->tb,
        yew_textbuf_line_span(cx->win->buf->tb,
                              yew_textbuf_line_of(cx->win->buf->tb, next)),
        next);
    yew_win_follow_cursor(cx->win);
    return YEW_CMD_OK;
}

CmdStatus yew_flapi_cmd_span_yank(CmdCtx *cx)
{
    RegVal value;
    u8 name = 0U;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->buf == NULL || cx->win->buf->tb == NULL ||
        !cx->range.given || cx->range.tok.lo > cx->range.tok.hi ||
        cx->range.tok.hi > yew_textbuf_len(cx->win->buf->tb))
        return YEW_CMD_ERR_ARG;
    if (cx->sarg_len != 0U) {
        if (cx->sarg == NULL || cx->sarg_len != 1U)
            return YEW_CMD_ERR_ARG;
        name = (u8)cx->sarg[0];
        if (!((name >= (u8)'a' && name <= (u8)'z') ||
              (name >= (u8)'A' && name <= (u8)'Z') || name == (u8)'"' ||
              name == (u8)'+' || name == (u8)'*' || name == (u8)'_'))
            return YEW_CMD_ERR_ARG;
    }
    yew_regval_init(&value);
    yew_regval_from_span(&value, cx->win->buf->tb, cx->range.tok,
                         YEW_REG_CHARWISE, &cx->win->buf->meta);
    yew_reg_yank(&cx->ed->regs, name, &value);
    yew_regval_free(&value);
    return YEW_CMD_OK;
}

CmdStatus yew_flapi_cmd_reg_set(CmdCtx *cx)
{
    RegVal value;
    u8 name;

    if (cx == NULL || cx->ed == NULL || cx->iarg < (i64)'a' ||
        cx->iarg > (i64)'z' || (cx->sarg == NULL && cx->sarg_len != 0U))
        return YEW_CMD_ERR_ARG;
    name = (u8)cx->iarg;
    yew_regval_init(&value);
    value.type = YEW_REG_CHARWISE;
    bytebuf_append(&value.bytes, cx->sarg, cx->sarg_len);
    yew_reg_set_macro(&cx->ed->regs, name, &value, cx->bang);
    fl_macro_cache_invalidate(cx->ed->fl, name);
    yew_regval_free(&value);
    return YEW_CMD_OK;
}

CmdStatus yew_flapi_reg_write(Ed *ed, u8 name, const u8 *bytes,
                              u32 len, bool append)
{
    static const char command[] = "ed.reg.set";
    CmdCtx cx = {0};
    CmdId id;

    if (ed == NULL || name < (u8)'a' || name > (u8)'z' ||
        (bytes == NULL && len != 0U))
        return YEW_CMD_ERR_ARG;
    id = yew_cmd_lookup(command, (u32)(sizeof(command) - 1U));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = name;
    cx.sarg = (const char *)bytes;
    cx.sarg_len = len;
    cx.bang = append;
    cx.source = YEW_SRC_FLETCH;
    return yew_cmd_invoke(id, &cx);
}
