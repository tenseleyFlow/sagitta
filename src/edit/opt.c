#include "edit/opt.h"

#include <limits.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/flhook.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/layout.h"
#include "ui/viewport.h"
#include "unicode/width.h"

static const char *const builtin_names[] = {
    "tabwidth",
    "wrap",
    "chord_timeout_ms",
    "clipboard.sync",
    "undo.bytes_max",
    "undo.break_on_newline",
    "registers.ring_depth",
    "errorbells",
    "ambiguous_wide",
    "scrolloff",
    "hooks.error_limit"
};

static bool name_is(const char *name, u32 len, const char *want)
{
    size_t n = strlen(want);

    return name != NULL && n == (size_t)len && memcmp(name, want, n) == 0;
}

static Buffer *current_buffer(Ed *ed)
{
    return ed == NULL ? NULL : sag_ed_doc(ed);
}

static const char *clip_name(u8 mode)
{
    switch ((SagClipboardSync)mode) {
    case SAG_CLIP_SYNC_OFF: return "off";
    case SAG_CLIP_SYNC_YANK: return "yank";
    case SAG_CLIP_SYNC_ALL: return "all";
    case SAG_CLIP_SYNC_UNNAMED: return "unnamed";
    }
    return "off";
}

static bool builtin_get(Ed *ed, const char *name, u32 len, OptVal *out)
{
    Buffer *b = current_buffer(ed);
    const char *s;

    if (ed == NULL || out == NULL)
        return false;
    if (name_is(name, len, "tabwidth") && b != NULL) {
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)b->tabwidth}};
    } else if (name_is(name, len, "wrap") && ed->win != NULL) {
        *out = (OptVal){SAG_OPT_BOOL, {.b = ed->win->vp.wrap}};
    } else if (name_is(name, len, "chord_timeout_ms")) {
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)ed->chord_timeout_ms}};
    } else if (name_is(name, len, "clipboard.sync")) {
        s = clip_name(ed->regs.clipboard_sync);
        *out = (OptVal){SAG_OPT_ENUM,
                        {.str = {s, (u32)strlen(s)}}};
    } else if (name_is(name, len, "undo.bytes_max") && b != NULL &&
               b->undo != NULL) {
        if (b->undo->bytes_max > (u64)INT64_MAX)
            return false;
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)b->undo->bytes_max}};
    } else if (name_is(name, len, "undo.break_on_newline")) {
        *out = (OptVal){SAG_OPT_BOOL, {.b = ed->undo_break_on_newline}};
    } else if (name_is(name, len, "registers.ring_depth")) {
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)ed->regs.ring_depth}};
    } else if (name_is(name, len, "errorbells")) {
        *out = (OptVal){SAG_OPT_BOOL, {.b = ed->errorbells}};
    } else if (name_is(name, len, "ambiguous_wide")) {
        *out = (OptVal){SAG_OPT_BOOL, {.b = ed->ambiguous_wide}};
    } else if (name_is(name, len, "scrolloff") && ed->win != NULL) {
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)ed->win->vp.scrolloff}};
    } else if (name_is(name, len, "hooks.error_limit")) {
        *out = (OptVal){SAG_OPT_INT, {.i = (i64)ed->hooks.error_limit}};
    } else {
        return false;
    }
    return true;
}

static bool bool_value(const OptVal *v, bool *out, const char **err)
{
    if (v != NULL && v->type == (u8)SAG_OPT_BOOL) {
        *out = v->as.b;
        return true;
    }
    *err = "option requires a bool";
    return false;
}

static bool int_between(const OptVal *v, i64 lo, i64 hi, i64 *out,
                        const char **err)
{
    if (v == NULL || v->type != (u8)SAG_OPT_INT) {
        *err = "option requires an int";
        return false;
    }
    if (v->as.i < lo || v->as.i > hi) {
        *err = "option value is outside its allowed range";
        return false;
    }
    *out = v->as.i;
    return true;
}

static bool str_value_is(const OptVal *v, const char *want)
{
    size_t n = strlen(want);

    return v != NULL &&
           (v->type == (u8)SAG_OPT_STR || v->type == (u8)SAG_OPT_ENUM) &&
           v->as.str.len == (u32)n && memcmp(v->as.str.s, want, n) == 0;
}

static void invalidate_buffer_views(Ed *ed, Buffer *buffer)
{
    u32 i;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 k;

        sag_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                SAG_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++) {
            Win *win = leaves[k]->win;

            if (win != NULL && win->buf == buffer)
                sag_vp_invalidate(win);
        }
    }
}

