#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 27 §5: context menus.
 *
 * THE LAW THIS FILE DEFENDS: the menu captures its target at open time
 * and never re-resolves it.  The tab strip can scroll under an open
 * menu, tabs can close, and every index renumbers when one does — so
 * the entry at the coordinates the menu was opened from may be a
 * different file by the time a row is clicked.
 *
 * `ctxmenu_a_row_handler_reading_a_payload_is_a_bug` proves that is not
 * merely documented: it installs a deliberately wrong handler and shows
 * the program aborts rather than acting on the wrong file.
 *
 * The other half is geometry.  Placement CLAMPS, never flips: sliding
 * the box back inside the allowed rectangle keeps the row the user
 * aimed at under the pointer, while flipping it above the anchor puts a
 * DIFFERENT row there and the click that follows opens something the
 * user never chose.
 */
#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/pane_cmds.h"
#include "ui/ctxmenu.h"
#include "ui/glyphs.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"

enum { CX_A = 11, CX_B = 22, CX_C = 33, CX_D = 44, CX_E = 55 };

static void cx_three_rows(void)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("Close Tab", "C-w", CX_A, true, 0U);
    yew_ctx_item("Close Other Tabs", NULL, CX_B, false, 0U); /* disabled */
    yew_ctx_sep();
    yew_ctx_item("Copy Path", NULL, CX_C, true, 0U);
}

/*
 * One row per priority, each section closed by a separator, so a shed
 * takes the separator with it rather than leaving the box hemmed by a
 * rule with nothing under it.
 */
static void cx_priority_rows(void)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("Keep A", NULL, CX_A, true, 0U);
    yew_ctx_item("Keep B", NULL, CX_B, true, 0U);
    yew_ctx_sep();
    yew_ctx_item("Maybe C", NULL, CX_C, true, 1U);
    yew_ctx_sep();
    yew_ctx_item("Maybe D", NULL, CX_D, true, 2U);
    yew_ctx_sep();
    yew_ctx_item("Shed E", NULL, CX_E, true, 3U);
}

/* 30 label cells and a 7-cell accelerator: 39 content cells, a 43-cell
 * box with the accelerators and a 34-cell box without them. */
static void cx_wide_rows(void)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("Absolutely Enormous Label Here", "C-x C-c", CX_A, true,
                 0U);
    yew_ctx_item("Small", "C-w", CX_B, true, 0U);
}

/* ---------------------------------------------------------------- */
/* Geometry                                                         */
/* ---------------------------------------------------------------- */

void test_ctxmenu_placement_clamps_at_all_four_edges(void)
{
    Rect allowed = {0U, 0U, 80U, 23U};
    Rect box;

    /* Bottom-right: the box slides back inside, it does not flip. */
    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(79U, 22U, allowed));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_U64((u64)(box.x + box.w), (u64)(allowed.x + allowed.w));
    YEW_ASSERT_EQ_U64((u64)(box.y + box.h), (u64)(allowed.y + allowed.h));
    /*
     * NEVER FLIPS: the box's top stays at or below the anchor's row
     * only because it was clamped, not because it was mirrored above
     * it.  A flip would have put box.y + box.h at the anchor.
     */
    YEW_ASSERT(box.y <= 22U);
    /* A bordered box: one row of frame above the rows and one below. */
    YEW_ASSERT_EQ_U64(box.h, (u64)yew_ctx_rows() + 2U);

    /* Top-left: nothing to clamp, so the corner sits exactly one cell
     * below-right of the anchor. */
    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, allowed));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_U64(box.x, 1U);
    YEW_ASSERT_EQ_U64(box.y, 1U);

    /* An allowed rectangle that does not start at the origin clamps to
     * ITS edges, not the screen's. */
    cx_three_rows();
    {
        Rect inner = {10U, 5U, 30U, 10U};

        YEW_ASSERT(yew_ctx_show(39U, 14U, inner));
        box = yew_ctx_box();
        YEW_ASSERT(box.x >= inner.x);
        YEW_ASSERT(box.y >= inner.y);
        YEW_ASSERT_EQ_U64((u64)(box.x + box.w), (u64)(inner.x + inner.w));
        YEW_ASSERT_EQ_U64((u64)(box.y + box.h), (u64)(inner.y + inner.h));
    }
    yew_ctx_close();
}

