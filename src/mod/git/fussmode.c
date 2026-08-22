#define _POSIX_C_SOURCE 200809L

#include "mod/git/fussmode.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/file_cmds.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/mode.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/git/fusscommit.h"
#include "mod/git/fusstree.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"
#include "syn/theme.h"
#include "term/grid.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/draw.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/statusline.h"
#include "ui/tabs.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "ws/state.h"
#include "ws/walk.h"

typedef enum FussPromptAction {
    FUSS_PROMPT_NONE,
    FUSS_PROMPT_BRANCH_CREATE,
    FUSS_PROMPT_BRANCH_DELETE,
    FUSS_PROMPT_STASH_PUSH,
    FUSS_PROMPT_TAG,
    FUSS_PROMPT_DISCARD,
    FUSS_PROMPT_DELETE,
    FUSS_PROMPT_RENAME,
    FUSS_PROMPT_PUSH_FORCE,
    FUSS_PROMPT_REBASE,
    FUSS_PROMPT_COMMIT,
    FUSS_PROMPT_COMMIT_AMEND
} FussPromptAction;

typedef enum FussPickAction {
    FUSS_PICK_NONE,
    FUSS_PICK_BRANCH_SWITCH,
    FUSS_PICK_BRANCH_DELETE,
    FUSS_PICK_MERGE,
    FUSS_PICK_RESET_COMMIT,
    FUSS_PICK_RESET_MODE,
    FUSS_PICK_REBASE,
    FUSS_PICK_CHERRY_BRANCH,
    FUSS_PICK_CHERRY_COMMIT,
    FUSS_PICK_REVERT,
    FUSS_PICK_STASH,
    FUSS_PICK_REMOTE,
    FUSS_PICK_REMOTE_CHECK,
    FUSS_PICK_REBASE_CONFIRM,
    FUSS_PICK_COMMIT_AMEND
} FussPickAction;

typedef struct FussPreviewJob {
    char *value;
    u32 job_id;
} FussPreviewJob;

struct FussMode {
    FussTree tree;
    FussOpts opts;
    FussSel sel;
    FussJump jump;
    bool active;
    bool ascii_glyphs;
    bool viewer;
    Pane *saved_root;
    Pane *saved_focus;
    Win *saved_win;
    Pane *fuss_root;
    Pane *viewer_leaf;
    u16 scroll;
    u32 saved_buffer_id;
    u32 viewer_buffer_id;
    u32 commit_buffer_id;
    u32 pending_job;
    u32 seen_result_job;
    bool pending_view;
    bool commit_editing;
    bool commit_amend;
    u8 commit_comment;
    FussPromptAction prompt_action;
    char *prompt_path;
    bool prompt_untracked;
    FussPickAction pending_pick;
    FussPickAction picker_action;
    PickItem *picker_items;
    char **picker_values;
    u32 picker_count;
    char *picker_aux;
    char *picker_aux2;
    bool picker_alt;
    u32 preview_job;
    char *preview_value;
    Bytebuf preview_bytes;
    bool preview_ready;
    FileList files;
    WalkState *walk;
    FileList expand_files;
    WalkState *expand_walk;
    char *expand_path;
    bool expand_enter;
    i32 rebase_step;
    i32 rebase_total;
};

static bool fuss_picker_result(Ed *ed, FussPickAction action,
                               const GitResult *result);
static void fuss_picker_open(Ed *ed, FussPickAction action,
                             const char *title);
static CmdStatus fuss_commit_begin(Ed *ed, bool amend, const u8 *prefill,
                                   size_t prefill_len);
static bool fuss_commit_guard(Ed *ed, const GitSnapshot *snap);
static CmdStatus fuss_rebase_prepare(Ed *ed, const char *base);
static void fuss_rebase_progress_update(Ed *ed, const GitSnapshot *snap);
static void fuss_prompt_clear(FussMode *f);
static void fuss_prompt_done(Ed *ed, bool accepted, const u8 *text,
                             size_t len, void *ctx);
static void fuss_damage(Ed *ed);
static char *fuss_join_root(const Ed *ed, const char *path);
static void fuss_walk_restart(Ed *ed);
static void fuss_expand_clear(FussMode *f);
static bool fuss_expand_start(Ed *ed, bool enter);
static void fuss_expand_tick(Ed *ed);

static void fuss_viewer_close(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->viewer_leaf != NULL) {
        (void)yew_pane_close(ed, f->viewer_leaf);
        f->viewer_leaf = NULL;
    }
    f->viewer = false;
    f->viewer_buffer_id = 0U;
    if (f->fuss_root != NULL && f->fuss_root->is_leaf) {
        ed->focus = f->fuss_root;
        ed->win = f->fuss_root->win;
    }
    fuss_damage(ed);
}

static void fuss_layout_restore(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    fuss_viewer_close(ed);
    if (f->fuss_root != NULL) {
        f->fuss_root->win = NULL;
        yew_pane_free(ed, f->fuss_root);
    }
    ed->pane_root = f->saved_root;
    ed->focus = f->saved_focus;
    ed->win = f->saved_win;
    f->saved_root = NULL;
    f->saved_focus = NULL;
    f->saved_win = NULL;
    f->fuss_root = NULL;
    fuss_damage(ed);
}

static bool fuss_viewer_open(Ed *ed, Buffer *buffer)
{
    FussMode *f;
    Pane *viewer;
    u16 usable;
    u16 wanted;

    if (ed == NULL || buffer == NULL || ed->fuss == NULL)
        return false;
    f = ed->fuss;
    if (!f->active || f->fuss_root == NULL)
        return false;
    if (f->viewer_leaf == NULL) {
        yew_layout(ed);
        if (!ed->grid_ready && f->fuss_root->rect.w == 0U)
            f->fuss_root->rect =
                (Rect){0U, 0U, (u16)(YEW_PANE_MIN_W * 2U + 1U),
                       (u16)YEW_PANE_MIN_H};
        viewer = yew_pane_split(ed, f->fuss_root, YEW_SPLIT_H);
        if (viewer == NULL) {
            yew_msg(ed, YEW_MSG_ERROR,
                    "terminal is too narrow for a FUSS viewer pane");
            return false;
        }
        f->viewer_leaf = viewer;
        usable = f->fuss_root->rect.w > 0U ?
                 (u16)(f->fuss_root->rect.w - 1U) : 0U;
        wanted = (u16)((u32)f->fuss_root->rect.w * 45U / 100U);
        if (wanted > 56U)
            wanted = 56U;
        if (wanted < YEW_PANE_MIN_W)
            wanted = YEW_PANE_MIN_W;
        if (usable > YEW_PANE_MIN_W &&
            wanted > (u16)(usable - YEW_PANE_MIN_W))
            wanted = (u16)(usable - YEW_PANE_MIN_W);
        if (usable != 0U)
            f->fuss_root->ratio = (float)wanted / (float)usable;
        yew_layout(ed);
    }
    yew_ed_win_set_buffer(ed, f->viewer_leaf->win, buffer);
    ed->focus = f->viewer_leaf;
    ed->win = f->viewer_leaf->win;
    f->viewer = true;
    f->viewer_buffer_id = buffer->id;
    fuss_damage(ed);
    return true;
}

static size_t fuss_cstr_len(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0U;
    while (*p != '\0')
        p++;
    return (size_t)(p - s);
}

void yew_fuss_jump_init(FussJump *jump)
{
    if (jump == NULL)
        return;
    (void)memset(jump, 0, sizeof(*jump));
    yew_typejump_clear(&jump->type);
}

void yew_fuss_jump_arm(FussJump *jump, i64 now_ms)
{
    if (jump == NULL)
        return;
    yew_typejump_clear(&jump->type);
    jump->armed = true;
    jump->deadline_ms = now_ms + (i64)YEW_TYPEJUMP_RESET_MS;
}

bool yew_fuss_jump_key(FussJump *jump, const Key *key, i64 now_ms,
                       const PickItem *items, u32 n, u32 *sel)
{
    const char **labels;
    FzRanked *ranked;
    u32 matched;
    u32 i;
    bool printable;

    if (jump == NULL || key == NULL || !jump->armed)
        return false;
    printable = key->ntext == 1U && key->text[0] >= 0x20U &&
                key->text[0] != 0x7fU &&
                (key->mods & (YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SUPER |
                              YEW_MOD_HYPER | YEW_MOD_META)) == 0U;
    if (key->code == YEW_KEY_ESCAPE || key->code == YEW_KEY_ENTER) {
        yew_fuss_jump_init(jump);
        return true;
    }
    if (key->code == YEW_KEY_BACKSPACE) {
        if (jump->type.len != 0U)
            jump->type.len--;
        jump->type.pat[jump->type.len] = '\0';
        jump->deadline_ms = now_ms + (i64)YEW_TYPEJUMP_RESET_MS;
        jump->type.deadline_ms = jump->deadline_ms;
        if (jump->type.len == 0U || items == NULL || n == 0U || sel == NULL)
            return true;
        printable = false;
    }
    if (!printable) {
        if (key->code != YEW_KEY_BACKSPACE) {
            yew_fuss_jump_init(jump);
            return false;
        }
    } else {
        if (items == NULL || n == 0U || sel == NULL)
            return false;
        if (jump->type.len == 0U || now_ms >= jump->deadline_ms)
            jump->type.len = 0U;
        if (jump->type.len + 1U < (u32)YEW_TYPEJUMP_PAT_MAX) {
            jump->type.pat[jump->type.len++] = (char)key->text[0];
            jump->type.pat[jump->type.len] = '\0';
        }
        jump->deadline_ms = now_ms + (i64)YEW_TYPEJUMP_RESET_MS;
        jump->type.deadline_ms = jump->deadline_ms;
    }
    if (*sel < n && items[*sel].label != NULL &&
        yew_fz_score(jump->type.pat, jump->type.len, items[*sel].label,
                     (u32)fuss_cstr_len(items[*sel].label), NULL) >= 10000)
        return true;
    labels = yew_xreallocarray(NULL, n, sizeof(*labels));
    ranked = yew_xreallocarray(NULL, n, sizeof(*ranked));
    for (i = 0U; i < n; i++)
        labels[i] = items[i].label;
    matched = yew_fz_rank(jump->type.pat, jump->type.len, labels, n, true,
                          ranked);
    if (matched != 0U)
        *sel = ranked[0].idx;
    free(labels);
    free(ranked);
    return true;
}

bool yew_fuss_jump_tick(FussJump *jump, i64 now_ms)
{
    if (jump == NULL || !jump->armed || now_ms < jump->deadline_ms)
        return false;
    yew_fuss_jump_init(jump);
    return true;
}

bool yew_fuss_jump_armed(const FussJump *jump)
{
    return jump != NULL && jump->armed;
}

const char *yew_fuss_jump_pattern(const FussJump *jump, u32 *len)
{
    if (len != NULL)
        *len = jump == NULL ? 0U : jump->type.len;
    return jump == NULL ? "" : jump->type.pat;
}

static void fuss_damage(Ed *ed)
{
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
}

static bool fuss_show_buffer(Ed *ed, Buffer *buffer)
{
    return fuss_viewer_open(ed, buffer);
}

static bool fuss_replace_buffer(Ed *ed, Buffer *buffer, const u8 *bytes,
                                u64 len)
{
    EditCtx edit;
    u64 old_len;
    bool ok = true;

    if (ed == NULL || buffer == NULL || buffer->tb == NULL ||
        (bytes == NULL && len != 0U))
        return false;
    edit = yew_ed_edit_ctx_buffer(ed, buffer);
    if ((buffer->flags & YEW_BUF_NOUNDO) != 0U)
        edit.undo = NULL;
    old_len = yew_textbuf_len(buffer->tb);
    if (old_len != 0U)
        ok = yew_edit_delete(&edit, (Span){0U, old_len});
    if (ok && len != 0U)
        ok = yew_edit_insert(&edit, BYTEOFF(0U), bytes, len);
    yew_ed_finish_edit(ed, &edit);
    return ok;
}

