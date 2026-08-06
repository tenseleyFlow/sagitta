/*
 * Sprint 26 §6: the three picker instances.
 *
 * The claim §5 makes is that every future list is a PickerSpec and not
 * a new widget.  These tests are where that is either true or it is
 * not, so each instance is driven THROUGH the picker — open, filter,
 * accept — rather than by calling its accept callback directly.
 *
 * Two properties carry their own risk:
 *
 * PREVIEW ALLOCATES NOTHING (DoD 11).  Going through the buffer list
 * would make a deferred tab look resident, and residency is asked of
 * the allocation — so allocating IS the lie.  Counted, not asserted by
 * inspection.
 *
 * THE UNDO PICKER'S CLOCK IS INJECTED.  "3 minutes ago" read from a
 * real clock is a row that differs on every run, which is a golden
 * nobody can pin (invariant 5).
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "term/input.h"
#include "text/file.h"
#include "ui/layout.h"
#include "ui/picker.h"
#include "ui/pickers.h"
#include "ui/tabs.h"
#include "util/arena.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct PsFix {
    char root[128];
    Ed ed;
} PsFix;

static void ps_rm_rf(const char *path)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void ps_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);

    SAG_ASSERT(n > 0 && (size_t)n < cap);
}

static void ps_file(const PsFix *f, const char *rel, const char *body)
{
    char path[512];
    FILE *fp;

    ps_join(path, sizeof(path), f->root, rel);
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs(body, fp);
    (void)fclose(fp);
}

static void ps_dir(const PsFix *f, const char *rel)
{
    char path[512];

    ps_join(path, sizeof(path), f->root, rel);
    SAG_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void ps_make(PsFix *f)
{
    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-pickers-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->root);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    SAG_ASSERT(sag_grid_init(&f->ed.grid, &f->ed.interner, 24U, 80U));
    f->ed.grid_ready = true;
    sag_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void ps_remove(PsFix *f)
{
    sag_picker_close(&f->ed, false);
    sag_ed_free(&f->ed);
    ps_rm_rf(f->root);
}

static CmdStatus ps_run(PsFix *f, CmdStatus (*fn)(CmdCtx *))
{
    CmdCtx cx = {0};

    cx.ed = &f->ed;
    cx.win = f->ed.win;
    return fn(&cx);
}

static void ps_type(PsFix *f, const char *s)
{
    size_t i;

    for (i = 0U; s[i] != '\0'; i++) {
        Key k;

        (void)memset(&k, 0, sizeof(k));
        k.code = (u32)(u8)s[i];
        k.text[0] = (u8)s[i];
        k.ntext = 1U;
        SAG_ASSERT(sag_picker_key(&f->ed, &k));
    }
}

static void ps_press(PsFix *f, u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = code;
    SAG_ASSERT(sag_picker_key(&f->ed, &k));
}

/* ---------------------------------------------------------------- */
/* File finder                                                      */
/* ---------------------------------------------------------------- */

/* Opens, ranks, and opens the chosen file as a tab. */
void test_pickers_file_finder_opens_a_tab(void)
{
    PsFix f;
    int before;

    ps_make(&f);
    ps_file(&f, "alpha.c", "int a;\n");
    ps_dir(&f, "src");
    ps_file(&f, "src/tabs.c", "int t;\n");
    ps_file(&f, "src/other.c", "int o;\n");

    before = sag_tab_count(&f.ed);
    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_file), SAG_CMD_OK);
    SAG_ASSERT(sag_picker_active(&f.ed));
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 3U);

    /* Narrow to the one file, then take it. */
    ps_type(&f, "tabs");
    SAG_ASSERT(sag_picker_shown(&f.ed) >= 1U);
    ps_press(&f, SAG_KEY_ENTER);
    SAG_ASSERT(!sag_picker_active(&f.ed));
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), before + 1);
    {
        char want[512];

        ps_join(want, sizeof(want), f.root, "src/tabs.c");
        SAG_ASSERT(sag_tab_find_by_path(&f.ed, want) >= 0);
    }
    ps_remove(&f);
}

/* The finder honours .gitignore, because it walks with it on. */
void test_pickers_file_finder_respects_gitignore(void)
{
    PsFix f;

    ps_make(&f);
    ps_file(&f, ".gitignore", "*.o\n");
    ps_file(&f, "keep.c", "int k;\n");
    ps_file(&f, "drop.o", "junk\n");

    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_file), SAG_CMD_OK);
    /* keep.c only — .gitignore itself is hidden, drop.o is ignored. */
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 1U);
    ps_remove(&f);
}

