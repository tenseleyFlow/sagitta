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
#include <unistd.h>

#include "edit/ed.h"
#include "edit/file_cmds.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/mode.h"
#include "edit/option.h"
#include "edit/pane_cmds.h"
#include "fl/flruntime.h"
#include "mod/git/fusscommit.h"
#include "mod/git/fusstree.h"
#include "mod/git/editor.h"
#include "mod/git/diffview.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"
#include "mod/git/modal.h"
#include "syn/theme.h"
#include "term/grid.h"
#include "text/piece.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/draw.h"
#include "ui/message.h"
#include "ui/panel.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/statusline.h"
#include "ui/tabs.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "util/sort.h"
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
    FUSS_PROMPT_RESET_HARD,
    FUSS_PROMPT_REBASE_ABORT,
    FUSS_PROMPT_COMMIT,
    FUSS_PROMPT_COMMIT_AMEND
} FussPromptAction;

typedef enum FussPickAction {
    FUSS_PICK_NONE,
    FUSS_PICK_ACTIONS,
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

enum { FUSS_OPENING_FRAME_MS = 16 };

struct FussMode {
    u32 diff_job;
    GitModalBridge bridge;
    FussTree tree;
    FussOpenMemory manual_open;
    FussOpenMemory open_files;
    FussPathRef *open_scratch;
    u32 open_scratch_len;
    u32 open_scratch_cap;
    FussPathRef *open_refs;
    u32 open_refs_cap;
    u16 natural_cols;
    FussOpts opts;
    FussSel sel;
    FussJump jump;
    bool active;
    bool opening;
    i64 opening_until_ms;
    bool ascii_glyphs;
    bool viewer;
    u32 viewer_win_id;
    bool draw_dirty;
    bool backdrop_dirty;
    GitAsyncState seen_detect_state;
    GitStatusCode seen_detect_result;
    u16 scroll;
    u32 saved_buffer_id;
    u32 viewer_buffer_id;
    u32 pending_job;
    u32 seen_result_job;
    u32 mutation_snapshot_gen;
    bool pending_view;
    bool mutation_snapshot_pending;
    bool commit_editing;
    bool commit_amend;
    bool commit_cancel_pending;
    u8 commit_comment;
    u32 commit_win_id;
    char *commit_draft;
    bool draft_amend;
    CmdStatus commit_status;
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
    i32 rebase_step;
    i32 rebase_total;
};

static bool fuss_picker_result(Ed *ed, FussPickAction action,
                               const GitResult *result);
static void fuss_picker_open(Ed *ed, FussPickAction action,
                             const char *title);
static void fuss_actions_open(Ed *ed);
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
static bool fuss_expand_start(Ed *ed);
static void fuss_expand_tick(Ed *ed);
static bool fuss_buffer_bytes(const Buffer *buffer, Bytebuf *out);
static i32 fuss_row(const FussMode *f);
static void fuss_select_row(FussMode *f, i32 row);
static bool fuss_open_files_refresh(Ed *ed);
static void fuss_apply_effective(Ed *ed);
static char *fuss_dup_bytes(const char *bytes, size_t len);

static void fuss_viewer_close(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->viewer) {
        Win *owner = yew_ed_win_by_id(ed, f->viewer_win_id);

        if (owner != NULL)
            yew_panel_close(ed, &owner->panel);
    }
    f->viewer = false;
    f->viewer_win_id = 0U;
    f->viewer_buffer_id = 0U;
    fuss_damage(ed);
}

static bool fuss_viewer_open(Ed *ed, Buffer *buffer)
{
    FussMode *f;
    Bytebuf body;
    PanelSpec spec;
    Rect content;
    Rect drawer;
    FussDrawerLayout layout;
    u32 area_right;
    u32 drawer_right;
    u16 anchor_x;
    u16 room;

    if (ed == NULL || buffer == NULL || ed->fuss == NULL)
        return false;
    f = ed->fuss;
    if (!f->active || ed->win == NULL)
        return false;
    if (!fuss_buffer_bytes(buffer, &body))
        return false;
    if (body.len > UINT32_MAX) {
        bytebuf_free(&body);
        return false;
    }
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = buffer->name == NULL ? "preview" : buffer->name;
    spec.body = body.data;
    spec.len = (u32)body.len;
    content = yew_fuss_backdrop_rect(ed);
    drawer = yew_fuss_drawer_rect(ed);
    layout = yew_fuss_drawer_layout(ed->grid.cols, f->natural_cols);
    if (layout.fullscreen) {
        Rect area = content;

        if (content.w < 3U || content.h < 3U) {
            bytebuf_free(&body);
            f->viewer = true;
            f->viewer_win_id = ed->win->id;
            f->viewer_buffer_id = buffer->id;
            return true;
        }
        if (area.w > 4U) {
            area.x = (u16)(area.x + 2U);
            area.w = (u16)(area.w - 4U);
        }
        if (area.h > 4U) {
            area.y = (u16)(area.y + 2U);
            area.h = (u16)(area.h - 4U);
        }
        spec.place = YEW_PANEL_CENTER;
        spec.area = area;
        spec.has_area = true;
        spec.max_w = area.w;
        if (spec.max_w > YEW_PANEL_MAX_W)
            spec.max_w = YEW_PANEL_MAX_W;
        spec.max_h = area.h;
        if (spec.max_h > YEW_PANEL_MAX_H)
            spec.max_h = YEW_PANEL_MAX_H;
        spec.role = "git.ignored";
        if (!yew_panel_open(ed, &ed->win->panel, &spec)) {
            bytebuf_free(&body);
            return false;
        }
        bytebuf_free(&body);
        f->viewer = true;
        f->viewer_win_id = ed->win->id;
        f->viewer_buffer_id = buffer->id;
        ed->full_damage = true;
        fuss_damage(ed);
        return true;
    }
    area_right = (u32)ed->win->rect.x + ed->win->rect.w;
    drawer_right = (u32)drawer.x + drawer.w;
    anchor_x = ed->win->rect.x;
    if (drawer_right > anchor_x && drawer_right <= UINT16_MAX)
        anchor_x = (u16)drawer_right;
    spec.x = anchor_x;
    spec.y = ed->win->rect.y;
    spec.place = YEW_PANEL_CURSOR;
    room = area_right > (u32)anchor_x + 2U ?
               (u16)(area_right - anchor_x - 2U) : 0U;
    spec.max_w = room < YEW_PANEL_MAX_W ? room : YEW_PANEL_MAX_W;
    spec.max_h = YEW_PANEL_MAX_H;
    spec.role = "git.ignored";
    if (room == 0U) {
        bytebuf_free(&body);
        f->viewer = true;
        f->viewer_win_id = ed->win->id;
        f->viewer_buffer_id = buffer->id;
        return true;
    }
    if (!yew_panel_open(ed, &ed->win->panel, &spec)) {
        bytebuf_free(&body);
        return false;
    }
    bytebuf_free(&body);
    f->viewer = true;
    f->viewer_win_id = ed->win->id;
    f->viewer_buffer_id = buffer->id;
    ed->full_damage = true;
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

static bool fuss_jump_printable(const Key *key)
{
    if (key == NULL || key->ntext != 1U)
        return false;
    if ((key->mods & (YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SUPER |
                      YEW_MOD_HYPER | YEW_MOD_META)) != 0U)
        return false;
    /* Space expands/collapses the selected directory.  Every other ordinary
     * printable byte can occur in a filename and belongs to type-to-jump. */
    return key->text[0] > 0x20U && key->text[0] != 0x7fU;
}

bool yew_fuss_jump_key(FussJump *jump, const Key *key, i64 now_ms,
                       const PickItem *items, u32 n, u32 *sel)
{
    const char **labels;
    FzRanked *ranked;
    u32 matched;
    u32 i;
    bool printable;

    if (jump == NULL || key == NULL)
        return false;
    printable = fuss_jump_printable(key);
    if (key->code == YEW_KEY_BACKSPACE) {
        if (!jump->armed)
            return false;
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
        if (!jump->armed)
            yew_fuss_jump_arm(jump, now_ms);
        if (jump->type.len == 0U || now_ms >= jump->deadline_ms)
            jump->type.len = 0U;
        if (jump->type.len + 1U < (u32)YEW_TYPEJUMP_PAT_MAX) {
            jump->type.pat[jump->type.len++] = (char)key->text[0];
            jump->type.pat[jump->type.len] = '\0';
        }
        jump->deadline_ms = now_ms + (i64)YEW_TYPEJUMP_RESET_MS;
        jump->type.deadline_ms = jump->deadline_ms;
    }
    if (items == NULL || n == 0U || sel == NULL)
        return true;
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
    yew_xfree(labels);
    yew_xfree(ranked);
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
    if (ed == NULL || ed->fuss == NULL)
        return;
    ed->fuss->draw_dirty = true;
    ed->footer_dirty = true;
}

static bool fuss_show_buffer(Ed *ed, Buffer *buffer)
{
    return fuss_viewer_open(ed, buffer);
}


void yew_fuss_win_releasing(Ed *ed, u32 win_id)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL || win_id == 0U)
        return;
    f = ed->fuss;
    if (f->viewer_win_id == win_id) {
        f->viewer = false;
        f->viewer_win_id = 0U;
        f->viewer_buffer_id = 0U;
    }
    if (f->commit_win_id == win_id) {
        f->commit_win_id = 0U;
        f->commit_editing = false;
        f->commit_amend = false;
        f->commit_comment = 0U;
        f->commit_cancel_pending = true;
    }
}

