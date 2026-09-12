#ifndef YEW_MOD_GIT_FUSSMODE_H
#define YEW_MOD_GIT_FUSSMODE_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "term/input.h"
#include "ui/layout.h"
#include "ui/typejump.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;
typedef struct FussMode FussMode;
typedef struct FussTree FussTree;

typedef struct FussJump {
    TypeJump type;
    bool armed;
    i64 deadline_ms;
} FussJump;

enum {
    YEW_FUSS_INDENT_CELLS = 2,
    YEW_FUSS_DRAWER_MIN_CELLS = 24,
    YEW_FUSS_DRAWER_BASE_MAX_CELLS = 64,
    YEW_FUSS_EDITOR_RETAIN_CELLS = 40,
    YEW_FUSS_DRAWER_EDGE_CELLS = 1
};

typedef struct FussDrawerLayout {
    u16 width;
    u16 tree_width;
    u16 edge_col;
    bool fullscreen;
} FussDrawerLayout;

void yew_fuss_jump_init(FussJump *jump);
void yew_fuss_jump_arm(FussJump *jump, i64 now_ms);
bool yew_fuss_jump_key(FussJump *jump, const Key *key, i64 now_ms,
                       const PickItem *items, u32 n, u32 *sel);
bool yew_fuss_jump_tick(FussJump *jump, i64 now_ms);
bool yew_fuss_jump_armed(const FussJump *jump);
const char *yew_fuss_jump_pattern(const FussJump *jump, u32 *len);

void yew_fuss_state_init(Ed *ed);
void yew_fuss_state_free(Ed *ed);
void yew_fuss_workspace_changed(Ed *ed);
void yew_fuss_win_releasing(Ed *ed, u32 win_id);
void yew_fuss_windows_changed(Ed *ed);
bool yew_fuss_active(const Ed *ed);
CmdStatus yew_fuss_mode_enter(Ed *ed);
void yew_fuss_mode_leave(Ed *ed);
bool yew_fuss_key(Ed *ed, const Key *key, i64 now_ms);
void yew_fuss_tick(Ed *ed, i64 now_ms);
i64 yew_fuss_deadline(const Ed *ed, i64 now_ms);
u16 yew_fuss_footer_rows(const Ed *ed);
u16 yew_fuss_tree_natural_width(const FussTree *tree);
FussDrawerLayout yew_fuss_drawer_layout(u16 content_cols,
                                        u16 natural_cols);
Rect yew_fuss_drawer_rect(const Ed *ed);
Rect yew_fuss_backdrop_rect(const Ed *ed);

/*
 * Sprint 57.11 §3: the three seams the mouse router needs and only it.
 *
 * `yew_fuss_path_is_dir` answers the ONE question a FUSS row's context
 * menu turns on — file menu or directory menu — and answers it from the
 * tree rather than from the path text, which is wrong for an
 * extensionless file.  False means UNKNOWN (no F mode, a stripped
 * build, a path the tree no longer holds) and `*is_dir` is untouched:
 * the caller falls back to the drawer menu rather than guessing.  It is
 * const because the router's context resolution is a pure function.
 *
 * `yew_fuss_select_path` is the single-click select the survey found
 * missing, and `yew_fuss_scroll` is the wheel.  Both are no-ops under
 * the shim, so the router needs no #ifdef.
 */
bool yew_fuss_path_is_dir(const Ed *ed, u32 path_id, bool *is_dir);
/*
 * Sprint 57.11 §4: everything a FUSS row's menu turns on, in one
 * answer.
 *
 * The row set for a file depends on its git status — `Stage` is dead
 * for an already-staged file, `Unstage` is dead for one that is not,
 * `Discard...` is dead for an untracked one — and a directory's on
 * whether it is expanded and whether anything below it is staged.
 * Those flags already exist on the tree node; this is the const window
 * onto them, so ui/ctxrows.c never learns the tree's shape.
 *
 * FLAGS ON A DIRECTORY ARE AGGREGATES of everything below it, which is
 * exactly what `Stage All Below` and `Unstage All Below` mean.
 *
 * False means UNKNOWN — no F mode, a stripped build, a path the tree no
 * longer holds — and `*out` is zeroed: the caller greys the rows it
 * cannot justify rather than guessing at them.
 */
typedef struct FussTarget {
    bool known;
    bool status_known;
    bool is_file;
    bool staged;
    bool unstaged;
    bool untracked;
    bool incoming;
    bool conflicted;
    /* Directories only; false for a file and for an unknown path. */
    bool expanded;
} FussTarget;

