#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "mod/git/fussmode.h"
#include "util/arena.h"

typedef struct FussDrawerFix {
    char root[64];
    char file[96];
} FussDrawerFix;

static void fussdrawer_fix_make(FussDrawerFix *fix)
{
    FILE *file;

    (void)snprintf(fix->root, sizeof(fix->root),
                   "/tmp/yew-fussdrawer-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fix->root));
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
    ed.win->rect = (Rect){0U, 0U, 80U, 24U};
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