/*
 * A menu that cannot fit does not open at all.  Drawing one half off
 * the screen is worse than none: the rows the user cannot see are still
 * clickable everywhere else.
 */
void test_ctxmenu_refuses_a_space_it_cannot_fit(void)
{
    cx_three_rows();
    YEW_ASSERT(!yew_ctx_show(0U, 0U, (Rect){0U, 0U, 4U, 20U}));
    YEW_ASSERT(!yew_ctx_active());
    cx_three_rows();
    YEW_ASSERT(!yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 2U}));
    YEW_ASSERT(!yew_ctx_active());
    /* And a menu with no rows is not a menu. */
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    YEW_ASSERT(!yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT(!yew_ctx_active());
    yew_ctx_close();
}

void test_ctxmenu_is_at_least_the_minimum_width(void)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("x", NULL, CX_A, true, 0U);
    yew_ctx_item("y", NULL, CX_B, true, 0U);
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT(yew_ctx_box().w >= (u16)YEW_CTX_MIN_WIDTH);
    yew_ctx_close();
}

/* ---------------------------------------------------------------- */
/* Navigation                                                       */
/* ---------------------------------------------------------------- */

static Key cx_key(u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_KEY;
    k.ev = (u8)YEW_KEY_PRESS;
    k.code = code;
    return k;
}

void test_ctxmenu_arrows_skip_separators_and_disabled_rows(void)
{
    Key down = cx_key(YEW_KEY_DOWN);
    Key up = cx_key(YEW_KEY_UP);

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    /* Row 0 is enabled; the cursor starts there rather than on the
     * disabled row above whatever the caller listed first. */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    YEW_ASSERT(!yew_ctx_row_enabled(1U)); /* disabled */
    YEW_ASSERT(!yew_ctx_row_enabled(2U)); /* separator */

    YEW_ASSERT(yew_ctx_key(&down));
    /* Row 1 is greyed and row 2 is a separator: both are DRAWN and
     * neither is reachable. */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
    YEW_ASSERT(yew_ctx_key(&down));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0); /* wraps */
    YEW_ASSERT(yew_ctx_key(&up));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
    yew_ctx_close();
}

void test_ctxmenu_enter_invokes_and_escape_closes(void)
{
    Key enter = cx_key(YEW_KEY_ENTER);
    Key escape = cx_key(YEW_KEY_ESCAPE);

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT(yew_ctx_key(&enter));
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_take(), (u64)CX_A);
    /* Taken once and only once: a choice that survived a second poll
     * would fire twice. */
    YEW_ASSERT_EQ_U64(yew_ctx_take(), 0U);

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT(yew_ctx_key(&escape));
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_take(), 0U);
    yew_ctx_close();
}

/*
 * An open menu SWALLOWS what it does not use.  A menu that let `d`
 * through would delete a line behind an open pop-up.
 */
void test_ctxmenu_swallows_unhandled_keys(void)
{
    Key d = cx_key((u32)'d');

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT(yew_ctx_key(&d));
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_take(), 0U);
    yew_ctx_close();
    /* Closed, it claims nothing — the key belongs to the document
     * again. */
    YEW_ASSERT(!yew_ctx_key(&d));
}

void test_ctxmenu_a_disabled_row_is_unreachable_by_pointer(void)
{
    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    yew_ctx_hover(1); /* the greyed row */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    yew_ctx_hover(2); /* the separator */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    yew_ctx_invoke(1);
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_take(), 0U);
    yew_ctx_invoke(3);
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_take(), (u64)CX_C);
    yew_ctx_close();
}