static bool builtin_set(Ed *ed, const char *name, u32 len,
                        const OptVal *value, const char **err)
{
    Buffer *b = current_buffer(ed);
    i64 integer;
    bool boolean;

    if (err != NULL)
        *err = NULL;
    if (ed == NULL || value == NULL || err == NULL)
        return false;
    if (name_is(name, len, "tabwidth")) {
        if (b == NULL) { *err = "no current buffer"; return false; }
        if (!int_between(value, 1, 32, &integer, err)) return false;
        b->tabwidth = (u32)integer;
        invalidate_buffer_views(ed, b);
    } else if (name_is(name, len, "wrap")) {
        if (ed->win == NULL) { *err = "no current window"; return false; }
        if (!bool_value(value, &boolean, err)) return false;
        ed->win->vp.wrap = boolean;
        ed->win->wrap_goal_valid = false;
        sag_vp_invalidate(ed->win);
    } else if (name_is(name, len, "chord_timeout_ms")) {
        if (!int_between(value, 1, UINT32_MAX, &integer, err)) return false;
        ed->chord_timeout_ms = (u32)integer;
    } else if (name_is(name, len, "clipboard.sync")) {
        if (str_value_is(value, "off"))
            ed->regs.clipboard_sync = (u8)SAG_CLIP_SYNC_OFF;
        else if (str_value_is(value, "yank"))
            ed->regs.clipboard_sync = (u8)SAG_CLIP_SYNC_YANK;
        else if (str_value_is(value, "all"))
            ed->regs.clipboard_sync = (u8)SAG_CLIP_SYNC_ALL;
        else if (str_value_is(value, "unnamed"))
            ed->regs.clipboard_sync = (u8)SAG_CLIP_SYNC_UNNAMED;
        else { *err = "clipboard.sync must be off, yank, all, or unnamed";
               return false; }
    } else if (name_is(name, len, "undo.bytes_max")) {
        if (b == NULL || b->undo == NULL) {
            *err = "no current undo tree"; return false;
        }
        if (!int_between(value, 1, INT64_MAX, &integer, err)) return false;
        sag_undo_set_limits(b->undo, (u64)integer, b->undo->min_nodes,
                            b->undo->persist_bytes_max);
    } else if (name_is(name, len, "undo.break_on_newline")) {
        if (!bool_value(value, &boolean, err)) return false;
        ed->undo_break_on_newline = boolean;
    } else if (name_is(name, len, "registers.ring_depth")) {
        if (!int_between(value, 0, SAG_KILL_RING_MAX, &integer, err))
            return false;
        sag_reg_ring_set_depth(&ed->regs, (u32)integer);
    } else if (name_is(name, len, "errorbells")) {
        if (!bool_value(value, &boolean, err)) return false;
        ed->errorbells = boolean;
    } else if (name_is(name, len, "ambiguous_wide")) {
        SagWidthOpts opts;

        if (!bool_value(value, &boolean, err)) return false;
        ed->ambiguous_wide = boolean;
        opts.ambiguous_wide = boolean;
        sag_width_set_opts(&opts);
        ed->full_damage = true;
    } else if (name_is(name, len, "scrolloff")) {
        if (ed->win == NULL) { *err = "no current window"; return false; }
        if (!int_between(value, 0, UINT8_MAX, &integer, err)) return false;
        ed->win->vp.scrolloff = (u8)integer;
        sag_vp_follow(ed->win);
    } else if (name_is(name, len, "hooks.error_limit")) {
        if (!int_between(value, 1, UINT32_MAX, &integer, err)) return false;
        fl_hook_error_limit(&ed->hooks, (u32)integer);
    } else {
        *err = "unknown option";
        return false;
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    return true;
}

static u32 builtin_list(Ed *ed, const char **out, u32 max)
{
    u32 n = (u32)SAG_ARRAY_LEN(builtin_names);
    u32 i;

    (void)ed;
    if (out == NULL)
        return n;
    if (max < n)
        n = max;
    for (i = 0U; i < n; i++)
        out[i] = builtin_names[i];
    return n;
}

static const OptProvider builtin_provider = {
    builtin_get, builtin_set, builtin_list
};

void sag_opt_provider_set(Ed *ed, const OptProvider *provider)
{
    if (ed != NULL)
        ed->opt_provider = provider == NULL ? &builtin_provider : provider;
}

const OptProvider *sag_opt_provider(const Ed *ed)
{
    return ed == NULL || ed->opt_provider == NULL ? &builtin_provider :
                                                    ed->opt_provider;
}

CmdStatus sag_opt_cmd_get(CmdCtx *cx)
{
    const OptProvider *provider;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->opt_out == NULL)
        return SAG_CMD_ERR_ARG;
    provider = sag_opt_provider(cx->ed);
    if (!provider->get(cx->ed, cx->sarg, cx->sarg_len, cx->opt_out)) {
        cx->opt_error = SAG_OPT_ERROR_NAME;
        return SAG_CMD_ERR_ARG;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_opt_cmd_set(CmdCtx *cx)
{
    const OptProvider *provider;
    const char *err = NULL;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->opt_in == NULL)
        return SAG_CMD_ERR_ARG;
    provider = sag_opt_provider(cx->ed);
    if (!provider->set(cx->ed, cx->sarg, cx->sarg_len, cx->opt_in, &err)) {
        cx->opt_error = err != NULL && strcmp(err, "unknown option") == 0 ?
                        SAG_OPT_ERROR_NAME : SAG_OPT_ERROR_TYPE;
        cx->opt_error_msg = err;
        return SAG_CMD_ERR_ARG;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_fl_cmd_eval(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_fl_eval(cx->ed, cx->sarg, cx->sarg_len);
}