static void fuss_show_result(Ed *ed, const GitResult *result)
{
    Buffer *buffer;

    if (ed == NULL || result == NULL || result->out_len > (u64)SIZE_MAX)
        return;
    buffer = yew_ws_scratch_find(ed, "*git-view*");
    if (buffer == NULL)
        buffer = yew_ws_scratch_new(ed, "*git-view*",
                                    YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (buffer == NULL || buffer->tb == NULL)
        return;
    if (!fuss_replace_buffer(ed, buffer, result->out, result->out_len)) {
        yew_msg(ed, YEW_MSG_ERROR, "could not update Git viewer buffer");
        return;
    }
    yew_undo_mark_saved(buffer->undo);
    (void)fuss_show_buffer(ed, buffer);
}

static bool fuss_result_has(const u8 *bytes, u64 len, const char *needle)
{
    size_t needle_len = fuss_cstr_len(needle);
    u64 at;

    if (bytes == NULL || needle == NULL || needle_len == 0U ||
        len < (u64)needle_len)
        return false;
    for (at = 0U; at <= len - (u64)needle_len; at++) {
        if (memcmp(bytes + at, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static void fuss_result_tick(Ed *ed)
{
    FussMode *f;
    const GitResult *result;
    FussPickAction pick;
    u64 err_len;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->pending_job == 0U)
        return;
    result = yew_git_result(ed);
    if (result == NULL || result->job_id != f->pending_job ||
        result->job_id == f->seen_result_job)
        return;
    f->seen_result_job = result->job_id;
    f->pending_job = 0U;
    pick = f->pending_pick;
    f->pending_pick = FUSS_PICK_NONE;
    if (result->state == YEW_GIT_OK) {
        if (pick == FUSS_PICK_COMMIT_AMEND) {
            if (result->out_len > (u64)SIZE_MAX)
                yew_msg(ed, YEW_MSG_ERROR,
                        "previous commit message is too large to edit");
            else
                (void)fuss_commit_begin(ed, true, result->out,
                                        (size_t)result->out_len);
        } else if (pick == FUSS_PICK_REBASE_CONFIRM) {
            fuss_show_result(ed, result);
            f->prompt_action = FUSS_PROMPT_REBASE;
            yew_cmdline_open_input(ed, NULL, fuss_prompt_done, f);
            if (ed->cmdline.active)
                yew_msg(ed, YEW_MSG_WARN,
                        "review the rebase range, then type 'rebase' to continue");
            else
                fuss_prompt_clear(f);
        } else if (pick != FUSS_PICK_NONE)
            (void)fuss_picker_result(ed, pick, result);
        else if (f->pending_view)
            fuss_show_result(ed, result);
        else
            yew_msg(ed, YEW_MSG_INFO, "%s complete",
                    result->verb == NULL ? "git" : result->verb);
        if (result->verb != NULL &&
            strcmp(result->verb, "branch-delete") == 0 &&
            f->prompt_action == FUSS_PROMPT_NONE) {
            free(f->prompt_path);
            f->prompt_path = NULL;
        }
    } else if (result->verb != NULL &&
               strcmp(result->verb, "branch-delete") == 0 &&
               f->prompt_path != NULL &&
               f->prompt_action == FUSS_PROMPT_NONE &&
               fuss_result_has(result->err, result->err_len,
                               "not fully merged")) {
        f->prompt_action = FUSS_PROMPT_BRANCH_DELETE;
        yew_cmdline_open_input(ed, NULL, fuss_prompt_done, f);
        if (ed->cmdline.active)
            yew_msg(ed, YEW_MSG_WARN,
                    "force-delete branch %s — type 'delete' to confirm",
                    f->prompt_path);
        else
            fuss_prompt_clear(f);
    } else if (result->err != NULL && result->err_len != 0U) {
        err_len = result->err_len > (u64)INT_MAX ? (u64)INT_MAX :
                                                       result->err_len;
        yew_msg(ed, YEW_MSG_ERROR, "%.*s", (int)err_len,
                (const char *)result->err);
    } else {
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                yew_git_state_str(result->state));
    }
    if (result->state != YEW_GIT_OK && result->verb != NULL &&
        strcmp(result->verb, "branch-delete") == 0 &&
        f->prompt_action == FUSS_PROMPT_NONE) {
        free(f->prompt_path);
        f->prompt_path = NULL;
    }
    if (result->state != YEW_GIT_OK &&
        pick == FUSS_PICK_REBASE_CONFIRM)
        fuss_prompt_clear(f);
    if (result->state != YEW_GIT_OK && pick == FUSS_PICK_REMOTE_CHECK)
        fuss_picker_open(ed, FUSS_PICK_REMOTE, "remote");
    f->pending_view = false;
    fuss_damage(ed);
}

static i32 fuss_row(const FussMode *f)
{
    return f == NULL ? -1 : yew_fuss_row_of(&f->tree, &f->sel);
}

static const FussItem *fuss_item(const FussMode *f, i32 row)
{
    if (f == NULL || row < 0 || (size_t)row >= f->tree.items.len)
        return NULL;
    return &f->tree.items.data[row];
}

static const FussNode *fuss_node(const FussMode *f, const FussItem *item)
{
    if (f == NULL || item == NULL || item->node >= f->tree.nodes.len)
        return NULL;
    return &f->tree.nodes.data[item->node];
}

static void fuss_select_row(FussMode *f, i32 row)
{
    if (f == NULL)
        return;
    yew_fuss_sel_from_row(&f->sel, &f->tree, row);
}

static void fuss_build(Ed *ed, const GitSnapshot *snap, bool force)
{
    FussMode *f = ed->fuss;
    Arena saved;
    char **paths = NULL;
    u32 npaths;

    if (f == NULL || snap == NULL ||
        (!force && f->tree.snap_gen == snap->gen))
        return;
    arena_init(&saved);
    npaths = yew_fuss_harvest_collapsed(&f->tree, &saved, &paths);
    yew_fuss_build(&f->tree, snap, &f->opts);
    fuss_rebase_progress_update(ed, snap);
    if (f->opts.all_files && f->walk == NULL && f->files.paths.len != 0U)
        (void)yew_fuss_merge_files(&f->tree, &f->files, snap, &f->opts);
    yew_fuss_restore_collapsed(&f->tree, paths, npaths);
    yew_fuss_flatten(&f->tree);
    if (fuss_row(f) < 0 && f->tree.items.len != 0U)
        fuss_select_row(f, 0);
    arena_free_all(&saved);
    f->scroll = 0U;
    fuss_damage(ed);
}

static void fuss_rebuild(Ed *ed, const GitSnapshot *snap)
{
    fuss_build(ed, snap, false);
}

void yew_fuss_state_init(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss != NULL)
        return;
    f = yew_xcalloc(1U, sizeof(*f));
    yew_fuss_tree_init(&f->tree);
    yew_fuss_jump_init(&f->jump);
    yew_filelist_init(&f->files);
    yew_filelist_init(&f->expand_files);
    bytebuf_init(&f->preview_bytes);
    ed->fuss = f;
}

void yew_fuss_state_free(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->active || f->fuss_root != NULL) {
        f->commit_editing = false;
        f->active = false;
        fuss_layout_restore(ed);
    }
    free(f->prompt_path);
    {
        u32 i;

        for (i = 0U; i < f->picker_count; i++) {
            free((char *)f->picker_items[i].label);
            free((char *)f->picker_items[i].detail);
            free(f->picker_values[i]);
        }
    }
    free(f->picker_items);
    free(f->picker_values);
    free(f->picker_aux);
    free(f->picker_aux2);
    if (f->preview_job != 0U)
        (void)yew_job_signal(ed, f->preview_job, SIGTERM);
    free(f->preview_value);
    bytebuf_free(&f->preview_bytes);
    if (f->walk != NULL)
        yew_walk_end(f->walk);
    fuss_expand_clear(f);
    yew_filelist_free(&f->files);
    yew_filelist_free(&f->expand_files);
    yew_fuss_sel_clear(&f->sel);
    yew_fuss_tree_drop(&f->tree);
    free(f);
    ed->fuss = NULL;
}

bool yew_fuss_active(const Ed *ed)
{
    return ed != NULL && ed->fuss != NULL && ed->fuss->active;
}

CmdStatus yew_fuss_mode_enter(Ed *ed)
{
    GitAsyncState avail;
    const GitSnapshot *snap;
    OptVal ascii;

    if (ed == NULL || ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    avail = yew_git_avail_state(ed);
    if (avail == YEW_GIT_ASYNC_FAILED) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", yew_git_state_str(YEW_GIT_NO_GIT));
        return YEW_CMD_ERR_STATE;
    }
    if (yew_opt_get(ed, NULL, NULL, "git.ascii_glyphs",
                    (u32)(sizeof("git.ascii_glyphs") - 1U), &ascii) &&
        ascii.type == (u8)YEW_OPT_BOOL)
        ed->fuss->ascii_glyphs = ascii.as.b;
    if (!ed->fuss->active) {
        ed->fuss->opts.all_files = yew_state_option_bool(
            ed, "git.tree.all_files", false);
        ed->fuss->opts.show_hidden = yew_state_option_bool(
            ed, "git.tree.show_hidden", false);
        ed->fuss->active = true;
        ed->fuss->saved_buffer_id = ed->win != NULL && ed->win->buf != NULL ?
                                    ed->win->buf->id : 0U;
        ed->fuss->saved_root = ed->pane_root;
        ed->fuss->saved_focus = ed->focus;
        ed->fuss->saved_win = ed->win;
        ed->fuss->fuss_root = yew_pane_new_leaf(ed->win);
        ed->pane_root = ed->fuss->fuss_root;
        ed->focus = ed->fuss->fuss_root;
        fuss_walk_restart(ed);
    }
    snap = yew_git_snapshot(ed);
    if (snap != NULL)
        fuss_rebuild(ed, snap);
    if (ed->fuss->walk != NULL && !yew_walk_step(ed->fuss->walk, 2000)) {
        yew_walk_end(ed->fuss->walk);
        ed->fuss->walk = NULL;
        if (snap != NULL && yew_fuss_merge_files(&ed->fuss->tree,
                                                  &ed->fuss->files, snap,
                                                  &ed->fuss->opts)) {
            if (fuss_row(ed->fuss) < 0 && ed->fuss->tree.items.len != 0U)
                fuss_select_row(ed->fuss, 0);
            fuss_damage(ed);
        }
    }
    (void)yew_git_refresh(ed, false);
    fuss_damage(ed);
    return YEW_CMD_OK;
}

void yew_fuss_mode_leave(Ed *ed)
{
    FussMode *f;
    Buffer *saved;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->commit_editing)
        return;
    f->active = false;
    fuss_expand_clear(f);
    yew_fuss_jump_init(&f->jump);
    saved = yew_ws_buf_by_id(ed, f->saved_buffer_id);
    if (saved != NULL && f->saved_win != NULL && f->saved_win->buf != saved)
        yew_ed_win_set_buffer(ed, f->saved_win, saved);
    fuss_layout_restore(ed);
    fuss_damage(ed);
}

u16 yew_fuss_footer_rows(const Ed *ed)
{
    return yew_fuss_active(ed) ? 2U : 0U;
}

void yew_fuss_tick(Ed *ed, i64 now_ms)
{
    const GitSnapshot *snap;

    if (ed == NULL || ed->fuss == NULL)
        return;
    fuss_result_tick(ed);
    if (!yew_fuss_active(ed))
        return;
    if (yew_git_avail_state(ed) == YEW_GIT_ASYNC_FAILED) {
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                yew_git_state_str(YEW_GIT_NO_GIT));
        if (ed->mode == YEW_MODE_F)
            (void)yew_mode_enter(ed, YEW_MODE_L);
        return;
    }
    if (yew_git_detect_state(ed) == YEW_GIT_ASYNC_FAILED)
        return;
    snap = yew_git_snapshot(ed);
    if (snap != NULL)
        fuss_rebuild(ed, snap);
    if (ed->fuss->walk != NULL &&
        !yew_walk_step(ed->fuss->walk, 2000)) {
        yew_walk_end(ed->fuss->walk);
        ed->fuss->walk = NULL;
        if (snap != NULL &&
            yew_fuss_merge_files(&ed->fuss->tree, &ed->fuss->files, snap,
                                  &ed->fuss->opts)) {
            if (fuss_row(ed->fuss) < 0 &&
                ed->fuss->tree.items.len != 0U)
                fuss_select_row(ed->fuss, 0);
            fuss_damage(ed);
        }
    }
    fuss_expand_tick(ed);
    if (yew_fuss_jump_tick(&ed->fuss->jump, now_ms))
        ed->footer_dirty = true;
}

i64 yew_fuss_deadline(const Ed *ed, i64 now_ms)
{
    const FussMode *f;

    if (ed == NULL || ed->fuss == NULL || !ed->fuss->active)
        return -1;
    f = ed->fuss;
    if (f->walk != NULL || f->expand_walk != NULL)
        return 0;
    if (!f->jump.armed)
        return -1;
    if (f->jump.deadline_ms <= now_ms)
        return 0;
    if (now_ms < 0 && f->jump.deadline_ms > INT64_MAX + now_ms)
        return INT64_MAX;
    return f->jump.deadline_ms - now_ms;
}

static bool fuss_jump_items(FussMode *f, PickItem **out, u32 *out_n)
{
    PickItem *items;
    size_t i;

    *out = NULL;
    *out_n = 0U;
    if (f == NULL || f->tree.items.len == 0U ||
        f->tree.items.len > (size_t)UINT32_MAX)
        return false;
    items = yew_xcalloc(f->tree.items.len, sizeof(*items));
    for (i = 0U; i < f->tree.items.len; i++) {
        items[i].label = f->tree.items.data[i].path;
        items[i].payload = (i32)i;
    }
    *out = items;
    *out_n = (u32)f->tree.items.len;
    return true;
}

bool yew_fuss_key(Ed *ed, const Key *key, i64 now_ms)
{
    FussMode *f;
    PickItem *items;
    u32 n;
    u32 row;
    bool consumed;

    if (!yew_fuss_active(ed) || key == NULL)
        return false;
    f = ed->fuss;
    if (!f->jump.armed)
        return false;
    if (!fuss_jump_items(f, &items, &n)) {
        yew_fuss_jump_init(&f->jump);
        return false;
    }
    row = fuss_row(f) < 0 ? 0U : (u32)fuss_row(f);
    consumed = yew_fuss_jump_key(&f->jump, key, now_ms, items, n, &row);
    free(items);
    if (consumed && row < n) {
        fuss_select_row(f, (i32)row);
        ed->full_damage = true;
        ed->footer_dirty = true;
    }
    return consumed;
}

static void fuss_put_bytes(Grid *g, u16 row, u16 *col, u16 right,
                           const u8 *bytes, size_t len, YewColor fg,
                           YewColor bg, u16 attrs)
{
    size_t clipped;
    int cells;

    if (*col >= right || len == 0U)
        return;
    clipped = yew_str_clip(bytes, len, (u16)(right - *col), &cells);
    if (clipped == 0U || cells <= 0)
        return;
    *col = yew_grid_puts(g, row, *col, bytes, clipped, fg, bg, attrs);
}

static void fuss_put_lit(Grid *g, u16 row, u16 *col, u16 right,
                         const char *text, size_t len, YewColor fg,
                         YewColor bg, u16 attrs)
{
    fuss_put_bytes(g, row, col, right, (const u8 *)text, len, fg, bg,
                   attrs);
}

static ThemeEnt fuss_base_style(const Ed *ed)
{
    const ThemeEnt *fg = yew_theme_ui_tab(ed, "fg");
    const ThemeEnt *bg = yew_theme_ui_tab(ed, "bg");
    ThemeEnt style = {
        {YEW_COLOR_RGB, 218U, 229U, 240U},
        {YEW_COLOR_DEFAULT, 0U, 0U, 0U},
        0U
    };

    if (fg != NULL) {
        style.fg = fg->fg;
        style.attrs = fg->attrs;
    }
    if (bg != NULL) {
        style.bg = bg->bg;
        style.attrs = (u16)(style.attrs | bg->attrs);
    }
    return style;
}

static ThemeEnt fuss_role_style(const Ed *ed, const char *role,
                                 ThemeEnt fallback)
{
    const ThemeEnt *themed = yew_theme_ui_tab(ed, role);

    if (themed != NULL) {
        fallback.fg = themed->fg;
        fallback.attrs = (u16)(fallback.attrs | themed->attrs);
    }
    return fallback;
}

static void fuss_header(Ed *ed, u16 row, u16 left, u16 right)
{
    const GitSnapshot *snap;
    ThemeEnt style = fuss_base_style(ed);
    Bytebuf counts;
    u16 col = left;

    fuss_put_lit(&ed->grid, row, &col, right, "yew:", sizeof("yew:") - 1U,
                 style.fg, style.bg,
                 (u16)(style.attrs | YEW_ATTR_BOLD));
    if (yew_git_detect_state(ed) == YEW_GIT_ASYNC_FAILED) {
        const char *state = yew_git_state_str(yew_git_detect_result(ed));

        fuss_put_lit(&ed->grid, row, &col, right, state,
                     fuss_cstr_len(state), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_DIM));
        return;
    }
    snap = yew_git_snapshot(ed);
    if (snap == NULL || snap->gen == 0U) {
        fuss_put_lit(&ed->grid, row, &col, right, "loading",
                     sizeof("loading") - 1U, style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_DIM));
        return;
    }
    if (snap->branch != NULL)
        fuss_put_lit(&ed->grid, row, &col, right, snap->branch,
                     fuss_cstr_len(snap->branch), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
    else
        fuss_put_lit(&ed->grid, row, &col, right, "(detached)",
                     sizeof("(detached)") - 1U, style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
    bytebuf_init(&counts);
    bytebuf_printf(&counts, "  ↑%d ↓%d", snap->ahead < 0 ? 0 : snap->ahead,
                   snap->behind < 0 ? 0 : snap->behind);
    if (snap->state == YEW_GIT_MID_REBASE) {
        if (ed->fuss->rebase_step > 0 && ed->fuss->rebase_total > 0)
            bytebuf_printf(&counts, "  REBASING %d/%d",
                           ed->fuss->rebase_step,
                           ed->fuss->rebase_total);
        else
            bytebuf_append(&counts, (const u8 *)"  REBASING",
                           sizeof("  REBASING") - 1U);
    }
    fuss_put_lit(&ed->grid, row, &col, right, (const char *)counts.data,
                 counts.len, style.fg, style.bg, style.attrs);
    bytebuf_free(&counts);
}

static void fuss_marker(Ed *ed, u16 row, u16 *col, u16 right,
                        const char *glyph, size_t len, ThemeEnt style,
                        u16 selected_attrs)
{
    u16 attrs = (u16)(style.attrs | selected_attrs);

    fuss_put_lit(&ed->grid, row, col, right, " ", 1U, style.fg, style.bg,
                 attrs);
    fuss_put_lit(&ed->grid, row, col, right, glyph, len, style.fg,
                 style.bg, attrs);
}

static const FussNode *fuss_prefix_ancestor(const FussMode *f,
                                            const FussItem *item,
                                            u16 depth)
{
    u32 node;
    u16 climb;

    if (f == NULL || item == NULL || depth >= item->depth)
        return NULL;
    node = item->node;
    climb = (u16)(item->depth - depth);
    while (climb-- != 0U) {
        if (node >= f->tree.nodes.len)
            return NULL;
        node = f->tree.nodes.data[node].parent;
    }
    return node < f->tree.nodes.len ? &f->tree.nodes.data[node] : NULL;
}

static void fuss_tree_row(Ed *ed, u16 row, u16 left, u16 right,
                          const FussItem *item, bool selected)
{
    FussMode *f = ed->fuss;
    const FussNode *node = fuss_node(f, item);
    ThemeEnt normal = fuss_base_style(ed);
    ThemeEnt ignored = fuss_role_style(ed, "git.ignored", normal);
    ThemeEnt staged = fuss_role_style(ed, "git.staged", normal);
    ThemeEnt modified = fuss_role_style(ed, "git.modified", normal);
    ThemeEnt untracked = fuss_role_style(ed, "git.untracked", normal);
    ThemeEnt incoming = fuss_role_style(ed, "git.incoming", normal);
    ThemeEnt conflict = fuss_role_style(ed, "git.conflict", normal);
    Cell blank = ed->grid.blank;
    u16 selected_attrs = selected ? YEW_ATTR_REVERSE : 0U;
    u16 col = left;
    u16 depth;
    const char *branch;
    size_t branch_len;
    FussMarkerKind markers[4];
    u8 marker_count;
    u8 marker_i;

    if (node == NULL)
        return;
    blank.fg = normal.fg;
    blank.bg = normal.bg;
    blank.attrs = (u16)(normal.attrs | selected_attrs);
    yew_grid_fill(&ed->grid, row, left, right, blank);
    for (depth = 0U; depth < item->depth && col < right; depth++) {
        const FussNode *ancestor = fuss_prefix_ancestor(f, item, depth);
        bool open = ancestor != NULL && ancestor->next_sibling != 0U;
        const char *prefix = open ? (f->ascii_glyphs ? "|   " : "│   ") :
                                    "    ";
        size_t prefix_len = open && !f->ascii_glyphs ?
                            sizeof("│   ") - 1U : 4U;

        fuss_put_lit(&ed->grid, row, &col, right, prefix, prefix_len,
                     normal.fg, normal.bg,
                     (u16)(normal.attrs | selected_attrs));
    }
    if (f->ascii_glyphs) {
        branch = node->next_sibling == 0U ? "`-- " : "|-- ";
        branch_len = 4U;
    } else {
        branch = node->next_sibling == 0U ? "└── " : "├── ";
        branch_len = sizeof("└── ") - 1U;
    }
    fuss_put_lit(&ed->grid, row, &col, right, branch, branch_len, normal.fg,
                 normal.bg, (u16)(normal.attrs | selected_attrs));
    if (!node->is_file) {
        const char *dir = f->ascii_glyphs ? (node->expanded ? "v " : "> ") :
                          (node->expanded ? "▼ " : "▶ ");
        size_t dir_len = f->ascii_glyphs ? 2U : sizeof("▼ ") - 1U;

        fuss_put_lit(&ed->grid, row, &col, right, dir, dir_len, normal.fg,
                     normal.bg, (u16)(normal.attrs | selected_attrs));
    }
    fuss_put_lit(&ed->grid, row, &col, right, node->name, node->name_len,
                 node->ignored ? ignored.fg : normal.fg, normal.bg,
                 (u16)((node->ignored ? ignored.attrs : normal.attrs) |
                       selected_attrs));
    marker_count = yew_fuss_marker_kinds(node, markers);
    for (marker_i = 0U; marker_i < marker_count; marker_i++) {
        const char *glyph;
        size_t glyph_len;
        ThemeEnt marker_style;
        u16 attrs = selected_attrs;

        switch (markers[marker_i]) {
        case YEW_FUSS_MARK_STAGED:
            glyph = f->ascii_glyphs ? "^" : "↑";
            glyph_len = f->ascii_glyphs ? 1U : sizeof("↑") - 1U;
            marker_style = staged;
            break;
        case YEW_FUSS_MARK_UNSTAGED:
            glyph = f->ascii_glyphs ? "x" : "✗";
            glyph_len = f->ascii_glyphs ? 1U : sizeof("✗") - 1U;
            marker_style = modified;
            break;
        case YEW_FUSS_MARK_UNTRACKED:
            glyph = f->ascii_glyphs ? "x" : "✗";
            glyph_len = f->ascii_glyphs ? 1U : sizeof("✗") - 1U;
            marker_style = untracked;
            break;
        case YEW_FUSS_MARK_INCOMING:
            glyph = f->ascii_glyphs ? "v" : "↓";
            glyph_len = f->ascii_glyphs ? 1U : sizeof("↓") - 1U;
            marker_style = incoming;
            break;
        case YEW_FUSS_MARK_CONFLICT:
            glyph = "!";
            glyph_len = 1U;
            marker_style = conflict;
            attrs = (u16)(attrs | YEW_ATTR_BOLD);
            break;
        default:
            continue;
        }
        fuss_marker(ed, row, &col, right, glyph, glyph_len, marker_style,
                    attrs);
    }
}

static void fuss_draw_viewer(Ed *ed)
{
    FussMode *f;
    Win *w;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    w = f->viewer_leaf == NULL ? NULL : f->viewer_leaf->win;
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->rect.w == 0U || w->rect.h == 0U)
        return;
    yew_draw_document_rows(ed, w, 0U, w->rect.h);
}

