#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "mod/git/fussmode.h"
#include "mod/git/fusstree.h"
#include "mod/git/git_int.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct FussDrawerFix {
    char root[PATH_MAX];
    char file[PATH_MAX + sizeof("/plain.txt")];
} FussDrawerFix;

static void fussdrawer_fix_make(FussDrawerFix *fix)
{
    const char *tmp = getenv("TMPDIR");
    char *resolved;
    FILE *file;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "build/tmp";
    (void)snprintf(fix->root, sizeof(fix->root),
                   "%s/yew-fussdrawer-XXXXXX", tmp);
    YEW_ASSERT_NOT_NULL(mkdtemp(fix->root));
    resolved = yew_xrealpath(fix->root);
    YEW_ASSERT_NOT_NULL(resolved);
    YEW_ASSERT(strlen(resolved) < sizeof(fix->root));
    (void)memcpy(fix->root, resolved, strlen(resolved) + 1U);
    yew_xfree(resolved);
    (void)snprintf(fix->file, sizeof(fix->file), "%s/plain.txt",
                   fix->root);
    file = fopen(fix->file, "wb");
    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT(fputs("not a git repository\n", file) >= 0);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void fussdrawer_fix_drop(const FussDrawerFix *fix)
{
    YEW_ASSERT_EQ_I64(unlink(fix->file), 0);
    YEW_ASSERT_EQ_I64(rmdir(fix->root), 0);
}

static void fussdrawer_enter_non_git(Ed *ed, const FussDrawerFix *fix)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    ed->ws.dir = arena_strdup(&ed->arena, fix->root);
    YEW_ASSERT_EQ_I64(yew_mode_enter(ed, YEW_MODE_F), YEW_CMD_OK);
    yew_fuss_tick(ed, ed->now_ms + 20);
}

static void fussdrawer_grid(Ed *ed)
{
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    ed->tab_strip_rect = (Rect){0U, 0U, 80U, 1U};
    ed->footer_rect = (Rect){0U, 22U, 80U, 2U};
    ed->win->rect = (Rect){0U, 1U, 80U, 21U};
}

static void fussdrawer_click(Ed *ed, u16 col, u16 row)
{
    Key key = {0};

    key.kind = (u16)YEW_EV_MOUSE;
    key.button = (u8)YEW_MB_LEFT;
    key.col = col;
    key.row = row;
    key.ev = (u8)YEW_KEY_PRESS;
    yew_mouse_event(ed, &key);
    key.ev = (u8)YEW_KEY_RELEASE;
    yew_mouse_event(ed, &key);
}

static bool fussdrawer_row_contains(const Grid *grid, u16 row,
                                    const char *needle)
{
    char text[81U * 8U + 1U];
    size_t at = 0U;
    u16 col;

    if (grid == NULL || row >= grid->rows)
        return false;
    for (col = 0U; col < grid->cols && at < sizeof(text) - 1U; col++) {
        const Cell *cell = &grid->back[(size_t)row * grid->cols + col];
        const char *bytes;
        size_t len;

        if (cell->w == 0U)
            continue;
        if ((cell->flags & CELL_INTERNED) != 0U) {
            bytes = yew_intern_str(grid->gi, cell->id);
            len = yew_intern_len(grid->gi, cell->id);
        } else if (cell->utf8[0] != 0U) {
            bytes = (const char *)cell->utf8;
            len = strnlen(bytes, sizeof(cell->utf8));
        } else {
            bytes = " ";
            len = 1U;
        }
        if (bytes == NULL)
            continue;
        if (len > sizeof(text) - 1U - at)
            len = sizeof(text) - 1U - at;
        (void)memcpy(text + at, bytes, len);
        at += len;
    }
    text[at] = '\0';
    return strstr(text, needle) != NULL;
}

void test_fussdrawer_width_follows_the_locked_clamp_table(void)
{
    YEW_ASSERT_EQ_U64(yew_fuss_drawer_width(40U), 20U);
    YEW_ASSERT_EQ_U64(yew_fuss_drawer_width(80U), 20U);
    YEW_ASSERT_EQ_U64(yew_fuss_drawer_width(120U), 20U);
    YEW_ASSERT_EQ_U64(yew_fuss_drawer_width(240U), 40U);
    YEW_ASSERT_EQ_U64(yew_fuss_drawer_width(600U), 48U);
}