/*
 * DoD 11: previewing creates NO Buffer and NO TextBuf.
 *
 * Twenty previews are twenty reads and zero buffers.  A preview that
 * went through sag_ws_file_buf would leave twenty deferred buffers
 * behind, each of which then reports resident — the s24 pitfall
 * restated.
 */
void test_pickers_preview_allocates_no_buffer(void)
{
    PsFix f;
    u32 bufs_before;
    u32 i;

    ps_make(&f);
    for (i = 0U; i < 20U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "f%02u.c", (unsigned)i);
        ps_file(&f, name, "line one\nline two\n");
    }
    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_file), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 20U);

    bufs_before = f.ed.ws.nbufs;
    sag_pickers_preview_reset();
    sag_file_load_count_reset();
    for (i = 0U; i < 20U; i++) {
        sag_pickers_preview_file(&f.ed, NULL, (i32)i,
                                 (Rect){0U, 0U, 40U, 10U});
    }
    /* Twenty reads... */
    SAG_ASSERT_EQ_U64(sag_pickers_preview_reads(), 20U);
    /* ...zero buffers, and not one of them went through the loader. */
    SAG_ASSERT_EQ_U64(f.ed.ws.nbufs, bufs_before);
    SAG_ASSERT_EQ_U64(sag_file_load_count(), 0U);
    ps_remove(&f);
}

/* A binary file previews as a summary rather than spraying control
 * bytes at the terminal. */
void test_pickers_preview_reports_a_binary_file(void)
{
    PsFix f;
    char path[512];
    FILE *fp;

    ps_make(&f);
    ps_join(path, sizeof(path), f.root, "blob.bin");
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fwrite("ab\0cd", 1U, 5U, fp);
    (void)fclose(fp);

    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_file), SAG_CMD_OK);
    sag_pickers_preview_reset();
    sag_pickers_preview_file(&f.ed, NULL, 0, (Rect){0U, 0U, 40U, 10U});
    /* It read it and did not crash; the NUL heuristic did the rest. */
    SAG_ASSERT_EQ_U64(sag_pickers_preview_reads(), 1U);
    ps_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Buffer switcher                                                  */
/* ---------------------------------------------------------------- */

/*
 * The payload is a TAB ID, so a tab closing while the picker is open
 * cannot make Enter switch to the wrong file.
 */
void test_pickers_buffer_switcher_uses_tab_ids(void)
{
    PsFix f;
    char pa[512];
    char pb[512];
    int t_a;
    int t_b;
    u32 id_b;

    ps_make(&f);
    ps_file(&f, "a.c", "int a;\n");
    ps_file(&f, "b.c", "int b;\n");
    ps_join(pa, sizeof(pa), f.root, "a.c");
    ps_join(pb, sizeof(pb), f.root, "b.c");
    t_a = sag_tab_open(&f.ed, pa);
    t_b = sag_tab_open(&f.ed, pb);
    SAG_ASSERT(t_a >= 0 && t_b >= 0);
    id_b = sag_tab_at(&f.ed, t_b)->tab_id;

    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_buffer), SAG_CMD_OK);
    /* The scratch tab plus the two files. */
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 3U);
    ps_type(&f, "b.c");
    /* The selection is the tab's ID, not its index. */
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), (i32)id_b);
    ps_press(&f, SAG_KEY_ENTER);
    SAG_ASSERT_EQ_I64(sag_tab_at(&f.ed, f.ed.tabs.active)->tab_id,
                      (i32)id_b);
    ps_remove(&f);
}

/*
 * Flags are DERIVED: modified comes from undo, deferred from the
 * allocation.  Neither is a stored bit that can drift.
 */