void yew_fuss_draw(Ed *ed)
{
    FussMode *f;
    Rect content;
    Rect tree;
    u16 top;
    u16 first;
    u16 visible;
    i32 selected;
    size_t i;
    Cell blank;

    if (!yew_fuss_active(ed))
        return;
    f = ed->fuss;
    yew_region_frame_begin();
    yew_tab_strip_draw(ed, ed->tab_strip_rect);
    top = (u16)(ed->tab_strip_rect.y + ed->tab_strip_rect.h);
    content = (Rect){0U, top, ed->grid.cols,
                     ed->footer_rect.y > top ?
                         (u16)(ed->footer_rect.y - top) : 0U};
    blank = ed->grid.blank;
    for (i = content.y; i < (size_t)content.y + content.h; i++)
        yew_grid_fill(&ed->grid, (u16)i, 0U, ed->grid.cols, blank);
    if (content.h == 0U)
        return;
    tree = f->fuss_root == NULL ? content : f->fuss_root->rect;
    if (f->viewer_leaf != NULL && f->fuss_root != NULL &&
        !f->fuss_root->is_leaf)
        tree = f->fuss_root->a->rect;
    fuss_header(ed, tree.y, tree.x, (u16)(tree.x + tree.w));
    visible = tree.h > 1U ? (u16)(tree.h - 1U) : 0U;
    if (yew_git_detect_state(ed) == YEW_GIT_ASYNC_FAILED && visible != 0U) {
        ThemeEnt style = fuss_base_style(ed);
        const char *message = "not a Git repository — use :git.init";
        u16 col = tree.x;

        fuss_put_lit(&ed->grid, (u16)(tree.y + 1U), &col,
                     (u16)(tree.x + tree.w), message,
                     fuss_cstr_len(message), style.fg, style.bg,
                     YEW_ATTR_DIM);
        return;
    }
    selected = fuss_row(f);
    if (selected >= 0 && visible != 0U) {
        if ((u32)selected < f->scroll)
            f->scroll = (u16)selected;
        else if ((u32)selected >= (u32)f->scroll + visible)
            f->scroll = (u16)((u32)selected - visible + 1U);
    }
    first = f->scroll;
    for (i = 0U; i < visible && (size_t)first + i < f->tree.items.len; i++) {
        u16 row = (u16)(tree.y + 1U + (u16)i);
        const FussItem *item = fuss_item(f, (i32)((size_t)first + i));
        u32 path_id;

        fuss_tree_row(ed, row, tree.x, (u16)(tree.x + tree.w), item,
                      (i32)((size_t)first + i) == selected);
        path_id = item == NULL ? 0U :
                  yew_intern(&ed->interner, item->path, item->path_len);
        if (path_id <= (u32)INT32_MAX)
            yew_region_add(YEW_REGION_FUSS_ROW,
                           (Rect){tree.x, row, tree.w, 1U},
                           (i32)path_id);
    }
    if (f->viewer_leaf != NULL && f->fuss_root != NULL &&
        !f->fuss_root->is_leaf) {
        ThemeEnt border_style = fuss_role_style(ed, "git.ignored",
                                                 fuss_base_style(ed));
        u16 border = (u16)(tree.x + tree.w);
        u16 row;

        for (row = content.y; row < (u16)(content.y + content.h); row++)
            (void)yew_grid_put(&ed->grid, row, border, (const u8 *)"│",
                               sizeof("│") - 1U, border_style.fg,
                               border_style.bg, border_style.attrs);
        fuss_draw_viewer(ed);
    }
}

void yew_fuss_draw_footer(Ed *ed, Rect footer)
{
    FussMode *f;
    YewUiStyle style;
    Cell blank;
    u16 col;
    const GitSnapshot *snap;
    const char *line1;
    const char *line2;

    if (!yew_fuss_active(ed) || footer.h == 0U)
        return;
    f = ed->fuss;
    line1 = f->ascii_glyphs ?
        "Legend: ^ staged  x modified  x untracked  v incoming  ! conflict" :
        "Legend: ↑ staged  ✗ modified  ✗ untracked  ↓ incoming  ! conflict";
    line2 = f->ascii_glyphs ?
        "<> tree | ^v siblings | a stage | m commit | / jump | q leave" :
        "←→ tree · ↑↓ siblings · a stage · m commit · / jump · q leave";
    snap = yew_git_snapshot(ed);
    if (snap != NULL && snap->state == YEW_GIT_MID_REBASE)
        line2 = "rebase stopped · :git.rebase.continue · :git.rebase.abort";
    style = yew_statusline_mode_style(YEW_MODE_F);
    blank = ed->grid.blank;
    blank.fg = style.chip_fg;
    blank.bg = style.chip_bg;
    blank.attrs = 0U;
    yew_grid_fill(&ed->grid, footer.y, footer.x,
                  (u16)(footer.x + footer.w), blank);
    if (footer.h > 1U)
        yew_grid_fill(&ed->grid, (u16)(footer.y + 1U), footer.x,
                      (u16)(footer.x + footer.w), blank);
    col = footer.x;
    if (f->jump.armed) {
        fuss_put_lit(&ed->grid, footer.y, &col,
                     (u16)(footer.x + footer.w), "jump: ",
                     sizeof("jump: ") - 1U, style.chip_fg, style.chip_bg,
                     YEW_ATTR_BOLD);
        fuss_put_lit(&ed->grid, footer.y, &col,
                     (u16)(footer.x + footer.w), f->jump.type.pat,
                     f->jump.type.len, style.chip_fg, style.chip_bg,
                     YEW_ATTR_BOLD);
        fuss_put_lit(&ed->grid, footer.y, &col,
                     (u16)(footer.x + footer.w), "▏",
                     sizeof("▏") - 1U, style.chip_fg, style.chip_bg,
                     YEW_ATTR_BOLD);
    } else if (yew_msg_visible(ed)) {
        fuss_put_lit(&ed->grid, footer.y, &col,
                     (u16)(footer.x + footer.w), ed->msg.text,
                     fuss_cstr_len(ed->msg.text), style.chip_fg,
                     style.chip_bg, 0U);
    } else {
        if (yew_git_detect_state(ed) == YEW_GIT_ASYNC_FAILED) {
            const char *state =
                yew_git_state_str(yew_git_detect_result(ed));

            fuss_put_lit(&ed->grid, footer.y, &col,
                         (u16)(footer.x + footer.w), state,
                         fuss_cstr_len(state), style.chip_fg, style.chip_bg,
                         0U);
        } else {
            if (snap == NULL || snap->gen == 0U)
                fuss_put_lit(&ed->grid, footer.y, &col,
                             (u16)(footer.x + footer.w), "loading…",
                             sizeof("loading…") - 1U, style.chip_fg,
                             style.chip_bg, 0U);
            else
                fuss_put_lit(&ed->grid, footer.y, &col,
                             (u16)(footer.x + footer.w), line1,
                             fuss_cstr_len(line1), style.chip_fg,
                             style.chip_bg, 0U);
        }
    }
    if (footer.h > 1U) {
        col = footer.x;
        fuss_put_lit(&ed->grid, (u16)(footer.y + 1U), &col,
                     (u16)(footer.x + footer.w), line2,
                     fuss_cstr_len(line2), style.chip_fg, style.chip_bg,
                     0U);
    }
    if (ed->cmdline.active)
        yew_cmdline_draw(ed,
                         (Rect){footer.x,
                                (u16)(footer.y + footer.h - 1U), footer.w,
                                1U});
}

static CmdStatus fuss_require(CmdCtx *cx, FussMode **out)
{
    if (cx == NULL || cx->ed == NULL || cx->ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    if (out != NULL)
        *out = cx->ed->fuss;
    return YEW_CMD_OK;
}

static bool fuss_safe_path(const char *path, size_t len)
{
    size_t at = 0U;

    if (path == NULL || len == 0U || path[0] == '/' ||
        memchr(path, '\0', len) != NULL)
        return false;
    while (at < len) {
        size_t start = at;

        while (at < len && path[at] != '/')
            at++;
        if (at - start == 2U && path[start] == '.' &&
            path[start + 1U] == '.')
            return false;
        if (at < len)
            at++;
    }
    return true;
}

static char *fuss_dup_bytes(const char *bytes, size_t len)
{
    char *copy;

    if (bytes == NULL || len == 0U || memchr(bytes, '\0', len) != NULL)
        return NULL;
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, bytes, len);
    copy[len] = '\0';
    return copy;
}

static char *fuss_selected_path(CmdCtx *cx)
{
    FussMode *f;
    const FussItem *item;
    i32 row;

    if (fuss_require(cx, &f) != YEW_CMD_OK)
        return NULL;
    if (cx->sarg != NULL && cx->sarg_len != 0U) {
        if (!fuss_safe_path(cx->sarg, cx->sarg_len))
            return NULL;
        return fuss_dup_bytes(cx->sarg, cx->sarg_len);
    }
    row = fuss_row(f);
    item = fuss_item(f, row);
    if (item == NULL || !fuss_safe_path(item->path, item->path_len))
        return NULL;
    return fuss_dup_bytes(item->path, item->path_len);
}

typedef struct FussTarget {
    bool known;
    bool status_known;
    bool is_file;
    bool staged;
    bool unstaged;
    bool untracked;
    bool incoming;
    bool conflicted;
} FussTarget;

typedef enum FussTargetGuard {
    FUSS_TARGET_ANY,
    FUSS_TARGET_FILE,
    FUSS_TARGET_STAGED_FILE,
    FUSS_TARGET_DIRTY_FILE
} FussTargetGuard;

static bool fuss_target_lookup(CmdCtx *cx, const char *path,
                               FussTarget *target)
{
    FussMode *f;
    size_t len;
    size_t i;
    char *absolute;
    struct stat st;

    if (target == NULL)
        return false;
    (void)memset(target, 0, sizeof(*target));
    if (fuss_require(cx, &f) != YEW_CMD_OK || path == NULL)
        return false;
    len = fuss_cstr_len(path);
    for (i = 0U; i < f->tree.items.len; i++) {
        const FussItem *item = &f->tree.items.data[i];
        const FussNode *node;

        if (item->path_len != len || memcmp(item->path, path, len) != 0)
            continue;
        node = fuss_node(f, item);
        if (node == NULL)
            break;
        target->known = true;
        target->status_known = true;
        target->is_file = node->is_file;
        target->staged = node->staged;
        target->unstaged = node->unstaged;
        target->untracked = node->untracked;
        target->incoming = node->incoming;
        target->conflicted = node->conflicted;
        return true;
    }
    absolute = fuss_join_root(cx->ed, path);
    if (absolute != NULL && lstat(absolute, &st) == 0) {
        target->known = true;
        target->is_file = !S_ISDIR(st.st_mode);
    }
    free(absolute);
    return target->known;
}

static bool fuss_target_guard(CmdCtx *cx, const char *path,
                              FussTargetGuard guard, const char *action,
                              FussTarget *target)
{
    FussTarget found;

    if (guard == FUSS_TARGET_ANY) {
        if (target != NULL)
            (void)memset(target, 0, sizeof(*target));
        return true;
    }
    if (!fuss_target_lookup(cx, path, &found)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "select a valid workspace path");
        return false;
    }
    if (guard != FUSS_TARGET_ANY && !found.is_file) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "select a file to %s", action);
        return false;
    }
    if (guard == FUSS_TARGET_STAGED_FILE && found.status_known &&
        !found.staged) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "nothing staged to unstage");
        return false;
    }
    if (guard == FUSS_TARGET_DIRTY_FILE &&
        ((!found.staged && !found.unstaged) || found.untracked ||
         found.conflicted)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "nothing dirty to discard");
        return false;
    }
    if (target != NULL)
        *target = found;
    return true;
}

static char *fuss_join_root(const Ed *ed, const char *path)
{
    const char *root = yew_ws_root(ed);
    size_t root_len = fuss_cstr_len(root);
    size_t path_len = fuss_cstr_len(path);
    bool slash = root_len != 0U && root[root_len - 1U] != '/';
    char *joined;

    if (!fuss_safe_path(path, path_len) ||
        root_len > SIZE_MAX - path_len - (slash ? 2U : 1U))
        return NULL;
    joined = yew_xmalloc(root_len + path_len + (slash ? 2U : 1U));
    if (root_len != 0U)
        (void)memcpy(joined, root, root_len);
    if (slash)
        joined[root_len++] = '/';
    (void)memcpy(joined + root_len, path, path_len);
    joined[root_len + path_len] = '\0';
    return joined;
}

char *yew_fuss_selected_directory(CmdCtx *cx)
{
    FussMode *f;
    const FussItem *item;
    const FussNode *node;
    i32 row;

    if (fuss_require(cx, &f) != YEW_CMD_OK)
        return NULL;
    row = fuss_row(f);
    item = fuss_item(f, row);
    node = fuss_node(f, item);
    if (item == NULL || node == NULL || node->is_file ||
        !fuss_safe_path(item->path, item->path_len))
        return NULL;
    return fuss_join_root(cx->ed, item->path);
}