/* Two menus can never both be open: beginning a second discards the
 * first, so there is never a hidden one still answering keys. */
void test_ctxmenu_two_menus_are_never_both_open(void)
{
    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_TAB);

    yew_ctx_begin((u32)YEW_CTX_KIND_GROUP);
    YEW_ASSERT(!yew_ctx_active());
    yew_ctx_item("Dissolve Group", NULL, CX_B, true, 0U);
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_GROUP);
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 1U);
    yew_ctx_close();
}

/* ---------------------------------------------------------------- */
/* Target capture (DoD 4)                                            */
/* ---------------------------------------------------------------- */

static void cx_fixture(Ed *ed, int extra)
{
    int i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    for (i = 0; i < extra; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-ctx-%d.txt", i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
    yew_ed_layout(ed);
    ed->now_ms = 1000;
}

/*
 * THE test.  A menu is opened for tab 3; the strip then scrolls so that
 * a different tab occupies the cells it was opened from; the row is
 * invoked; the ORIGINAL tab is what gets acted on.
 */
void test_ctxmenu_acts_on_the_target_captured_at_open(void)
{
    Ed ed;
    u32 target_id;
    u32 other_id;

    cx_fixture(&ed, 4);
    target_id = yew_tab_at(&ed, 3)->tab_id;
    other_id = yew_tab_at(&ed, 1)->tab_id;
    yew_tab_switch(&ed, 0);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press;

        (void)memset(&press, 0, sizeof(press));
        press.kind = (u16)YEW_EV_MOUSE;
        press.button = (u8)YEW_MB_RIGHT;
        press.ev = (u8)YEW_KEY_PRESS;
        press.col = 22U;
        press.row = 0U;
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), target_id);
    YEW_ASSERT_NOT_NULL(yew_ctx_target_path());

    /* The strip scrolls: those cells now name a different tab. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 1);
    yew_mouse_menu_draw(&ed);

    /* Row 0 is Close Tab.  Invoked by keyboard, so no coordinates are
     * involved at all — and it still closes the tab the menu was
     * opened for. */
    {
        Key enter = cx_key(YEW_KEY_ENTER);

        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, target_id), -1);
    YEW_ASSERT(yew_tab_index_of_id(&ed, other_id) >= 0);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * And with the target CLOSED under the open menu: the row must do
 * nothing at all rather than fall back to an index, which would name
 * whichever tab slid into the vacated slot.
 */
void test_ctxmenu_target_closed_under_it_is_inert(void)
{
    Ed ed;
    u32 target_id;
    u32 count_before;

    cx_fixture(&ed, 4);
    target_id = yew_tab_at(&ed, 3)->tab_id;
    yew_tab_switch(&ed, 0);
    YEW_ASSERT(yew_mouse_open_tab_menu(&ed, target_id, 0U, 1U));
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, target_id)));
    count_before = yew_tab_count(&ed);
    {
        Key enter = cx_key(YEW_KEY_ENTER);

        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), count_before);
    yew_ctx_close();
    yew_ed_free(&ed);
}

