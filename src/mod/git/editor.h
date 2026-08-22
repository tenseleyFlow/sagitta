#ifndef YEW_MOD_GIT_EDITOR_H
#define YEW_MOD_GIT_EDITOR_H

#include "edit/cmd.h"
#include "text/coords.h"

typedef struct Ed Ed;
typedef struct Win Win;
typedef struct BlameLine BlameLine;

void yew_git_editor_state_init(Ed *ed);
void yew_git_editor_state_free(Ed *ed);
void yew_git_editor_tick(Ed *ed, i64 now_ms);
i64 yew_git_editor_deadline(const Ed *ed, i64 now_ms);
void yew_git_editor_prepare(Ed *ed, Win *w);
CmdStatus yew_git_cmd_hunk_next(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_prev(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_first(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_last(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_stage(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_unstage(CmdCtx *cx);
CmdStatus yew_git_cmd_hunk_discard(CmdCtx *cx);
CmdStatus yew_git_cmd_blame_toggle(CmdCtx *cx);
CmdStatus yew_git_cmd_diff_view(CmdCtx *cx);
CmdStatus yew_git_cmd_conflict_scope(CmdCtx *cx);
const BlameLine *yew_git_blame_at(Ed *ed, Win *w, LineNo line);
void yew_git_blame_draw(Ed *ed, Win *w, u16 lo, u16 hi);

#endif