static CmdStatus fuss_spawn(Ed *ed, const char *verb_name,
                            char *const *argv, bool literal_paths,
                            const u8 *stdin_bytes, u64 stdin_len,
                            bool view)
{
    FussMode *f;
    const GitVerb *verb;
    GitReq req = {0};
    char error[160];
    u32 job;

    if (ed == NULL || ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    f = ed->fuss;
    if (f->pending_job != 0U) {
        yew_msg(ed, YEW_MSG_WARN, "a FUSS git command is still running");
        return YEW_CMD_ERR_STATE;
    }
    verb = yew_git_verb(verb_name);
    if (verb == NULL)
        return YEW_CMD_ERR_STATE;
    req.kind = YEW_GREQ_VERB;
    req.literal_paths = literal_paths;
    req.stdin_bytes = stdin_bytes;
    req.stdin_len = stdin_len;
    job = yew_git_spawn(ed, verb, argv, &req, error, sizeof(error));
    if (job == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                error[0] == '\0' ? "cannot start git command" : error);
        return YEW_CMD_ERR_IO;
    }
    f->pending_job = job;
    f->pending_view = view;
    f->seen_result_job = 0U;
    yew_msg(ed, YEW_MSG_INFO, "%s running", verb_name);
    return YEW_CMD_OK;
}

static void fuss_picker_clear(FussMode *f)
{
    u32 i;

    if (f == NULL)
        return;
    for (i = 0U; i < f->picker_count; i++) {
        free((char *)f->picker_items[i].label);
        free((char *)f->picker_items[i].detail);
        free(f->picker_values[i]);
    }
    free(f->picker_items);
    free(f->picker_values);
    free(f->picker_aux);
    free(f->picker_aux2);
    free(f->preview_value);
    bytebuf_free(&f->preview_bytes);
    bytebuf_init(&f->preview_bytes);
    f->picker_items = NULL;
    f->picker_values = NULL;
    f->picker_count = 0U;
    f->picker_aux = NULL;
    f->picker_aux2 = NULL;
    f->picker_action = FUSS_PICK_NONE;
    f->picker_alt = false;
    f->preview_job = 0U;
    f->preview_value = NULL;
    f->preview_ready = false;
}

static bool fuss_picker_add(FussMode *f, const char *label, size_t label_len,
                            const char *detail, size_t detail_len,
                            const char *value, size_t value_len)
{
    PickItem *items;
    char **values;
    u32 at;

    if (f == NULL || label == NULL || label_len == 0U || value == NULL ||
        value_len == 0U || f->picker_count >= (u32)INT32_MAX)
        return false;
    at = f->picker_count;
    items = yew_xreallocarray(f->picker_items, (size_t)at + 1U,
                              sizeof(*items));
    values = yew_xreallocarray(f->picker_values, (size_t)at + 1U,
                               sizeof(*values));
    f->picker_items = items;
    f->picker_values = values;
    (void)memset(&f->picker_items[at], 0, sizeof(f->picker_items[at]));
    f->picker_items[at].label = fuss_dup_bytes(label, label_len);
    if (detail != NULL && detail_len != 0U)
        f->picker_items[at].detail = fuss_dup_bytes(detail, detail_len);
    f->picker_items[at].payload = (i32)at;
    f->picker_values[at] = fuss_dup_bytes(value, value_len);
    if (f->picker_items[at].label == NULL || f->picker_values[at] == NULL) {
        free((char *)f->picker_items[at].label);
        free((char *)f->picker_items[at].detail);
        free(f->picker_values[at]);
        return false;
    }
    f->picker_count++;
    return true;
}

static const PickItem *fuss_picker_items(void *ctx, u32 *n)
{
    FussMode *f = ctx;

    if (n != NULL)
        *n = f == NULL ? 0U : f->picker_count;
    return f == NULL ? NULL : f->picker_items;
}

static CmdStatus fuss_pick_list(Ed *ed, FussPickAction action,
                                const char *verb_name, char *const *argv)
{
    CmdStatus status;

    if (ed == NULL || ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    fuss_picker_clear(ed->fuss);
    status = fuss_spawn(ed, verb_name, argv, false, NULL, 0U, false);
    if (status == YEW_CMD_OK)
        ed->fuss->pending_pick = action;
    return status;
}

static CmdStatus fuss_pick_branches(Ed *ed, FussPickAction action)
{
    char *argv_all[] = {
        (char *)"for-each-ref",
        (char *)"--format=%(refname:short)%1f%(upstream:short)%1f%(committerdate:unix)%00",
        (char *)"refs/heads", (char *)"refs/remotes", NULL
    };
    char *argv_local[] = {
        (char *)"for-each-ref",
        (char *)"--format=%(refname:short)%1f%(upstream:short)%1f%(committerdate:unix)%00",
        (char *)"refs/heads", NULL
    };

    return fuss_pick_list(ed, action, "branch-list",
                          action == FUSS_PICK_BRANCH_DELETE ? argv_local :
                                                             argv_all);
}

static CmdStatus fuss_pick_commits(Ed *ed, FussPickAction action,
                                   const char *branch)
{
    char *argv[12];
    size_t at = 0U;

    argv[at++] = (char *)"log";
    if (branch != NULL) {
        argv[at++] = (char *)branch;
        argv[at++] = (char *)"--not";
        argv[at++] = (char *)"HEAD";
    }
    argv[at++] = (char *)"-n";
    argv[at++] = (char *)"200";
    argv[at++] = (char *)"-z";
    argv[at++] = (char *)"--date-order";
    argv[at++] = (char *)"--pretty=format:%H%x1f%h%x1f%at%x1f%s";
    argv[at] = NULL;
    return fuss_pick_list(ed, action, "log", argv);
}

static CmdStatus fuss_pick_stashes(Ed *ed)
{
    char *argv[] = {
        (char *)"stash", (char *)"list", (char *)"-z",
        (char *)"--pretty=format:%gd%x1f%at%x1f%gs", NULL
    };

    return fuss_pick_list(ed, FUSS_PICK_STASH, "stash-pop", argv);
}

static CmdStatus fuss_pick_remotes(Ed *ed)
{
    char *argv[] = {(char *)"remote", (char *)"-v", NULL};

    return fuss_pick_list(ed, FUSS_PICK_REMOTE, "remote-list", argv);
}

static CmdStatus fuss_path_verb(CmdCtx *cx, const char *verb_name,
                                char *const *prefix, size_t prefix_n,
                                FussTargetGuard guard, bool view)
{
    char *path;
    char **argv;
    CmdStatus status;
    FussTarget target;
    size_t i;

    if (fuss_require(cx, NULL) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    path = fuss_selected_path(cx);
    if (path == NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "select a valid workspace path");
        return YEW_CMD_ERR_ARG;
    }
    if (!fuss_target_guard(cx, path, guard, verb_name, &target)) {
        free(path);
        return YEW_CMD_ERR_STATE;
    }
    argv = yew_xcalloc(prefix_n + 3U, sizeof(*argv));
    for (i = 0U; i < prefix_n; i++)
        argv[i] = prefix[i];
    if (strcmp(verb_name, "diff") == 0 && target.incoming && prefix_n > 1U)
        argv[1] = (char *)"HEAD...@{upstream}";
    argv[prefix_n] = (char *)"--";
    argv[prefix_n + 1U] = path;
    status = fuss_spawn(cx->ed, verb_name, argv, true, NULL, 0U, view);
    free(argv);
    free(path);
    return status;
}

static void fuss_prompt_clear(FussMode *f)
{
    if (f == NULL)
        return;
    f->prompt_action = FUSS_PROMPT_NONE;
    free(f->prompt_path);
    f->prompt_path = NULL;
    f->prompt_untracked = false;
}

static bool fuss_text_is(const u8 *text, size_t len, const char *literal,
                         size_t literal_len)
{
    return text != NULL && len == literal_len &&
           memcmp(text, literal, literal_len) == 0;
}

static CmdStatus fuss_commit_bytes(Ed *ed, const u8 *text, size_t len,
                                   bool amend, bool *empty_out)
{
    FussMode *f;
    const GitSnapshot *snap;
    Bytebuf clean;
    u8 comment = (u8)'#';
    char *argv_commit[] = {
        (char *)"commit", (char *)"-F", (char *)"-",
        (char *)"--cleanup=verbatim", NULL
    };
    char *argv_amend[] = {
        (char *)"commit", (char *)"--amend", (char *)"-F",
        (char *)"-", (char *)"--cleanup=verbatim", NULL
    };
    CmdStatus status;

    if (empty_out != NULL)
        *empty_out = false;
    f = ed == NULL ? NULL : ed->fuss;
    snap = yew_git_snapshot(ed);
    if (!fuss_commit_guard(ed, snap))
        return YEW_CMD_ERR_STATE;
    if (f != NULL && f->commit_editing && f->commit_comment != 0U) {
        comment = f->commit_comment;
    } else if (snap != NULL &&
               !yew_fuss_commit_select_comment(
                   text, len, (const u8 *)snap->comment_char,
                   snap->comment_char_len, &comment)) {
        yew_msg(ed, YEW_MSG_ERROR,
                "cannot resolve core.commentChar for commit message");
        return YEW_CMD_ERR_STATE;
    }
    bytebuf_init(&clean);
    if (!yew_fuss_commit_cleanup(&clean, text, len, comment) ||
        yew_fuss_commit_empty(clean.data, clean.len)) {
        bytebuf_free(&clean);
        if (empty_out != NULL)
            *empty_out = true;
        yew_msg(ed, YEW_MSG_ERROR,
                "aborting commit due to empty commit message");
        return YEW_CMD_ERR_STATE;
    }
    status = fuss_spawn(ed, amend ? "commit-amend" : "commit",
                        amend ? argv_amend : argv_commit, false,
                        clean.data, (u64)clean.len, false);
    bytebuf_free(&clean);
    return status;
}

static void fuss_append_lit(Bytebuf *out, const char *text, size_t len)
{
    bytebuf_append(out, (const u8 *)text, len);
}

static bool fuss_buffer_bytes(const Buffer *buffer, Bytebuf *out)
{
    TextIter iter;
    const u8 *chunk;
    u64 len;

    if (buffer == NULL || buffer->tb == NULL || out == NULL)
        return false;
    bytebuf_init(out);
    if (yew_textbuf_len(buffer->tb) == 0U)
        return true;
    if (!yew_textiter_begin(&iter, buffer->tb, BYTEOFF(0U)))
        return false;
    do {
        if (!yew_textiter_chunk(&iter, buffer->tb, &chunk, &len)) {
            bytebuf_free(out);
            return false;
        }
        if (len != 0U) {
            if (len > (u64)SIZE_MAX) {
                bytebuf_free(out);
                return false;
            }
            bytebuf_append(out, chunk, (size_t)len);
        }
    } while (yew_textiter_advance(&iter, buffer->tb));
    return true;
}

static void fuss_commit_resume(Ed *ed, Buffer *commit)
{
    FussMode *f = ed->fuss;

    if (f == NULL)
        return;
    f->commit_editing = false;
    f->commit_buffer_id = 0U;
    f->commit_amend = false;
    f->commit_comment = 0U;
    fuss_viewer_close(ed);
    if (commit != NULL && commit != &ed->buffer)
        yew_ws_scratch_drop(ed, commit);
    if (yew_mode_enter(ed, YEW_MODE_F) != YEW_CMD_OK)
        (void)yew_mode_enter(ed, YEW_MODE_L);
}

static u32 fuss_conflict_count(const GitSnapshot *snap)
{
    size_t i;
    u32 count = 0U;

    if (snap == NULL)
        return 0U;
    for (i = 0U; i < snap->entries.len; i++) {
        if (snap->entries.data[i].conflicted && count != UINT32_MAX)
            count++;
    }
    return count;
}

static bool fuss_commit_guard(Ed *ed, const GitSnapshot *snap)
{
    u32 conflicts = fuss_conflict_count(snap);

    if (conflicts == 0U)
        return true;
    yew_msg(ed, YEW_MSG_ERROR,
            "resolve conflicts before committing (%u files)",
            (unsigned int)conflicts);
    return false;
}

static void fuss_comment_line(Bytebuf *out, u8 comment, const char *text,
                              size_t len)
{
    bytebuf_push_u8(out, comment);
    if (len != 0U) {
        bytebuf_push_u8(out, (u8)' ');
        fuss_append_lit(out, text, len);
    }
    bytebuf_push_u8(out, (u8)'\n');
}

static void fuss_comment_prefix(Bytebuf *out, u8 comment)
{
    bytebuf_push_u8(out, comment);
    bytebuf_push_u8(out, (u8)' ');
}

static void fuss_template_path(Bytebuf *out, const char *path, u32 len)
{
    u32 i;

    for (i = 0U; i < len; i++) {
        u8 byte = (u8)path[i];

        if (byte == (u8)'\n')
            fuss_append_lit(out, "\\n", sizeof("\\n") - 1U);
        else if (byte == (u8)'\r')
            fuss_append_lit(out, "\\r", sizeof("\\r") - 1U);
        else if (byte == (u8)'\t')
            fuss_append_lit(out, "\\t", sizeof("\\t") - 1U);
        else
            bytebuf_push_u8(out, byte);
    }
}

static const char *fuss_change_label(const GitEntry *entry, bool staged)
{
    char code = staged ? entry->x : entry->y;

    if (entry->kind == GIT_E_RENAME || code == 'R' || code == 'C')
        return "renamed";
    if (code == 'A' || entry->untracked)
        return "new file";
    if (code == 'D')
        return "deleted";
    if (code == 'T')
        return "typechange";
    return "modified";
}

static void fuss_status_line(Bytebuf *out, u8 comment,
                             const GitEntry *entry, bool staged)
{
    const char *label = fuss_change_label(entry, staged);

    bytebuf_push_u8(out, comment);
    bytebuf_push_u8(out, (u8)'\t');
    fuss_append_lit(out, label, fuss_cstr_len(label));
    fuss_append_lit(out, ":   ", sizeof(":   ") - 1U);
    fuss_template_path(out, entry->path, entry->path_len);
    bytebuf_push_u8(out, (u8)'\n');
}

static bool fuss_has_status(const GitSnapshot *snap, u8 kind)
{
    size_t i;

    for (i = 0U; snap != NULL && i < snap->entries.len; i++) {
        const GitEntry *entry = &snap->entries.data[i];

        if ((kind == 0U && entry->staged) ||
            (kind == 1U && entry->unstaged && !entry->untracked) ||
            (kind == 2U && entry->untracked))
            return true;
    }
    return false;
}

static bool fuss_commit_template(Bytebuf *text, const GitSnapshot *snap,
                                 const u8 *prefill, size_t prefill_len,
                                 bool amend, u8 *comment_out)
{
    const u8 *setting = (const u8 *)"#";
    size_t setting_len = 1U;
    u8 comment = (u8)'#';
    size_t i;

    if (text == NULL || comment_out == NULL ||
        (prefill == NULL && prefill_len != 0U))
        return false;
    if (snap != NULL && snap->comment_char != NULL) {
        setting = (const u8 *)snap->comment_char;
        setting_len = snap->comment_char_len;
    }
    if (!yew_fuss_commit_select_comment(prefill, prefill_len, setting,
                                        setting_len, &comment))
        return false;
    bytebuf_init(text);
    if (prefill_len != 0U) {
        bytebuf_append(text, prefill, prefill_len);
        if (prefill[prefill_len - 1U] != (u8)'\n')
            bytebuf_push_u8(text, (u8)'\n');
        bytebuf_push_u8(text, (u8)'\n');
    } else {
        bytebuf_push_u8(text, (u8)'\n');
    }
    fuss_comment_line(text, comment,
        "Please enter the commit message for your changes. Lines starting",
        sizeof("Please enter the commit message for your changes. Lines starting") - 1U);
    fuss_comment_prefix(text, comment);
    fuss_append_lit(text, "with '", sizeof("with '") - 1U);
    bytebuf_push_u8(text, comment);
    fuss_append_lit(text,
                    "' will be ignored, and an empty message aborts the commit.\n",
                    sizeof("' will be ignored, and an empty message aborts the commit.\n") - 1U);
    fuss_comment_line(text, comment, NULL, 0U);
    if (snap != NULL && snap->branch != NULL) {
        fuss_comment_prefix(text, comment);
        fuss_append_lit(text, "On branch ", sizeof("On branch ") - 1U);
        fuss_append_lit(text, snap->branch, fuss_cstr_len(snap->branch));
        bytebuf_push_u8(text, (u8)'\n');
    }
    if (snap != NULL && snap->upstream != NULL) {
        fuss_comment_prefix(text, comment);
        if (snap->ahead > 0 && snap->behind > 0)
            bytebuf_printf(text,
                           "Your branch and '%s' have diverged (%d ahead, %d behind).\n",
                           snap->upstream, snap->ahead, snap->behind);
        else if (snap->ahead > 0)
            bytebuf_printf(text, "Your branch is ahead of '%s' by %d commits.\n",
                           snap->upstream, snap->ahead);
        else if (snap->behind > 0)
            bytebuf_printf(text, "Your branch is behind '%s' by %d commits.\n",
                           snap->upstream, snap->behind);
        else
            bytebuf_printf(text, "Your branch is up to date with '%s'.\n",
                           snap->upstream);
        if (amend && snap->ahead == 0) {
            fuss_comment_prefix(text, comment);
            bytebuf_printf(text, "This commit has been pushed to %s.\n",
                           snap->upstream);
        }
    }
    fuss_comment_line(text, comment, NULL, 0U);
    if (fuss_has_status(snap, 0U)) {
        fuss_comment_line(text, comment, "Changes to be committed:",
                          sizeof("Changes to be committed:") - 1U);
        for (i = 0U; i < snap->entries.len; i++) {
            const GitEntry *entry = &snap->entries.data[i];

            if (entry->staged)
                fuss_status_line(text, comment, entry, true);
        }
        fuss_comment_line(text, comment, NULL, 0U);
    }
    if (fuss_has_status(snap, 1U)) {
        fuss_comment_line(text, comment, "Changes not staged for commit:",
                          sizeof("Changes not staged for commit:") - 1U);
        for (i = 0U; i < snap->entries.len; i++) {
            const GitEntry *entry = &snap->entries.data[i];

            if (entry->unstaged && !entry->untracked)
                fuss_status_line(text, comment, entry, false);
        }
        fuss_comment_line(text, comment, NULL, 0U);
    }
    if (fuss_has_status(snap, 2U)) {
        fuss_comment_line(text, comment, "Untracked files:",
                          sizeof("Untracked files:") - 1U);
        for (i = 0U; i < snap->entries.len; i++) {
            const GitEntry *entry = &snap->entries.data[i];

            if (entry->untracked)
                fuss_status_line(text, comment, entry, false);
        }
        fuss_comment_line(text, comment, NULL, 0U);
    }
    fuss_comment_line(text, comment,
        "Save this buffer to commit; close it without saving to abort.",
        sizeof("Save this buffer to commit; close it without saving to abort.") - 1U);
    *comment_out = comment;
    return true;
}

static CmdStatus fuss_commit_begin(Ed *ed, bool amend, const u8 *prefill,
                                   size_t prefill_len)
{
    FussMode *f;
    Buffer *buffer;
    const GitSnapshot *snap;
    Bytebuf text;
    u8 comment;

    if (ed == NULL || ed->fuss == NULL || !ed->fuss->active)
        return YEW_CMD_ERR_STATE;
    f = ed->fuss;
    snap = yew_git_snapshot(ed);
    if (!fuss_commit_guard(ed, snap))
        return YEW_CMD_ERR_STATE;
    if (!fuss_commit_template(&text, snap, prefill, prefill_len, amend,
                              &comment)) {
        yew_msg(ed, YEW_MSG_ERROR,
                "cannot resolve core.commentChar for commit template");
        return YEW_CMD_ERR_STATE;
    }
    buffer = yew_ws_scratch_find(ed, "*commit*");
    if (buffer == NULL)
        buffer = yew_ws_scratch_new(ed, "*commit*", 0U);
    if (buffer == NULL || buffer->tb == NULL) {
        bytebuf_free(&text);
        return YEW_CMD_ERR_IO;
    }
    if (!fuss_replace_buffer(ed, buffer, text.data, (u64)text.len)) {
        bytebuf_free(&text);
        return YEW_CMD_ERR_IO;
    }
    bytebuf_free(&text);
    yew_undo_free(buffer->undo);
    buffer->undo = yew_undo_new(buffer->tb);
    yew_undo_mark_saved(buffer->undo);
    f->commit_editing = true;
    f->commit_buffer_id = buffer->id;
    f->commit_amend = amend;
    f->commit_comment = comment;
    if (!fuss_show_buffer(ed, buffer)) {
        f->commit_editing = false;
        f->commit_buffer_id = 0U;
        f->commit_amend = false;
        f->commit_comment = 0U;
        return YEW_CMD_ERR_STATE;
    }
    if (yew_mode_enter(ed, YEW_MODE_I) != YEW_CMD_OK) {
        f->commit_editing = false;
        f->commit_buffer_id = 0U;
        f->commit_amend = false;
        f->commit_comment = 0U;
        fuss_viewer_close(ed);
        return YEW_CMD_ERR_STATE;
    }
    yew_msg(ed, YEW_MSG_INFO,
            "edit *commit*, then save to commit or close to abort");
    return YEW_CMD_OK;
}

static char *fuss_dir_path(const char *dir, const char *name)
{
    size_t dir_len;
    size_t name_len;
    bool slash;
    char *path;

    if (dir == NULL || name == NULL)
        return NULL;
    dir_len = fuss_cstr_len(dir);
    name_len = fuss_cstr_len(name);
    slash = dir_len != 0U && dir[dir_len - 1U] != '/';
    if (dir_len > SIZE_MAX - name_len - (slash ? 2U : 1U))
        return NULL;
    path = yew_xmalloc(dir_len + name_len + (slash ? 2U : 1U));
    if (dir_len != 0U)
        (void)memcpy(path, dir, dir_len);
    if (slash)
        path[dir_len++] = '/';
    (void)memcpy(path + dir_len, name, name_len + 1U);
    return path;
}

static bool fuss_read_small_file(const char *path, Bytebuf *out)
{
    enum { FUSS_COMMIT_FILE_MAX = 1024 * 1024 };
    int fd;
    u8 bytes[4096];

    if (path == NULL || out == NULL)
        return false;
    bytebuf_init(out);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    for (;;) {
        ssize_t got = read(fd, bytes, sizeof(bytes));

        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            bytebuf_free(out);
            return false;
        }
        if (out->len > FUSS_COMMIT_FILE_MAX - (size_t)got) {
            (void)close(fd);
            bytebuf_free(out);
            return false;
        }
        bytebuf_append(out, bytes, (size_t)got);
    }
    if (close(fd) != 0) {
        bytebuf_free(out);
        return false;
    }
    return true;
}