void test_ctxmenu_group_menu_captures_the_gid(void)
{
    Ed ed;
    u32 g;

    cx_fixture(&ed, 4);
    g = yew_group_create(&ed, "/src", "grp");
    yew_group_add_member(&ed, g, 3);
    yew_group_add_member(&ed, g, 4);
    yew_tab_switch(&ed, 0);
    yew_ed_layout(&ed);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, -(i32)g);
    {
        Key press;

        (void)memset(&press, 0, sizeof(press));
        press.kind = (u16)YEW_EV_MOUSE;
        press.button = (u8)YEW_MB_RIGHT;
        press.ev = (u8)YEW_KEY_PRESS;
        press.col = 22U;
        press.row = 0U;
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_GROUP);
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), g);
    /* A group menu has no path to capture — the group is not a file. */
    YEW_ASSERT(yew_ctx_target_path() == NULL);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * Sprint 57.11 supersedes Sprint 27 §9: a right-click inside a PANE
 * opens the DOCUMENT menu.
 *
 * The old test pinned the deferral ("opens nothing, so nobody wires a
 * menu to it before the decision is made"); the decision is made, and
 * the row set is §4's.  What this pins now is the shape the rest of the
 * sprint depends on: the menu opens, it opens for the DOC kind, and it
 * captured the leaf and the clicked cell so a row can put the cursor
 * where the user pointed.
 */
void test_ctxmenu_right_click_in_a_pane_opens_the_document_menu(void)
{
    Ed ed;
    Rect cell;

    cx_fixture(&ed, 1);
    yew_pane_tables_reset(&ed);
    YEW_ASSERT_EQ_I64(yew_pane_table_add_leaf(&ed, ed.pane_root), 0);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, 0);
    {
        Key press;

        (void)memset(&press, 0, sizeof(press));
        press.kind = (u16)YEW_EV_MOUSE;
        press.button = (u8)YEW_MB_RIGHT;
        press.ev = (u8)YEW_KEY_PRESS;
        press.col = 10U;
        press.row = 5U;
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_DOC);
    /* The leaf index, and the cell — not the pane rect, because the row
     * that places the cursor needs the cell the user clicked. */
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), 0U);
    cell = yew_ctx_target_rect_get();
    YEW_ASSERT_EQ_U64(cell.x, 10U);
    YEW_ASSERT_EQ_U64(cell.y, 5U);
    YEW_ASSERT(yew_ctx_rows() > 0U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The payload law, made fatal                                       */
/* ---------------------------------------------------------------- */

/*
 * A deliberately wrong handler: one that re-resolves its target from
 * the cells under the pointer instead of from the identity the menu
 * captured.  While a row's action runs, the region table is FROZEN, so
 * asking it anything aborts — which is what turns §5's rule from a
 * comment into something the build cannot ship past.
 */
void test_ctxmenu_a_row_handler_reading_a_payload_is_a_bug(void)
{
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    bytebuf_init(&output);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        yew_region_frame_begin();
        yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 3);
        yew_region_freeze(true);
        /* The wrong handler, in one line. */
        (void)yew_region_hit(1U, 0U);
        _exit(99);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    YEW_ASSERT_NOT_NULL(strstr((const char *)output.data,
                               "captured at open time"));
    bytebuf_free(&output);
    /* And an unfrozen table answers normally, so the guard is not
     * simply always on. */
    YEW_ASSERT(!yew_region_frozen());
}

/* ---------------------------------------------------------------- */
/* Sprint 57.11 §2: the bordered box                                 */
/* ---------------------------------------------------------------- */

/*
 * The corner goes BELOW-RIGHT of the anchor, so the cell the pointer is
 * on when the menu opens is the top-left BORDER cell.  A release on the
 * very cell that opened the menu then activates nothing — which is the
 * whole reason the box is offset rather than centred on the pointer.
 */
void test_ctxmenu_box_is_bordered_below_right_of_the_anchor(void)
{
    Rect box;

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(10U, 4U, (Rect){0U, 0U, 80U, 23U}));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_U64(box.x, 11U);
    YEW_ASSERT_EQ_U64(box.y, 5U);
    /* h = rows + 2; w = widest + 2·pad + 2, and `Close Other Tabs` is
     * the widest row at 16 cells. */
    YEW_ASSERT_EQ_U64(box.h, (u64)yew_ctx_rows() + 2U);
    YEW_ASSERT_EQ_U64(box.w, 20U);
    /* The anchor cell itself is not a row, and neither is the corner. */
    YEW_ASSERT(!yew_ctx_hover_at(10U, 4U));
    YEW_ASSERT(!yew_ctx_hover_at(box.x, box.y));
    yew_ctx_close();
}