void yew_fuss_windows_changed(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->active && fuss_open_files_refresh(ed)) {
        i32 row;

        fuss_apply_effective(ed);
        row = fuss_row(f);
        if (row >= 0)
            fuss_select_row(f, row);
        fuss_damage(ed);
    }
    if (!f->commit_cancel_pending)
        return;
    f->commit_cancel_pending = false;
    if (ed->cmdline.active && ed->cmdline.modal)
        yew_cmdline_close(ed, false);
    yew_msg(ed, YEW_MSG_INFO,
            "commit aborted because its editor view closed");
    if (yew_mode_enter(ed, YEW_MODE_F) != YEW_CMD_OK)
        (void)yew_mode_enter(ed, YEW_MODE_L);
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
    Bytebuf text;
    char title[128];

    if (ed == NULL || result == NULL || result->out_len > (u64)SIZE_MAX)
        return;
    (void)snprintf(title, sizeof(title), "*git %s%s*",
                    result->verb == NULL ? "output" : result->verb,
                    result->state == YEW_GIT_OK ? "" : " failed");
    buffer = yew_ws_scratch_new(ed, title,
        YEW_BUF_NOUNDO | YEW_BUF_READONLY | YEW_BUF_TRANSIENT);
    if (buffer == NULL || buffer->tb == NULL)
        return;
    bytebuf_init(&text);
    bytebuf_append(&text, result->out, (size_t)result->out_len);
    if (result->err_len != 0U && result->err_len <= (u64)SIZE_MAX) {
        if (text.len != 0U)
            bytebuf_push_u8(&text, (u8)'\n');
        bytebuf_append(&text, result->err, (size_t)result->err_len);
    }
    if (!fuss_replace_buffer(ed, buffer, text.data, (u64)text.len)) {
        bytebuf_free(&text);
        yew_ws_scratch_drop(ed, buffer);
        yew_msg(ed, YEW_MSG_ERROR, "could not update Git viewer buffer");
        return;
    }
    bytebuf_free(&text);
    yew_undo_mark_saved(buffer->undo);
    if (yew_tab_open_transient(ed, buffer) < 0)
        yew_ws_scratch_drop(ed, buffer);
    else
        yew_msg(ed, result->state == YEW_GIT_OK ? YEW_MSG_INFO : YEW_MSG_ERROR,
                "Git %s%s; Esc closes output",
                result->verb == NULL ? "command" : result->verb,
                result->state == YEW_GIT_OK ? " complete" : " failed");
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

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    if (f->pending_job == 0U)
        return;
    /* A background result must not cancel a different modal's draft. */
    if (ed->cmdline.active || yew_picker_active(ed))
        return;
    result = yew_git_result(ed);
    if (result == NULL || result->job_id != f->pending_job ||
        result->job_id == f->seen_result_job)
        return;
    f->seen_result_job = result->job_id;
    if (f->bridge.job_id == result->job_id)
        yew_git_modal_close(ed, &f->bridge);
    f->pending_job = 0U;
    pick = f->pending_pick;
    f->pending_pick = FUSS_PICK_NONE;
    if (f->commit_draft != NULL && result->verb != NULL &&
        (strcmp(result->verb, "commit") == 0 ||
         strcmp(result->verb, "commit-amend") == 0)) {
        if (result->state != YEW_GIT_OK) {
            char *draft = f->commit_draft;

            fuss_show_result(ed, result);
            if (fuss_commit_begin(ed, f->draft_amend, (const u8 *)draft,
                                  strlen(draft)) == YEW_CMD_OK) {
                yew_xfree(ed->cmdline.modal_help);
                ed->cmdline.modal_help = yew_xstrdup(
                    "Commit failed; draft retained. Esc to inspect the error tab.");
            }
            f->pending_view = false;
            return;
        }
        yew_xfree(f->commit_draft);
        f->commit_draft = NULL;
    }
    if (result->state == YEW_GIT_OK) {
        const GitVerb *completed = result->verb == NULL ? NULL :
                                   yew_git_verb(result->verb);
        const GitSnapshot *snapshot;

        if (completed != NULL && completed->kind != YEW_GV_READ) {
            snapshot = yew_git_snapshot_cached(ed);
            f->mutation_snapshot_gen = snapshot == NULL ? 0U : snapshot->gen;
            f->mutation_snapshot_pending = true;
        }
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
            yew_cmdline_open_modal(ed, "Interactive rebase",
                "Review the range, then type 'rebase' to continue",
                NULL, false, fuss_prompt_done, f);
            if (ed->cmdline.active)
                yew_msg(ed, YEW_MSG_WARN,
                        "review the rebase range, then type 'rebase' to continue");
            else
                fuss_prompt_clear(f);
        } else if (pick != FUSS_PICK_NONE)
            (void)fuss_picker_result(ed, pick, result);
        else if (f->pending_view || result->out_len != 0U || result->err_len != 0U)
            fuss_show_result(ed, result);
        else
            yew_msg(ed, YEW_MSG_INFO, "%s complete",
                    result->verb == NULL ? "git" : result->verb);
        if (result->verb != NULL &&
            strcmp(result->verb, "branch-delete") == 0 &&
            f->prompt_action == FUSS_PROMPT_NONE) {
            yew_xfree(f->prompt_path);
            f->prompt_path = NULL;
        }
    } else if (result->verb != NULL &&
               strcmp(result->verb, "branch-delete") == 0 &&
               f->prompt_path != NULL &&
               f->prompt_action == FUSS_PROMPT_NONE &&
               fuss_result_has(result->err, result->err_len,
                               "not fully merged")) {
        Bytebuf help;

        fuss_show_result(ed, result);
        f->prompt_action = FUSS_PROMPT_BRANCH_DELETE;
        bytebuf_init(&help);
        bytebuf_printf(&help,
            "Branch %s is not fully merged. Type 'delete' to force-delete it.",
            f->prompt_path);
        bytebuf_push_u8(&help, 0U);
        yew_cmdline_open_modal(ed, "Force-delete branch",
            (const char *)help.data,
            NULL, false, fuss_prompt_done, f);
        bytebuf_free(&help);
        if (ed->cmdline.active)
            yew_msg(ed, YEW_MSG_WARN,
                    "force-delete branch %s — type 'delete' to confirm",
                    f->prompt_path);
        else
            fuss_prompt_clear(f);
    } else if (result->err != NULL && result->err_len != 0U) {
        fuss_show_result(ed, result);
    } else {
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                yew_git_state_str(result->state));
    }
    if (result->state != YEW_GIT_OK && result->verb != NULL &&
        strcmp(result->verb, "branch-delete") == 0 &&
        f->prompt_action == FUSS_PROMPT_NONE) {
        yew_xfree(f->prompt_path);
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

static int fuss_path_ref_cmp(const void *left, const void *right, void *ctx)
{
    const FussPathRef *a = left;
    const FussPathRef *b = right;
    u32 common = a->path_len < b->path_len ? a->path_len : b->path_len;
    int cmp = common == 0U ? 0 : memcmp(a->path, b->path, common);

    (void)ctx;
    if (cmp != 0)
        return (cmp > 0) - (cmp < 0);
    return (a->path_len > b->path_len) - (a->path_len < b->path_len);
}

static bool fuss_relative_path_valid(const char *path, size_t len)
{
    size_t at = 0U;

    if (path == NULL || len == 0U || path[0] == '/')
        return false;
    while (at < len) {
        size_t start = at;
        size_t part_len;

        while (at < len && path[at] != '/')
            at++;
        part_len = at - start;
        if (part_len == 0U ||
            (part_len == 1U && path[start] == '.') ||
            (part_len == 2U && path[start] == '.' &&
             path[start + 1U] == '.'))
            return false;
        at++;
    }
    return path[len - 1U] != '/';
}

static void fuss_open_scratch_push(FussMode *f, const char *path,
                                   const char *root)
{
    size_t path_len;
    size_t root_len;
    const char *relative;
    size_t relative_len;

    if (f == NULL || path == NULL || path[0] == '\0' || root == NULL)
        return;
    path_len = fuss_cstr_len(path);
    root_len = fuss_cstr_len(root);
    relative = NULL;
    relative_len = 0U;
    if (path[0] != '/') {
        while (path_len >= 2U && path[0] == '.' && path[1] == '/') {
            path += 2U;
            path_len -= 2U;
        }
        relative = path;
        relative_len = path_len;
    } else if (root_len == 1U && root[0] == '/' && path_len > 1U) {
        relative = path + 1U;
        relative_len = path_len - 1U;
    } else if (root_len != 0U && path_len > root_len + 1U &&
               memcmp(path, root, root_len) == 0 && path[root_len] == '/') {
        relative = path + root_len + 1U;
        relative_len = path_len - root_len - 1U;
    }
    if (relative == NULL || !fuss_relative_path_valid(relative, relative_len) ||
        relative_len > UINT32_MAX)
        return;
    if (f->open_scratch_len == f->open_scratch_cap) {
        u32 cap = f->open_scratch_cap == 0U ? 16U : f->open_scratch_cap;

        if (cap > UINT32_MAX / 2U)
            cap = UINT32_MAX;
        else
            cap *= 2U;
        if (cap <= f->open_scratch_len)
            YEW_BUG("FUSS open-path scratch exceeds UINT32_MAX rows");
        f->open_scratch = yew_xreallocarray(
            f->open_scratch, cap, sizeof(*f->open_scratch));
        f->open_scratch_cap = cap;
    }
    f->open_scratch[f->open_scratch_len++] =
        (FussPathRef){relative, (u32)relative_len};
}

static void fuss_collect_pane_paths(FussMode *f, const Pane *root,
                                    const char *workspace)
{
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    yew_pane_collect_leaves((Pane *)root, leaves, YEW_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++) {
        const Win *win = leaves[i] == NULL ? NULL : leaves[i]->win;
        const Buffer *buffer = win == NULL ? NULL : win->buf;

        if (buffer != NULL && (buffer->flags & YEW_BUF_SCRATCH) == 0U &&
            buffer->meta.realpath != NULL)
            fuss_open_scratch_push(f, buffer->meta.realpath, workspace);
    }
}

static bool fuss_open_files_refresh(Ed *ed)
{
    FussMode *f;
    const char *workspace;
    u32 unique;
    size_t tab;
    u32 i;
    bool changed;

    if (ed == NULL || ed->fuss == NULL)
        return false;
    f = ed->fuss;
    workspace = yew_ws_root(ed);
    f->open_scratch_len = 0U;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        const Tab *item = &ed->tabs.v.data[tab];

        fuss_open_scratch_push(f, item->path, workspace);
        fuss_collect_pane_paths(f, item->root, workspace);
    }
    fuss_collect_pane_paths(f, ed->pane_root, workspace);
    yew_sort_stable(f->open_scratch, f->open_scratch_len,
                    sizeof(*f->open_scratch), fuss_path_ref_cmp, NULL);
    unique = 0U;
    for (i = 0U; i < f->open_scratch_len; i++) {
        if (unique != 0U &&
            fuss_path_ref_cmp(&f->open_scratch[unique - 1U],
                              &f->open_scratch[i], NULL) == 0)
            continue;
        f->open_scratch[unique++] = f->open_scratch[i];
    }
    f->open_scratch_len = unique;
    changed = f->open_files.len != unique;
    for (i = 0U; !changed && i < unique; i++) {
        const FussOpenPath *old = &f->open_files.data[i];
        const FussPathRef *now = &f->open_scratch[i];

        changed = old->path_len != now->path_len ||
                  memcmp(old->path, now->path, now->path_len) != 0;
    }
    if (!changed)
        return false;
    while (f->open_files.len != 0U) {
        const FussOpenPath *last =
            &f->open_files.data[f->open_files.len - 1U];

        (void)yew_fuss_open_memory_set(&f->open_files, last->path,
                                        last->path_len, false);
    }
    for (i = 0U; i < unique; i++)
        (void)yew_fuss_open_memory_set(&f->open_files,
                                       f->open_scratch[i].path,
                                       f->open_scratch[i].path_len, true);
    return true;
}

static void fuss_apply_effective(Ed *ed)
{
    FussMode *f;
    u16 old_natural;
    u32 i;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    old_natural = f->natural_cols;
    if (f->open_refs_cap < f->open_files.len) {
        f->open_refs = yew_xreallocarray(f->open_refs, f->open_files.len,
                                          sizeof(*f->open_refs));
        f->open_refs_cap = f->open_files.len;
    }
    for (i = 0U; i < f->open_files.len; i++) {
        f->open_refs[i].path = f->open_files.data[i].path;
        f->open_refs[i].path_len = f->open_files.data[i].path_len;
    }
    yew_fuss_apply_expansion(&f->tree, &f->manual_open, f->open_refs,
                             f->open_files.len);
    f->natural_cols = yew_fuss_tree_natural_width(&f->tree);
    if (f->natural_cols != old_natural) {
        f->backdrop_dirty = true;
        ed->layout_dirty = true;
        ed->full_damage = true;
    }
}

static void fuss_build(Ed *ed, const GitSnapshot *snap, bool force)
{
    FussMode *f = ed->fuss;
    const GitRepo *repo;

    if (f == NULL || snap == NULL)
        return;
    repo = yew_git_repo_cached(ed);
    if (repo != NULL && repo->top_level != NULL)
        (void)yew_fuss_tree_scope_roots(&f->tree, repo->top_level,
                                         yew_ws_root(ed));
    if (!force && f->tree.snap_gen == snap->gen)
        return;
    yew_fuss_build(&f->tree, snap, &f->opts);
    fuss_rebase_progress_update(ed, snap);
    if (f->opts.all_files && f->walk == NULL && f->files.paths.len != 0U)
        (void)yew_fuss_merge_files(&f->tree, &f->files, snap, &f->opts);
    (void)fuss_open_files_refresh(ed);
    fuss_apply_effective(ed);
    if (fuss_row(f) < 0 && f->tree.items.len != 0U)
        fuss_select_row(f, 0);
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
    yew_fuss_open_memory_init(&f->manual_open);
    yew_fuss_open_memory_init(&f->open_files);
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
    yew_xfree(f->commit_draft);
    yew_git_modal_close(ed, &f->bridge);
    f->commit_editing = false;
    f->active = false;
    yew_xfree(f->prompt_path);
    {
        u32 i;

        for (i = 0U; i < f->picker_count; i++) {
            yew_xfree((char *)f->picker_items[i].label);
            yew_xfree((char *)f->picker_items[i].detail);
            yew_xfree(f->picker_values[i]);
        }
    }
    yew_xfree(f->picker_items);
    yew_xfree(f->picker_values);
    yew_xfree(f->picker_aux);
    yew_xfree(f->picker_aux2);
    if (f->preview_job != 0U)
        (void)yew_job_signal(ed, f->preview_job, SIGTERM);
    yew_xfree(f->preview_value);
    bytebuf_free(&f->preview_bytes);
    if (f->walk != NULL)
        yew_walk_end(f->walk);
    fuss_expand_clear(f);
    yew_filelist_free(&f->files);
    yew_filelist_free(&f->expand_files);
    yew_fuss_open_memory_drop(&f->manual_open);
    yew_fuss_open_memory_drop(&f->open_files);
    yew_xfree(f->open_scratch);
    yew_xfree(f->open_refs);
    yew_fuss_sel_clear(&f->sel);
    yew_fuss_tree_drop(&f->tree);
    yew_xfree(f);
    ed->fuss = NULL;
}

