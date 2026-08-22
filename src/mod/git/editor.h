#ifndef YEW_MOD_GIT_EDITOR_H
#define YEW_MOD_GIT_EDITOR_H

#include "edit/cmd.h"
#include "mod/git/gutter.h"
#include "text/coords.h"

typedef struct Ed Ed;
typedef struct Win Win;
typedef struct Buffer Buffer;
typedef struct BlameLine BlameLine;

typedef struct YewGitEditorStats {
    u64 diff_started;
    u64 diff_slices;
    u64 diff_cancelled;
    u64 diff_published;
    u64 diff_max_slice_us;
} YewGitEditorStats;

void yew_git_editor_state_init(Ed *ed);
void yew_git_editor_state_free(Ed *ed);
void yew_git_editor_clock_anchor(Ed *ed, i64 monotonic_ms, i64 wall_secs);
i64 yew_git_editor_wall_now(const Ed *ed);
void yew_git_editor_tick(Ed *ed, i64 now_ms);
i64 yew_git_editor_deadline(const Ed *ed, i64 now_ms);
void yew_git_editor_stats(const Ed *ed, YewGitEditorStats *out);
void yew_git_editor_test_clock(Ed *ed, YewDiffNowUsFn now_us, void *ctx);
bool yew_git_editor_test_base(Ed *ed, Buffer *buf, const u8 *bytes,
                              size_t len, const char *oid,
                              bool base_is_head, i64 now_ms);
const HunkList *yew_git_editor_test_hunks(Ed *ed, Buffer *buf);
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