void test_ctxmenu_hover_at_maps_cells_by_the_box_geometry(void)
{
    Rect box;

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);

    /* Row 3 is `Copy Path`: the highlight moves, so the router repaints
     * exactly once. */
    YEW_ASSERT(yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + 4U)));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
    /* The same cell again moves nothing — and that is the point: a
     * motion report per cell must not become a render per cell. */
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + 4U)));

    /* Border cells map to nothing on all four sides. */
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U), box.y));
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U),
                                 (u16)(box.y + box.h - 1U)));
    YEW_ASSERT(!yew_ctx_hover_at(box.x, (u16)(box.y + 1U)));
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + box.w - 1U),
                                 (u16)(box.y + 1U)));
    /* So do the greyed row and the separator. */
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + 2U)));
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + 3U)));
    /* And cells outside the box entirely. */
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + box.w), (u16)(box.y + 1U)));
    YEW_ASSERT(!yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + box.h)));
    /* None of which disturbed the highlight. */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);

    YEW_ASSERT(yew_ctx_hover_at((u16)(box.x + 1U), (u16)(box.y + 1U)));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    yew_ctx_close();
    /* Closed, it hovers nothing rather than reading a stale box. */
    YEW_ASSERT(!yew_ctx_hover_at(1U, 1U));
}

void test_ctxmenu_home_and_end_reach_the_first_and_last_rows(void)
{
    Key home = cx_key(YEW_KEY_HOME);
    Key end = cx_key(YEW_KEY_END);
    Key up = cx_key(YEW_KEY_UP);

    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 80U, 23U}));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    YEW_ASSERT(yew_ctx_key(&end));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
    /* Absolute, not relative: End from the last row stays there rather
     * than stepping off it. */
    YEW_ASSERT(yew_ctx_key(&end));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
    YEW_ASSERT(yew_ctx_key(&home));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    YEW_ASSERT(yew_ctx_key(&home));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    /* End then Up skips the separator and the greyed row above it. */
    YEW_ASSERT(yew_ctx_key(&end));
    YEW_ASSERT(yew_ctx_key(&up));
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    yew_ctx_close();
}

/* ---------------------------------------------------------------- */
/* Sprint 57.11 §2: shedding                                         */
/* ---------------------------------------------------------------- */

void test_ctxmenu_sheds_the_lowest_priorities_first(void)
{
    /* Room for everything: nothing sheds, and each separator carries
     * the priority of the row above it. */
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 20U}));
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 8U);
    YEW_ASSERT_EQ_U64(yew_ctx_shed_count(), 0U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(2U), 0U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(4U), 1U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(6U), 2U);

    /* Seven rows of room: priority 3 goes, and the separator it
     * orphaned goes with it rather than trailing the box. */
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 9U}));
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 6U);
    YEW_ASSERT_EQ_U64(yew_ctx_shed_count(), 2U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(5U), 2U);

    /* Four rows of room: priority 2 goes as well. */
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 6U}));
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 4U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(3U), 1U);

    /*
     * Three rows of room and two rows of room give the SAME menu: a
     * whole level goes at a time.  Shedding "just enough" would make
     * the surviving rows depend on the terminal height in ways the eye
     * cannot predict.
     */
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 5U}));
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 2U);
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 4U}));
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 2U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(0U), 0U);
    YEW_ASSERT_EQ_U64(yew_ctx_priority(1U), 0U);
    yew_ctx_close();
}

/*
 * Refusing is the LAST resort, not the first: a short terminal gets a
 * shorter menu, and only a terminal that cannot hold the rows which
 * never shed gets nothing at all.
 */
void test_ctxmenu_refuses_only_when_priority_zero_rows_do_not_fit(void)
{
    cx_priority_rows();
    YEW_ASSERT(!yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 3U}));
    YEW_ASSERT(!yew_ctx_active());

    /* One more row of room, and the same menu opens shorn to its
     * priority-0 rows. */
    cx_priority_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 4U}));
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 2U);
    YEW_ASSERT_EQ_U64(yew_ctx_shed_count(), 6U);
    /* A shorn menu is still navigable: the cursor lands on a row that
     * survived, not on an index into the menu that was built. */
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 0);
    yew_ctx_close();
}

