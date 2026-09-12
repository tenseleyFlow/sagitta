#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.11 §4: the row sets, pinned.
 *
 * WHAT THIS FILE IS FOR.  A context menu is a promise about WHERE the
 * pointer has to go: the hand learns that `Copy` is the second row and
 * stops reading.  So the label list, its order, its separators and its
 * priorities are a contract, and the enable flags are the other half of
 * it — a row that is greyed when it should be live is a feature the
 * user concludes is broken, and a row that is live when it should be
 * greyed is a command that fails after the click.
 *
 * Every builder is therefore checked by EXACT LABEL LIST, and every
 * enable condition is exercised in BOTH states.  The one thing these
 * tests deliberately do not check is what a row's command does: that
 * belongs to the command's own tests, and
 * `ctxrows_every_row_resolves_to_a_registry_command` is the seam
 * between the two — it proves each row names something that exists.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "text/edit.h"
#include "ui/ctxmenu.h"
#include "ui/ctxrows.h"
#include "ui/groups.h"
#include "ui/gutter.h"
#include "ui/layout.h"
#include "term/tty.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "ui/win.h"
#if YEW_WITH_LSP
#include "mod/lsp/lsp.h"
#endif
#include "util/intern.h"

#if YEW_WITH_FUSS
#include "mod/git/fussmode.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"
#endif

/* ---------------------------------------------------------------- */
/* Fixture and row assertions                                       */
/* ---------------------------------------------------------------- */

static void cr_fixture(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    yew_ed_layout(ed);
    ed->now_ms = 1000;
}

/*
 * The leaf index a document menu is addressed by.  It is a PAINT-TIME
 * identity — the renderer hands each leaf its number as it draws it —
 * so a test that wants a document menu has to have painted, or the id
 * resolves to no window and every row reads as "no buffer".
 */
static i32 cr_leaf(Ed *ed)
{
    i32 leaf;

    yew_pane_tables_reset(ed);
    leaf = yew_pane_table_add_leaf(ed, ed->focus);
    YEW_ASSERT(leaf >= 0);
    return leaf;
}

static void cr_build(Ed *ed, CtxKind kind, u32 id)
{
    CtxContext c;

    (void)memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.id = id;
    c.payload = (i32)id;
    /*
     * CLOSED FIRST, exactly as the router does it (`menu_open_at`
     * closes whatever was up before it builds).  Without it a builder
     * that refuses — a target that went away — would leave the PREVIOUS
     * menu's rows standing, and a test would read them as this build's.
     */
    yew_ctx_close();
    yew_ctx_build(ed, &c);
}

/*
 * The row list, exactly.  An empty string is a separator — which is a
 * row for these purposes, because a separator that moves changes where
 * every row below it is.
 */
static void cr_rows_are(const char *const *want, size_t n)
{
    size_t i;

    YEW_ASSERT_EQ_U64(yew_ctx_rows(), (u64)n);
    for (i = 0U; i < n; i++) {
        if (want[i][0] == '\0') {
            YEW_ASSERT(yew_ctx_row_is_sep((u32)i));
            continue;
        }
        YEW_ASSERT(!yew_ctx_row_is_sep((u32)i));
        YEW_ASSERT_EQ_STR(yew_ctx_row_label((u32)i), want[i]);
    }
}

static u32 cr_index(const char *label)
{
    u32 rows = yew_ctx_rows();
    u32 i;

    for (i = 0U; i < rows; i++)
        if (strcmp(yew_ctx_row_label(i), label) == 0)
            return i;
    return UINT32_MAX;
}

static bool cr_has(const char *label)
{
    return cr_index(label) != UINT32_MAX;
}

static bool cr_enabled(const char *label)
{
    u32 at = cr_index(label);

    YEW_ASSERT(at != UINT32_MAX);
    return yew_ctx_row_enabled(at);
}

static u8 cr_priority(const char *label)
{
    u32 at = cr_index(label);

    YEW_ASSERT(at != UINT32_MAX);
    return yew_ctx_priority(at);
}

/* ---------------------------------------------------------------- */
/* The document                                                     */
/* ---------------------------------------------------------------- */

/*
 * A scratch buffer in no repository with no language server: the
 * baseline shape, and the one both optional sections are absent from.
 */