void test_fussdrawer_rect_uses_content_between_tabs_and_footer(void)
{
    Ed ed = {0};
    Rect backdrop;
    Rect drawer;

    ed.grid.cols = 120U;
    ed.tab_strip_rect = (Rect){0U, 0U, 120U, 2U};
    ed.footer_rect = (Rect){0U, 28U, 120U, 2U};
    backdrop = yew_fuss_backdrop_rect(&ed);
    drawer = yew_fuss_drawer_rect(&ed);

    YEW_ASSERT_EQ_U64(backdrop.x, 0U);
    YEW_ASSERT_EQ_U64(backdrop.y, 2U);
    YEW_ASSERT_EQ_U64(backdrop.w, 120U);
    YEW_ASSERT_EQ_U64(backdrop.h, 26U);
    YEW_ASSERT_EQ_U64(drawer.x, 0U);
    YEW_ASSERT_EQ_U64(drawer.y, 2U);
    YEW_ASSERT_EQ_U64(drawer.w, 20U);
    YEW_ASSERT_EQ_U64(drawer.h, 26U);
}

void test_fussdrawer_header_preserves_divergence_in_compact_width(void)
{
    FussDrawerFix fix;
    GitSnapshot *snap;
    Ed ed;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    fussdrawer_grid(&ed);
    snap = yew_git_test_snapshot_mut(&ed);
    YEW_ASSERT_NOT_NULL(snap);
    snap->gen = 1U;
    snap->taken_ms = yew_now_ms();
    snap->state = YEW_GIT_OK;
    snap->branch = (char *)"trunk";
    snap->upstream = (char *)"origin/trunk";
    snap->ahead = 1;
    snap->behind = 1;
    yew_region_frame_begin();
    yew_fuss_draw(&ed);
    YEW_ASSERT(fussdrawer_row_contains(&ed.grid, 1U, "↑1 ↓1"));
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_entry_leave_preserves_live_pane_identity(void)
{
    Ed ed;
    Pane *root;
    Pane *focus;
    Win *win;
    ByteOff cursor;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    root = ed.pane_root;
    focus = ed.focus;
    win = ed.win;
    cursor = yew_ed_cursor(&ed)->pos;

    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.pane_root, root);
    YEW_ASSERT_EQ_U64(ed.focus, focus);
    YEW_ASSERT_EQ_U64(ed.win, win);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor.v);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.pane_root, root);
    YEW_ASSERT_EQ_U64(ed.focus, focus);
    YEW_ASSERT_EQ_U64(ed.win, win);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor.v);
    yew_ed_free(&ed);
}