/* Invariant 5: same state, same grid.  Two shows of the same menu into
 * the same rectangle must shed identically. */
void test_ctxmenu_shedding_is_deterministic(void)
{
    u8 pri[YEW_CTX_MAX_ROWS];
    Rect box;
    u32 rows = 0U;
    u32 shed = 0U;
    u32 i;
    int pass;

    (void)memset(pri, 0, sizeof(pri));
    (void)memset(&box, 0, sizeof(box));
    for (pass = 0; pass < 2; pass++) {
        cx_priority_rows();
        YEW_ASSERT(yew_ctx_show(3U, 2U, (Rect){0U, 0U, 40U, 6U}));
        if (pass == 0) {
            rows = yew_ctx_rows();
            shed = yew_ctx_shed_count();
            box = yew_ctx_box();
            for (i = 0U; i < rows; i++)
                pri[i] = yew_ctx_priority(i);
            continue;
        }
        YEW_ASSERT_EQ_U64(yew_ctx_rows(), rows);
        YEW_ASSERT_EQ_U64(yew_ctx_shed_count(), shed);
        YEW_ASSERT_EQ_U64(yew_ctx_box().x, box.x);
        YEW_ASSERT_EQ_U64(yew_ctx_box().y, box.y);
        YEW_ASSERT_EQ_U64(yew_ctx_box().w, box.w);
        YEW_ASSERT_EQ_U64(yew_ctx_box().h, box.h);
        for (i = 0U; i < rows; i++)
            YEW_ASSERT_EQ_U64(yew_ctx_priority(i), pri[i]);
    }
    yew_ctx_close();
}

/*
 * A separator is punctuation, not a row: one that would lead, trail or
 * double up is dropped, whether the caller wrote it that way or a shed
 * made it so.
 */
void test_ctxmenu_separators_never_lead_trail_or_double(void)
{
    u32 i;

    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_sep();
    yew_ctx_sep();
    yew_ctx_item("One", NULL, CX_A, true, 0U);
    yew_ctx_sep();
    yew_ctx_sep();
    yew_ctx_item("Two", NULL, CX_B, true, 0U);
    yew_ctx_sep();
    yew_ctx_sep();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 20U}));
    /* `One`, one rule, `Two` — and nothing else. */
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 3U);
    YEW_ASSERT(yew_ctx_row_enabled(0U));
    YEW_ASSERT(!yew_ctx_row_enabled(1U));
    YEW_ASSERT(yew_ctx_row_enabled(2U));
    for (i = 0U; i < yew_ctx_rows(); i++)
        YEW_ASSERT_EQ_U64(yew_ctx_priority(i), 0U);
    yew_ctx_close();
}

/* ---------------------------------------------------------------- */
/* Sprint 57.11 §2: the width clamp                                  */
/* ---------------------------------------------------------------- */

void test_ctxmenu_width_clamp_drops_accels_before_clipping_labels(void)
{
    /* Room for label, gap and accelerator: the column stays. */
    cx_wide_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 60U, 20U}));
    YEW_ASSERT(!yew_ctx_accels_hidden());
    YEW_ASSERT_EQ_U64(yew_ctx_box().w, 43U);

    /*
     * Four cells short: the ACCELERATORS go, as a column.  A menu with
     * half its accelerators is worse than one with none — the eye reads
     * the gap as "this row has no shortcut".
     */
    cx_wide_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 40U, 20U}));
    YEW_ASSERT(yew_ctx_accels_hidden());
    YEW_ASSERT_EQ_U64(yew_ctx_box().w, 34U);

    /* Narrower still: only now are the labels clipped, and the box
     * takes exactly the width it was allowed. */
    cx_wide_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 24U, 20U}));
    YEW_ASSERT(yew_ctx_accels_hidden());
    YEW_ASSERT_EQ_U64(yew_ctx_box().w, 24U);

    /* A menu with no accelerators at all, squeezed the same way, has
     * hidden nothing — the seam says what it means. */
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("Absolutely Enormous Label Here", NULL, CX_A, true, 0U);
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 24U, 20U}));
    YEW_ASSERT(!yew_ctx_accels_hidden());
    YEW_ASSERT_EQ_U64(yew_ctx_box().w, 24U);
    yew_ctx_close();
}