void test_ctxrows_doc_rows_match_the_contract(void)
{
    static const char *const want[] = {
        "Cut", "Copy", "Paste", "Delete", "Select All",
        "",
        "Undo", "Redo",
        "",
        "Split Right", "Split Below", "Close Pane",
        "",
        "Save", "Save As...", "Reload",
        "",
        "Command Palette...", "Find File...", "Go to Line...",
        "Toggle Wrap"
    };
    Ed ed;

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    /* The priorities §4 brackets, which decide what a short terminal
     * keeps.  The palette is priority 0 because it is the row that can
     * always take the user the rest of the way. */
    YEW_ASSERT_EQ_U64(cr_priority("Cut"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Copy"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Paste"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Delete"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Select All"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Undo"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Redo"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Split Right"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Split Below"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Close Pane"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Save"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Save As..."), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Reload"), 3U);
    YEW_ASSERT_EQ_U64(cr_priority("Command Palette..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Find File..."), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Go to Line..."), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Toggle Wrap"), 3U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The enable conditions, each in BOTH states.
 *
 * A scratch buffer starts clean, unselected, writable and with an empty
 * register, so the first build is the "everything off" half and the
 * changes below are the other.
 */
void test_ctxrows_doc_enables_follow_the_live_state(void)
{
    Ed ed;
    RegVal v;
    Cursor *c;

    cr_fixture(&ed);

    /* No selection, nothing in the register, nothing to undo. */
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(!cr_enabled("Cut"));
    YEW_ASSERT(!cr_enabled("Copy"));
    YEW_ASSERT(!cr_enabled("Delete"));
    YEW_ASSERT(!cr_enabled("Paste"));
    YEW_ASSERT(!cr_enabled("Undo"));
    YEW_ASSERT(!cr_enabled("Redo"));
    YEW_ASSERT(!cr_enabled("Save"));
    YEW_ASSERT(!cr_enabled("Reload")); /* a scratch has no path */
    YEW_ASSERT(!cr_enabled("Close Pane")); /* one leaf */
    /* Select All is the row that is always live: there is always a
     * buffer to select. */
    YEW_ASSERT(cr_enabled("Select All"));

    /* A selection, made the ordinary way. */
    {
        EditCtx ec = yew_ed_edit_ctx(&ed);

        yew_edit_insert(&ec, yew_ed_cursor(&ed)->pos,
                        (const u8 *)"hello world\n", 12U);
        yew_ed_finish_edit(&ed, &ec);
    }
    c = yew_ed_cursor(&ed);
    c->anchor = (ByteOff){0U};
    c->pos = (ByteOff){5U};
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(cr_enabled("Cut"));
    YEW_ASSERT(cr_enabled("Copy"));
    YEW_ASSERT(cr_enabled("Delete"));
    /* The insert made it dirty and gave it something to undo. */
    YEW_ASSERT(cr_enabled("Save"));
    YEW_ASSERT(cr_enabled("Undo"));
    YEW_ASSERT(!cr_enabled("Redo"));

    /* H mode is a selection even with pos == anchor: H over one
     * grapheme IS a selection of one. */
    c->anchor = c->pos;
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(!cr_enabled("Copy"));
    ed.mode = YEW_MODE_H;
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(cr_enabled("Copy"));
    ed.mode = YEW_MODE_L;

    /* Undone: now there is a redo and no undo. */
    {
        EditCtx ec = yew_ed_edit_ctx(&ed);

        YEW_ASSERT(yew_undo(&ec));
        yew_ed_finish_edit(&ed, &ec);
    }
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(!cr_enabled("Undo"));
    YEW_ASSERT(cr_enabled("Redo"));

    /* Register `"` non-empty unlocks Paste; read-only locks every row
     * that writes, and locks NONE of the rows that only read. */
    yew_regval_init(&v);
    bytebuf_append(&v.bytes, (const u8 *)"clip", 4U);
    v.type = (u8)YEW_REG_CHARWISE;
    yew_reg_set(&ed.regs, (u8)'"', &v);
    yew_regval_free(&v);
    c->anchor = (ByteOff){0U};
    c->pos = (ByteOff){3U};
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(cr_enabled("Paste"));
    YEW_ASSERT(cr_enabled("Cut"));
    ed.buffer.flags |= YEW_BUF_READONLY;
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(!cr_enabled("Paste"));
    YEW_ASSERT(!cr_enabled("Cut"));
    YEW_ASSERT(!cr_enabled("Delete"));
    YEW_ASSERT(cr_enabled("Copy")); /* reading is always allowed */
    ed.buffer.flags &= ~(u32)YEW_BUF_READONLY;

    /* A second leaf unlocks Close Pane. */
    {
        CmdCtx cx = {0};

        cx.ed = &ed;
        cx.win = ed.win;
        cx.count = 1U;
        cx.source = YEW_SRC_TEST;
        YEW_ASSERT_EQ_I64(yew_pane_cmd_split_h(&cx), YEW_CMD_OK);
    }
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(cr_enabled("Close Pane"));
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The section-omission rule, from the side that is always testable:
 * with no server attached and no repository, neither section exists —
 * not greyed, ABSENT.  In a MODULES="" build the same assertion holds
 * for the stronger reason that the code is not compiled.
 */
void test_ctxrows_doc_optional_sections_are_absent_not_greyed(void)
{
    Ed ed;

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_DOC, (u32)cr_leaf(&ed));
    YEW_ASSERT(!cr_has("Go to Definition"));
    YEW_ASSERT(!cr_has("Find References..."));
    YEW_ASSERT(!cr_has("Rename..."));
    YEW_ASSERT(!cr_has("Hover"));
    YEW_ASSERT(!cr_has("Toggle Blame"));
    YEW_ASSERT(!cr_has("Diff"));
#if YEW_WITH_LSP
    /* The query the section hangs on, asked directly: a scratch buffer
     * has no server, and the shim answers the same in a stripped
     * build. */
    YEW_ASSERT(!yew_lsp_attached(&ed, &ed.buffer));
#endif
#if YEW_WITH_FUSS
    /* And the git half: no detected repository, so no git section. */
    YEW_ASSERT_NULL(yew_git_repo_cached(&ed));
#endif
    /* Whatever is absent, the menu still ends in the rows that always
     * exist — the shape is stable per (kind, availability). */
    YEW_ASSERT(cr_has("Command Palette..."));
    YEW_ASSERT(cr_has("Toggle Wrap"));
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The tab strip                                                    */
/* ---------------------------------------------------------------- */

void test_ctxrows_tab_rows_match_the_contract(void)
{
    static const char *const want[] = {
        "Close Tab", "Close Other Tabs",
        "",
        "Copy Path", "Remove from Group",
        "",
        "New Tab", "Open in Split Right", "Open in Split Below"
    };
    Ed ed;
    const Tab *t;
    u32 gid;

    cr_fixture(&ed);
    t = yew_tab_at(&ed, ed.tabs.active);
    YEW_ASSERT_NOT_NULL(t);
    cr_build(&ed, YEW_CTX_KIND_TAB, t->tab_id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    /* One tab, no path, no group: the three conditional rows are all
     * greyed, and all three are still THERE. */
    YEW_ASSERT(!cr_enabled("Close Tab"));
    YEW_ASSERT(!cr_enabled("Close Other Tabs"));
    YEW_ASSERT(!cr_enabled("Copy Path"));
    YEW_ASSERT(!cr_enabled("Remove from Group"));
    YEW_ASSERT(cr_enabled("New Tab"));
    YEW_ASSERT_EQ_U64(cr_priority("Close Tab"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Close Other Tabs"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Copy Path"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Remove from Group"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("New Tab"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Open in Split Right"), 3U);
    YEW_ASSERT_EQ_U64(cr_priority("Open in Split Below"), 3U);

    /* A second tab, with a path, in a group: the other half of all
     * three conditions.  The file has to EXIST — a tab's path is its
     * canonical realpath, and an absent file leaves it NULL, which is
     * the very condition `Copy Path` is greyed by. */
    {
        char file[1200];
        const char *tmp = getenv("TMPDIR");
        FILE *out;
        int idx;

        if (tmp == NULL || tmp[0] == '\0')
            tmp = "build/tmp";
        (void)snprintf(file, sizeof(file), "%s/yew-ctxrows-tab.txt", tmp);
        out = fopen(file, "wb");
        YEW_ASSERT_NOT_NULL(out);
        YEW_ASSERT(fputs("tab\n", out) >= 0);
        YEW_ASSERT_EQ_I64(fclose(out), 0);
        idx = yew_tab_open(&ed, file);
        YEW_ASSERT(idx >= 0);
        yew_tab_switch(&ed, idx);
        (void)unlink(file);
    }
    t = yew_tab_at(&ed, ed.tabs.active);
    YEW_ASSERT_NOT_NULL(t);
    gid = yew_group_create(&ed, "/tmp", "grp");
    YEW_ASSERT(gid != 0U);
    yew_group_add_member(&ed, gid, ed.tabs.active);
    t = yew_tab_at(&ed, ed.tabs.active);
    YEW_ASSERT_NOT_NULL(t);
    YEW_ASSERT(t->group_id == gid);
    cr_build(&ed, YEW_CTX_KIND_TAB, t->tab_id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT(cr_enabled("Close Tab"));
    YEW_ASSERT(cr_enabled("Close Other Tabs"));
    YEW_ASSERT(cr_enabled("Copy Path"));
    YEW_ASSERT(cr_enabled("Remove from Group"));
    /* The path the rows will act on, captured at build time. */
    YEW_ASSERT_NOT_NULL(yew_ctx_target_path());
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), t->tab_id);
    yew_ctx_close();
    yew_ed_free(&ed);
}

void test_ctxrows_group_rows_match_the_contract(void)
{
    static const char *const want[] = {
        "Edit Group...", "Rename Group...",
        "",
        "Close Group", "Dissolve Group"
    };
    Ed ed;
    u32 gid;

    cr_fixture(&ed);
    gid = yew_group_create(&ed, "/tmp", "grp");
    YEW_ASSERT(gid != 0U);
    cr_build(&ed, YEW_CTX_KIND_GROUP, gid);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), gid);
    YEW_ASSERT_EQ_U64(cr_priority("Edit Group..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Rename Group..."), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Close Group"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Dissolve Group"), 1U);
    /*
     * A gid that is not a group builds NOTHING — no rows, and in
     * particular not the previous menu's, which is why the router
     * closes before it builds and why cr_build does too.  A menu of
     * rows pointing at a group that is gone is worse than no menu.
     */
    cr_build(&ed, YEW_CTX_KIND_GROUP, gid + 99U);
    YEW_ASSERT_EQ_U64(yew_ctx_rows(), 0U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The strip's blank tail and the bare editor backdrop are ONE row set,
 * because they mean one thing: the pointer is on the workspace rather
 * than on anything in it.
 */
void test_ctxrows_strip_and_editor_share_one_row_set(void)
{
    static const char *const want[] = {
        "New Tab", "Open File...", "New Group...",
        "",
        "Command Palette...", "Find Buffer..."
    };
    Ed ed;

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_STRIP, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_STRIP);
    cr_build(&ed, YEW_CTX_KIND_EDITOR, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    /* Same rows, but the menu still knows which surface it is on —
     * the kind is a golden's vocabulary and the router's overlay
     * switch. */
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_EDITOR);
    YEW_ASSERT_EQ_U64(cr_priority("New Tab"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Open File..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("New Group..."), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Command Palette..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Find Buffer..."), 2U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Border and footer                                                */
/* ---------------------------------------------------------------- */

void test_ctxrows_border_rows_need_a_second_pane(void)
{
    static const char *const want[] = {
        "Close Pane", "Grow", "Shrink",
        "",
        "Focus Next"
    };
    Ed ed;
    CmdCtx cx = {0};

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_BORDER, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    /* With one leaf there is no border to have been clicked, but the
     * kind is reachable through `ed.ui.context_menu`; every row is
     * greyed rather than the menu being empty. */
    YEW_ASSERT(!cr_enabled("Close Pane"));
    YEW_ASSERT(!cr_enabled("Grow"));
    YEW_ASSERT(!cr_enabled("Shrink"));
    YEW_ASSERT(!cr_enabled("Focus Next"));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_pane_cmd_split_v(&cx), YEW_CMD_OK);
    cr_build(&ed, YEW_CTX_KIND_BORDER, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT(cr_enabled("Close Pane"));
    YEW_ASSERT(cr_enabled("Grow"));
    YEW_ASSERT(cr_enabled("Shrink"));
    YEW_ASSERT(cr_enabled("Focus Next"));
    YEW_ASSERT_EQ_U64(cr_priority("Close Pane"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Grow"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Shrink"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Focus Next"), 2U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The footer's number row NAMES THE CURRENT STYLE.  A row that read
 * `Line Numbers: rel` while the gutter showed absolute numbers would be
 * a row whose effect nobody could predict.
 */
void test_ctxrows_footer_names_the_current_number_style(void)
{
    static const char *const want[] = {
        "Command Palette...", "Go to Line...",
        "",
        "Toggle Wrap", "Line Numbers: none",
        "",
        "Disable Mouse"
    };
    Ed ed;

    cr_fixture(&ed);
    ed.win->number_style = YEW_NUM_NONE;
    cr_build(&ed, YEW_CTX_KIND_FOOTER, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(cr_priority("Command Palette..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Go to Line..."), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Toggle Wrap"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Line Numbers: none"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Disable Mouse"), 3U);
    ed.win->number_style = YEW_NUM_ABS;
    cr_build(&ed, YEW_CTX_KIND_FOOTER, 0U);
    YEW_ASSERT(cr_has("Line Numbers: abs"));
    ed.win->number_style = YEW_NUM_REL;
    cr_build(&ed, YEW_CTX_KIND_FOOTER, 0U);
    YEW_ASSERT(cr_has("Line Numbers: rel"));
    ed.win->number_style = YEW_NUM_HYBRID;
    cr_build(&ed, YEW_CTX_KIND_FOOTER, 0U);
    YEW_ASSERT(cr_has("Line Numbers: hybrid"));
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The transient overlays                                           */
/* ---------------------------------------------------------------- */

void test_ctxrows_overlay_rows_match_the_contract(void)
{
    static const char *const pick[] = {
        "Open", "Open in Split Right", "Open in Split Below",
        "",
        "Close"
    };
    static const char *const compl_rows[] = {
        "Accept", "Toggle Docs",
        "",
        "Cancel"
    };
    static const char *const one_close[] = {"Close"};
    static const char *const gp_row[] = {
        "Toggle",
        "",
        "Confirm", "Cancel"
    };
    static const char *const gp_box[] = {"Confirm", "Cancel"};
    Ed ed;

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_PICK_ROW, 7U);
    cr_rows_are(pick, YEW_ARRAY_LEN(pick));
    /* The ITEM PAYLOAD is the target, not the row index: the list
     * refilters while the menu is up. */
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), 7U);
    YEW_ASSERT_EQ_U64(cr_priority("Open"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Open in Split Right"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Open in Split Below"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Close"), 0U);

    cr_build(&ed, YEW_CTX_KIND_PICKER, 0U);
    cr_rows_are(one_close, YEW_ARRAY_LEN(one_close));
    cr_build(&ed, YEW_CTX_KIND_PANEL, 0U);
    cr_rows_are(one_close, YEW_ARRAY_LEN(one_close));
    cr_build(&ed, YEW_CTX_KIND_COMPL_ROW, 3U);
    cr_rows_are(compl_rows, YEW_ARRAY_LEN(compl_rows));
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), 3U);
    /*
     * The group picker's two shapes.  `Toggle` is a ROW's row — it
     * ticks the path the pointer is over — so it exists only on
     * GP_ROW, and its target is the LISTING INDEX the region carries.
     * `Confirm` and `Cancel` are the dialog's and are on both.
     */
    cr_build(&ed, YEW_CTX_KIND_GP_ROW, 2U);
    cr_rows_are(gp_row, YEW_ARRAY_LEN(gp_row));
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Toggle"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Confirm"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Cancel"), 0U);
    cr_build(&ed, YEW_CTX_KIND_GP, 0U);
    cr_rows_are(gp_box, YEW_ARRAY_LEN(gp_box));
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * ONE PANEL SLOT, TWO SHAPES.
 *
 * Hover and signature help have nothing to answer, so their menu is
 * the bare `Close`.  The rename confirmation is a question, and it gets
 * its three answers — and deliberately NOT `Close`, which would close
 * the panel and leave the rename waiting, so the user's next Enter
 * would apply a rename they believe they dismissed.
 *
 * The rename half needs a live language server, so it lives with the
 * other LSP row tests; what is checked unconditionally here is that the
 * plain panel shape is exactly one row and that it is the closing one.
 */
void test_ctxrows_panel_without_a_rename_is_a_bare_close(void)
{
    static const char *const one_close[] = {"Close"};
    Ed ed;
    u32 action;

    cr_fixture(&ed);
    cr_build(&ed, YEW_CTX_KIND_PANEL, 0U);
    cr_rows_are(one_close, YEW_ARRAY_LEN(one_close));
    action = yew_ctx_row_action(0U);
    YEW_ASSERT_EQ_U64(action, (u64)CTXA_OVERLAY_CLOSE);
    /* The closing row is the one action with no command, by design. */
    YEW_ASSERT(yew_ctx_actions[action].cmd == NULL);
    YEW_ASSERT(!cr_has("Apply"));
    YEW_ASSERT(!cr_has("Show Diff"));
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* FUSS                                                             */
/* ---------------------------------------------------------------- */

#if YEW_WITH_FUSS
typedef struct CrFussFix {
    char root[1024];
    char file[1200];
    char dir[1200];
    char nested[1400];
} CrFussFix;

static void cr_fuss_make(CrFussFix *fix)
{
    const char *tmp = getenv("TMPDIR");
    char *resolved;
    FILE *file;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "build/tmp";
    (void)snprintf(fix->root, sizeof(fix->root), "%s/yew-ctxrows-XXXXXX",
                   tmp);
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
    YEW_ASSERT(fputs("tracked bytes\n", file) >= 0);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    (void)snprintf(fix->dir, sizeof(fix->dir), "%s/sub", fix->root);
    YEW_ASSERT_EQ_I64(mkdir(fix->dir, 0700), 0);
    (void)snprintf(fix->nested, sizeof(fix->nested), "%s/deep.txt",
                   fix->dir);
    file = fopen(fix->nested, "wb");
    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT(fputs("nested bytes\n", file) >= 0);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void cr_fuss_drop(const CrFussFix *fix)
{
    (void)unlink(fix->nested);
    (void)rmdir(fix->dir);
    (void)unlink(fix->file);
    (void)rmdir(fix->root);
}

static void cr_fuss_enter(Ed *ed, const CrFussFix *fix)
{
    cr_fixture(ed);
    ed->ws.dir = arena_strdup(&ed->arena, fix->root);
    YEW_ASSERT_EQ_I64(yew_mode_enter(ed, YEW_MODE_F), YEW_CMD_OK);
    yew_fuss_tick(ed, ed->now_ms + 20);
}

/*
 * Publishes a status snapshot and rebuilds the tree from it — the same
 * path a real `git status` result takes, so the flags the rows read are
 * the flags the tree really carries.
 */
static void cr_fuss_status(Ed *ed, GitEntry *entries, size_t n)
{
    GitSnapshot *snap = yew_git_test_snapshot_mut(ed);

    YEW_ASSERT_NOT_NULL(snap);
    snap->state = YEW_GIT_OK;
    snap->entries.data = entries;
    snap->entries.len = n;
    snap->gen++;
    yew_fuss_tick(ed, ed->now_ms + 40);
}

static GitEntry cr_entry(const char *path, bool staged, bool unstaged,
                         bool untracked)
{
    GitEntry entry;

    (void)memset(&entry, 0, sizeof(entry));
    entry.kind = untracked ? GIT_E_UNTRACKED : GIT_E_ORDINARY;
    entry.path = (char *)path;
    entry.path_len = (u32)strlen(path);
    entry.staged = staged;
    entry.unstaged = unstaged;
    entry.untracked = untracked;
    return entry;
}

static u32 cr_path_id(Ed *ed, const char *path)
{
    u32 id = yew_intern(&ed->interner, path, strlen(path));

    YEW_ASSERT(id != 0U);
    return id;
}

void test_ctxrows_fuss_file_rows_follow_git_status(void)
{
    static const char *const want[] = {
        "Open", "Open in Split Right", "Open in Split Below", "Preview",
        "",
        "Stage", "Unstage", "Discard...",
        "",
        "Diff", "Blame",
        "",
        "Rename...", "Delete...", "Copy Path"
    };
    CrFussFix fix;
    GitEntry entry;
    Ed ed;
    u32 id;

    cr_fuss_make(&fix);
    cr_fuss_enter(&ed, &fix);
    id = cr_path_id(&ed, "plain.txt");

    /* UNSTAGED: stage yes, unstage no, discard yes. */
    entry = cr_entry("plain.txt", false, true, false);
    cr_fuss_status(&ed, &entry, 1U);
    cr_build(&ed, YEW_CTX_KIND_FUSS_FILE, id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_STR(yew_ctx_target_path(), "plain.txt");
    YEW_ASSERT(cr_enabled("Stage"));
    YEW_ASSERT(!cr_enabled("Unstage"));
    YEW_ASSERT(cr_enabled("Discard..."));
    YEW_ASSERT_EQ_U64(cr_priority("Open"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Open in Split Right"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Preview"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Stage"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Unstage"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Discard..."), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Diff"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Blame"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Rename..."), 3U);
    YEW_ASSERT_EQ_U64(cr_priority("Delete..."), 3U);
    YEW_ASSERT_EQ_U64(cr_priority("Copy Path"), 3U);

    /* STAGED: the mirror image, and the shape is identical. */
    entry = cr_entry("plain.txt", true, false, false);
    cr_fuss_status(&ed, &entry, 1U);
    cr_build(&ed, YEW_CTX_KIND_FUSS_FILE, id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT(!cr_enabled("Stage"));
    YEW_ASSERT(cr_enabled("Unstage"));
    YEW_ASSERT(cr_enabled("Discard..."));

    /* UNTRACKED: stageable, and NOT discardable — `git restore` has
     * nothing to restore it to, which is the guard the command
     * applies. */
    entry = cr_entry("plain.txt", false, false, true);
    cr_fuss_status(&ed, &entry, 1U);
    cr_build(&ed, YEW_CTX_KIND_FUSS_FILE, id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT(cr_enabled("Stage"));
    YEW_ASSERT(!cr_enabled("Unstage"));
    YEW_ASSERT(!cr_enabled("Discard..."));

    /* CLEAN: nothing to stage, nothing to unstage, nothing to discard —
     * and the rows are all still there, greyed. */
    entry = cr_entry("plain.txt", false, false, false);
    cr_fuss_status(&ed, &entry, 1U);
    cr_build(&ed, YEW_CTX_KIND_FUSS_FILE, id);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT(!cr_enabled("Stage"));
    YEW_ASSERT(!cr_enabled("Unstage"));
    YEW_ASSERT(!cr_enabled("Discard..."));
    /* The rows that only need a path never grey. */
    YEW_ASSERT(cr_enabled("Open"));
    YEW_ASSERT(cr_enabled("Diff"));
    YEW_ASSERT(cr_enabled("Blame"));
    /* Copying a name asks git nothing, so it survives every status. */
    YEW_ASSERT(cr_enabled("Copy Path"));
    yew_ctx_close();
    yew_ed_free(&ed);
    cr_fuss_drop(&fix);
}

/*
 * The directory menu: one toggle row whose LABEL is the state, and two
 * "all below" rows driven by the aggregate of the subtree.
 */
void test_ctxrows_fuss_dir_rows_follow_expansion_and_subtree(void)
{
    CrFussFix fix;
    GitEntry entries[1];
    Ed ed;
    u32 id;
    FussTarget target;

    cr_fuss_make(&fix);
    cr_fuss_enter(&ed, &fix);
    entries[0] = cr_entry("sub/deep.txt", false, true, false);
    cr_fuss_status(&ed, entries, 1U);
    id = cr_path_id(&ed, "sub");
    YEW_ASSERT(yew_fuss_path_target(&ed, id, &target));
    YEW_ASSERT(!target.is_file);
    /* The aggregate: the directory is unstaged because something under
     * it is. */
    YEW_ASSERT(target.unstaged);

    cr_build(&ed, YEW_CTX_KIND_FUSS_DIR, id);
    {
        const char *want[] = {
            "Open as Group...", target.expanded ? "Collapse" : "Expand",
            "",
            "Stage All Below", "Unstage All Below",
            "",
            "Copy Path"
        };

        cr_rows_are(want, YEW_ARRAY_LEN(want));
    }
    YEW_ASSERT_EQ_STR(yew_ctx_target_path(), "sub");
    YEW_ASSERT(cr_enabled("Stage All Below"));
    YEW_ASSERT(!cr_enabled("Unstage All Below"));
    YEW_ASSERT_EQ_U64(cr_priority("Open as Group..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Stage All Below"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Unstage All Below"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Copy Path"), 3U);
    YEW_ASSERT(cr_enabled("Copy Path"));

    /* Staged below: the other half of both rows. */
    entries[0] = cr_entry("sub/deep.txt", true, false, false);
    cr_fuss_status(&ed, entries, 1U);
    cr_build(&ed, YEW_CTX_KIND_FUSS_DIR, id);
    YEW_ASSERT(!cr_enabled("Stage All Below"));
    YEW_ASSERT(cr_enabled("Unstage All Below"));

    /* THE LABEL IS THE STATE.  Toggling the row's own command flips
     * which of the two words the row carries — one row, never two of
     * which one is always dead. */
    {
        bool was;
        CmdCtx cx = {0};

        YEW_ASSERT(yew_fuss_path_target(&ed, id, &target));
        was = target.expanded;
        YEW_ASSERT(cr_has(was ? "Collapse" : "Expand"));
        yew_fuss_select_path(&ed, id);
        cx.ed = &ed;
        cx.win = ed.win;
        cx.count = 1U;
        cx.source = YEW_SRC_TEST;
        YEW_ASSERT_EQ_I64(yew_fuss_cmd_nav_toggle(&cx), YEW_CMD_OK);
        YEW_ASSERT(yew_fuss_path_target(&ed, id, &target));
        YEW_ASSERT(target.expanded != was);
        cr_build(&ed, YEW_CTX_KIND_FUSS_DIR, id);
        YEW_ASSERT(cr_has(target.expanded ? "Collapse" : "Expand"));
        YEW_ASSERT(!cr_has(target.expanded ? "Expand" : "Collapse"));
    }
    yew_ctx_close();
    yew_ed_free(&ed);
    cr_fuss_drop(&fix);
}

/*
 * THE ROW THE MENU WAS OPENED ON IS THE ROW THAT ACTS.
 *
 * `Expand` / `Collapse` reads the SELECTED tree row, not an argument,
 * so the path target has to move the selection to the clicked row
 * before the command runs — otherwise a right-click on `sub` would
 * expand whatever the keyboard happened to be on.  This drives the
 * whole path: build, show, hover, Enter, through the router.
 */
void test_ctxrows_fuss_dir_menu_acts_on_the_clicked_row(void)
{
    CrFussFix fix;
    GitEntry entries[1];
    Ed ed;
    u32 dir_id;
    u32 file_id;
    FussTarget before;
    FussTarget after;
    u32 row;
    u32 rows;
    bool found = false;

    cr_fuss_make(&fix);
    cr_fuss_enter(&ed, &fix);
    entries[0] = cr_entry("sub/deep.txt", false, true, false);
    cr_fuss_status(&ed, entries, 1U);
    dir_id = cr_path_id(&ed, "sub");
    file_id = cr_path_id(&ed, "plain.txt");
    /* The selection starts somewhere ELSE, which is the only way this
     * test can tell "acted on the clicked row" from "acted on whatever
     * was selected". */
    yew_fuss_select_path(&ed, file_id);
    YEW_ASSERT(yew_fuss_path_target(&ed, dir_id, &before));

    cr_build(&ed, YEW_CTX_KIND_FUSS_DIR, dir_id);
    YEW_ASSERT(yew_ctx_show(2U, 2U,
                            (Rect){0U, 0U, ed.grid.cols,
                                   (u16)(ed.grid.rows - 1U)}));
    rows = yew_ctx_rows();
    for (row = 0U; row < rows; row++) {
        const char *label = yew_ctx_row_label(row);

        if (strcmp(label, "Expand") == 0 ||
            strcmp(label, "Collapse") == 0) {
            yew_ctx_hover((i32)row);
            found = true;
            break;
        }
    }
    YEW_ASSERT(found);
    {
        Key enter;

        (void)memset(&enter, 0, sizeof(enter));
        enter.kind = (u16)YEW_EV_KEY;
        enter.code = YEW_KEY_ENTER;
        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    /* Balanced on the way out, whatever the row did. */
    YEW_ASSERT(!yew_region_frozen());
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(yew_fuss_path_target(&ed, dir_id, &after));
    YEW_ASSERT(after.expanded != before.expanded);
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
    cr_fuss_drop(&fix);
}

void test_ctxrows_fuss_blank_rows_match_the_contract(void)
{
    static const char *const want[] = {
        "Refresh", "Commit...",
        "",
        "Push", "Pull", "Fetch",
        "",
        "Status", "History",
        "",
        "Leave FUSS"
    };
    CrFussFix fix;
    Ed ed;

    cr_fuss_make(&fix);
    cr_fuss_enter(&ed, &fix);
    cr_build(&ed, YEW_CTX_KIND_FUSS_BLANK, 0U);
    cr_rows_are(want, YEW_ARRAY_LEN(want));
    YEW_ASSERT_EQ_U64(cr_priority("Refresh"), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Commit..."), 0U);
    YEW_ASSERT_EQ_U64(cr_priority("Push"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Pull"), 1U);
    YEW_ASSERT_EQ_U64(cr_priority("Fetch"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("Status"), 2U);
    YEW_ASSERT_EQ_U64(cr_priority("History"), 2U);
    /* Leaving is priority 0: a drawer the pointer cannot get out of is
     * the one row a short terminal must never shed. */
    YEW_ASSERT_EQ_U64(cr_priority("Leave FUSS"), 0U);
    yew_ctx_close();
    yew_ed_free(&ed);
    cr_fuss_drop(&fix);
}

/*
 * An unknown path — the tree rebuilt under the menu, or a stripped
 * build — greys the status rows and keeps the rest.  It must not
 * pretend the file is clean, and it must not build an empty menu.
 */
void test_ctxrows_fuss_unknown_path_greys_the_status_rows(void)
{
    CrFussFix fix;
    Ed ed;
    u32 id;
    FussTarget target;

    cr_fuss_make(&fix);
    cr_fuss_enter(&ed, &fix);
    id = cr_path_id(&ed, "gone.txt");
    YEW_ASSERT(!yew_fuss_path_target(&ed, id, &target));
    YEW_ASSERT(!target.known);
    cr_build(&ed, YEW_CTX_KIND_FUSS_FILE, id);
    YEW_ASSERT(cr_has("Open"));
    YEW_ASSERT(!cr_enabled("Stage"));
    YEW_ASSERT(!cr_enabled("Unstage"));
    YEW_ASSERT(!cr_enabled("Discard..."));
    yew_ctx_close();
    yew_ed_free(&ed);
    cr_fuss_drop(&fix);
}
#endif /* YEW_WITH_FUSS */

/* ---------------------------------------------------------------- */
/* The seam between a row and its command                           */
/* ---------------------------------------------------------------- */

/*
 * EVERY ROW OF EVERY KIND NAMES SOMETHING THAT EXISTS.
 *
 * A row whose command is not in the registry is a row that does nothing
 * when it is clicked, and nothing about the menu would say so — the
 * label would look exactly like the ones that work.  This walks every
 * kind, builds it, and looks up each row's command by name.
 *
 * The two NULL-command actions are legitimate and are checked for what
 * they are rather than skipped: closing an overlay, and a picker row
 * whose target IS its action.
 */
void test_ctxrows_every_row_resolves_to_a_registry_command(void)
{
    static const CtxKind kinds[] = {
        YEW_CTX_KIND_TAB, YEW_CTX_KIND_GROUP, YEW_CTX_KIND_STRIP,
        YEW_CTX_KIND_DOC, YEW_CTX_KIND_BORDER, YEW_CTX_KIND_FOOTER,
        YEW_CTX_KIND_FUSS_FILE, YEW_CTX_KIND_FUSS_DIR,
        YEW_CTX_KIND_FUSS_BLANK, YEW_CTX_KIND_PICK_ROW,
        YEW_CTX_KIND_PICKER, YEW_CTX_KIND_COMPL_ROW,
        YEW_CTX_KIND_PANEL, YEW_CTX_KIND_GP_ROW, YEW_CTX_KIND_GP,
        YEW_CTX_KIND_EDITOR
    };
    Ed ed;
    size_t k;
    u32 gid;
    u32 path_id;
    u32 checked = 0U;

    cr_fixture(&ed);
    gid = yew_group_create(&ed, "/tmp", "grp");
    YEW_ASSERT(gid != 0U);
    path_id = yew_intern(&ed.interner, "plain.txt",
                         sizeof("plain.txt") - 1U);
    YEW_ASSERT(path_id != 0U);
    for (k = 0U; k < YEW_ARRAY_LEN(kinds); k++) {
        u32 rows;
        u32 i;
        u32 id = 0U;

        if (kinds[k] == YEW_CTX_KIND_GROUP)
            id = gid;
        else if (kinds[k] == YEW_CTX_KIND_TAB)
            id = yew_tab_at(&ed, ed.tabs.active)->tab_id;
        else if (kinds[k] == YEW_CTX_KIND_FUSS_FILE ||
                 kinds[k] == YEW_CTX_KIND_FUSS_DIR)
            id = path_id;
        cr_build(&ed, kinds[k], id);
        rows = yew_ctx_rows();
        /* Every kind builds SOMETHING.  "Right-click does nothing
         * here" is the feel this sprint exists to remove. */
        YEW_ASSERT(rows != 0U);
        for (i = 0U; i < rows; i++) {
            u32 action;
            const CtxActionDesc *d;

            if (yew_ctx_row_is_sep(i))
                continue;
            action = yew_ctx_row_action(i);
            YEW_ASSERT(action != (u32)CTXA_NONE);
            YEW_ASSERT(action < (u32)CTXA__N);
            d = &yew_ctx_actions[action];
            if (d->cmd == NULL) {
                /* Handled inline: an overlay close, or a picker accept
                 * whose mode is the argument. */
                YEW_ASSERT(d->target == CTX_TGT_NONE ||
                           d->target == CTX_TGT_PICK);
                continue;
            }
            {
                CmdId cid = yew_cmd_lookup(d->cmd, strlen(d->cmd));

                YEW_ASSERT(cid.v != YEW_CMD_NONE.v);
            }
            checked++;
        }
    }
    /* A guard against the walk silently checking nothing. */
    YEW_ASSERT(checked > 40U);
    yew_ctx_close();
    yew_ed_free(&ed);
}

/*
 * The whole table, not only the rows a fixture happens to build: an
 * action added without a command, or with a misspelt one, or with a
 * target its command's ARITY cannot accept, fails here even if no
 * builder uses it yet.
 *
 * THE ARITY HALF IS NOT PEDANTRY.  `yew_cmd_prepare` refuses a command
 * invoked with an `sarg` its arity does not declare — before the
 * command runs and without a message the user sees — so a
 * CTX_TGT_PATH row on a no-argument command is a row that silently
 * does nothing.  Deliverable 4 shipped exactly that bug once (the FUSS
 * `Expand` / `Collapse` row, which reads the selection and takes no
 * argument); CTX_TGT_FUSS_ROW is what it became, and this is what
 * would have caught it.
 */
void test_ctxrows_the_action_table_is_wholly_resolvable(void)
{
    Ed ed;
    u32 a;

    cr_fixture(&ed);
    for (a = 1U; a < (u32)CTXA__N; a++) {
        const CtxActionDesc *d = &yew_ctx_actions[a];
        const CmdDesc *desc;
        CmdId id;

        if (d->cmd == NULL) {
            YEW_ASSERT(d->target == CTX_TGT_NONE ||
                       d->target == CTX_TGT_PICK);
            continue;
        }
        id = yew_cmd_lookup(d->cmd, strlen(d->cmd));
        YEW_ASSERT(id.v != YEW_CMD_NONE.v);
        desc = yew_cmd_desc(id);
        YEW_ASSERT_NOT_NULL(desc);
        if (d->target == CTX_TGT_PATH) {
            YEW_ASSERT(desc->arity == (u8)YEW_ARITY_STR ||
                       desc->arity == (u8)YEW_ARITY_OPT_STR);
        } else {
            /* Every other target passes no argument, so a command that
             * REQUIRES one would be refused the same way. */
            YEW_ASSERT(desc->arity != (u8)YEW_ARITY_STR &&
                       desc->arity != (u8)YEW_ARITY_INT);
        }
    }
    yew_ed_free(&ed);
}