void yew_fuss_workspace_changed(Ed *ed)
{
    FussMode *f;

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    yew_fuss_open_memory_drop(&f->manual_open);
    yew_fuss_open_memory_init(&f->manual_open);
    yew_fuss_open_memory_drop(&f->open_files);
    yew_fuss_open_memory_init(&f->open_files);
    f->open_scratch_len = 0U;
    if (f->active) {
        (void)fuss_open_files_refresh(ed);
        fuss_apply_effective(ed);
        fuss_damage(ed);
    }
}

bool yew_fuss_active(const Ed *ed)
{
    return ed != NULL && ed->fuss != NULL && ed->fuss->active;
}

CmdStatus yew_fuss_mode_enter(Ed *ed)
{
    const GitSnapshot *snap;
    OptVal ascii;

    if (ed == NULL || ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    if (yew_opt_get(ed, NULL, NULL, "git.ascii_glyphs",
                    (u32)(sizeof("git.ascii_glyphs") - 1U), &ascii) &&
        ascii.type == (u8)YEW_OPT_BOOL)
        ed->fuss->ascii_glyphs = ascii.as.b;
    if (!ed->fuss->active) {
        ed->fuss->opts.all_files = yew_state_option_bool(
            ed, "git.tree.all_files", true);
        ed->fuss->opts.show_hidden = yew_state_option_bool(
            ed, "git.tree.show_hidden", false);
        ed->fuss->active = true;
        /* F mode owns a deterministic loading frame even when Sprint 53's
         * editor/statusline consumers have already warmed the Git cache.
         * Besides preserving the mode-entry contract, this keeps a fast
         * repository from collapsing entry and publication into one paint. */
        ed->fuss->opening = true;
        ed->fuss->opening_until_ms =
            ed->now_ms > INT64_MAX - FUSS_OPENING_FRAME_MS ? INT64_MAX :
            ed->now_ms + FUSS_OPENING_FRAME_MS;
        ed->fuss->saved_buffer_id = ed->win != NULL && ed->win->buf != NULL ?
                                    ed->win->buf->id : 0U;
        ed->fuss->backdrop_dirty = true;
        ed->fuss->seen_detect_state = yew_git_detect_state(ed);
        ed->fuss->seen_detect_result = yew_git_detect_result(ed);
        ed->layout_dirty = true;
        ed->full_damage = true;
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
            (void)fuss_open_files_refresh(ed);
            fuss_apply_effective(ed);
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

    if (ed == NULL || ed->fuss == NULL)
        return;
    f = ed->fuss;
    f->active = false;
    fuss_expand_clear(f);
    yew_fuss_jump_init(&f->jump);
    fuss_viewer_close(ed);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
}

u16 yew_fuss_footer_rows(const Ed *ed)
{
    return yew_fuss_active(ed) ? 2U : 0U;
}

void yew_fuss_tick(Ed *ed, i64 now_ms)
{
    const GitSnapshot *snap;
    GitAsyncState detect_state;
    GitStatusCode detect_result;

    if (ed == NULL || ed->fuss == NULL)
        return;
    yew_git_modal_tick(ed, &ed->fuss->bridge, now_ms);
    fuss_result_tick(ed);
    if (!yew_fuss_active(ed))
        return;
    if (ed->fuss->opening && now_ms >= ed->fuss->opening_until_ms) {
        ed->fuss->opening = false;
        fuss_damage(ed);
    }
    detect_state = yew_git_detect_state(ed);
    detect_result = yew_git_detect_result(ed);
    if (detect_state != ed->fuss->seen_detect_state ||
        detect_result != ed->fuss->seen_detect_result) {
        /* Repository detection owns both the FUSS header and footer.  The
         * footer is dirtied by message publication, but a completion that
         * lands after the opening-frame timer otherwise leaves the header's
         * `loading` suffix cached until unrelated input causes a repaint. */
        ed->fuss->seen_detect_state = detect_state;
        ed->fuss->seen_detect_result = detect_result;
        fuss_damage(ed);
    }
    snap = yew_git_snapshot(ed);
    if (snap != NULL)
        fuss_rebuild(ed, snap);
    if (ed->fuss->mutation_snapshot_pending && snap != NULL &&
        snap->gen != ed->fuss->mutation_snapshot_gen &&
        !yew_git_refresh_busy(ed)) {
        /* An idle hook is the public observation point for asynchronous
         * FUSS state.  Rearm it only after the mutation's final refresh,
         * rather than at the command result: an older in-flight status may
         * publish first and would make a state checker observe stale Git.
         * The loop consumes this request only after queued input drains. */
        ed->fuss->mutation_snapshot_pending = false;
        ed->fl_idle_pending = true;
        ed->fl_idle_fired = false;
    }
    if (ed->fuss->walk != NULL &&
        !yew_walk_step(ed->fuss->walk, 2000)) {
        yew_walk_end(ed->fuss->walk);
        ed->fuss->walk = NULL;
        if (snap != NULL &&
            yew_fuss_merge_files(&ed->fuss->tree, &ed->fuss->files, snap,
                                  &ed->fuss->opts)) {
            (void)fuss_open_files_refresh(ed);
            fuss_apply_effective(ed);
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

    if (ed == NULL || ed->fuss == NULL)
        return -1;
    f = ed->fuss;
    if (f->bridge.dir != NULL && !f->bridge.waiting)
        return f->bridge.next_poll <= now_ms ? 0 : f->bridge.next_poll - now_ms;
    if (!f->active)
        return -1;
    if (f->opening) {
        if (f->opening_until_ms <= now_ms)
            return 0;
        if (now_ms < 0 && f->opening_until_ms > INT64_MAX + now_ms)
            return INT64_MAX;
        return f->opening_until_ms - now_ms;
    }
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

    /* Modal input belongs to its private prompt, never FUSS type-to-jump. */
    if (!yew_fuss_active(ed) || ed->mode != YEW_MODE_F || key == NULL)
        return false;
    f = ed->fuss;
    /* Once dispatch owns a structural chord, its printable tail must reach
     * the keymap rather than begin a new search (C-w s is the important
     * shipped case). */
    if (ed->chord.n != 0U)
        return false;
    if (!fuss_jump_printable(key) && key->code != YEW_KEY_BACKSPACE) {
        if (f->jump.armed) {
            yew_fuss_jump_init(&f->jump);
            ed->footer_dirty = true;
        }
        return false;
    }
    if (!f->jump.armed && key->code == YEW_KEY_BACKSPACE)
        return false;
    if (!fuss_jump_items(f, &items, &n)) {
        consumed = yew_fuss_jump_key(&f->jump, key, now_ms,
                                     NULL, 0U, NULL);
        if (consumed)
            fuss_damage(ed);
        return consumed;
    }
    row = fuss_row(f) < 0 ? 0U : (u32)fuss_row(f);
    consumed = yew_fuss_jump_key(&f->jump, key, now_ms, items, n, &row);
    yew_xfree(items);
    if (consumed && row < n) {
        fuss_select_row(f, (i32)row);
        fuss_damage(ed);
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

static ThemeEnt fuss_surface_style(const Ed *ed)
{
    const ThemeEnt *themed = yew_theme_ui_tab(ed, "git.drawer");

    return themed == NULL ? fuss_base_style(ed) : *themed;
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

static void fuss_edge(Ed *ed, Rect drawer, FussDrawerLayout layout,
                      u16 rows)
{
    ThemeEnt fallback = fuss_surface_style(ed);
    ThemeEnt style;
    const char *glyph;
    size_t glyph_len;
    u16 row;

    if (layout.fullscreen || layout.edge_col == UINT16_MAX || rows == 0U ||
        layout.edge_col >= drawer.w)
        return;
    fallback.fg = (YewColor){YEW_COLOR_RGB, 255U, 255U, 255U};
    fallback.attrs = (u16)(fallback.attrs | YEW_ATTR_BOLD);
    style = fuss_role_style(ed, "git.drawer.edge", fallback);
    glyph = ed->fuss->ascii_glyphs ? "|" : "│";
    glyph_len = ed->fuss->ascii_glyphs ? 1U : sizeof("│") - 1U;
    if (rows > drawer.h)
        rows = drawer.h;
    for (row = 0U; row < rows; row++) {
        u16 col = (u16)(drawer.x + layout.edge_col);

        fuss_put_lit(&ed->grid, (u16)(drawer.y + row), &col,
                     (u16)(col + 1U), glyph, glyph_len,
                     style.fg, style.bg, style.attrs);
    }
}

static void fuss_header(Ed *ed, u16 row, u16 left, u16 right)
{
    const GitSnapshot *snap;
    ThemeEnt style = fuss_surface_style(ed);
    char suffix[96] = {0};
    const char *branch;
    size_t suffix_len;
    int branch_cells;
    int suffix_cells;
    u16 prefix_cells;
    u16 prefix_right;
    u16 title_right;
    u16 col = left;

    const char *root = yew_ws_root(ed);
    const char *title = root;
    const char *slash;

    slash = root == NULL ? NULL : strrchr(root, '/');
    if (slash != NULL && slash[1] != '\0')
        title = slash + 1;
    if (title == NULL || title[0] == '\0')
        title = "/";
    if (ed->fuss->opening) {
        fuss_put_lit(&ed->grid, row, &col, right, title,
                     fuss_cstr_len(title), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
        fuss_put_lit(&ed->grid, row, &col, right, " · loading",
                     sizeof(" · loading") - 1U, style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_DIM));
        return;
    }
    if (yew_git_detect_state(ed) == YEW_GIT_ASYNC_FAILED) {
        fuss_put_lit(&ed->grid, row, &col, right, title,
                     fuss_cstr_len(title), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
        return;
    }
    snap = yew_git_snapshot(ed);
    if (snap == NULL || snap->gen == 0U) {
        fuss_put_lit(&ed->grid, row, &col, right, title,
                     fuss_cstr_len(title), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
        fuss_put_lit(&ed->grid, row, &col, right, " · loading",
                     sizeof(" · loading") - 1U, style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_DIM));
        return;
    }
    if (snap->ahead > 0 && snap->behind > 0)
        (void)snprintf(suffix, sizeof(suffix), " ↑%d ↓%d",
                       snap->ahead, snap->behind);
    else if (snap->ahead > 0)
        (void)snprintf(suffix, sizeof(suffix), " ↑%d", snap->ahead);
    else if (snap->behind > 0)
        (void)snprintf(suffix, sizeof(suffix), " ↓%d", snap->behind);
    suffix_len = fuss_cstr_len(suffix);
    if (snap->state == YEW_GIT_MID_REBASE) {
        int n;

        if (ed->fuss->rebase_step > 0 && ed->fuss->rebase_total > 0)
            n = snprintf(suffix + suffix_len,
                         sizeof(suffix) - suffix_len, " REBASING %d/%d",
                         ed->fuss->rebase_step, ed->fuss->rebase_total);
        else
            n = snprintf(suffix + suffix_len,
                         sizeof(suffix) - suffix_len, " REBASING");
        if (n > 0 && (size_t)n < sizeof(suffix) - suffix_len)
            suffix_len += (size_t)n;
    }
    suffix_cells = yew_str_width((const u8 *)suffix, suffix_len, 1U);
    if (suffix_cells < 0)
        suffix_cells = 0;
    prefix_right = right;
    if ((u16)suffix_cells < (u16)(right - left))
        prefix_right = (u16)(right - (u16)suffix_cells);
    else if (suffix_cells != 0)
        prefix_right = left;

    branch = snap->branch == NULL ? "(detached)" : snap->branch;
    branch_cells = yew_str_width((const u8 *)branch,
                                 fuss_cstr_len(branch), 1U);
    if (branch_cells < 0)
        branch_cells = 0;
    prefix_cells = (u16)(prefix_right - left);
    title_right = prefix_right;
    if (prefix_cells > 4U) {
        u16 branch_budget = (u16)branch_cells;

        if (branch_budget > (u16)(prefix_cells - 4U))
            branch_budget = (u16)(prefix_cells - 4U);
        title_right = (u16)(prefix_right - branch_budget - 3U);
    }
    col = left;
    fuss_put_lit(&ed->grid, row, &col, title_right, title,
                 fuss_cstr_len(title), style.fg, style.bg,
                 (u16)(style.attrs | YEW_ATTR_BOLD));
    if (title_right < prefix_right) {
        fuss_put_lit(&ed->grid, row, &col, prefix_right, " · ",
                     sizeof(" · ") - 1U, style.fg, style.bg,
                     style.attrs);
        fuss_put_lit(&ed->grid, row, &col, prefix_right, branch,
                     fuss_cstr_len(branch), style.fg, style.bg,
                     (u16)(style.attrs | YEW_ATTR_BOLD));
    }
    col = prefix_right;
    fuss_put_lit(&ed->grid, row, &col, right, suffix, suffix_len,
                 style.fg, style.bg, style.attrs);
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

static size_t fuss_tail_fit(const char *bytes, size_t len, u16 cells)
{
    size_t at = 0U;

    while (at < len && yew_str_width((const u8 *)bytes + at, len - at,
                                     YEW_VP_TABWIDTH) > (int)cells) {
        size_t next = yew_gb_next_bytes((const u8 *)bytes, len, at);

        if (next <= at || next > len)
            return len;
        at = next;
    }
    return at;
}

static void fuss_tree_row(Ed *ed, u16 row, u16 left, u16 right,
                          const FussItem *item, bool selected)
{
    FussMode *f = ed->fuss;
    const FussNode *node = fuss_node(f, item);
    ThemeEnt normal = fuss_surface_style(ed);
    ThemeEnt ignored = fuss_role_style(ed, "git.ignored", normal);
    ThemeEnt staged = fuss_role_style(ed, "git.staged", normal);
    ThemeEnt modified = fuss_role_style(ed, "git.modified", normal);
    ThemeEnt untracked = fuss_role_style(ed, "git.untracked", normal);
    ThemeEnt incoming = fuss_role_style(ed, "git.incoming", normal);
    ThemeEnt conflict = fuss_role_style(ed, "git.conflict", normal);
    Cell blank = ed->grid.blank;
    u16 selected_attrs = selected ? YEW_ATTR_REVERSE : 0U;
    u16 col = left;
    FussMarkerKind markers[4];
    u8 marker_count;
    u8 marker_i;
    u16 content_right = right;
    u32 indent_cells = (u32)item->depth * YEW_FUSS_INDENT_CELLS;
    u32 indent_skip = 0U;
    size_t name_at = 0U;

    if (node == NULL)
        return;
    marker_count = yew_fuss_marker_kinds(node, markers);
    if (selected) {
        u16 marker_cells = (u16)(marker_count * 2U);
        u16 row_cells = right > left ? (u16)(right - left) : 0U;
        u16 name_cells;
        u16 name_budget;
        u16 prefix_budget;
        int measured = yew_str_width((const u8 *)node->name,
                                     node->name_len, YEW_VP_TABWIDTH);

        if (marker_cells < row_cells)
            content_right = (u16)(right - marker_cells);
        else
            content_right = left;
        name_budget = content_right > left + 3U ?
                      (u16)(content_right - left - 3U) : 0U;
        name_cells = measured <= 0 ? 0U :
                     measured > UINT16_MAX ? UINT16_MAX : (u16)measured;
        if (name_cells > name_budget) {
            name_at = fuss_tail_fit(node->name, node->name_len, name_budget);
            indent_skip = indent_cells;
        } else {
            u32 keep;

            prefix_budget = (u16)(name_budget - name_cells);
            keep = prefix_budget < indent_cells ? prefix_budget : indent_cells;
            indent_skip = indent_cells - keep;
        }
    }
    blank.fg = normal.fg;
    blank.bg = normal.bg;
    blank.attrs = (u16)(normal.attrs | selected_attrs);
    yew_grid_fill(&ed->grid, row, left, right, blank);
    fuss_put_lit(&ed->grid, row, &col, content_right, " ", 1U,
                 normal.fg, normal.bg,
                 (u16)(normal.attrs | selected_attrs));
    while (indent_skip < indent_cells && col < content_right) {
        fuss_put_lit(&ed->grid, row, &col, content_right, " ", 1U,
                     normal.fg, normal.bg,
                     (u16)(normal.attrs | selected_attrs));
        indent_skip++;
    }
    fuss_put_lit(&ed->grid, row, &col, content_right,
                 node->is_file ? "  " : (node->expanded ? "- " : "+ "),
                 2U, normal.fg, normal.bg,
                 (u16)(normal.attrs | selected_attrs));
    fuss_put_lit(&ed->grid, row, &col, content_right, node->name + name_at,
                 node->name_len - name_at,
                 node->ignored ? ignored.fg : normal.fg, normal.bg,
                 (u16)((node->ignored ? ignored.attrs : normal.attrs) |
                       selected_attrs));
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

u16 yew_fuss_tree_natural_width(const FussTree *tree)
{
    u64 widest = 0U;
    size_t i;

    if (tree == NULL || tree->items.len == 0U)
        return 0U;
    for (i = 0U; i < tree->items.len; i++) {
        const FussItem *item = &tree->items.data[i];
        const FussNode *node;
        FussMarkerKind markers[4];
        u8 marker_count;
        int measured;
        u64 cells;

        if (item->node >= tree->nodes.len)
            continue;
        node = &tree->nodes.data[item->node];
        measured = yew_str_width((const u8 *)node->name, node->name_len,
                                 YEW_VP_TABWIDTH);
        if (measured < 0)
            measured = 0;
        marker_count = yew_fuss_marker_kinds(node, markers);
        cells = 1U + (u64)item->depth * YEW_FUSS_INDENT_CELLS + 2U +
                (u64)measured + (u64)marker_count * 2U + 1U +
                YEW_FUSS_DRAWER_EDGE_CELLS;
        if (cells > widest)
            widest = cells;
    }
    return widest > UINT16_MAX ? UINT16_MAX : (u16)widest;
}

FussDrawerLayout yew_fuss_drawer_layout(u16 content_cols,
                                        u16 natural_cols)
{
    FussDrawerLayout layout = {0U, 0U, UINT16_MAX, true};
    u32 base = ((u32)content_cols + 3U) / 4U;
    u32 want;
    u32 overlay_cap;

    if (base < YEW_FUSS_DRAWER_MIN_CELLS)
        base = YEW_FUSS_DRAWER_MIN_CELLS;
    if (base > YEW_FUSS_DRAWER_BASE_MAX_CELLS)
        base = YEW_FUSS_DRAWER_BASE_MAX_CELLS;
    if (base > content_cols)
        base = content_cols;
    want = natural_cols > base ? natural_cols : base;
    overlay_cap = content_cols > YEW_FUSS_EDITOR_RETAIN_CELLS ?
                  (u32)content_cols - YEW_FUSS_EDITOR_RETAIN_CELLS : 0U;
    if (content_cols < YEW_FUSS_DRAWER_MIN_CELLS || want > overlay_cap) {
        layout.width = content_cols;
        layout.tree_width = content_cols;
        return layout;
    }
    layout.width = (u16)want;
    layout.tree_width = layout.width - YEW_FUSS_DRAWER_EDGE_CELLS;
    layout.edge_col = layout.tree_width;
    layout.fullscreen = false;
    return layout;
}

Rect yew_fuss_backdrop_rect(const Ed *ed)
{
    FussDrawerLayout layout;
    Rect drawer;
    u16 top;
    u16 bottom;
    u16 natural;

    if (ed == NULL)
        return (Rect){0U, 0U, 0U, 0U};
    natural = ed->fuss == NULL ? 0U : ed->fuss->natural_cols;
    layout = yew_fuss_drawer_layout(ed->grid.cols, natural);
    bottom = ed->footer_rect.h == 0U ? ed->grid.rows : ed->footer_rect.y;
    if (layout.fullscreen)
        return (Rect){0U, 0U, ed->grid.cols, bottom};
    drawer = yew_fuss_drawer_rect(ed);
    top = (u16)(ed->tab_strip_rect.y + ed->tab_strip_rect.h);
    return (Rect){drawer.w, top,
                  ed->grid.cols > drawer.w ?
                      (u16)(ed->grid.cols - drawer.w) : 0U,
                  bottom > top ? (u16)(bottom - top) : 0U};
}

Rect yew_fuss_drawer_rect(const Ed *ed)
{
    u16 natural;
    FussDrawerLayout layout;

    if (ed == NULL)
        return (Rect){0U, 0U, 0U, 0U};
    natural = ed->fuss == NULL ? 0U : ed->fuss->natural_cols;
    layout = yew_fuss_drawer_layout(ed->grid.cols, natural);
    return (Rect){0U, 0U, layout.width, ed->grid.rows};
}

/* ---------------------------------------------------------------- */
/* Sprint 57.13 §3: the mouse router's three seams                  */
/* ---------------------------------------------------------------- */

/*
 * The row a YEW_REGION_FUSS_ROW payload names, or NULL.
 *
 * The payload is an INTERNED PATH, not a row index, because the tree
 * rebuilds under the pointer — a background status result reorders it,
 * a walk merges files into it — and an index captured at paint time
 * names a different file by the press.  Resolving through the path is
 * what makes a click land on what was pointed at.
 */
static const FussItem *fuss_item_by_path_id(const Ed *ed, u32 path_id)
{
    const FussMode *f;
    const char *path;
    size_t len;
    size_t i;

    if (!yew_fuss_active(ed) || path_id == 0U)
        return NULL;
    f = ed->fuss;
    path = yew_intern_str(&ed->interner, path_id);
    len = yew_intern_len(&ed->interner, path_id);
    if (path == NULL || len == 0U)
        return NULL;
    for (i = 0U; i < f->tree.items.len; i++) {
        const FussItem *item = &f->tree.items.data[i];

        if (item->path_len == (u32)len &&
            memcmp(item->path, path, len) == 0)
            return item;
    }
    return NULL;
}

bool yew_fuss_path_is_dir(const Ed *ed, u32 path_id, bool *is_dir)
{
    const FussItem *item = fuss_item_by_path_id(ed, path_id);

    if (item == NULL || is_dir == NULL)
        return false;
    *is_dir = !item->is_file;
    return true;
}

bool yew_fuss_path_target(const Ed *ed, u32 path_id, FussTarget *out)
{
    const FussItem *item = fuss_item_by_path_id(ed, path_id);
    const FussNode *node;

    if (out == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    if (item == NULL)
        return false;
    node = fuss_node(ed->fuss, item);
    if (node == NULL)
        return false;
    out->known = true;
    /*
     * STATUS IS KNOWN BECAUSE THE NODE IS IN THE TREE, not because
     * anything below it is dirty: a clean file answers false to every
     * flag, and a menu that read that as "unknown" would grey `Stage`
     * on the one file the user just edited and unstaged.
     */
    out->status_known = true;
    out->is_file = node->is_file;
    out->staged = node->staged;
    out->unstaged = node->unstaged;
    out->untracked = node->untracked;
    out->incoming = node->incoming;
    out->conflicted = node->conflicted;
    out->expanded = !node->is_file && node->expanded;
    return true;
}

bool yew_fuss_state(const Ed *ed, YewFussState *out)
{
    const FussMode *f;
    const FussItem *item;
    const FussNode *node;
    const GitSnapshot *snap;

    if (out == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    if (!yew_fuss_active(ed))
        return false;
    f = ed->fuss;
    out->active = true;
    item = fuss_item(f, fuss_row(f));
    node = fuss_node(f, item);
    if (item != NULL && node != NULL) {
        out->path = item->path;
        out->path_len = item->path_len;
        out->known = true;
        out->is_file = node->is_file;
        out->staged = node->staged;
        out->unstaged = node->unstaged;
        out->untracked = node->untracked;
        out->expanded = !node->is_file && node->expanded;
    }
    out->actions_open = f->picker_action == FUSS_PICK_ACTIONS &&
                        yew_picker_active(ed);
    snap = yew_git_snapshot_cached(ed);
    out->clean = snap != NULL && snap->entries.len == 0U &&
                 (snap->state == YEW_GIT_OK ||
                  snap->state == YEW_GIT_NO_HEAD ||
                  snap->state == YEW_GIT_DETACHED ||
                  snap->state == YEW_GIT_NO_UPSTREAM);
    return true;
}

bool yew_fuss_selected_anchor(Ed *ed, u32 *path_id, u16 *x, u16 *y)
{
    FussMode *f;
    Rect tree;
    i32 row;
    const FussItem *item;
    u32 id;
    u32 offset;

    if (!yew_fuss_active(ed) || path_id == NULL || x == NULL || y == NULL)
        return false;
    f = ed->fuss;
    tree = yew_fuss_drawer_rect(ed);
    if (tree.w == 0U || tree.h == 0U)
        return false;
    row = yew_fuss_row_of(&f->tree, &f->sel);
    item = fuss_item(f, row);
    if (item == NULL || (u32)row < f->scroll)
        return false;
    /*
     * The +1 is the drawer's header row, and it is the SAME arithmetic
     * yew_fuss_draw uses to place each row's region — deriving it twice
     * is how a menu comes to open one row off the thing it names.
     */
    offset = (u32)row - f->scroll;
    if (offset + 1U >= (u32)tree.h)
        return false;
    id = yew_intern(&ed->interner, item->path, item->path_len);
    if (id == 0U || id > (u32)INT32_MAX)
        return false;
    *path_id = id;
    *x = tree.x;
    *y = (u16)(tree.y + 1U + offset);
    return true;
}

void yew_fuss_select_path(Ed *ed, u32 path_id)
{
    const FussItem *item = fuss_item_by_path_id(ed, path_id);

    if (item == NULL)
        return;
    /*
     * Selecting is not opening.  A single click moves the cursor and
     * nothing else — the double-click that opens is a separate gesture,
     * and a tree where one click opened a file would make browsing it
     * impossible.
     */
    yew_fuss_sel_set(&ed->fuss->sel, item->path, item->path_len);
    fuss_damage(ed);
}

void yew_fuss_scroll(Ed *ed, i32 rows)
{
    FussMode *f;
    i32 row;
    i32 last;

    if (!yew_fuss_active(ed) || rows == 0)
        return;
    f = ed->fuss;
    if (f->tree.items.len == 0U)
        return;
    /*
     * THROUGH THE SELECTION, exactly as the picker's wheel is (s26 §5).
     * The drawer has no independent scroll offset to move: `f->scroll`
     * is DERIVED from the selected row every time the tree is drawn, so
     * a wheel that moved the offset alone would be snapped back on the
     * next frame and the tree would look frozen.
     */
    last = (i32)f->tree.items.len - 1;
    row = yew_fuss_row_of(&f->tree, &f->sel);
    if (row < 0)
        row = 0;
    row += rows;
    if (row < 0)
        row = 0;
    if (row > last)
        row = last;
    fuss_select_row(f, row);
    fuss_damage(ed);
}

bool yew_fuss_draw_dirty(const Ed *ed)
{
    return yew_fuss_active(ed) && ed->fuss->draw_dirty;
}

void yew_fuss_draw(Ed *ed)
{
    FussMode *f;
    FussDrawerLayout layout;
    Rect content;
    Rect tree;
    u16 first;
    u16 visible;
    u16 tree_bottom;
    u16 tree_right;
    i32 selected;
    size_t i;
    Cell blank;
    ThemeEnt surface;

    if (!yew_fuss_active(ed))
        return;
    f = ed->fuss;
    yew_region_remove_kind(YEW_REGION_FUSS_ROW);
    content = yew_fuss_backdrop_rect(ed);
    blank = ed->grid.blank;
    if (f->backdrop_dirty || ed->full_damage) {
        Cell dim = blank;

        dim.attrs = YEW_ATTR_DIM;
        for (i = content.y; i < (size_t)content.y + content.h; i++)
            yew_grid_overlay(&ed->grid, (u16)i, content.x,
                             (u16)(content.x + content.w), &dim,
                             YEW_OVERLAY_ATTRS);
        f->backdrop_dirty = false;
    }
    tree = yew_fuss_drawer_rect(ed);
    if (tree.w == 0U || tree.h == 0U) {
        f->draw_dirty = false;
        return;
    }
    layout = yew_fuss_drawer_layout(ed->grid.cols, f->natural_cols);
    tree_right = (u16)(tree.x + layout.tree_width);
    surface = fuss_surface_style(ed);
    blank = ed->grid.blank;
    blank.fg = surface.fg;
    blank.bg = surface.bg;
    blank.attrs = surface.attrs;
    for (i = tree.y; i < (size_t)tree.y + tree.h; i++)
        yew_grid_fill(&ed->grid, (u16)i, tree.x,
                      (u16)(tree.x + tree.w), blank);
    fuss_header(ed, tree.y, tree.x, tree_right);
    tree_bottom = (u16)(tree.y + tree.h);
    if (layout.fullscreen && ed->footer_rect.h != 0U &&
        ed->footer_rect.y < tree_bottom)
        tree_bottom = ed->footer_rect.y;
    visible = tree_bottom > (u16)(tree.y + 1U) ?
                  (u16)(tree_bottom - tree.y - 1U) : 0U;
    if (f->opening) {
        fuss_edge(ed, tree, layout, tree.h);
        f->draw_dirty = false;
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

        fuss_tree_row(ed, row, tree.x, tree_right, item,
                      (i32)((size_t)first + i) == selected);
        path_id = item == NULL ? 0U :
                  yew_intern(&ed->interner, item->path, item->path_len);
        if (path_id <= (u32)INT32_MAX)
            yew_region_add(YEW_REGION_FUSS_ROW,
                           (Rect){tree.x, row, layout.tree_width, 1U},
                           (i32)path_id);
    }
    fuss_edge(ed, tree, layout, tree.h);
    f->draw_dirty = false;
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
        "type jump | Alt+key actions | C-S-/ list | Esc leave" :
        "type jump · Alt+key actions · C-S-/ list · Esc leave";
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

/* `FussTarget` itself is in fussmode.h: the context menus turn on the
 * same flags these guards do, and two spellings of one answer is how a
 * greyed row and a refused command come to disagree. */
typedef enum FussTargetGuard {
    FUSS_TARGET_ANY,
    FUSS_TARGET_FILE,
    /*
     * Staged, FILE OR DIRECTORY.  `git restore --staged -- dir` is a
     * valid unstage of everything below it, and 57.13 §4's FUSS
     * directory menu offers exactly that as `Unstage All Below`; the
     * flags on a directory node are aggregates of its subtree, so the
     * "nothing staged" refusal below reads correctly for both.
     */
    FUSS_TARGET_STAGED,
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
    yew_xfree(absolute);
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
    if (guard != FUSS_TARGET_ANY && guard != FUSS_TARGET_STAGED &&
        !found.is_file) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "select a file to %s", action);
        return false;
    }
    if (guard == FUSS_TARGET_STAGED && found.status_known &&
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

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
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
    if (strcmp(verb_name, "merge") == 0 || strcmp(verb_name, "pull") == 0 ||
        strcmp(verb_name, "cherry-pick") == 0 || strcmp(verb_name, "revert") == 0) {
        if (!yew_git_modal_start(ed, &f->bridge))
            return YEW_CMD_ERR_IO;
    }
    if (f->bridge.dir != NULL) {
        req.editor_env = f->bridge.editor_env;
        req.sequence_env = f->bridge.sequence_env;
    }
    job = yew_git_spawn(ed, verb, argv, &req, error, sizeof(error));
    if (job == 0U) {
        yew_git_modal_close(ed, &f->bridge);
        yew_msg(ed, YEW_MSG_ERROR, "%s",
                error[0] == '\0' ? "cannot start git command" : error);
        return YEW_CMD_ERR_IO;
    }
    f->pending_job = job;
    if (f->bridge.dir != NULL)
        f->bridge.job_id = job;
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
        yew_xfree((char *)f->picker_items[i].label);
        yew_xfree((char *)f->picker_items[i].detail);
        yew_xfree(f->picker_values[i]);
    }
    yew_xfree(f->picker_items);
    yew_xfree(f->picker_values);
    yew_xfree(f->picker_aux);
    yew_xfree(f->picker_aux2);
    yew_xfree(f->preview_value);
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
        yew_xfree((char *)f->picker_items[at].label);
        yew_xfree((char *)f->picker_items[at].detail);
        yew_xfree(f->picker_values[at]);
        return false;
    }
    f->picker_count++;
    return true;
}

typedef struct FussActionBuild {
    Ed *ed;
    FussMode *f;
    bool ok;
} FussActionBuild;

static bool fuss_action_reachable(const KeyId *seq, u32 n)
{
    const u16 command_mods = YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SUPER |
                             YEW_MOD_HYPER | YEW_MOD_META;
    u32 code;
    u16 mods;

    if (seq == NULL || n == 0U)
        return false;
    code = (u32)(seq[0].v >> 16U);
    mods = (u16)(seq[0].v & UINT16_MAX);
    /* These rows are consumed by FUSS type-to-jump before dispatch.  Space
     * remains structural and is deliberately reachable. */
    return code >= YEW_KEY_BASE || (mods & command_mods) != 0U ||
           code <= 0x20U || code == 0x7fU;
}

static bool fuss_action_visit(const KeyId *seq, u32 n,
                              const Binding *binding, void *opaque)
{
    FussActionBuild *build = opaque;
    const Binding *effective = NULL;
    const CmdDesc *desc;
    KeyMatch match;
    Bytebuf label;
    bool added;

    if (build == NULL || binding == NULL)
        return false;
    match = yew_keystack_lookup(&build->ed->keys, seq, n, NULL, NULL,
                                &effective);
    if ((match != YEW_MATCH_FULL && match != YEW_MATCH_FULL_PREFIX) ||
        effective != binding || !fuss_action_reachable(seq, n))
        return true;
    desc = yew_cmd_desc(binding->cmd);
    if (desc == NULL || (desc->flags & YEW_CMD_INTERNAL) != 0U ||
        strcmp(desc->name, "ed.git.actions") == 0 ||
        (strncmp(desc->name, "ed.git.", sizeof("ed.git.") - 1U) != 0 &&
         strcmp(desc->name, "ed.group.from_dir") != 0))
        return true;
    bytebuf_init(&label);
    yew_key_format_seq(seq, n, &label);
    added = fuss_picker_add(build->f, (const char *)label.data, label.len,
                            desc->help, strlen(desc->help), desc->name,
                            strlen(desc->name));
    bytebuf_free(&label);
    if (!added)
        build->ok = false;
    return added;
}

static void fuss_actions_sort(FussMode *f)
{
    u32 i;

    for (i = 1U; i < f->picker_count; i++) {
        PickItem item = f->picker_items[i];
        char *value = f->picker_values[i];
        u32 at = i;

        while (at != 0U &&
               strcmp(f->picker_items[at - 1U].label, item.label) > 0) {
            f->picker_items[at] = f->picker_items[at - 1U];
            f->picker_values[at] = f->picker_values[at - 1U];
            at--;
        }
        f->picker_items[at] = item;
        f->picker_values[at] = value;
    }
    for (i = 0U; i < f->picker_count; i++)
        f->picker_items[i].payload = (i32)i;
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
        yew_xfree(path);
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
    yew_xfree(argv);
    yew_xfree(path);
    return status;
}

static void fuss_prompt_clear(FussMode *f)
{
    if (f == NULL)
        return;
    f->prompt_action = FUSS_PROMPT_NONE;
    yew_xfree(f->prompt_path);
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

static void fuss_commit_done(Ed *ed, bool accepted, const u8 *text,
                              size_t len, void *ctx)
{
    FussMode *f = ctx;
    bool empty = false;
    bool amend = f->commit_amend;

    f->commit_status = YEW_CMD_OK;
    if (accepted) {
        yew_xfree(f->commit_draft);
        f->commit_draft = fuss_dup_bytes((const char *)text, len);
        f->draft_amend = amend;
        f->commit_status = fuss_commit_bytes(ed, text, len, amend, &empty);
    } else {
        yew_msg(ed, YEW_MSG_INFO, "commit cancelled");
    }
    f->commit_editing = false;
    f->commit_win_id = 0U;
    f->commit_amend = false;
    f->commit_comment = 0U;
    if (!accepted || empty) {
        yew_xfree(f->commit_draft);
        f->commit_draft = NULL;
    } else if (f->commit_status != YEW_CMD_OK) {
        CmdStatus status = f->commit_status;

        (void)fuss_commit_begin(ed, amend, text, len);
        f->commit_status = status;
    }
}

static CmdStatus fuss_commit_begin(Ed *ed, bool amend, const u8 *prefill,
                                   size_t prefill_len)
{
    FussMode *f;
    const GitSnapshot *snap;
    u8 comment;
    char *seed;
    char help[256];
    size_t staged = 0U;
    size_t i;

    if (ed == NULL || ed->fuss == NULL)
        return YEW_CMD_ERR_STATE;
    f = ed->fuss;
    snap = yew_git_snapshot(ed);
    if (!fuss_commit_guard(ed, snap))
        return YEW_CMD_ERR_STATE;
    if (!yew_fuss_commit_select_comment(prefill, prefill_len,
            (const u8 *)(snap == NULL ? "#" : snap->comment_char),
            snap == NULL ? 1U : snap->comment_char_len, &comment)) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot resolve core.commentChar");
        return YEW_CMD_ERR_STATE;
    }
    seed = fuss_dup_bytes((const char *)prefill, prefill_len);
    for (i = 0U; snap != NULL && i < snap->entries.len; i++)
        if (snap->entries.data[i].staged)
            staged++;
    (void)snprintf(help, sizeof(help), "%s | %zu staged | %s",
                    snap == NULL || snap->branch == NULL ? "HEAD" : snap->branch,
                    staged, amend && snap != NULL && snap->upstream != NULL &&
                    snap->ahead == 0 ? "Warning: amending a pushed commit" :
                    "Empty message cancels");
    fuss_viewer_close(ed);
    yew_cmdline_open_modal(ed, amend ? "Amend commit message" : "Commit message",
                           help, seed, true, fuss_commit_done, f);
    yew_xfree(seed);
    f->commit_editing = true;
    f->commit_win_id = ed->win == NULL ? 0U : ed->win->id;
    f->commit_amend = amend;
    f->commit_comment = comment;
    /* Start at the message, not at the end of a merge template. */
    yew_cmdline_target(ed)->cs.curs.data[0].pos = BYTEOFF(0U);
    yew_cmdline_target(ed)->cs.curs.data[0].anchor = BYTEOFF(0U);
    yew_cmdline_sync(ed);
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
        yew_xfree(step_path);
        yew_xfree(total_path);
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
    yew_xfree(step_path);
    yew_xfree(total_path);
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
            yew_xfree(path);
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "cannot read the pending merge message");
            return YEW_CMD_ERR_IO;
        }
        yew_xfree(path);
        status = fuss_commit_begin(cx->ed, false, message.data,
                                   message.len);
        bytebuf_free(&message);
        return status;
    }
    return fuss_commit_begin(cx->ed, false, NULL, 0U);
}

CmdStatus yew_fuss_commit_save(Ed *ed, Buffer *buffer, bool *handled)
{
    if (handled != NULL)
        *handled = false;
    if (ed == NULL || ed->fuss == NULL || !ed->fuss->commit_editing ||
        !ed->cmdline.active || buffer != yew_cmdline_target(ed)->buf)
        return YEW_CMD_ERR_STATE;
    if (handled != NULL)
        *handled = true;
    yew_cmdline_close(ed, true);
    return ed->fuss->commit_status;
}

CmdStatus yew_fuss_commit_close(Ed *ed, Buffer *buffer, bool *handled)
{
    if (handled != NULL)
        *handled = false;
    if (ed == NULL || ed->fuss == NULL || !ed->fuss->commit_editing ||
        !ed->cmdline.active || buffer != yew_cmdline_target(ed)->buf)
        return YEW_CMD_ERR_STATE;
    if (handled != NULL)
        *handled = true;
    yew_cmdline_close(ed, false);
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
        yew_xfree(range);
        return YEW_CMD_ERR_ARG;
    }
    argv[0] = (char *)"log";
    argv[1] = range;
    argv[2] = (char *)"--date-order";
    argv[3] = (char *)"--pretty=format:%h%x1f%at%x1f%an%x1f%s";
    argv[4] = NULL;
    status = fuss_spawn(ed, "log", argv, false, NULL, 0U, false);
    yew_xfree(range);
    if (status == YEW_CMD_OK) {
        f->pending_pick = FUSS_PICK_REBASE_CONFIRM;
        yew_msg(ed, YEW_MSG_INFO, "loading rebase range");
    } else {
        fuss_prompt_clear(f);
    }
    return status;
}

static CmdStatus fuss_rebase_start(Ed *ed, const char *operation,
                                  const char *base)
{
    FussMode *f = ed->fuss;
    char *argv[4];
    size_t at = 0U;
    CmdStatus status;

    if (f->pending_job != 0U) {
        yew_msg(ed, YEW_MSG_WARN, "a FUSS git command is still running");
        return YEW_CMD_ERR_STATE;
    }
    if (!yew_git_modal_start(ed, &f->bridge))
        return YEW_CMD_ERR_IO;
    argv[at++] = (char *)"rebase";
    argv[at++] = (char *)operation;
    if (base != NULL)
        argv[at++] = (char *)base;
    argv[at] = NULL;
    status = fuss_spawn(ed, "rebase", argv, false, NULL, 0U, true);
    if (status == YEW_CMD_OK)
        f->bridge.job_id = f->pending_job;
    else
        yew_git_modal_close(ed, &f->bridge);
    return status;
}

static void fuss_prompt_done(Ed *ed, bool accepted, const u8 *text,
                             size_t len, void *ctx)
{
    FussMode *f = ctx;
    FussPromptAction action;
    char *value = NULL;
    const char *expected = NULL;

    if (ed == NULL || f == NULL)
        return;
    action = f->prompt_action;
    /* Cancel is a terminal outcome even if the draft already contains the
     * exact destructive confirmation word. Never dispatch on draft text. */
    if (!accepted) {
        fuss_prompt_clear(f);
        yew_msg(ed, YEW_MSG_INFO, "git command cancelled");
        return;
    }
    switch (action) {
    case FUSS_PROMPT_DISCARD: expected = "discard"; break;
    case FUSS_PROMPT_DELETE:
    case FUSS_PROMPT_BRANCH_DELETE: expected = "delete"; break;
    case FUSS_PROMPT_PUSH_FORCE: expected = "force"; break;
    case FUSS_PROMPT_REBASE: expected = "rebase"; break;
    case FUSS_PROMPT_RESET_HARD: expected = "reset"; break;
    case FUSS_PROMPT_REBASE_ABORT: expected = "abort"; break;
    default: break;
    }
    if (accepted && expected != NULL &&
        !fuss_text_is(text, len, expected, strlen(expected))) {
        Bytebuf help;
        char *seed = fuss_dup_bytes((const char *)text, len);

        bytebuf_init(&help);
        bytebuf_printf(&help, "Nothing changed. Type '%s' exactly. Target: %s",
                        expected, f->prompt_path == NULL ? "current repository" :
                                                           f->prompt_path);
        bytebuf_push_u8(&help, 0U);
        yew_cmdline_open_modal(ed, "Confirm Git action", (const char *)help.data,
                               seed, false, fuss_prompt_done, f);
        bytebuf_free(&help);
        yew_xfree(seed);
        return;
    }
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
        } else {
            char *argv[] = {(char *)"stash", (char *)"push", NULL};

            (void)fuss_spawn(ed, "stash-push", argv, false, NULL, 0U, false);
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
                yew_xfree(absolute);
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
                yew_xfree(old_path);
                yew_xfree(new_path);
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
            (void)fuss_rebase_start(ed, "-i", f->prompt_path);
        break;
    case FUSS_PROMPT_RESET_HARD:
        if (accepted && f->prompt_path != NULL) {
            char *argv[] = {(char *)"reset", (char *)"--hard", f->prompt_path, NULL};

            (void)fuss_spawn(ed, "reset", argv, false, NULL, 0U, false);
        }
        break;
    case FUSS_PROMPT_REBASE_ABORT:
        if (accepted)
            (void)fuss_rebase_start(ed, "--abort", NULL);
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
    yew_xfree(value);
    fuss_prompt_clear(f);
}

static CmdStatus fuss_prompt(CmdCtx *cx, FussPromptAction action,
                             const char *seed, const char *path,
                             const char *hint)
{
    FussMode *f;
    const char *title;

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
    switch (action) {
    case FUSS_PROMPT_BRANCH_CREATE: title = "Create branch"; break;
    case FUSS_PROMPT_BRANCH_DELETE: title = "Delete branch"; break;
    case FUSS_PROMPT_STASH_PUSH: title = "Stash message"; break;
    case FUSS_PROMPT_TAG: title = "Create tag"; break;
    case FUSS_PROMPT_DISCARD: title = "Discard changes"; break;
    case FUSS_PROMPT_DELETE: title = "Delete path"; break;
    case FUSS_PROMPT_RENAME: title = "Rename path"; break;
    case FUSS_PROMPT_PUSH_FORCE: title = "Force push with lease"; break;
    case FUSS_PROMPT_REBASE: title = "Interactive rebase"; break;
    case FUSS_PROMPT_RESET_HARD: title = "Hard reset"; break;
    case FUSS_PROMPT_REBASE_ABORT: title = "Abort rebase"; break;
    default: title = "Git action"; break;
    }
    yew_cmdline_open_modal(cx->ed, title, hint, seed,
                           action == FUSS_PROMPT_TAG, fuss_prompt_done, f);
    if (!cx->ed->cmdline.active) {
        fuss_prompt_clear(f);
        return YEW_CMD_ERR_STATE;
    }
    if (action == FUSS_PROMPT_DISCARD && path != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN,
                "discard %s — type 'discard' to confirm\nuse hunk "
                "discard for an undoable version", path);
    else if (action == FUSS_PROMPT_DELETE && path != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN,
                "delete %s — type 'delete' to confirm", path);
    else if (action == FUSS_PROMPT_RESET_HARD && path != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN,
                "Discard tracked worktree/index changes and reset to %s. "
                "Type 'reset' to confirm.", path);
    else if (hint != NULL)
        yew_msg(cx->ed, YEW_MSG_WARN, "%s", hint);
    if (cx->ed->msg.active) {
        yew_xfree(cx->ed->cmdline.modal_help);
        cx->ed->cmdline.modal_help = yew_xstrdup(cx->ed->msg.full == NULL ?
            cx->ed->msg.text : cx->ed->msg.full);
        yew_msg_clear(cx->ed);
    }
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
        yew_xfree(aux);
        return false;
    }
    switch (action) {
    case FUSS_PICK_ACTIONS: {
        CmdId id = yew_cmd_lookup(value, (u32)strlen(value));
        const CmdDesc *desc = yew_cmd_desc(id);
        CmdCtx cx = {0};

        if (id.v == 0U || desc == NULL ||
            (desc->flags & YEW_CMD_INTERNAL) != 0U ||
            strcmp(desc->name, "ed.git.actions") == 0)
            break;
        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.source = YEW_SRC_KEY;
        ed->last_cmd = id;
        ed->last_status = yew_ed_invoke(ed, id, &cx);
        ed->dispatch_count++;
        break;
    }
    case FUSS_PICK_BRANCH_SWITCH: {
        char *argv[] = {(char *)"switch", (char *)"--", value, NULL};
        (void)fuss_spawn(ed, "switch", argv, false, NULL, 0U, false);
        break;
    }
    case FUSS_PICK_BRANCH_DELETE:
        yew_xfree(f->prompt_path);
        f->prompt_path = fuss_dup_bytes(value, fuss_cstr_len(value));
        if (f->prompt_path != NULL) {
            char *argv[] = {
                (char *)"branch", (char *)"-d", (char *)"--",
                f->prompt_path, NULL
            };
            if (fuss_spawn(ed, "branch-delete", argv, false, NULL, 0U,
                           false) != YEW_CMD_OK) {
                yew_xfree(f->prompt_path);
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
            if (strcmp(value, "--hard") == 0) {
                CmdCtx cx = {0};

                cx.ed = ed;
                cx.win = ed->win;
                (void)fuss_prompt(&cx, FUSS_PROMPT_RESET_HARD, NULL, aux, NULL);
            } else {
                char *argv[] = {(char *)"reset", value, aux, NULL};

                (void)fuss_spawn(ed, "reset", argv, false, NULL, 0U, false);
            }
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
    yew_xfree(value);
    yew_xfree(aux);
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
    yew_xfree(owner->value);
    yew_xfree(owner);
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
        yew_xfree(owner);
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
    yew_xfree(f->preview_value);
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

static void fuss_actions_open(Ed *ed)
{
    FussMode *f = ed->fuss;
    FussActionBuild build = {ed, f, true};
    PickerSpec spec = {0};
    u32 i;

    fuss_picker_clear(f);
    for (i = 0U; i < ed->keys.n && build.ok; i++) {
        if (yew_keymap_binding_count(ed->keys.l[i]) != 0U &&
            !yew_keymap_visit(ed->keys.l[i], fuss_action_visit, &build) &&
            build.ok)
            build.ok = false;
    }
    if (!build.ok || f->picker_count == 0U) {
        yew_msg(ed, YEW_MSG_WARN, "%s",
                build.ok ? "no FUSS actions are bound" :
                           "cannot build FUSS action list");
        fuss_picker_clear(f);
        return;
    }
    fuss_actions_sort(f);
    f->picker_action = FUSS_PICK_ACTIONS;
    spec.title = "FUSS actions";
    spec.items = fuss_picker_items;
    spec.accept = fuss_picker_accept;
    spec.footer = "Enter run · / filter · Esc close";
    spec.filter_requires_slash = true;
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

size_t yew_fuss_relative_time(const Ed *ed, char *dst, size_t cap,
                              i64 author_epoch)
{
    i64 now = yew_git_editor_wall_now(ed);
    i64 age;
    i64 count;
    const char *unit;
    int n;

    if (dst == NULL || cap == 0U)
        return 0U;
    dst[0] = '\0';
    if (now <= 0 || author_epoch < 0)
        return 0U;
    /* YEW-F-018: picker age derives from yew's startup wall anchor and
     * monotonic editor time; rendering never consults the system clock. */
    age = now > author_epoch ? now - author_epoch : 0;
    if (age < 60) {
        n = snprintf(dst, cap, "now");
    } else if (age < 60 * 60) {
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
    if (age >= 60) {
        n = snprintf(dst, cap, "%lld %s%s ago", (long long)count, unit,
                     count == 1 ? "" : "s");
    }
    if (n < 0) {
        dst[0] = '\0';
        return 0U;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1U;
}

static void fuss_detail_relative(Ed *ed, Bytebuf *detail, const char *text,
                                 size_t len)
{
    char relative[64];
    i64 stamp;
    size_t relative_len;

    if (!fuss_parse_epoch(text, len, &stamp))
        return;
    relative_len = yew_fuss_relative_time(ed, relative, sizeof(relative),
                                           stamp);
    fuss_detail_part(detail, relative, relative_len);
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
                fuss_detail_relative(ed, &detail, fields[2], lens[2]);
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
                fuss_detail_relative(ed, &detail, fields[2], lens[2]);
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
                fuss_detail_relative(ed, &detail, fields[1], lens[1]);
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
                    yew_xfree(ref);
                    return true;
                }
                yew_xfree(ref);
                yew_xfree(f->picker_aux2);
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
            yew_xfree(branch);
            yew_xfree(remote);
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
    case FUSS_PICK_ACTIONS:
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
    if (kind == 4 && fuss_expand_start(cx->ed))
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
        else if (kind == 4) {
            const FussItem *item = fuss_item(f, row);
            const FussNode *node = fuss_node(f, item);

            if (item != NULL && node != NULL && !node->is_file &&
                !node->expanded) {
                (void)yew_fuss_open_memory_set(&f->manual_open,
                                                item->path, item->path_len,
                                                true);
                fuss_apply_effective(cx->ed);
                row = yew_fuss_row_of(&f->tree, &f->sel);
            } else {
                row = yew_fuss_nav_enter(&f->tree, row);
            }
        }
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
    yew_xfree(f->expand_path);
    f->expand_path = NULL;
}

static bool fuss_expand_start(Ed *ed)
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
    opts.hidden = f->opts.show_hidden;
    opts.include_dirs = true;
    opts.max_depth = 1U;
    f->expand_walk = yew_walk_begin(root, &opts, &f->expand_files);
    yew_xfree(root);
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
    snap = yew_git_snapshot(ed);
    arena_init(&arena);
    if (fuss_expand_children(f, snap, &children, &arena) &&
        yew_fuss_expand_untracked(&f->tree, node, &children)) {
        (void)yew_fuss_open_memory_set(&f->manual_open, f->expand_path,
                                        (u32)fuss_cstr_len(f->expand_path),
                                        true);
        fuss_apply_effective(ed);
        fuss_select_row(f, yew_fuss_row_of(&f->tree, &f->sel));
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

CmdStatus yew_fuss_cmd_actions(CmdCtx *cx)
{
    FussMode *f;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    fuss_actions_open(cx->ed);
    return yew_picker_active(cx->ed) ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
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
    const FussItem *item;
    const FussNode *node;
    i32 row;
    bool remembered;

    if (fuss_require(cx, &f) != YEW_CMD_OK || !f->active)
        return YEW_CMD_ERR_STATE;
    row = fuss_row(f);
    item = fuss_item(f, row);
    node = fuss_node(f, item);
    if (item == NULL || node == NULL || node->is_file)
        return YEW_CMD_OK;
    if (fuss_expand_start(cx->ed))
        return YEW_CMD_OK;
    remembered = yew_fuss_open_memory_has(&f->manual_open,
                                           item->path, item->path_len);
    if (node->expanded && !remembered) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "kept open: contains an open file");
        return YEW_CMD_OK;
    }
    (void)yew_fuss_open_memory_set(&f->manual_open,
                                    item->path, item->path_len,
                                    !node->expanded);
    fuss_apply_effective(cx->ed);
    fuss_select_row(f, yew_fuss_row_of(&f->tree, &f->sel));
    fuss_damage(cx->ed);
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
                          FUSS_TARGET_STAGED, false);
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
    const GitRepo *repo;
    const GitSnapshot *snap;
    Bytebuf help;
    CmdStatus status;

    if (fuss_require(cx, NULL) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    repo = yew_git_repo_cached(cx->ed);
    snap = yew_git_snapshot(cx->ed);
    bytebuf_init(&help);
    bytebuf_printf(&help,
        "Repository: %s\nBranch: %s\nRun git push --force-with-lease using "
        "your configured push destination/refspecs. Type 'force' to confirm.",
        repo == NULL || repo->top_level == NULL ? "current" : repo->top_level,
        snap == NULL || snap->branch == NULL ? "HEAD" : snap->branch);
    bytebuf_push_u8(&help, 0U);
    status = fuss_prompt(cx, FUSS_PROMPT_PUSH_FORCE, NULL, NULL,
                          (const char *)help.data);
    bytebuf_free(&help);
    return status;
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

typedef struct FussDiffJob {
    u32 buffer_id;
    u32 job_id;
    bool no_index;
    bool incoming;
} FussDiffJob;

static void fuss_diff_decorate(Pane *pane, Buffer *buffer,
                               const DiffUnified *view)
{
    Win *w;
    size_t i, first_change = SIZE_MAX;

    if (pane == NULL)
        return;
    if (!pane->is_leaf) {
        fuss_diff_decorate(pane->a, buffer, view);
        fuss_diff_decorate(pane->b, buffer, view);
        return;
    }
    w = pane->win;
    if (w == NULL || w->buf != buffer)
        return;
    YewGitDiffRowStyleVec_free(&w->git_diff_rows);
    YewGitDiffSourceVec_free(&w->git_diff_sources);
    for (i = 0U; i < 2U + view->rows.left.len; i++) {
        YewGitDiffSource source = {-1, -1};
        YewGitDiffRowStyle style = YEW_GIT_DIFF_ROW_NONE;

        if (i >= 2U) {
            source.before = view->rows.left.data[i - 2U];
            source.after = view->rows.right.data[i - 2U];
            if (source.before < 0)
                style = YEW_GIT_DIFF_ROW_ADD;
            else if (source.after < 0)
                style = YEW_GIT_DIFF_ROW_DEL;
        }
        YewGitDiffSourceVec_push(&w->git_diff_sources, source);
        YewGitDiffRowStyleVec_push(&w->git_diff_rows, style);
        if (style != YEW_GIT_DIFF_ROW_NONE && first_change == SIZE_MAX)
            first_change = i;
    }
    if (first_change != SIZE_MAX && w->cs.curs.len != 0U) {
        Cursor *cursor = &w->cs.curs.data[w->cs.primary];

        cursor->pos = yew_textbuf_line_start(buffer->tb, LINENO(first_change));
        cursor->anchor = cursor->pos;
        yew_vp_follow(w);
    }
}

static void fuss_diff_complete(void *opaque, Ed *ed, const YewJob *job)
{
    FussDiffJob *owner = opaque;
    Buffer *buffer = yew_ws_buf_by_id(ed, owner->buffer_id);
    DiffUnified view = {0};
    DiffUnifiedResult result = YEW_DIFF_UNIFIED_INVALID;
    Bytebuf text = {0};
    const char *notice = NULL;
    size_t i;

    if (ed->fuss != NULL && ed->fuss->diff_job == owner->job_id)
        ed->fuss->diff_job = 0U;
    /* No retained Buffer/Win pointers: Escape may have freed the tab. */
    if (buffer == NULL)
        return;
    bytebuf_printf(&text, "%s | old / new | - removed / + added\n",
        owner->incoming ? "Merge base -> upstream" :
        owner->no_index ? "Empty -> saved worktree" : "HEAD -> saved worktree");
    if (job->collect_capped) {
        notice = "Diff exceeds the 16 MiB collection limit; no partial view shown.";
    } else if (job->state != YEW_JOB_EXITED ||
               (job->exit_code != 0 &&
                !(owner->no_index && job->exit_code == 1))) {
        notice = "Git diff failed or timed out; no file content was changed.";
    } else {
        result = yew_diff_unified_build(&view, job->collect.data,
                                         job->collect.len);
        switch (result) {
        case YEW_DIFF_UNIFIED_OK: break;
        case YEW_DIFF_UNIFIED_EMPTY:
            notice = "No saved-file differences."; break;
        case YEW_DIFF_UNIFIED_BINARY:
            notice = "Binary files differ; text comparison is unavailable."; break;
        case YEW_DIFF_UNIFIED_METADATA:
            notice = "File metadata changed; no text hunks."; break;
        case YEW_DIFF_UNIFIED_LIMIT:
            notice = "Diff exceeds the 200,000-row/16 MiB view limit."; break;
        case YEW_DIFF_UNIFIED_INVALID:
            notice = "Unsupported or incomplete file diff; no partial view shown.";
            break;
        }
    }
    if (notice != NULL) {
        bytebuf_printf(&text, "\n%s\n", notice);
        bytebuf_append(&text, job->collect_err.data, job->collect_err.len);
    } else {
        bytebuf_printf(&text, "Alt-Up/Down changes | Esc closes%s%s\n",
            view.before_no_lf ? " | old: no final newline" : "",
            view.after_no_lf ? " | new: no final newline" : "");
        bytebuf_append(&text, view.bytes.data, view.bytes.len);
    }
    if (fuss_replace_buffer(ed, buffer, text.data, (u64)text.len)) {
        yew_undo_mark_saved(buffer->undo);
        if (result == YEW_DIFF_UNIFIED_OK) {
            for (i = 0U; i < ed->tabs.v.len; i++)
                fuss_diff_decorate(ed->tabs.v.data[i].root, buffer, &view);
        }
        ed->layout_dirty = true;
        ed->full_damage = true;
    }
    bytebuf_free(&text);
    yew_diff_unified_drop(&view);
}

static void fuss_diff_destroy(void *opaque)
{
    yew_xfree(opaque);
}

static const YewJobCallbackOps fuss_diff_ops = {
    fuss_diff_complete, fuss_diff_destroy
};

CmdStatus yew_fuss_cmd_diff(CmdCtx *cx)
{
    char *path;
    FussTarget target;
    FussDiffJob *owner;
    const GitSnapshot *snapshot;
    Buffer *buffer;
    char title[192], error[160];
    char *argv[24];
    size_t at = 0U;
    u32 job;
    static const u8 loading[] = "Loading file comparison... Esc closes.";

    if (fuss_require(cx, NULL) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    if (cx->ed->fuss->diff_job != 0U) {
        yew_msg(cx->ed, YEW_MSG_WARN, "a file diff is still running");
        return YEW_CMD_ERR_STATE;
    }
    path = fuss_selected_path(cx);
    if (path == NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "select a valid workspace path");
        return YEW_CMD_ERR_ARG;
    }
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE, "diff", &target)) {
        yew_xfree(path);
        return YEW_CMD_ERR_STATE;
    }
    snapshot = yew_git_snapshot(cx->ed);
    owner = yew_xcalloc(1U, sizeof(*owner));
    /* A path can be changed on both sides of the upstream comparison.
     * Local saved edits take precedence; never hide them behind incoming. */
    owner->incoming = target.incoming && !target.conflicted &&
                      !target.staged && !target.unstaged && !target.untracked;
    owner->no_index = !owner->incoming &&
        (target.untracked || (snapshot != NULL && snapshot->unborn));
    (void)snprintf(title, sizeof(title), "*diff %s*", path);
    buffer = yew_ws_scratch_new(cx->ed, title,
        YEW_BUF_NOUNDO | YEW_BUF_READONLY | YEW_BUF_TRANSIENT);
    if (buffer == NULL ||
        !fuss_replace_buffer(cx->ed, buffer, loading, sizeof(loading) - 1U)) {
        if (buffer != NULL)
            yew_ws_scratch_drop(cx->ed, buffer);
        yew_xfree(owner);
        yew_xfree(path);
        return YEW_CMD_ERR_IO;
    }
    owner->buffer_id = buffer->id;
    argv[at++] = (char *)"-c";
    argv[at++] = (char *)"diff.suppressBlankEmpty=false";
    argv[at++] = (char *)"diff";
    argv[at++] = (char *)"--no-ext-diff";
    argv[at++] = (char *)"--no-textconv";
    argv[at++] = (char *)"--no-color";
    argv[at++] = (char *)"--no-renames";
    argv[at++] = (char *)"--word-diff=none";
    argv[at++] = (char *)"--unified=200000";
    argv[at++] = (char *)"--output-indicator-new=+";
    argv[at++] = (char *)"--output-indicator-old=-";
    argv[at++] = (char *)"--output-indicator-context= ";
    argv[at++] = (char *)"--line-prefix=";
    argv[at++] = owner->no_index ? (char *)"--no-index" :
                  owner->incoming ? (char *)"HEAD...@{upstream}" : (char *)"HEAD";
    argv[at++] = (char *)"--";
    if (owner->no_index)
        argv[at++] = (char *)"/dev/null";
    argv[at++] = path;
    argv[at] = NULL;
    job = yew_git_spawn_callback(cx->ed, yew_git_verb("diff-file"), argv,
                                 owner, &fuss_diff_ops, error, sizeof(error));
    yew_xfree(path);
    if (job == 0U) {
        fuss_diff_destroy(owner);
        yew_ws_scratch_drop(cx->ed, buffer);
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", error);
        return YEW_CMD_ERR_IO;
    }
    owner->job_id = job;
    cx->ed->fuss->diff_job = job;
    yew_undo_mark_saved(buffer->undo);
    if (yew_tab_open_transient(cx->ed, buffer) < 0) {
        yew_ws_scratch_drop(cx->ed, buffer);
        return YEW_CMD_ERR_IO;
    }
    return YEW_CMD_OK;
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
    const FussItem *item;
    const FussNode *node;
    i32 row;
    int tab;
    bool was_resident;

    if (fuss_require(cx, &f) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    row = fuss_row(f);
    item = fuss_item(f, row);
    node = fuss_node(f, item);
    if (cx->sarg == NULL && node != NULL && !node->is_file) {
        return yew_fuss_cmd_nav_toggle(cx);
    }
    path = fuss_selected_path(cx);
    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE,
                           leave ? "open" : "view", NULL)) {
        yew_xfree(path);
        return YEW_CMD_ERR_STATE;
    }
    absolute = fuss_join_root(cx->ed, path);
    yew_xfree(path);
    if (absolute == NULL)
        return YEW_CMD_ERR_ARG;
    buffer = yew_ws_file_buf(cx->ed, absolute);
    yew_xfree(absolute);
    if (buffer == NULL)
        return YEW_CMD_ERR_IO;
    was_resident = yew_buf_resident(buffer);
    if (yew_buf_hydrate(cx->ed, buffer) != 0)
        return YEW_CMD_ERR_IO;
    if (!was_resident)
        yew_fl_hook_buffer(cx->ed, FL_EV_BUF_OPEN, buffer);
    if (leave) {
        fuss_viewer_close(cx->ed);
        tab = yew_tab_open(cx->ed, buffer->meta.realpath);
        if (tab < 0)
            return YEW_CMD_ERR_STATE;
        if (yew_mode_enter(cx->ed, YEW_MODE_L) != YEW_CMD_OK)
            return YEW_CMD_ERR_STATE;
        yew_tab_switch(cx->ed, tab);
        return YEW_CMD_OK;
    }
    return fuss_show_buffer(cx->ed, buffer) ? YEW_CMD_OK :
                                             YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_view(CmdCtx *cx) { return fuss_open_path(cx, false); }

static CmdStatus fuss_open_split(CmdCtx *cx, SplitDir dir)
{
    FussMode *f;
    char *path;
    char *absolute;
    Buffer *buffer;
    bool was_resident;
    Pane *leaf;
    Pane *split;

    if (fuss_require(cx, &f) != YEW_CMD_OK || cx->ed->focus == NULL)
        return YEW_CMD_ERR_STATE;
    path = fuss_selected_path(cx);
    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE, "open split", NULL)) {
        yew_xfree(path);
        return YEW_CMD_ERR_STATE;
    }
    absolute = fuss_join_root(cx->ed, path);
    yew_xfree(path);
    if (absolute == NULL)
        return YEW_CMD_ERR_ARG;
    buffer = yew_ws_file_buf(cx->ed, absolute);
    yew_xfree(absolute);
    if (buffer == NULL)
        return YEW_CMD_ERR_IO;
    was_resident = yew_buf_resident(buffer);
    if (yew_buf_hydrate(cx->ed, buffer) != 0)
        return YEW_CMD_ERR_IO;
    if (!was_resident)
        yew_fl_hook_buffer(cx->ed, FL_EV_BUF_OPEN, buffer);

    leaf = cx->ed->focus;
    split = yew_pane_split(cx->ed, leaf, dir);
    if (split == NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "no room to split");
        return YEW_CMD_ERR_STATE;
    }
    yew_ed_win_set_buffer(cx->ed, split->win, buffer);
    yew_pane_refocus(cx->ed, split);
    if (yew_mode_enter(cx->ed, YEW_MODE_L) != YEW_CMD_OK)
        return YEW_CMD_ERR_STATE;
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_open_split_h(CmdCtx *cx)
{
    return fuss_open_split(cx, YEW_SPLIT_H);
}

CmdStatus yew_fuss_cmd_open_split_v(CmdCtx *cx)
{
    return fuss_open_split(cx, YEW_SPLIT_V);
}

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
        yew_xfree(name);
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
            yew_xfree(name);
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "branch name must not begin with '-'");
            return YEW_CMD_ERR_ARG;
        }
        {
            char *argv[] = {(char *)"switch", (char *)"-c", name, NULL};
            status = fuss_spawn(cx->ed, "switch-create", argv, false, NULL,
                                0U, false);
        }
        yew_xfree(name);
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
        yew_xfree(name);
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
        yew_xfree(name);
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
            yew_xfree(value);
            return YEW_CMD_ERR_ARG;
        }
        {
            char *argv[] = {(char *)"reset", (char *)mode, ref, NULL};
            status = fuss_spawn(cx->ed, "reset", argv, false, NULL, 0U,
                                false);
        }
        yew_xfree(value);
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
        yew_xfree(base);
        return status;
    }
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_pick_commits(cx->ed, FUSS_PICK_REBASE, NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_rebase_continue(CmdCtx *cx)
{
    return fuss_require(cx, NULL) == YEW_CMD_OK ?
           fuss_rebase_start(cx->ed, "--continue", NULL) :
           YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_cmd_rebase_abort(CmdCtx *cx)
{
    return fuss_prompt(cx, FUSS_PROMPT_REBASE_ABORT, NULL, NULL,
        "Discard the in-progress rebase and return to its original branch. "
        "Type 'abort' to confirm.");
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
        yew_xfree(commit);
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
        yew_xfree(commit);
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
        yew_xfree(message);
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
        yew_xfree(ref);
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
        yew_xfree(name);
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
        yew_xfree(path);
        return YEW_CMD_ERR_STATE;
    }
    status = fuss_prompt(cx, FUSS_PROMPT_DISCARD, NULL, path,
                         "type 'discard' to permanently discard the path");
    yew_xfree(path);
    return status;
}

CmdStatus yew_fuss_cmd_file_delete(CmdCtx *cx)
{
    char *path = fuss_selected_path(cx);
    CmdStatus status;

    if (path == NULL)
        return YEW_CMD_ERR_ARG;
    if (!fuss_target_guard(cx, path, FUSS_TARGET_FILE, "delete", NULL)) {
        yew_xfree(path);
        return YEW_CMD_ERR_STATE;
    }
    status = fuss_prompt(cx, FUSS_PROMPT_DELETE, NULL, path,
                         "type 'delete' to permanently delete the path");
    yew_xfree(path);
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
    yew_xfree(path);
    return status;
}

/*
 * The FUSS menus' `Copy Path`, and the only `ed.git.*` verb that
 * touches no repository at all: a path is a path whether git has heard
 * of it or not, so there is no `fuss_target_guard` here.  The path
 * still comes through `fuss_selected_path`, which is what makes the
 * row honour `sarg` (the menu's captured path) and fall back to the
 * selected row for the keyboard route — and what rejects an absolute
 * path or one containing `..`.
 */
CmdStatus yew_fuss_cmd_copy_path(CmdCtx *cx)
{
    char *path = fuss_selected_path(cx);
    RegVal v;

    if (path == NULL) {
        if (cx != NULL && cx->ed != NULL)
            yew_msg(cx->ed, YEW_MSG_ERROR, "no path is selected");
        return YEW_CMD_ERR_ARG;
    }
    yew_regval_init(&v);
    bytebuf_append(&v.bytes, (const u8 *)path, strlen(path));
    v.type = (u8)YEW_REG_CHARWISE;
    /* Register `+` is the system clipboard, so this also travels out
     * through Sprint 12's OSC 52 path — the same register and the same
     * kind `ed.tab.copy_path` writes, because two spellings of "the
     * clipboard" is how a paste comes to find the wrong one. */
    yew_reg_yank(&cx->ed->regs, (u8)'+', &v);
    yew_regval_free(&v);
    yew_msg(cx->ed, YEW_MSG_INFO, "copied %s", path);
    yew_xfree(path);
    return YEW_CMD_OK;
}

CmdStatus yew_fuss_cmd_open(CmdCtx *cx) { return fuss_open_path(cx, true); }