void test_pickers_buffer_switcher_marks_deferred_tabs(void)
{
    PsFix f;
    char pa[512];
    int t;

    ps_make(&f);
    ps_file(&f, "a.c", "int a;\n");
    ps_join(pa, sizeof(pa), f.root, "a.c");
    t = sag_tab_open(&f.ed, pa);
    SAG_ASSERT(t >= 0);
    /* Freshly opened and never switched to: not resident. */
    SAG_ASSERT(!sag_tab_is_resident(&f.ed, t));

    SAG_ASSERT_EQ_I64(ps_run(&f, sag_find_cmd_buffer), SAG_CMD_OK);
    ps_type(&f, "a.c");
    /* Accepting hydrates it — which is exactly when the read is worth
     * paying for. */
    ps_press(&f, SAG_KEY_ENTER);
    SAG_ASSERT(sag_tab_is_resident(&f.ed, t));
    ps_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Undo branch picker — Sprint 10 §11's deferral                    */
/* ---------------------------------------------------------------- */

/*
 * Rows come from the undo tree, and the clock is INJECTED so the
 * descriptions are reproducible.
 */
void test_pickers_undo_branches_lists_the_tree(void)
{
    PsFix f;
    EditCtx ec;
    u32 total_a;
    u32 total_b;

    ps_make(&f);
    /* Three edits, so the tree has something to list. */
    {
        u32 i;

        for (i = 0U; i < 3U; i++) {
            ec = sag_ed_edit_ctx(&f.ed);
            sag_undo_begin(&ec, SAG_TXN_TYPE);
            (void)sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U);
            sag_undo_end(&ec);
            sag_ed_finish_edit(&f.ed, &ec);
        }
    }
    sag_pickers_set_now(1700000000);
    SAG_ASSERT_EQ_I64(ps_run(&f, sag_undo_cmd_branches), SAG_CMD_OK);
    SAG_ASSERT(sag_picker_active(&f.ed));
    total_a = sag_picker_total(&f.ed);
    SAG_ASSERT(total_a >= 2U);
    sag_picker_close(&f.ed, false);

    /* The SAME injected clock produces the same list — which is what
     * makes a golden of these rows possible at all. */
    SAG_ASSERT_EQ_I64(ps_run(&f, sag_undo_cmd_branches), SAG_CMD_OK);
    total_b = sag_picker_total(&f.ed);
    SAG_ASSERT_EQ_U64(total_a, total_b);
    sag_pickers_set_now(0);
    ps_remove(&f);
}

/* Accepting a row moves the document to that undo state. */
void test_pickers_undo_branches_travels_to_a_state(void)
{
    PsFix f;
    EditCtx ec;
    u64 len_after_edits;

    ps_make(&f);
    ec = sag_ed_edit_ctx(&f.ed);
    sag_undo_begin(&ec, SAG_TXN_TYPE);
    (void)sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"hello", 5U);
    sag_undo_end(&ec);
    sag_ed_finish_edit(&f.ed, &ec);
    len_after_edits = sag_textbuf_len(sag_ed_doc(&f.ed)->tb);
    SAG_ASSERT_EQ_U64(len_after_edits, 5U);

    sag_pickers_set_now(1700000000);
    SAG_ASSERT_EQ_I64(ps_run(&f, sag_undo_cmd_branches), SAG_CMD_OK);
    /* Row 0 is the root — the state before the insert. */
    ps_press(&f, SAG_KEY_HOME);
    ps_press(&f, SAG_KEY_ENTER);
    SAG_ASSERT(!sag_picker_active(&f.ed));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(sag_ed_doc(&f.ed)->tb), 0U);
    sag_pickers_set_now(0);
    ps_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Registration                                                     */
/* ---------------------------------------------------------------- */

void test_pickers_commands_are_registered(void)
{
    static const char *const names[] = {
        "ed.find.file", "ed.find.buffer", "ed.undo.branches"};
    u32 i;

    sag_cmd_shutdown();
    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(names); i++) {
        CmdId id = sag_cmd_lookup(names[i], (u32)strlen(names[i]));
        const CmdDesc *desc;

        SAG_ASSERT(id.v != 0U);
        desc = sag_cmd_desc(id);
        SAG_ASSERT_NOT_NULL(desc);
        /* Live, not deferred: these three ship this sprint. */
        SAG_ASSERT((desc->flags & SAG_CMD_DEFERRED) == 0U);
    }
    sag_cmd_shutdown();
    sag_cmd_init();
}

/*
 * DoD 13: the pickers this sprint does NOT ship name their sprint
 * rather than reading as "no such command".
 */
void test_pickers_deferred_ones_name_their_sprint(void)
{
    CmdId id;
    const CmdDesc *desc;

    sag_cmd_shutdown();
    sag_cmd_init();
    id = sag_cmd_lookup("ed.find.command", 15U);
    SAG_ASSERT(id.v != 0U);
    desc = sag_cmd_desc(id);
    SAG_ASSERT_NOT_NULL(desc);
    SAG_ASSERT((desc->flags & SAG_CMD_DEFERRED) != 0U);
    SAG_ASSERT_NOT_NULL(strstr(desc->help, "Sprint 38"));
    sag_cmd_shutdown();
    sag_cmd_init();
}