static bool fuss_positive_int(const Bytebuf *bytes, i32 *out)
{
    size_t at = 0U;
    i32 value = 0;
    bool any = false;

    if (bytes == NULL || out == NULL)
        return false;
    while (at < bytes->len &&
           (bytes->data[at] == (u8)' ' || bytes->data[at] == (u8)'\t' ||
            bytes->data[at] == (u8)'\r' || bytes->data[at] == (u8)'\n'))
        at++;
    while (at < bytes->len && bytes->data[at] >= (u8)'0' &&
           bytes->data[at] <= (u8)'9') {
        i32 digit = (i32)(bytes->data[at] - (u8)'0');

        if (value > (INT_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
        any = true;
        at++;
    }
    while (at < bytes->len &&
           (bytes->data[at] == (u8)' ' || bytes->data[at] == (u8)'\t' ||
            bytes->data[at] == (u8)'\r' || bytes->data[at] == (u8)'\n'))
        at++;
    if (!any || at != bytes->len || value <= 0)
        return false;
    *out = value;
    return true;
}

static void fuss_rebase_progress_update(Ed *ed, const GitSnapshot *snap)
{
    const GitRepo *repo;
    const char *step_name;
    const char *total_name;
    char *step_path;
    char *total_path;
    Bytebuf step;
    Bytebuf total;
    bool step_ok;
    bool total_ok;

    if (ed == NULL || ed->fuss == NULL)
        return;
    ed->fuss->rebase_step = 0;
    ed->fuss->rebase_total = 0;
    if (snap == NULL || snap->state != YEW_GIT_MID_REBASE)
        return;
    repo = yew_git_repo_cached(ed);
    if (repo == NULL || repo->git_dir == NULL)
        return;
    step_name = "rebase-merge/msgnum";
    total_name = "rebase-merge/end";
    step_path = fuss_dir_path(repo->git_dir, step_name);
    total_path = fuss_dir_path(repo->git_dir, total_name);
    step_ok = step_path != NULL && fuss_read_small_file(step_path, &step);
    total_ok = total_path != NULL && fuss_read_small_file(total_path, &total);
    if (!step_ok || !total_ok) {
        if (step_ok)
            bytebuf_free(&step);
        if (total_ok)
            bytebuf_free(&total);
        free(step_path);
        free(total_path);
        step_name = "rebase-apply/next";
        total_name = "rebase-apply/last";
        step_path = fuss_dir_path(repo->git_dir, step_name);
        total_path = fuss_dir_path(repo->git_dir, total_name);
        step_ok = step_path != NULL && fuss_read_small_file(step_path, &step);
        total_ok = total_path != NULL && fuss_read_small_file(total_path,
                                                               &total);
    }
    if (step_ok && total_ok &&
        fuss_positive_int(&step, &ed->fuss->rebase_step) &&
        fuss_positive_int(&total, &ed->fuss->rebase_total) &&
        ed->fuss->rebase_step <= ed->fuss->rebase_total) {
        /* Values published above. */
    } else {
        ed->fuss->rebase_step = 0;
        ed->fuss->rebase_total = 0;
    }
    if (step_ok)
        bytebuf_free(&step);
    if (total_ok)
        bytebuf_free(&total);
    free(step_path);
    free(total_path);
}

static CmdStatus fuss_commit_open(CmdCtx *cx, bool amend)
{
    FussMode *f;
    const GitSnapshot *snap;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    snap = yew_git_snapshot(cx->ed);
    if (!fuss_commit_guard(cx->ed, snap))
        return YEW_CMD_ERR_STATE;
    if (amend) {
        char *argv[] = {
            (char *)"log", (char *)"-1", (char *)"--pretty=%B", NULL
        };
        CmdStatus status = fuss_spawn(cx->ed, "log", argv, false, NULL,
                                      0U, false);

        if (status == YEW_CMD_OK)
            f->pending_pick = FUSS_PICK_COMMIT_AMEND;
        return status;
    }
    if (snap != NULL && snap->state == YEW_GIT_MID_MERGE) {
        const GitRepo *repo = yew_git_repo_cached(cx->ed);
        char *path = repo == NULL ? NULL :
                     fuss_dir_path(repo->git_dir, "MERGE_MSG");
        Bytebuf message;
        CmdStatus status;

        if (path == NULL || !fuss_read_small_file(path, &message)) {
            free(path);
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "cannot read the pending merge message");
            return YEW_CMD_ERR_IO;
        }
        free(path);
        status = fuss_commit_begin(cx->ed, false, message.data,
                                   message.len);
        bytebuf_free(&message);
        return status;
    }
    return fuss_commit_begin(cx->ed, false, NULL, 0U);
}

CmdStatus yew_fuss_commit_save(Ed *ed, Buffer *buffer, bool *handled)
{
    FussMode *f;
    Bytebuf message;
    CmdStatus status;
    bool empty;

    if (handled != NULL)
        *handled = false;
    if (ed == NULL || ed->fuss == NULL || buffer == NULL)
        return YEW_CMD_ERR_STATE;
    f = ed->fuss;
    if (!f->commit_editing || buffer->id != f->commit_buffer_id)
        return YEW_CMD_ERR_STATE;
    if (handled != NULL)
        *handled = true;
    if (!fuss_buffer_bytes(buffer, &message))
        return YEW_CMD_ERR_IO;
    status = fuss_commit_bytes(ed, message.data, message.len,
                               f->commit_amend, &empty);
    bytebuf_free(&message);
    if (status != YEW_CMD_OK && !empty)
        return status;
    if (empty) {
        fuss_commit_resume(ed, buffer);
        return status;
    }
    yew_undo_mark_saved(buffer->undo);
    fuss_commit_resume(ed, buffer);
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_commit_close(Ed *ed, Buffer *buffer, bool *handled)
{
    FussMode *f;

    if (handled != NULL)
        *handled = false;
    if (ed == NULL || ed->fuss == NULL || buffer == NULL)
        return YEW_CMD_ERR_STATE;
    f = ed->fuss;
    if (!f->commit_editing || buffer->id != f->commit_buffer_id)
        return YEW_CMD_ERR_STATE;
    if (handled != NULL)
        *handled = true;
    yew_msg(ed, YEW_MSG_INFO, "commit aborted");
    fuss_commit_resume(ed, buffer);
    return YEW_CMD_OK;
}

static CmdStatus fuss_rebase_prepare(Ed *ed, const char *base)
{
    FussMode *f;
    size_t base_len;
    char *range;
    char *argv[6];
    CmdStatus status;

    if (ed == NULL || ed->fuss == NULL || base == NULL || base[0] == '\0')
        return YEW_CMD_ERR_ARG;
    f = ed->fuss;
    base_len = fuss_cstr_len(base);
    if (base[0] == '-' || base_len > SIZE_MAX - sizeof("..HEAD")) {
        yew_msg(ed, YEW_MSG_ERROR, "invalid rebase base");
        return YEW_CMD_ERR_ARG;
    }
    range = yew_xmalloc(base_len + sizeof("..HEAD"));
    (void)memcpy(range, base, base_len);
    (void)memcpy(range + base_len, "..HEAD", sizeof("..HEAD"));
    fuss_prompt_clear(f);
    f->prompt_path = fuss_dup_bytes(base, base_len);
    if (f->prompt_path == NULL) {
        free(range);
        return YEW_CMD_ERR_ARG;
    }
    argv[0] = (char *)"log";
    argv[1] = range;
    argv[2] = (char *)"--date-order";
    argv[3] = (char *)"--pretty=format:%h%x1f%at%x1f%an%x1f%s";
    argv[4] = NULL;
    status = fuss_spawn(ed, "log", argv, false, NULL, 0U, false);
    free(range);
    if (status == YEW_CMD_OK) {
        f->pending_pick = FUSS_PICK_REBASE_CONFIRM;
        yew_msg(ed, YEW_MSG_INFO, "loading rebase range");
    } else {
        fuss_prompt_clear(f);
    }
    return status;
}

static CmdStatus fuss_rebase_sync(Ed *ed, const char *operation,
                                  const char *base)
{
    const char *self = yew_job_self_exe();
    const char *env[6];
    char *sequence_env;
    char *editor_env;
    size_t self_len;
    char *argv[7];
    YewJobSpec spec = {0};
    YewJobWait wait = {0};
    char error[192];
    size_t at = 0U;
    bool ran;

    if (self == NULL || self[0] == '\0') {
        yew_msg(ed, YEW_MSG_ERROR,
                "rebase handover failed: cannot resolve yew executable");
        return YEW_CMD_ERR_IO;
    }
    self_len = fuss_cstr_len(self);
    sequence_env = yew_xmalloc(sizeof("GIT_SEQUENCE_EDITOR=") + self_len);
    editor_env = yew_xmalloc(sizeof("GIT_EDITOR=") + self_len);
    (void)memcpy(sequence_env, "GIT_SEQUENCE_EDITOR=",
                 sizeof("GIT_SEQUENCE_EDITOR=") - 1U);
    (void)memcpy(sequence_env + sizeof("GIT_SEQUENCE_EDITOR=") - 1U,
                 self, self_len + 1U);
    (void)memcpy(editor_env, "GIT_EDITOR=", sizeof("GIT_EDITOR=") - 1U);
    (void)memcpy(editor_env + sizeof("GIT_EDITOR=") - 1U, self,
                 self_len + 1U);
    env[0] = sequence_env;
    env[1] = editor_env;
    env[2] = "GIT_PAGER=cat";
    env[3] = "PAGER=cat";
    env[4] = "LC_ALL=C";
    env[5] = NULL;

    argv[at++] = (char *)"git";
    argv[at++] = (char *)"--no-pager";
    argv[at++] = (char *)"rebase";
    argv[at++] = (char *)operation;
    if (base != NULL)
        argv[at++] = (char *)base;
    argv[at] = NULL;
    spec.argv = argv;
    spec.cwd = yew_ws_root(ed);
    spec.sink = YEW_SINK_DISCARD;
    spec.env_set = env;
    spec.inherit_tty = true;
    ran = yew_job_run_sync(ed, &spec, &wait, error, sizeof(error));
    free(sequence_env);
    free(editor_env);
    if (!ran) {
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                error[0] == '\0' ? "rebase handover failed" : error);
        return YEW_CMD_ERR_IO;
    }
    yew_git_invalidate(ed);
    (void)yew_git_refresh(ed, true);
    if (wait.state != YEW_JOB_EXITED || wait.exit_code != 0) {
        if (wait.state == YEW_JOB_SIGNALED)
            yew_msg(ed, YEW_MSG_ERROR, "rebase stopped by signal %d",
                    wait.termsig);
        else
            yew_msg(ed, YEW_MSG_ERROR, "rebase exited with status %d",
                    wait.exit_code);
        return YEW_CMD_ERR_STATE;
    }
    yew_msg(ed, YEW_MSG_INFO, "rebase complete");
    return YEW_CMD_OK;
}

static void fuss_prompt_done(Ed *ed, bool accepted, const u8 *text,
                             size_t len, void *ctx)
{
    FussMode *f = ctx;
    FussPromptAction action;
    char *value = NULL;

    if (ed == NULL || f == NULL)
        return;
    action = f->prompt_action;
    if (accepted && len != 0U && text != NULL &&
        memchr(text, '\0', len) == NULL)
        value = fuss_dup_bytes((const char *)text, len);
    switch (action) {
    case FUSS_PROMPT_BRANCH_CREATE:
        if (value != NULL) {
            if (value[0] == '-') {
                yew_msg(ed, YEW_MSG_ERROR,
                        "branch name must not begin with '-'");
            } else {
                char *argv[] = {
                    (char *)"switch", (char *)"-c", value, NULL
                };

                (void)fuss_spawn(ed, "switch-create", argv, false, NULL,
                                 0U, false);
            }
        }
        break;
    case FUSS_PROMPT_BRANCH_DELETE:
        if (fuss_text_is(text, len, "delete", sizeof("delete") - 1U) &&
            f->prompt_path != NULL) {
            char *argv[] = {
                (char *)"branch", (char *)"-D", (char *)"--",
                f->prompt_path, NULL
            };
            (void)fuss_spawn(ed, "branch-delete", argv, false, NULL, 0U,
                             false);
        }
        break;
    case FUSS_PROMPT_STASH_PUSH:
        if (value != NULL) {
            char *argv[] = {
                (char *)"stash", (char *)"push", (char *)"-m", value, NULL
            };

            /* `git stash push` has no -F/stdin message form.  Direct argv
             * preserves every non-NUL byte without a shell or formatting. */
            (void)fuss_spawn(ed, "stash-push", argv, false, NULL, 0U,
                             false);
        }
        break;
    case FUSS_PROMPT_TAG:
        if (value != NULL) {
            char *message = strchr(value, '\n');

            if (message == NULL) {
                char *argv[] = {(char *)"tag", (char *)"--", value, NULL};
                (void)fuss_spawn(ed, "tag", argv, false, NULL, 0U, false);
            } else {
                char *argv[] = {
                    (char *)"tag", (char *)"-a", (char *)"-F",
                    (char *)"-", (char *)"--", value, NULL
                };
                *message++ = '\0';
                if (*value != '\0')
                    (void)fuss_spawn(ed, "tag", argv, false,
                                     (const u8 *)message,
                                     (u64)fuss_cstr_len(message), false);
            }
        }
        break;
    case FUSS_PROMPT_DISCARD:
        if (fuss_text_is(text, len, "discard", sizeof("discard") - 1U) &&
            f->prompt_path != NULL) {
            char *argv[] = {
                (char *)"restore", (char *)"--source=HEAD",
                (char *)"--staged", (char *)"--worktree", (char *)"--",
                f->prompt_path, NULL
            };
            (void)fuss_spawn(ed, "discard", argv, true, NULL, 0U, false);
        }
        break;
    case FUSS_PROMPT_DELETE:
        if (fuss_text_is(text, len, "delete", sizeof("delete") - 1U) &&
            f->prompt_path != NULL) {
            if (f->prompt_untracked) {
                char *absolute = fuss_join_root(ed, f->prompt_path);

                if (absolute == NULL) {
                    yew_msg(ed, YEW_MSG_ERROR, "cannot delete %s: invalid path",
                            f->prompt_path);
                } else if (unlink(absolute) != 0) {
                    yew_msg(ed, YEW_MSG_ERROR, "cannot delete %s: %s",
                            f->prompt_path, strerror(errno));
                } else {
                    yew_msg(ed, YEW_MSG_INFO, "deleted %s", f->prompt_path);
                    yew_git_invalidate(ed);
                    (void)yew_git_refresh(ed, true);
                }
                free(absolute);
            } else {
                char *argv[] = {
                    (char *)"rm", (char *)"-f", (char *)"--",
                    f->prompt_path, NULL
                };
                (void)fuss_spawn(ed, "rm", argv, true, NULL, 0U, false);
            }
        }
        break;
    case FUSS_PROMPT_RENAME:
        if (value != NULL && f->prompt_path != NULL &&
            fuss_safe_path(value, fuss_cstr_len(value))) {
            if (f->prompt_untracked) {
                char *old_path = fuss_join_root(ed, f->prompt_path);
                char *new_path = fuss_join_root(ed, value);

                if (old_path == NULL || new_path == NULL) {
                    yew_msg(ed, YEW_MSG_ERROR, "cannot rename %s: invalid path",
                            f->prompt_path);
                } else if (rename(old_path, new_path) != 0) {
                    yew_msg(ed, YEW_MSG_ERROR, "cannot rename %s: %s",
                            f->prompt_path, strerror(errno));
                } else {
                    yew_git_invalidate(ed);
                    (void)yew_git_refresh(ed, true);
                }
                free(old_path);
                free(new_path);
            } else {
                char *argv[] = {
                    (char *)"mv", (char *)"--", f->prompt_path, value, NULL
                };
                (void)fuss_spawn(ed, "mv", argv, true, NULL, 0U, false);
            }
        }
        break;
    case FUSS_PROMPT_PUSH_FORCE:
        if (fuss_text_is(text, len, "force", sizeof("force") - 1U)) {
            char *argv[] = {
                (char *)"push", (char *)"--force-with-lease", NULL
            };
            (void)fuss_spawn(ed, "push", argv, false, NULL, 0U, false);
        }
        break;
    case FUSS_PROMPT_REBASE:
        if (fuss_text_is(text, len, "rebase", sizeof("rebase") - 1U) &&
            f->prompt_path != NULL)
            (void)fuss_rebase_sync(ed, "-i", f->prompt_path);
        break;
    case FUSS_PROMPT_COMMIT:
    case FUSS_PROMPT_COMMIT_AMEND:
        if (accepted)
            (void)fuss_commit_bytes(ed, text, len,
                                    action == FUSS_PROMPT_COMMIT_AMEND,
                                    NULL);
        break;
    case FUSS_PROMPT_NONE:
        break;
    }
    if (!accepted && action != FUSS_PROMPT_NONE)
        yew_msg(ed, YEW_MSG_INFO, "git command cancelled");
    free(value);
    fuss_prompt_clear(f);
}

static CmdStatus fuss_prompt(CmdCtx *cx, FussPromptAction action,
                             const char *seed, const char *path,
                             const char *hint)
{
    FussMode *f;

    if (fuss_require(cx, &f) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    fuss_prompt_clear(f);
    f->prompt_action = action;
    if (path != NULL) {
        FussTarget target;

        f->prompt_path = fuss_dup_bytes(path, fuss_cstr_len(path));
        if (fuss_target_lookup(cx, path, &target))
            f->prompt_untracked = target.untracked;
    }
    yew_cmdline_open_input(cx->ed, seed, fuss_prompt_done, f);
    if (!cx->ed->cmdline.active) {
        fuss_prompt_clear(f);
        return YEW_CMD_ERR_STATE;
    }
    if (action == FUSS_PROMPT_DISCARD && path != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN,
                "discard %s — type 'discard' to confirm — use hunk "
                "discard for an undoable version", path);
    else if (action == FUSS_PROMPT_DELETE && path != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN,
                "delete %s — type 'delete' to confirm", path);
    else if (hint != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN, "%s", hint);
    return YEW_CMD_OK;
}

static bool fuss_picker_action(Ed *ed, void *ctx, i32 payload,
                               const Key *key)
{
    FussMode *f = ctx;

    (void)payload;
    if (ed == NULL || f == NULL || key == NULL ||
        f->picker_action != FUSS_PICK_STASH)
        return false;
    if ((key->mods & YEW_MOD_CTRL) != 0U &&
        (key->code == (u32)'v' || key->code == (u32)'V')) {
        f->picker_alt = true;
        (void)yew_picker_accept(ed);
        return true;
    }
    return false;
}

static bool fuss_picker_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    FussMode *f = ctx;
    FussPickAction action;
    char *value;
    char *aux;
    bool alt;

    (void)how;
    if (ed == NULL || f == NULL || payload < 0 ||
        (u32)payload >= f->picker_count)
        return false;
    action = f->picker_action;
    value = fuss_dup_bytes(f->picker_values[payload],
                           fuss_cstr_len(f->picker_values[payload]));
    aux = f->picker_aux == NULL ? NULL :
          fuss_dup_bytes(f->picker_aux, fuss_cstr_len(f->picker_aux));
    alt = f->picker_alt;
    fuss_picker_clear(f);
    if (value == NULL) {
        free(aux);
        return false;
    }
    switch (action) {
    case FUSS_PICK_BRANCH_SWITCH: {
        char *argv[] = {(char *)"switch", (char *)"--", value, NULL};
        (void)fuss_spawn(ed, "switch", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_BRANCH_DELETE:
        free(f->prompt_path);
        f->prompt_path = fuss_dup_bytes(value, fuss_cstr_len(value));
        if (f->prompt_path != NULL) {
            char *argv[] = {
                (char *)"branch", (char *)"-d", (char *)"--",
                f->prompt_path, NULL
            };
            if (fuss_spawn(ed, "branch-delete", argv, false, NULL, 0U,
                           false) != YEW_CMD_OK) {
                free(f->prompt_path);
                f->prompt_path = NULL;
            }
        }
        break;
    case FUSS_PICK_MERGE: {
        char *argv[] = {
            (char *)"merge", (char *)"--no-edit", (char *)"--", value, NULL
        };
        (void)fuss_spawn(ed, "merge", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_RESET_COMMIT: {
        PickerSpec spec = {0};

        f->picker_aux = value;
        value = NULL;
        (void)fuss_picker_add(f, "soft", sizeof("soft") - 1U, NULL, 0U,
                              "--soft", sizeof("--soft") - 1U);
        (void)fuss_picker_add(f, "mixed", sizeof("mixed") - 1U, NULL, 0U,
                              "--mixed", sizeof("--mixed") - 1U);
        (void)fuss_picker_add(f, "hard", sizeof("hard") - 1U, NULL, 0U,
                              "--hard", sizeof("--hard") - 1U);
        f->picker_action = FUSS_PICK_RESET_MODE;
        spec.title = "reset mode";
        spec.items = fuss_picker_items;
        spec.accept = fuss_picker_accept;
        spec.ctx = f;
        yew_picker_open(ed, &spec);
        break;
    }
    case FUSS_PICK_RESET_MODE:
        if (aux != NULL) {
            char *argv[] = {(char *)"reset", value, aux, NULL};
            (void)fuss_spawn(ed, "reset", argv, false, NULL, 0U, false);
        }
        break;
    case FUSS_PICK_REBASE:
        (void)fuss_rebase_prepare(ed, value);
        break;
    case FUSS_PICK_CHERRY_BRANCH:
        (void)fuss_pick_commits(ed, FUSS_PICK_CHERRY_COMMIT, value);
        break;
    case FUSS_PICK_CHERRY_COMMIT: {
        char *argv[] = {(char *)"cherry-pick", value, NULL};
        (void)fuss_spawn(ed, "cherry-pick", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_REVERT: {
        char *argv[] = {(char *)"revert", (char *)"--no-edit", value, NULL};
        (void)fuss_spawn(ed, "revert", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_STASH: {
        char *argv[] = {
            (char *)"stash", alt ? (char *)"apply" : (char *)"pop", value,
            NULL
        };
        (void)fuss_spawn(ed, "stash-pop", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_REMOTE:
        if (aux != NULL) {
            char *argv[] = {
                (char *)"push", (char *)"-u", value, aux, NULL
            };
            (void)fuss_spawn(ed, "push", argv, false, NULL, 0U, false);
        }
        break;
    case FUSS_PICK_COMMIT_AMEND:
    case FUSS_PICK_REBASE_CONFIRM:
    case FUSS_PICK_REMOTE_CHECK:
    case FUSS_PICK_NONE:
        break;
    }
    free(value);
    free(aux);
    return true;
}

static void fuss_preview_complete(void *opaque, Ed *ed,
                                  const YewJob *job)
{
    FussPreviewJob *owner = opaque;
    FussMode *f;
    static const char unavailable[] = "preview unavailable";

    if (owner == NULL || ed == NULL || ed->fuss == NULL || job == NULL)
        return;
    f = ed->fuss;
    if (f->preview_job != owner->job_id ||
        f->preview_value == NULL || owner->value == NULL ||
        strcmp(f->preview_value, owner->value) != 0)
        return;
    f->preview_job = 0U;
    bytebuf_free(&f->preview_bytes);
    bytebuf_init(&f->preview_bytes);
    if (job->state == YEW_JOB_EXITED && job->exit_code == 0 &&
        !job->collect_capped)
        bytebuf_append(&f->preview_bytes, job->collect.data,
                       job->collect.len);
    else
        bytebuf_append(&f->preview_bytes, (const u8 *)unavailable,
                       sizeof(unavailable) - 1U);
    f->preview_ready = true;
    fuss_damage(ed);
}

static void fuss_preview_destroy(void *opaque)
{
    FussPreviewJob *owner = opaque;

    if (owner == NULL)
        return;
    free(owner->value);
    free(owner);
}

static const YewJobCallbackOps fuss_preview_ops = {
    fuss_preview_complete,
    fuss_preview_destroy
};

static bool fuss_preview_start(Ed *ed, FussMode *f, const char *value)
{
    FussPreviewJob *owner;
    const GitVerb *verb;
    char *argv[6];
    char error[160];
    u32 job;

    if (ed == NULL || f == NULL || value == NULL || value[0] == '\0' ||
        value[0] == '-' || f->preview_job != 0U)
        return false;
    verb = yew_git_verb("show");
    if (verb == NULL)
        return false;
    owner = yew_xcalloc(1U, sizeof(*owner));
    owner->value = fuss_dup_bytes(value, fuss_cstr_len(value));
    if (owner->value == NULL) {
        free(owner);
        return false;
    }
    argv[0] = (char *)"show";
    argv[1] = (char *)"--stat";
    argv[2] = (char *)"--oneline";
    argv[3] = (char *)"--decorate";
    argv[4] = owner->value;
    argv[5] = NULL;
    job = yew_git_spawn_callback(ed, verb, argv, owner,
                                 &fuss_preview_ops, error, sizeof(error));
    if (job == 0U) {
        fuss_preview_destroy(owner);
        return false;
    }
    owner->job_id = job;
    free(f->preview_value);
    f->preview_value = fuss_dup_bytes(value, fuss_cstr_len(value));
    bytebuf_free(&f->preview_bytes);
    bytebuf_init(&f->preview_bytes);
    f->preview_ready = false;
    f->preview_job = job;
    return true;
}

static void fuss_preview_draw_text(Ed *ed, Rect r, const u8 *bytes,
                                   size_t len)
{
    ThemeEnt style = fuss_base_style(ed);
    size_t at = 0U;
    u16 row = 0U;

    while (at < len && row < r.h) {
        size_t end = at;
        size_t fit;
        int cells = 0;

        while (end < len && bytes[end] != (u8)'\n')
            end++;
        if (end > at && bytes[end - 1U] == (u8)'\r')
            end--;
        fit = yew_str_clip(bytes + at, end - at, (int)r.w, &cells);
        if (fit != 0U)
            (void)yew_grid_puts(&ed->grid, (u16)(r.y + row), r.x,
                                bytes + at, fit, style.fg, style.bg,
                                (u16)(style.attrs | YEW_ATTR_DIM));
        row++;
        while (end < len && bytes[end] != (u8)'\n')
            end++;
        at = end < len ? end + 1U : end;
    }
}

static void fuss_picker_preview(Ed *ed, void *ctx, i32 payload, Rect r)
{
    FussMode *f = ctx;
    const char *value;
    static const u8 loading[] = "loading…";

    if (ed == NULL || f == NULL || payload < 0 ||
        (u32)payload >= f->picker_count || r.w == 0U || r.h == 0U)
        return;
    value = f->picker_values[payload];
    if (value == NULL)
        return;
    if (f->preview_value == NULL ||
        strcmp(f->preview_value, value) != 0) {
        if (f->preview_job == 0U)
            (void)fuss_preview_start(ed, f, value);
        fuss_preview_draw_text(ed, r, loading, sizeof(loading) - 1U);
        return;
    }
    if (!f->preview_ready) {
        fuss_preview_draw_text(ed, r, loading, sizeof(loading) - 1U);
        return;
    }
    fuss_preview_draw_text(ed, r, f->preview_bytes.data,
                           f->preview_bytes.len);
}

static void fuss_picker_open(Ed *ed, FussPickAction action,
                             const char *title)
{
    FussMode *f = ed->fuss;
    PickerSpec spec = {0};

    if (f->picker_count == 0U) {
        yew_msg(ed, YEW_MSG_WARN, "no %s available", title);
        fuss_picker_clear(f);
        return;
    }
    f->picker_action = action;
    spec.title = title;
    spec.items = fuss_picker_items;
    spec.accept = fuss_picker_accept;
    if (action != FUSS_PICK_REMOTE && action != FUSS_PICK_RESET_MODE)
        spec.preview = fuss_picker_preview;
    spec.action = action == FUSS_PICK_STASH ? fuss_picker_action : NULL;
    spec.footer = action == FUSS_PICK_STASH ?
                  "Enter pop · C-v apply · Esc cancel" : NULL;
    spec.path_mode = false;
    spec.ctx = f;
    yew_picker_open(ed, &spec);
}

static bool fuss_parse_epoch(const char *text, size_t len, i64 *out)
{
    size_t at;
    i64 value = 0;

    if (text == NULL || len == 0U || out == NULL)
        return false;
    for (at = 0U; at < len; at++) {
        i64 digit;

        if (text[at] < '0' || text[at] > '9')
            return false;
        digit = (i64)(text[at] - '0');
        if (value > (INT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static void fuss_detail_part(Bytebuf *detail, const char *text, size_t len)
{
    if (detail == NULL || text == NULL || len == 0U)
        return;
    if (detail->len != 0U)
        bytebuf_append(detail, (const u8 *)" · ", sizeof(" · ") - 1U);
    bytebuf_append(detail, (const u8 *)text, len);
}

static void fuss_detail_relative(Bytebuf *detail, const char *text,
                                 size_t len)
{
    i64 stamp;
    i64 now;
    i64 age;
    i64 count;
    const char *unit;

    if (!fuss_parse_epoch(text, len, &stamp))
        return;
    now = (i64)time(NULL);
    age = now > stamp ? now - stamp : 0;
    if (age < 60) {
        fuss_detail_part(detail, "now", sizeof("now") - 1U);
        return;
    }
    if (age < 60 * 60) {
        count = age / 60;
        unit = "minute";
    } else if (age < 24 * 60 * 60) {
        count = age / (60 * 60);
        unit = "hour";
    } else if (age < 14 * 24 * 60 * 60) {
        count = age / (24 * 60 * 60);
        unit = "day";
    } else if (age < 90 * 24 * 60 * 60) {
        count = age / (7 * 24 * 60 * 60);
        unit = "week";
    } else if (age < 2 * 365 * 24 * 60 * 60) {
        count = age / (30 * 24 * 60 * 60);
        unit = "month";
    } else {
        count = age / (365 * 24 * 60 * 60);
        unit = "year";
    }
    if (detail->len != 0U)
        bytebuf_append(detail, (const u8 *)" · ", sizeof(" · ") - 1U);
    bytebuf_printf(detail, "%lld %s%s ago", (long long)count, unit,
                   count == 1 ? "" : "s");
}

static bool fuss_parse_records(Ed *ed, FussMode *f, FussPickAction action,
                               const u8 *out, size_t len)
{
    size_t off = 0U;
    const GitSnapshot *snap = yew_git_snapshot(ed);

    while (off < len) {
        size_t end = off;
        size_t at = 0U;
        const char *fields[4] = {NULL, NULL, NULL, NULL};
        size_t lens[4] = {0U, 0U, 0U, 0U};
        u32 nfield = 0U;

        while (off < len && (out[off] == (u8)'\n' ||
                             out[off] == (u8)'\r'))
            off++;
        if (off == len)
            break;
        end = off;

        while (end < len && out[end] != 0U)
            end++;
        while (at < end - off && nfield < YEW_ARRAY_LEN(fields)) {
            size_t start = at;

            while (at < end - off && out[off + at] != 0x1fU)
                at++;
            fields[nfield] = (const char *)out + off + start;
            lens[nfield++] = at - start;
            if (at < end - off)
                at++;
        }
        if ((action == FUSS_PICK_BRANCH_SWITCH ||
             action == FUSS_PICK_BRANCH_DELETE ||
             action == FUSS_PICK_MERGE ||
             action == FUSS_PICK_CHERRY_BRANCH) && nfield >= 1U) {
            bool include = true;
            Bytebuf detail;

            if (action == FUSS_PICK_BRANCH_DELETE && snap != NULL &&
                snap->branch != NULL &&
                lens[0] == fuss_cstr_len(snap->branch) &&
                memcmp(fields[0], snap->branch, lens[0]) == 0)
                include = false;
            bytebuf_init(&detail);
            if (nfield > 1U)
                fuss_detail_part(&detail, fields[1], lens[1]);
            if (nfield > 2U)
                fuss_detail_relative(&detail, fields[2], lens[2]);
            if (include)
                (void)fuss_picker_add(f, fields[0], lens[0],
                                      detail.len == 0U ? NULL :
                                                               (const char *)detail.data,
                                      detail.len,
                                      fields[0], lens[0]);
            bytebuf_free(&detail);
        } else if ((action == FUSS_PICK_RESET_COMMIT ||
                    action == FUSS_PICK_REBASE ||
                    action == FUSS_PICK_CHERRY_COMMIT ||
                    action == FUSS_PICK_REVERT) && nfield >= 2U) {
            Bytebuf detail;

            bytebuf_init(&detail);
            if (nfield > 2U)
                fuss_detail_relative(&detail, fields[2], lens[2]);
            if (nfield > 3U)
                fuss_detail_part(&detail, fields[3], lens[3]);
            (void)fuss_picker_add(f, fields[1], lens[1],
                                  detail.len == 0U ? NULL :
                                                           (const char *)detail.data,
                                  detail.len,
                                  fields[0], lens[0]);
            bytebuf_free(&detail);
        } else if (action == FUSS_PICK_STASH && nfield >= 1U) {
            Bytebuf detail;

            bytebuf_init(&detail);
            if (nfield > 1U)
                fuss_detail_relative(&detail, fields[1], lens[1]);
            if (nfield > 2U)
                fuss_detail_part(&detail, fields[2], lens[2]);
            (void)fuss_picker_add(f, fields[0], lens[0],
                                  detail.len == 0U ? NULL :
                                                           (const char *)detail.data,
                                  detail.len,
                                  fields[0], lens[0]);
            bytebuf_free(&detail);
        }
        off = end < len ? end + 1U : end;
    }
    return f->picker_count != 0U;
}

static bool fuss_parse_remotes(FussMode *f, const u8 *out, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        size_t end = off;
        size_t name_end;
        u32 i;
        bool seen = false;

        while (end < len && out[end] != (u8)'\n' && out[end] != 0U)
            end++;
        name_end = off;
        while (name_end < end && out[name_end] != (u8)'\t' &&
               out[name_end] != (u8)' ')
            name_end++;
        for (i = 0U; i < f->picker_count; i++) {
            size_t old_len = fuss_cstr_len(f->picker_values[i]);

            if (old_len == name_end - off &&
                memcmp(f->picker_values[i], out + off, old_len) == 0)
                seen = true;
        }
        if (!seen && name_end > off)
            (void)fuss_picker_add(f, (const char *)out + off, name_end - off,
                                  NULL, 0U, (const char *)out + off,
                                  name_end - off);
        off = end < len ? end + 1U : end;
    }
    return f->picker_count != 0U;
}

static bool fuss_picker_result(Ed *ed, FussPickAction action,
                               const GitResult *result)
{
    FussMode *f;
    size_t len;
    const GitSnapshot *snap;

    if (ed == NULL || ed->fuss == NULL || result == NULL ||
        result->out_len > (u64)SIZE_MAX)
        return false;
    f = ed->fuss;
    len = (size_t)result->out_len;
    if (action == FUSS_PICK_REMOTE)
        (void)fuss_parse_remotes(f, result->out, len);
    else if (action != FUSS_PICK_REMOTE_CHECK)
        (void)fuss_parse_records(ed, f, action, result->out, len);
    if (action == FUSS_PICK_REMOTE) {
        snap = yew_git_snapshot(ed);
        if (snap != NULL && snap->branch != NULL)
            f->picker_aux = fuss_dup_bytes(snap->branch,
                                           fuss_cstr_len(snap->branch));
        if (f->picker_count == 1U && f->picker_aux != NULL) {
            size_t branch_len = fuss_cstr_len(f->picker_aux);
            char *ref;

            if (branch_len <= SIZE_MAX - sizeof("refs/heads/")) {
                char *argv[6];

                ref = yew_xmalloc(sizeof("refs/heads/") + branch_len);
                (void)memcpy(ref, "refs/heads/",
                             sizeof("refs/heads/") - 1U);
                (void)memcpy(ref + sizeof("refs/heads/") - 1U,
                             f->picker_aux, branch_len + 1U);
                f->picker_aux2 = fuss_dup_bytes(f->picker_values[0],
                    fuss_cstr_len(f->picker_values[0]));
                argv[0] = (char *)"ls-remote";
                argv[1] = (char *)"--heads";
                argv[2] = f->picker_aux2;
                argv[3] = ref;
                argv[4] = NULL;
                if (f->picker_aux2 != NULL &&
                    fuss_spawn(ed, "remote-check", argv, false, NULL, 0U,
                               false) == YEW_CMD_OK) {
                    f->pending_pick = FUSS_PICK_REMOTE_CHECK;
                    free(ref);
                    return true;
                }
                free(ref);
                free(f->picker_aux2);
                f->picker_aux2 = NULL;
            }
        }
    } else if (action == FUSS_PICK_REMOTE_CHECK) {
        if (len == 0U && f->picker_aux != NULL && f->picker_aux2 != NULL) {
            char *branch = fuss_dup_bytes(f->picker_aux,
                                          fuss_cstr_len(f->picker_aux));
            char *remote = fuss_dup_bytes(f->picker_aux2,
                                          fuss_cstr_len(f->picker_aux2));
            CmdStatus status = YEW_CMD_ERR_STATE;

            fuss_picker_clear(f);
            if (branch != NULL && remote != NULL) {
                char *argv[] = {
                    (char *)"push", (char *)"-u", remote, branch, NULL
                };

                status = fuss_spawn(ed, "push", argv, false, NULL, 0U,
                                    false);
                if (status == YEW_CMD_OK)
                    yew_msg(ed, YEW_MSG_INFO,
                            "one remote; pushing %s to %s and setting upstream",
                            branch, remote);
            }
            free(branch);
            free(remote);
            return status == YEW_CMD_OK;
        }
        fuss_picker_open(ed, FUSS_PICK_REMOTE, "remote");
        return true;
    }
    switch (action) {
    case FUSS_PICK_BRANCH_SWITCH: fuss_picker_open(ed, action, "branch"); break;
    case FUSS_PICK_BRANCH_DELETE: fuss_picker_open(ed, action, "branch"); break;
    case FUSS_PICK_MERGE: fuss_picker_open(ed, action, "branch"); break;
    case FUSS_PICK_RESET_COMMIT: fuss_picker_open(ed, action, "commit"); break;
    case FUSS_PICK_REBASE: fuss_picker_open(ed, action, "rebase base"); break;
    case FUSS_PICK_CHERRY_BRANCH: fuss_picker_open(ed, action, "branch"); break;
    case FUSS_PICK_CHERRY_COMMIT: fuss_picker_open(ed, action, "commit"); break;
    case FUSS_PICK_REVERT: fuss_picker_open(ed, action, "commit"); break;
    case FUSS_PICK_STASH: fuss_picker_open(ed, action, "stash"); break;
    case FUSS_PICK_REMOTE: fuss_picker_open(ed, action, "remote"); break;
    case FUSS_PICK_COMMIT_AMEND:
    case FUSS_PICK_REBASE_CONFIRM:
    case FUSS_PICK_REMOTE_CHECK:
    case FUSS_PICK_RESET_MODE:
    case FUSS_PICK_NONE:
        return false;
    }
    return true;
}

static CmdStatus fuss_nav(CmdCtx *cx, i32 kind)
{
    FussMode *f;
    i32 row;
    u32 count;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    if (kind == 4 && fuss_expand_start(cx->ed, true))
        return YEW_CMD_OK;
    row = fuss_row(f);
    count = cx->count_given && cx->count != 0U ? cx->count : 1U;
    while (count-- != 0U) {
        if (kind == -1 || kind == 1)
            row = yew_fuss_nav_step(&f->tree, row, kind);
        else if (kind == -2 || kind == 2)
            row = yew_fuss_nav_raw(&f->tree, row, kind < 0 ? -1 : 1);
        else if (kind == 3)
            row = yew_fuss_nav_parent(&f->tree, row);
        else if (kind == 4)
            row = yew_fuss_nav_enter(&f->tree, row);
    }
    fuss_select_row(f, row);
    fuss_damage(cx->ed);
    return YEW_CMD_OK;
}

static void fuss_walk_restart(Ed *ed)
{
    FussMode *f = ed->fuss;
    WalkOpts opts = {0};

    if (f->walk != NULL) {
        yew_walk_end(f->walk);
        f->walk = NULL;
    }
    yew_filelist_free(&f->files);
    yew_filelist_init(&f->files);
    if (!f->opts.all_files)
        return;
    opts.hidden = f->opts.show_hidden;
    opts.use_gitignore = !f->opts.show_hidden;
    f->walk = yew_walk_begin(yew_ws_root(ed), &opts, &f->files);
}

static void fuss_expand_clear(FussMode *f)
{
    if (f == NULL)
        return;
    if (f->expand_walk != NULL)
        yew_walk_end(f->expand_walk);
    f->expand_walk = NULL;
    yew_filelist_free(&f->expand_files);
    yew_filelist_init(&f->expand_files);
    free(f->expand_path);
    f->expand_path = NULL;
    f->expand_enter = false;
}

static bool fuss_expand_start(Ed *ed, bool enter)
{
    FussMode *f;
    const FussItem *item;
    const FussNode *node;
    WalkOpts opts = {0};
    char *root;
    i32 row;

    if (ed == NULL || ed->fuss == NULL)
        return false;
    f = ed->fuss;
    row = fuss_row(f);
    item = fuss_item(f, row);
    node = fuss_node(f, item);
    if (item == NULL || node == NULL || !node->untracked_dir ||
        node->untracked_loaded)
        return false;
    if (f->expand_walk != NULL) {
        yew_msg(ed, YEW_MSG_INFO, "untracked directory scan is running");
        return true;
    }
    root = fuss_join_root(ed, item->path);
    if (root == NULL)
        return false;
    fuss_expand_clear(f);
    f->expand_path = fuss_dup_bytes(item->path, item->path_len);
    f->expand_enter = enter;
    opts.hidden = f->opts.show_hidden;
    opts.include_dirs = true;
    opts.max_depth = 1U;
    f->expand_walk = yew_walk_begin(root, &opts, &f->expand_files);
    free(root);
    if (f->expand_walk == NULL) {
        fuss_expand_clear(f);
        yew_msg(ed, YEW_MSG_ERROR, "cannot scan untracked directory");
        return true;
    }
    yew_msg(ed, YEW_MSG_INFO, "scanning untracked directory…");
    return true;
}

static bool fuss_expand_children(FussMode *f, const GitSnapshot *snap,
                                 GitPathList *children, Arena *arena)
{
    size_t base_len;
    size_t i;

    if (f == NULL || f->expand_path == NULL || children == NULL ||
        arena == NULL)
        return false;
    base_len = fuss_cstr_len(f->expand_path);
    children->len = 0U;
    children->data = f->expand_files.paths.len == 0U ? NULL :
        arena_alloc(arena, f->expand_files.paths.len * sizeof(GitPath),
                    _Alignof(GitPath));
    for (i = 0U; i < f->expand_files.paths.len; i++) {
        const char *tail = f->expand_files.paths.data[i];
        size_t tail_len = fuss_cstr_len(tail);
        size_t full_len;
        char *full;

        if (tail_len == 0U || base_len > SIZE_MAX - tail_len - 1U)
            continue;
        full_len = base_len + 1U + tail_len;
        if (full_len > UINT32_MAX)
            continue;
        full = arena_alloc(arena, full_len + 1U, _Alignof(char));
        (void)memcpy(full, f->expand_path, base_len);
        full[base_len] = '/';
        (void)memcpy(full + base_len + 1U, tail, tail_len + 1U);
        if (!f->opts.show_hidden && snap != NULL &&
            yew_git_ignored(&snap->ignored, full, (u32)full_len))
            continue;
        children->data[children->len].path = full;
        children->data[children->len].len = (u32)full_len;
        children->data[children->len].is_dir =
            i < f->expand_files.is_dir.len &&
            f->expand_files.is_dir.data[i] != 0U;
        children->len++;
    }
    return true;
}

static void fuss_expand_tick(Ed *ed)
{
    FussMode *f;
    const GitSnapshot *snap;
    FussSel lookup = {0};
    GitPathList children = {0};
    Arena arena;
    i32 row;
    u32 node;
    bool enter;

    if (ed == NULL || ed->fuss == NULL || ed->fuss->expand_walk == NULL)
        return;
    f = ed->fuss;
    if (yew_walk_step(f->expand_walk, 2000))
        return;
    yew_walk_end(f->expand_walk);
    f->expand_walk = NULL;
    yew_fuss_sel_set(&lookup, f->expand_path,
                     (u32)fuss_cstr_len(f->expand_path));
    row = yew_fuss_row_of(&f->tree, &lookup);
    yew_fuss_sel_clear(&lookup);
    if (row < 0 || (size_t)row >= f->tree.items.len ||
        f->tree.items.data[row].path_len != fuss_cstr_len(f->expand_path) ||
        memcmp(f->tree.items.data[row].path, f->expand_path,
               f->tree.items.data[row].path_len) != 0) {
        fuss_expand_clear(f);
        return;
    }
    node = f->tree.items.data[row].node;
    enter = f->expand_enter;
    snap = yew_git_snapshot(ed);
    arena_init(&arena);
    if (fuss_expand_children(f, snap, &children, &arena) &&
        yew_fuss_expand_untracked(&f->tree, node, &children)) {
        if (enter)
            row = yew_fuss_nav_enter(&f->tree, row);
        fuss_select_row(f, row);
        fuss_damage(ed);
    }
    arena_free_all(&arena);
    fuss_expand_clear(f);
}

CmdStatus yew_fuss_cmd_init(CmdCtx *cx)
{
    char *argv[] = {(char *)"init", NULL};

    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "init", argv, false, NULL, 0U, false) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_leave(CmdCtx *cx)
{
    if (fuss_require(cx, NULL) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    return yew_mode_enter(cx->ed, YEW_MODE_L);
}

CmdStatus yew_fuss_cmd_tree_all(CmdCtx *cx)
{
    FussMode *f;
    const GitSnapshot *snap;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    f->opts.all_files = !f->opts.all_files;
    (void)yew_state_option_bool_set(cx->ed, "git.tree.all_files",
                                    f->opts.all_files);
    snap = yew_git_snapshot(cx->ed);
    if (snap != NULL)
        fuss_build(cx->ed, snap, true);
    fuss_walk_restart(cx->ed);
    yew_msg(cx->ed, YEW_MSG_INFO, "all-files tree %s",
            f->opts.all_files ? "enabled" : "disabled");
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_tree_hidden(CmdCtx *cx)
{
    FussMode *f;
    const GitSnapshot *snap;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    f->opts.show_hidden = !f->opts.show_hidden;
    (void)yew_state_option_bool_set(cx->ed, "git.tree.show_hidden",
                                    f->opts.show_hidden);
    snap = yew_git_snapshot(cx->ed);
    if (snap != NULL)
        fuss_build(cx->ed, snap, true);
    fuss_walk_restart(cx->ed);
    yew_msg(cx->ed, YEW_MSG_INFO, "hidden files %s",
            f->opts.show_hidden ? "shown" : "hidden");
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_nav_prev(CmdCtx *cx) { return fuss_nav(cx, -1); }
CmdStatus yew_fuss_cmd_nav_next(CmdCtx *cx) { return fuss_nav(cx, 1); }
CmdStatus yew_fuss_cmd_nav_parent(CmdCtx *cx) { return fuss_nav(cx, 3); }
CmdStatus yew_fuss_cmd_nav_enter(CmdCtx *cx) { return fuss_nav(cx, 4); }
CmdStatus yew_fuss_cmd_nav_row_prev(CmdCtx *cx) { return fuss_nav(cx, -2); }
CmdStatus yew_fuss_cmd_nav_row_next(CmdCtx *cx) { return fuss_nav(cx, 2); }

CmdStatus yew_fuss_cmd_nav_toggle(CmdCtx *cx)
{
    FussMode *f;
    i32 row;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    row = fuss_row(f);
    if (yew_fuss_nav_toggle(&f->tree, row)) {
        fuss_select_row(f, yew_fuss_row_of(&f->tree, &f->sel));
        fuss_damage(cx->ed);
    }
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_jump_arm(CmdCtx *cx)
{
    FussMode *f;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    yew_fuss_jump_arm(&f->jump, yew_now_ms());
    cx->ed->footer_dirty = true;
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_stage(CmdCtx *cx)
{
    char *prefix[] = {(char *)"add"};
    return fuss_path_verb(cx, "stage", prefix, YEW_ARRAY_LEN(prefix),
                          FUSS_TARGET_ANY, false);
}

CmdStatus yew_fuss_cmd_unstage(CmdCtx *cx)
{
    char *prefix[] = {(char *)"restore", (char *)"--staged"};
    return fuss_path_verb(cx, "unstage", prefix, YEW_ARRAY_LEN(prefix),
                          FUSS_TARGET_STAGED_FILE, false);
}

CmdStatus yew_fuss_cmd_stage_all(CmdCtx *cx)
{
    char *argv[] = {(char *)"add", (char *)"--all", NULL};
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "stage-all", argv, false, NULL, 0U, false) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_unstage_all(CmdCtx *cx)
{
    char *argv[] = {
        (char *)"restore", (char *)"--staged", (char *)"--", (char *)".",
        NULL
    };
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "unstage-all", argv, true, NULL, 0U, false) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_commit(CmdCtx *cx)
{
    return fuss_commit_open(cx, false);
}

CmdStatus yew_fuss_cmd_commit_amend(CmdCtx *cx)
{
    return fuss_commit_open(cx, true);
}

CmdStatus yew_fuss_cmd_push(CmdCtx *cx)
{
    char *argv[] = {(char *)"push", NULL};
    const GitSnapshot *snap;

    if (fuss_require(cx, NULL) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    snap = yew_git_snapshot(cx->ed);
    if (snap != NULL && snap->branch != NULL &&
        (snap->upstream == NULL || snap->upstream[0] == '\0'))
        return fuss_pick_remotes(cx->ed);
    return fuss_spawn(cx->ed, "push", argv, false, NULL, 0U, false);
}

CmdStatus yew_fuss_cmd_push_force(CmdCtx *cx)
{
    return fuss_prompt(cx, FUSS_PROMPT_PUSH_FORCE, NULL, NULL,
                       "force-push — type 'force' to confirm");
}

CmdStatus yew_fuss_cmd_pull(CmdCtx *cx)
{
    char *argv[] = {(char *)"pull", NULL};
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "pull", argv, false, NULL, 0U, false) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_fetch(CmdCtx *cx)
{
    char *argv[] = {(char *)"fetch", NULL};
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "fetch", argv, false, NULL, 0U, false) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_diff(CmdCtx *cx)
{
    char *prefix[] = {(char *)"diff", (char *)"HEAD"};
    return fuss_path_verb(cx, "diff", prefix, YEW_ARRAY_LEN(prefix),
                          FUSS_TARGET_FILE, true);
}

CmdStatus yew_fuss_cmd_status(CmdCtx *cx)
{
    char *argv[] = {(char *)"status", NULL};
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "status", argv, false, NULL, 0U, true) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_blame(CmdCtx *cx)
{
    char *prefix[] = {(char *)"blame", (char *)"--porcelain"};
    return fuss_path_verb(cx, "blame", prefix, YEW_ARRAY_LEN(prefix),
                          FUSS_TARGET_FILE, true);
}

CmdStatus yew_fuss_cmd_history(CmdCtx *cx)
{
    char *argv[] = {
        (char *)"log", (char *)"-n", (char *)"200",
        (char *)"--date-order",
        (char *)"--pretty=format:%h%x1f%at%x1f%an%x1f%d%x1f%s", NULL
    };
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "log", argv, false, NULL, 0U, true) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_reflog(CmdCtx *cx)
{
    char *argv[] = {
        (char *)"reflog", (char *)"show", (char *)"-n", (char *)"200",
        (char *)"--pretty=format:%h%x1f%gd%x1f%at%x1f%gs", NULL
    };
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_spawn(cx->ed, "reflog", argv, false, NULL, 0U, true) :
           YEW_CMD_ERR_STATE;
}

static CmdStatus fuss_open_path(CmdCtx *cx, bool leave)
{
    FussMode *f;
    char *path;
    char *absolute;
    Buffer *buffer;
    bool was_resident;

    if (fuss_require(cx, &f) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    path = fuss_selected_path(cx);
    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE,
                           leave ? "open" : "view", NULL)) {
        free(path);
        return YEW_CMD_ERR_STATE;
    }
    absolute = fuss_join_root(cx->ed, path);
    free(path);
    if (absolute == NULL)
        return YEW_CMD_ERR_ARG;
    buffer = yew_ws_file_buf(cx->ed, absolute);
    free(absolute);
    if (buffer == NULL)
        return YEW_CMD_ERR_IO;
    was_resident = yew_buf_resident(buffer);
    if (yew_buf_hydrate(cx->ed, buffer) != 0)
        return YEW_CMD_ERR_IO;
    if (!was_resident)
        yew_fl_hook_buffer(cx->ed, FL_EV_BUF_OPEN, buffer);
    if (leave) {
        if (f->saved_win == NULL)
            return YEW_CMD_ERR_STATE;
        yew_ed_win_set_buffer(cx->ed, f->saved_win, buffer);
        f->saved_buffer_id = buffer->id;
        return yew_mode_enter(cx->ed, YEW_MODE_L);
    }
    return fuss_show_buffer(cx->ed, buffer) ? YEW_CMD_OK :
                                             YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_view(CmdCtx *cx) { return fuss_open_path(cx, false); }

CmdStatus yew_fuss_cmd_branch_switch(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *name = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (name == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {(char *)"switch", (char *)"--", name, NULL};
            status = fuss_spawn(cx->ed, "switch", argv, false, NULL, 0U,
                                false);
        }
        free(name);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_branches(cx->ed, FUSS_PICK_BRANCH_SWITCH) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_branch_create(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *name = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (name == NULL)
            return YEW_CMD_ERR_ARG;
        if (name[0] == '-') {
            free(name);
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "branch name must not begin with '-'");
            return YEW_CMD_ERR_ARG;
        }
        {
            char *argv[] = {(char *)"switch", (char *)"-c", name, NULL};
            status = fuss_spawn(cx->ed, "switch-create", argv, false, NULL,
                                0U, false);
        }
        free(name);
        return status;
    }
    return fuss_prompt(cx, FUSS_PROMPT_BRANCH_CREATE, NULL, NULL,
                       "new branch name");
}

CmdStatus yew_fuss_cmd_branch_delete(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *name = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (name == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {
                (char *)"branch", (char *)"-d", (char *)"--", name, NULL
            };
            status = fuss_spawn(cx->ed, "branch-delete", argv, false, NULL,
                                0U, false);
        }
        free(name);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_branches(cx->ed, FUSS_PICK_BRANCH_DELETE) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_merge(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *name = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (name == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {
                (char *)"merge", (char *)"--no-edit", (char *)"--", name,
                NULL
            };
            status = fuss_spawn(cx->ed, "merge", argv, false, NULL, 0U,
                                false);
        }
        free(name);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_branches(cx->ed, FUSS_PICK_MERGE) : YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_reset(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *value = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        const char *mode = "--mixed";
        char *ref = value;
        CmdStatus status;

        if (value == NULL)
            return YEW_CMD_ERR_ARG;
        if (cx->sarg_len > 5U && memcmp(value, "soft ", 5U) == 0) {
            mode = "--soft";
            ref += 5;
        } else if (cx->sarg_len > 6U && memcmp(value, "mixed ", 6U) == 0) {
            ref += 6;
        } else if (cx->sarg_len > 5U && memcmp(value, "hard ", 5U) == 0) {
            mode = "--hard";
            ref += 5;
        }
        if (*ref == '\0') {
            free(value);
            return YEW_CMD_ERR_ARG;
        }
        {
            char *argv[] = {(char *)"reset", (char *)mode, ref, NULL};
            status = fuss_spawn(cx->ed, "reset", argv, false, NULL, 0U,
                                false);
        }
        free(value);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_commits(cx->ed, FUSS_PICK_RESET_COMMIT, NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_rebase_interactive(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *base = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (base == NULL)
            return YEW_CMD_ERR_ARG;
        status = fuss_rebase_prepare(cx->ed, base);
        free(base);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_commits(cx->ed, FUSS_PICK_REBASE, NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_rebase_continue(CmdCtx *cx)
{
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_rebase_sync(cx->ed, "--continue", NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_rebase_abort(CmdCtx *cx)
{
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_rebase_sync(cx->ed, "--abort", NULL) : YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_cherry_pick(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *commit = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (commit == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {(char *)"cherry-pick", commit, NULL};
            status = fuss_spawn(cx->ed, "cherry-pick", argv, false, NULL,
                                0U, false);
        }
        free(commit);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_branches(cx->ed, FUSS_PICK_CHERRY_BRANCH) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_revert(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *commit = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (commit == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {
                (char *)"revert", (char *)"--no-edit", commit, NULL
            };
            status = fuss_spawn(cx->ed, "revert", argv, false, NULL, 0U,
                                false);
        }
        free(commit);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_commits(cx->ed, FUSS_PICK_REVERT, NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_stash_push(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL) {
        char *message;
        CmdStatus status;

        if (cx->sarg_len == 0U ||
            memchr(cx->sarg, '\0', cx->sarg_len) != NULL)
            return YEW_CMD_ERR_ARG;
        message = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        if (message == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {
                (char *)"stash", (char *)"push", (char *)"-m", message,
                NULL
            };

            status = fuss_spawn(cx->ed, "stash-push", argv, false, NULL,
                                0U, false);
        }
        free(message);
        return status;
    }
    return fuss_prompt(cx, FUSS_PROMPT_STASH_PUSH, NULL, NULL,
                       "stash message");
}

CmdStatus yew_fuss_cmd_stash_pop(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *ref = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (ref == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {(char *)"stash", (char *)"pop", ref, NULL};
            status = fuss_spawn(cx->ed, "stash-pop", argv, false, NULL, 0U,
                                false);
        }
        free(ref);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_stashes(cx->ed) : YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_tag(CmdCtx *cx)
{
    if (cx != NULL && cx->sarg != NULL && cx->sarg_len != 0U) {
        char *name = fuss_dup_bytes(cx->sarg, cx->sarg_len);
        CmdStatus status;

        if (name == NULL)
            return YEW_CMD_ERR_ARG;
        {
            char *argv[] = {(char *)"tag", (char *)"--", name, NULL};
            status = fuss_spawn(cx->ed, "tag", argv, false, NULL, 0U,
                                false);
        }
        free(name);
        return status;
    }
    return fuss_prompt(cx, FUSS_PROMPT_TAG, NULL, NULL,
                       "tag name; optional message on following lines");
}

CmdStatus yew_fuss_cmd_discard(CmdCtx *cx)
{
    char *path = fuss_selected_path(cx);
    CmdStatus status;

    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_DIRTY_FILE, "discard",
                           NULL)) {
        free(path);
        return YEW_CMD_ERR_STATE;
    }
    status = fuss_prompt(cx, FUSS_PROMPT_DISCARD, NULL, path,
                         "type 'discard' to permanently discard the path");
    free(path);
    return status;
}

CmdStatus yew_fuss_cmd_file_delete(CmdCtx *cx)
{
    char *path = fuss_selected_path(cx);
    CmdStatus status;

    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE, "delete", NULL)) {
        free(path);
        return YEW_CMD_ERR_STATE;
    }
    status = fuss_prompt(cx, FUSS_PROMPT_DELETE, NULL, path,
                         "type 'delete' to permanently delete the path");
    free(path);
    return status;
}

CmdStatus yew_fuss_cmd_file_rename(CmdCtx *cx)
{
    char *path = fuss_selected_path(cx);
    CmdStatus status;

    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    status = fuss_prompt(cx, FUSS_PROMPT_RENAME, path, path,
                         "new workspace-relative path");
    free(path);
    return status;
}

CmdStatus yew_fuss_cmd_open(CmdCtx *cx) { return fuss_open_path(cx, true); }
