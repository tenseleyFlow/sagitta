#include "edit/prof_cmds.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "text/edit.h"
#include "text/file.h"
#include "ui/message.h"
#include "ui/viewport.h"
#include "util/buf.h"

#define YEW_PROF_SCRATCH "*prof*"
#define YEW_PROF_OFF "profiling is off; restart with YEW_PROF=1"

static bool prof_ready(CmdCtx *cx)
{
    if (cx != NULL && cx->ed != NULL && cx->ed->prof.on)
        return true;
    if (cx != NULL && cx->ed != NULL)
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", YEW_PROF_OFF);
    return false;
}

static EditCtx scratch_edit_ctx(Ed *ed, Buffer *b)
{
    EditCtx ec = {0};
    Win *w = ed->win != NULL && ed->win->buf == b ? ed->win : NULL;

    ec.tb = b->tb;
    ec.marks = b->marks;
    ec.cset = w == NULL ? NULL : &w->cs;
    ec.now_ms = ed->now_ms;
    ec.ed = ed;
    ec.buffer = b;
    return ec;
}

static bool show_report(Ed *ed, const Bytebuf *out)
{
    Buffer *b = yew_ws_scratch_find(ed, YEW_PROF_SCRATCH);
    EditCtx ec;
    u64 old_len;

    if (b == NULL)
        b = yew_ws_scratch_new(ed, YEW_PROF_SCRATCH,
                               YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (b == NULL || b->tb == NULL)
        return false;
    ec = scratch_edit_ctx(ed, b);
    old_len = yew_textbuf_len(b->tb);
    if (old_len != 0U && !yew_edit_delete(&ec, (Span){0U, old_len}))
        return false;
    if (out->len != 0U &&
        !yew_edit_insert(&ec, BYTEOFF(0U), out->data, (u64)out->len))
        return false;
    if (!yew_ed_show_buffer(ed, b))
        return false;
    yew_vp_clamp(ed->win);
    yew_ed_damage_document(ed);
    return true;
}

CmdStatus yew_prof_cmd_report(CmdCtx *cx)
{
    Bytebuf out;
    bool ok;

    if (!prof_ready(cx))
        return YEW_CMD_ERR_STATE;
    bytebuf_init(&out);
    yew_prof_write(&cx->ed->prof, &out);
    ok = show_report(cx->ed, &out);
    bytebuf_free(&out);
    if (!ok) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not open profiler report");
        return YEW_CMD_ERR_STATE;
    }
    return YEW_CMD_OK;
}

CmdStatus yew_prof_cmd_reset(CmdCtx *cx)
{
    if (!prof_ready(cx))
        return YEW_CMD_ERR_STATE;
    yew_prof_reset(&cx->ed->prof);
    yew_msg(cx->ed, YEW_MSG_INFO, "profiler reset");
    return YEW_CMD_OK;
}

CmdStatus yew_prof_cmd_dump(CmdCtx *cx)
{
    Bytebuf out;
    char *path;
    YewSaveErr saved;

    if (!prof_ready(cx))
        return YEW_CMD_ERR_STATE;
    if (cx->sarg == NULL || cx->sarg_len == 0U ||
        memchr(cx->sarg, '\0', cx->sarg_len) != NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "prof dump needs a path");
        return YEW_CMD_ERR_ARG;
    }
    path = yew_xreallocarray(NULL, (size_t)cx->sarg_len + 1U, 1U);
    memcpy(path, cx->sarg, cx->sarg_len);
    path[cx->sarg_len] = '\0';
    bytebuf_init(&out);
    yew_prof_write(&cx->ed->prof, &out);
    saved = yew_file_write_atomic(path, out.data, out.len, 0600);
    bytebuf_free(&out);
    if (saved != YEW_SAVE_OK) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not write profiler dump: %s",
                path);
        free(path);
        return YEW_CMD_ERR_IO;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "wrote profiler dump: %s", path);
    free(path);
    return YEW_CMD_OK;
}

CmdStatus yew_prof_cmd_mark(CmdCtx *cx)
{
    u32 i;

    if (!prof_ready(cx))
        return YEW_CMD_ERR_STATE;
    if (cx->sarg == NULL || cx->sarg_len == 0U ||
        cx->sarg_len >= sizeof(cx->ed->prof.mark)) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "prof mark must be 1 to 31 bytes");
        return YEW_CMD_ERR_ARG;
    }
    for (i = 0U; i < cx->sarg_len; i++) {
        unsigned char ch = (unsigned char)cx->sarg[i];

        if (ch < 0x20U || ch == 0x7fU) {
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "prof mark cannot contain control bytes");
            return YEW_CMD_ERR_ARG;
        }
    }
    if (!yew_prof_mark(&cx->ed->prof, cx->sarg, cx->sarg_len))
        return YEW_CMD_ERR_ARG;
    yew_msg(cx->ed, YEW_MSG_INFO, "next profiler frame marked: %s",
            cx->ed->prof.mark);
    return YEW_CMD_OK;
}

CmdStatus yew_prof_cmd_frames(CmdCtx *cx)
{
    Bytebuf out;
    u32 limit = 40U;
    bool ok;

    if (!prof_ready(cx))
        return YEW_CMD_ERR_STATE;
    if (cx->iarg != 0) {
        if (cx->iarg <= 0 || (u64)cx->iarg > UINT32_MAX) {
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "prof frames count must be between 1 and 4294967295");
            return YEW_CMD_ERR_ARG;
        }
        limit = (u32)cx->iarg;
    }
    bytebuf_init(&out);
    yew_prof_write_frames(&cx->ed->prof, &out, limit);
    ok = show_report(cx->ed, &out);
    bytebuf_free(&out);
    if (!ok) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not open profiler frames");
        return YEW_CMD_ERR_STATE;
    }
    return YEW_CMD_OK;
}