void test_fussdrawer_non_git_tree_publishes_and_opens_a_file(void)
{
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Ed ed;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_open(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT_NOT_NULL(ed.win);
    YEW_ASSERT_NOT_NULL(ed.win->buf);
    YEW_ASSERT_EQ_STR(ed.win->buf->meta.realpath, fix.file);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_failed_split_is_atomic_and_keeps_f_mode(void)
{
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Ed ed;
    Pane *root;
    Pane *focus;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    root = ed.pane_root;
    focus = ed.focus;
    focus->rect.w = YEW_PANE_MIN_W;
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_open_split_h(&cx),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT(yew_fuss_active(&ed));
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    YEW_ASSERT_EQ_U64(ed.pane_root, root);
    YEW_ASSERT_EQ_U64(ed.focus, focus);
    YEW_ASSERT(root->is_leaf);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_preview_preserves_the_live_view_exactly(void)
{
    static const u8 original[] = "live editor text\nsecond line\n";
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Ed ed;
    Buffer *buffer;
    Viewport viewport;
    ByteOff cursor;

    fussdrawer_fix_make(&fix);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, original, sizeof(original) - 1U,
                                  "live.txt"));
    ed.ws.dir = arena_strdup(&ed.arena, fix.root);
    fussdrawer_grid(&ed);
    ed.win->vp.cols = 80U;
    ed.win->vp.rows = 24U;
    ed.win->vp.top = LINENO(1U);
    ed.win->vp.left = (CCol){3U};
    yew_ed_cursor(&ed)->pos = BYTEOFF(7U);
    buffer = ed.win->buf;
    viewport = ed.win->vp;
    cursor = yew_ed_cursor(&ed)->pos;
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    yew_fuss_tick(&ed, ed.now_ms + 20);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_view(&cx), YEW_CMD_OK);
    YEW_ASSERT(ed.win->panel.open);
    YEW_ASSERT(ed.win->panel.rect.x >= yew_fuss_drawer_rect(&ed).w);
    YEW_ASSERT_EQ_U64(ed.win->buf, buffer);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor.v);
    YEW_ASSERT_EQ_MEM(&ed.win->vp, &viewport, sizeof(viewport));
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    YEW_ASSERT(!ed.win->panel.open);
    YEW_ASSERT_EQ_U64(ed.win->buf, buffer);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor.v);
    YEW_ASSERT_EQ_MEM(&ed.win->vp, &viewport, sizeof(viewport));
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_selection_damage_stays_local(void)
{
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Ed ed;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    ed.full_damage = false;
    ed.layout_dirty = false;
    ed.footer_dirty = false;
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_nav_row_next(&cx), YEW_CMD_OK);
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT(!ed.layout_dirty);
    YEW_ASSERT(ed.footer_dirty);
    YEW_ASSERT(yew_fuss_draw_dirty(&ed));
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_typejump_selection_damage_stays_local(void)
{
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Key key = {0};
    Ed ed;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_jump_arm(&cx), YEW_CMD_OK);
    ed.full_damage = false;
    ed.layout_dirty = false;
    ed.footer_dirty = false;
    key.code = (u32)'p';
    key.ntext = 1U;
    key.text[0] = (u8)'p';
    YEW_ASSERT(yew_fuss_key(&ed, &key, yew_now_ms()));
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT(!ed.layout_dirty);
    YEW_ASSERT(ed.footer_dirty);
    YEW_ASSERT(yew_fuss_draw_dirty(&ed));
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_selected_row_keeps_the_final_component(void)
{
    static const char name[] = "aaaa-prefix-that-must-clip-final-component.c";
    FussDrawerFix fix;
    char path[sizeof(fix.root) + sizeof(name) + 2U];
    FILE *file;
    Ed ed;

    fussdrawer_fix_make(&fix);
    YEW_ASSERT(snprintf(path, sizeof(path), "%s/%s", fix.root, name) > 0);
    file = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT(fputs("long name\n", file) >= 0);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    fussdrawer_enter_non_git(&ed, &fix);
    fussdrawer_grid(&ed);
    yew_region_frame_begin();
    yew_fuss_draw(&ed);
    YEW_ASSERT(fussdrawer_row_contains(&ed.grid, 2U, "final-component.c"));
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_directory_tree_starts_collapsed(void)
{
    GitEntry entries[] = {
        {.path = "one/two/three.c", .path_len = 15U},
        {.path = "one/peer.c", .path_len = 10U}
    };
    GitSnapshot snap = {0};
    FussOpts opts = {true, false};
    FussTree tree;

    snap.entries.data = entries;
    snap.entries.len = YEW_ARRAY_LEN(entries);
    snap.gen = 1U;
    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &snap, &opts);
    YEW_ASSERT_EQ_U64(tree.items.len, 1U);
    YEW_ASSERT_EQ_STR(tree.items.data[0].path, "one");
    YEW_ASSERT(!tree.nodes.data[tree.items.data[0].node].expanded);
    yew_fuss_tree_drop(&tree);
}

