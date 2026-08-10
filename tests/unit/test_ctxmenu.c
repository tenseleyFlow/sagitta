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
#include "ui/ctxmenu.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"

enum { CX_A = 11, CX_B = 22, CX_C = 33 };

static void cx_three_rows(void)
{
    yew_ctx_begin((u32)YEW_CTX_KIND_TAB);
    yew_ctx_item("Close Tab", "C-w", CX_A, true);
    yew_ctx_item("Close Other Tabs", NULL, CX_B, false); /* disabled */
    yew_ctx_sep();
    yew_ctx_item("Copy Path", NULL, CX_C, true);
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
    YEW_ASSERT_EQ_U64(box.h, yew_ctx_rows());

    /* Top-left: nothing to clamp, the anchor is honoured exactly. */
    cx_three_rows();
    YEW_ASSERT(yew_ctx_show(0U, 0U, allowed));
    box = yew_ctx_box();
    YEW_ASSERT_EQ_U64(box.x, 0U);
    YEW_ASSERT_EQ_U64(box.y, 0U);

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
    yew_ctx_item("x", NULL, CX_A, true);
    yew_ctx_item("y", NULL, CX_B, true);
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
    yew_ctx_item("Dissolve Group", NULL, CX_B, true);
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
 * §9: a right-click inside a PANE opens nothing.  Named as a test so
 * nobody quietly wires a document context menu to it before the
 * post-1.0 decision about what belongs in one.
 */
void test_ctxmenu_right_click_in_a_pane_opens_nothing(void)
{
    Ed ed;

    cx_fixture(&ed, 1);
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
    YEW_ASSERT(!yew_ctx_active());
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