bool yew_fuss_path_target(const Ed *ed, u32 path_id, FussTarget *out);
/*
 * The selected row's interned path and the cell it is drawn at, so
 * `ed.ui.context_menu` can open the FUSS menu with no pointer involved
 * (invariant 9).  False when F mode is down, the tree is empty, or the
 * selection is scrolled out of the drawer.
 */
bool yew_fuss_selected_anchor(Ed *ed, u32 *path_id, u16 *x, u16 *y);
void yew_fuss_select_path(Ed *ed, u32 path_id);
void yew_fuss_scroll(Ed *ed, i32 rows);
bool yew_fuss_draw_dirty(const Ed *ed);
void yew_fuss_draw(Ed *ed);
void yew_fuss_draw_footer(Ed *ed, Rect footer);
CmdStatus yew_fuss_commit_save(Ed *ed, Buffer *buffer, bool *handled);
CmdStatus yew_fuss_commit_close(Ed *ed, Buffer *buffer, bool *handled);

/* Returns a heap-owned absolute path only when F mode currently selects a
 * directory row.  The command layer owns and frees the result. */
char *yew_fuss_selected_directory(CmdCtx *cx);

CmdStatus yew_fuss_cmd_init(CmdCtx *cx);
CmdStatus yew_fuss_cmd_actions(CmdCtx *cx);
CmdStatus yew_fuss_cmd_leave(CmdCtx *cx);
CmdStatus yew_fuss_cmd_tree_all(CmdCtx *cx);
CmdStatus yew_fuss_cmd_tree_hidden(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_prev(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_next(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_parent(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_enter(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_toggle(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_row_prev(CmdCtx *cx);
CmdStatus yew_fuss_cmd_nav_row_next(CmdCtx *cx);
CmdStatus yew_fuss_cmd_jump_arm(CmdCtx *cx);
CmdStatus yew_fuss_cmd_stage(CmdCtx *cx);
CmdStatus yew_fuss_cmd_unstage(CmdCtx *cx);
CmdStatus yew_fuss_cmd_stage_all(CmdCtx *cx);
CmdStatus yew_fuss_cmd_unstage_all(CmdCtx *cx);
CmdStatus yew_fuss_cmd_commit(CmdCtx *cx);
CmdStatus yew_fuss_cmd_commit_amend(CmdCtx *cx);
CmdStatus yew_fuss_cmd_push(CmdCtx *cx);
CmdStatus yew_fuss_cmd_push_force(CmdCtx *cx);
CmdStatus yew_fuss_cmd_pull(CmdCtx *cx);
CmdStatus yew_fuss_cmd_fetch(CmdCtx *cx);
CmdStatus yew_fuss_cmd_diff(CmdCtx *cx);
CmdStatus yew_fuss_cmd_status(CmdCtx *cx);
CmdStatus yew_fuss_cmd_blame(CmdCtx *cx);
CmdStatus yew_fuss_cmd_history(CmdCtx *cx);
CmdStatus yew_fuss_cmd_reflog(CmdCtx *cx);
CmdStatus yew_fuss_cmd_view(CmdCtx *cx);
CmdStatus yew_fuss_cmd_branch_switch(CmdCtx *cx);
CmdStatus yew_fuss_cmd_branch_create(CmdCtx *cx);
CmdStatus yew_fuss_cmd_branch_delete(CmdCtx *cx);
CmdStatus yew_fuss_cmd_merge(CmdCtx *cx);
CmdStatus yew_fuss_cmd_reset(CmdCtx *cx);
CmdStatus yew_fuss_cmd_rebase_interactive(CmdCtx *cx);
CmdStatus yew_fuss_cmd_rebase_continue(CmdCtx *cx);
CmdStatus yew_fuss_cmd_rebase_abort(CmdCtx *cx);
CmdStatus yew_fuss_cmd_cherry_pick(CmdCtx *cx);
CmdStatus yew_fuss_cmd_revert(CmdCtx *cx);
CmdStatus yew_fuss_cmd_stash_push(CmdCtx *cx);
CmdStatus yew_fuss_cmd_stash_pop(CmdCtx *cx);
CmdStatus yew_fuss_cmd_tag(CmdCtx *cx);
CmdStatus yew_fuss_cmd_discard(CmdCtx *cx);
CmdStatus yew_fuss_cmd_file_delete(CmdCtx *cx);
CmdStatus yew_fuss_cmd_file_rename(CmdCtx *cx);
CmdStatus yew_fuss_cmd_open(CmdCtx *cx);
CmdStatus yew_fuss_cmd_open_split_h(CmdCtx *cx);
CmdStatus yew_fuss_cmd_open_split_v(CmdCtx *cx);

#endif