void test_fussdrawer_mouse_double_click_uses_open_destination(void)
{
    FussDrawerFix fix;
    Ed ed;

    fussdrawer_fix_make(&fix);
    fussdrawer_enter_non_git(&ed, &fix);
    fussdrawer_grid(&ed);
    yew_region_frame_begin();
    yew_fuss_draw(&ed);
    ed.now_ms = 1000;
    fussdrawer_click(&ed, 1U, 2U);
    ed.now_ms = 1100;
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_FUSS_ROW, (Rect){0U, 2U, 20U, 1U},
                   ed.mouse.last_click_payload + 1);
    fussdrawer_click(&ed, 1U, 2U);
    YEW_ASSERT_EQ_U64(ed.mouse.click_n, 1U);
    YEW_ASSERT(yew_fuss_active(&ed));
    yew_region_frame_begin();
    yew_fuss_draw(&ed);
    ed.now_ms = 1200;
    fussdrawer_click(&ed, 1U, 2U);
    YEW_ASSERT_EQ_U64(ed.mouse.click_n, 1U);
    ed.now_ms = 1300;
    fussdrawer_click(&ed, 1U, 2U);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
    YEW_ASSERT(!yew_fuss_active(&ed));
    YEW_ASSERT_NOT_NULL(ed.win);
    YEW_ASSERT_NOT_NULL(ed.win->buf);
    YEW_ASSERT_EQ_STR(ed.win->buf->meta.realpath, fix.file);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_preview_owner_tab_can_close(void)
{
    FussDrawerFix fix;
    CmdCtx cx = {0};
    Ed ed;
    int owner;

    fussdrawer_fix_make(&fix);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    owner = yew_tab_open(&ed, fix.file);
    YEW_ASSERT(owner >= 0);
    yew_tab_switch(&ed, owner);
    ed.ws.dir = arena_strdup(&ed.arena, fix.root);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    yew_fuss_tick(&ed, ed.now_ms + 20);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_view(&cx), YEW_CMD_OK);
    YEW_ASSERT(yew_tab_close(&ed, owner));
    YEW_ASSERT(yew_fuss_active(&ed));
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_commit_owner_tab_close_cancels_cleanly(void)
{
    FussDrawerFix fix;
    GitEntry entry = {0};
    GitSnapshot *snap;
    CmdCtx cx = {0};
    Ed ed;
    int owner;

    fussdrawer_fix_make(&fix);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    owner = yew_tab_open(&ed, fix.file);
    YEW_ASSERT(owner >= 0);
    yew_tab_switch(&ed, owner);
    ed.ws.dir = arena_strdup(&ed.arena, fix.root);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    snap = yew_git_test_snapshot_mut(&ed);
    YEW_ASSERT_NOT_NULL(snap);
    entry.kind = GIT_E_ORDINARY;
    entry.path = "plain.txt";
    entry.path_len = 9U;
    entry.staged = true;
    snap->state = YEW_GIT_OK;
    snap->comment_char = (char *)"#";
    snap->comment_char_len = 1U;
    snap->entries.data = &entry;
    snap->entries.len = 1U;
    snap->gen++;
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_commit(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT(yew_tab_close(&ed, owner));
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    YEW_ASSERT(yew_fuss_active(&ed));
    YEW_ASSERT_NULL(yew_ws_scratch_find(&ed, "*commit*"));
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}

void test_fussdrawer_commit_owner_pane_close_cancels_cleanly(void)
{
    FussDrawerFix fix;
    GitEntry entry = {0};
    GitSnapshot *snap;
    CmdCtx cx = {0};
    Ed ed;
    u32 commit_win_id;
    bool handled = false;

    fussdrawer_fix_make(&fix);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    ed.focus->rect = (Rect){0U, 0U, 80U, 24U};
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_pane_cmd_split_v(&cx), YEW_CMD_OK);
    ed.ws.dir = arena_strdup(&ed.arena, fix.root);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_F), YEW_CMD_OK);
    snap = yew_git_test_snapshot_mut(&ed);
    YEW_ASSERT_NOT_NULL(snap);
    entry.kind = GIT_E_ORDINARY;
    entry.path = "plain.txt";
    entry.path_len = 9U;
    entry.staged = true;
    snap->state = YEW_GIT_OK;
    snap->comment_char = (char *)"#";
    snap->comment_char_len = 1U;
    snap->entries.data = &entry;
    snap->entries.len = 1U;
    snap->gen++;
    cx.win = ed.win;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_commit(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    commit_win_id = ed.win->id;
    cx.win = ed.win;
    YEW_ASSERT_EQ_I64(yew_pane_cmd_close(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(ed.pane_root), 1U);
    YEW_ASSERT_NOT_NULL(ed.win);
    YEW_ASSERT(ed.win->id != commit_win_id);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    YEW_ASSERT(yew_fuss_active(&ed));
    YEW_ASSERT_NULL(yew_ws_scratch_find(&ed, "*commit*"));

    /* A fresh commit must not be poisoned by stale deferred state. */
    cx.win = ed.win;
    YEW_ASSERT_EQ_I64(yew_fuss_cmd_commit(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_I);
    YEW_ASSERT_NOT_NULL(yew_ws_scratch_find(&ed, "*commit*"));
    YEW_ASSERT_EQ_I64(yew_fuss_commit_close(&ed, ed.win->buf, &handled),
                      YEW_CMD_OK);
    YEW_ASSERT(handled);
    YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_F);
    YEW_ASSERT_NULL(yew_ws_scratch_find(&ed, "*commit*"));
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    yew_ed_free(&ed);
    fussdrawer_fix_drop(&fix);
}