static void cx_assert_glyph(const Grid *g, u16 row, u16 col, YewGlyph want)
{
    const Cell *cell = &g->back[(size_t)row * g->cols + col];

    YEW_ASSERT((cell->flags & CELL_INTERNED) == 0U);
    YEW_ASSERT(memcmp(cell->utf8, yew_glyph(want),
                      yew_glyph_len(want)) == 0);
}

/*
 * The frame is drawn from the glyph table (s27 §7), so the ASCII
 * vocabulary gets a menu too — and the label is CLIPPED to the room the
 * row has.  A label that ran through the right-hand border would be the
 * width clamp failing silently, which is the one failure mode a golden
 * cannot catch until somebody looks at it.
 */
void test_ctxmenu_draw_frames_the_box_and_clips_inside_it(void)
{
    Ed ed;
    Rect box;
    u16 y;

    cx_fixture(&ed, 0);
    cx_wide_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, (Rect){0U, 0U, 24U, 20U}));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_U64(box.w, 24U);
    yew_region_frame_begin();
    yew_ctx_draw(&ed.grid, NULL);

    cx_assert_glyph(&ed.grid, box.y, box.x, YEW_GLYPH_BORDER_TL);
    cx_assert_glyph(&ed.grid, box.y, (u16)(box.x + box.w - 1U),
                    YEW_GLYPH_BORDER_TR);
    cx_assert_glyph(&ed.grid, (u16)(box.y + box.h - 1U), box.x,
                    YEW_GLYPH_BORDER_BL);
    cx_assert_glyph(&ed.grid, (u16)(box.y + box.h - 1U),
                    (u16)(box.x + box.w - 1U), YEW_GLYPH_BORDER_BR);
    cx_assert_glyph(&ed.grid, box.y, (u16)(box.x + 1U),
                    YEW_GLYPH_BORDER_H);
    for (y = (u16)(box.y + 1U); y < (u16)(box.y + box.h - 1U); y++) {
        cx_assert_glyph(&ed.grid, y, box.x, YEW_GLYPH_BORDER_V);
        cx_assert_glyph(&ed.grid, y, (u16)(box.x + box.w - 1U),
                        YEW_GLYPH_BORDER_V);
    }
    /* The label starts one pad cell in, and stops before the frame. */
    YEW_ASSERT_EQ_U64(ed.grid.back[(size_t)(box.y + 1U) * ed.grid.cols +
                                   box.x + 2U].utf8[0], (u64)'A');
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The document menu's target is a RECT — which leaf, which cell — so the
 * cursor can be placed where the click was when a row fires.  Like the
 * id and the path it is captured at open and cleared by the next begin.
 */
void test_ctxmenu_captures_a_target_rect(void)
{
    Rect got;

    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_target(7U, "/tmp/yew-ctx-rect.txt");
    yew_ctx_target_rect((Rect){3U, 9U, 1U, 1U});
    got = yew_ctx_target_rect_get();
    YEW_ASSERT_EQ_U64(got.x, 3U);
    YEW_ASSERT_EQ_U64(got.y, 9U);
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), 7U);

    yew_ctx_begin((u32)YEW_CTX_KIND_GROUP);
    got = yew_ctx_target_rect_get();
    YEW_ASSERT_EQ_U64(got.x, 0U);
    YEW_ASSERT_EQ_U64(got.y, 0U);
    YEW_ASSERT_EQ_U64(got.w, 0U);
    YEW_ASSERT_EQ_U64(got.h, 0U);
    YEW_ASSERT(yew_ctx_target_path() == NULL);
    yew_ctx_close();
}
