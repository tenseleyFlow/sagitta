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
#include "ui/cmdline.h"
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
    /*
     * Best-effort teardown of a fixture directory: the tests that
     * follow rebuild it, so a failure here is not one of theirs.
     *
     * Assigned rather than cast to void because glibc marks system
     * warn_unused_result under _FORTIFY_SOURCE, which Ubuntu's gcc
     * enables by default and Arch's does not — so the cast compiled
     * here and failed every gcc lane in CI.
     */
    {
        int removed = system(cmd);

        (void)removed;
    }
}

static void ps_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

static void ps_file(const PsFix *f, const char *rel, const char *body)
{
    char path[512];
    FILE *fp;

    ps_join(path, sizeof(path), f->root, rel);
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs(body, fp);
    (void)fclose(fp);
}

static void ps_dir(const PsFix *f, const char *rel)
{
    char path[512];

    ps_join(path, sizeof(path), f->root, rel);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void ps_make(PsFix *f)
{
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-pickers-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->root);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_grid_init(&f->ed.grid, &f->ed.interner, 24U, 80U));
    f->ed.grid_ready = true;
    yew_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void ps_remove(PsFix *f)
{
    yew_picker_close(&f->ed, false);
    yew_ed_free(&f->ed);
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
        YEW_ASSERT(yew_picker_key(&f->ed, &k));
    }
}

static void ps_press(PsFix *f, u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = code;
    YEW_ASSERT(yew_picker_key(&f->ed, &k));
}

static void ps_finish_scan(PsFix *f)
{
    while (yew_picker_tick(&f->ed))
        ;
}

static u32 command_probe_calls;

static CmdStatus command_probe(CmdCtx *cx)
{
    (void)cx;
    command_probe_calls++;
    return YEW_CMD_OK;
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

    before = yew_tab_count(&f.ed);
    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_file), YEW_CMD_OK);
    YEW_ASSERT(yew_picker_active(&f.ed));
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 3U);

    /* Narrow to the one file, then take it. */
    ps_type(&f, "tabs");
    YEW_ASSERT(yew_picker_shown(&f.ed) >= 1U);
    ps_press(&f, YEW_KEY_ENTER);
    YEW_ASSERT(!yew_picker_active(&f.ed));
    YEW_ASSERT_EQ_I64(yew_tab_count(&f.ed), before + 1);
    {
        char want[512];

        ps_join(want, sizeof(want), f.root, "src/tabs.c");
        YEW_ASSERT(yew_tab_find_by_path(&f.ed, want) >= 0);
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

    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_file), YEW_CMD_OK);
    /* keep.c only — .gitignore itself is hidden, drop.o is ignored. */
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 1U);
    ps_remove(&f);
}

/*
 * DoD 11: previewing creates NO Buffer and NO TextBuf.
 *
 * Twenty previews are twenty reads and zero buffers.  A preview that
 * went through yew_ws_file_buf would leave twenty deferred buffers
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
    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_file), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 20U);

    bufs_before = f.ed.ws.nbufs;
    yew_pickers_preview_reset();
    yew_file_load_count_reset();
    for (i = 0U; i < 20U; i++) {
        yew_pickers_preview_file(&f.ed, NULL, (i32)i,
                                 (Rect){0U, 0U, 40U, 10U});
    }
    /* Twenty reads... */
    YEW_ASSERT_EQ_U64(yew_pickers_preview_reads(), 20U);
    /* ...zero buffers, and not one of them went through the loader. */
    YEW_ASSERT_EQ_U64(f.ed.ws.nbufs, bufs_before);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), 0U);
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
    YEW_ASSERT_NOT_NULL(fp);
    (void)fwrite("ab\0cd", 1U, 5U, fp);
    (void)fclose(fp);

    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_file), YEW_CMD_OK);
    yew_pickers_preview_reset();
    yew_pickers_preview_file(&f.ed, NULL, 0, (Rect){0U, 0U, 40U, 10U});
    /* It read it and did not crash; the NUL heuristic did the rest. */
    YEW_ASSERT_EQ_U64(yew_pickers_preview_reads(), 1U);
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
    t_a = yew_tab_open(&f.ed, pa);
    t_b = yew_tab_open(&f.ed, pb);
    YEW_ASSERT(t_a >= 0 && t_b >= 0);
    id_b = yew_tab_at(&f.ed, t_b)->tab_id;

    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_buffer), YEW_CMD_OK);
    /* The scratch tab plus the two files. */
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 3U);
    ps_type(&f, "b.c");
    /* The selection is the tab's ID, not its index. */
    YEW_ASSERT_EQ_I64(yew_picker_selected(&f.ed), (i32)id_b);
    ps_press(&f, YEW_KEY_ENTER);
    YEW_ASSERT_EQ_I64(yew_tab_at(&f.ed, f.ed.tabs.active)->tab_id,
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
    t = yew_tab_open(&f.ed, pa);
    YEW_ASSERT(t >= 0);
    /* Freshly opened and never switched to: not resident. */
    YEW_ASSERT(!yew_tab_is_resident(&f.ed, t));

    YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_buffer), YEW_CMD_OK);
    ps_type(&f, "a.c");
    /* Accepting hydrates it — which is exactly when the read is worth
     * paying for. */
    ps_press(&f, YEW_KEY_ENTER);
    YEW_ASSERT(yew_tab_is_resident(&f.ed, t));
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
            ec = yew_ed_edit_ctx(&f.ed);
            yew_undo_begin(&ec, YEW_TXN_TYPE);
            (void)yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U);
            yew_undo_end(&ec);
            yew_ed_finish_edit(&f.ed, &ec);
        }
    }
    yew_pickers_set_now(1700000000);
    YEW_ASSERT_EQ_I64(ps_run(&f, yew_undo_cmd_branches), YEW_CMD_OK);
    YEW_ASSERT(yew_picker_active(&f.ed));
    total_a = yew_picker_total(&f.ed);
    YEW_ASSERT(total_a >= 2U);
    yew_picker_close(&f.ed, false);

    /* The SAME injected clock produces the same list — which is what
     * makes a golden of these rows possible at all. */
    YEW_ASSERT_EQ_I64(ps_run(&f, yew_undo_cmd_branches), YEW_CMD_OK);
    total_b = yew_picker_total(&f.ed);
    YEW_ASSERT_EQ_U64(total_a, total_b);
    yew_pickers_set_now(0);
    ps_remove(&f);
}

/* Accepting a row moves the document to that undo state. */
void test_pickers_undo_branches_travels_to_a_state(void)
{
    PsFix f;
    EditCtx ec;
    u64 len_after_edits;

    ps_make(&f);
    ec = yew_ed_edit_ctx(&f.ed);
    yew_undo_begin(&ec, YEW_TXN_TYPE);
    (void)yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"hello", 5U);
    yew_undo_end(&ec);
    yew_ed_finish_edit(&f.ed, &ec);
    len_after_edits = yew_textbuf_len(yew_ed_doc(&f.ed)->tb);
    YEW_ASSERT_EQ_U64(len_after_edits, 5U);

    yew_pickers_set_now(1700000000);
    YEW_ASSERT_EQ_I64(ps_run(&f, yew_undo_cmd_branches), YEW_CMD_OK);
    /* Row 0 is the root — the state before the insert. */
    ps_press(&f, YEW_KEY_HOME);
    ps_press(&f, YEW_KEY_ENTER);
    YEW_ASSERT(!yew_picker_active(&f.ed));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(yew_ed_doc(&f.ed)->tb), 0U);
    yew_pickers_set_now(0);
    ps_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Registration                                                     */
/* ---------------------------------------------------------------- */

void test_pickers_commands_are_registered(void)
{
    static const char *const names[] = {
        "ed.find.file", "ed.find.buffer", "ed.find.command",
        "ed.undo.branches"};
    u32 i;

    yew_cmd_shutdown();
    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        CmdId id = yew_cmd_lookup(names[i], (u32)strlen(names[i]));
        const CmdDesc *desc;

        YEW_ASSERT(id.v != 0U);
        desc = yew_cmd_desc(id);
        YEW_ASSERT_NOT_NULL(desc);
        /* Live, not deferred: these three ship this sprint. */
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
    }
    yew_cmd_shutdown();
    yew_cmd_init();

    {
        PsFix f;
        static const CmdDesc probe = {
            "ed.ui.toggle", command_probe, YEW_ARITY_NONE, 0U,
            "Palette execution probe", NULL
        };

        ps_make(&f);
        command_probe_calls = 0U;
        (void)yew_cmd_register(&probe);
        YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_command), YEW_CMD_OK);
        ps_type(&f, "Palette execution probe");
        ps_finish_scan(&f);
        YEW_ASSERT_EQ_U64(yew_picker_shown(&f.ed), 1U);
        ps_press(&f, YEW_KEY_ENTER);
        YEW_ASSERT_EQ_U64(command_probe_calls, 1U);
        YEW_ASSERT(!yew_picker_active(&f.ed));
        ps_remove(&f);
    }

    {
        PsFix f;
        Bytebuf text;

        ps_make(&f);
        YEW_ASSERT_EQ_I64(ps_run(&f, yew_find_cmd_command), YEW_CMD_OK);
        /* This phrase exists only in help, proving the palette searches
         * descriptions rather than command names alone. */
        ps_type(&f, "syntax language");
        ps_finish_scan(&f);
        YEW_ASSERT_EQ_U64(yew_picker_shown(&f.ed), 1U);
        YEW_ASSERT_EQ_U64((u32)yew_picker_selected(&f.ed),
                          yew_cmd_lookup("ed.syn.set", 10U).v);
        ps_press(&f, YEW_KEY_ENTER);
        YEW_ASSERT(!yew_picker_active(&f.ed));
        YEW_ASSERT(f.ed.cmdline.active);
        YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_CMD);
        bytebuf_init(&text);
        yew_cmdline_text(&f.ed, &text);
        YEW_ASSERT_EQ_U64(text.len, strlen("syn.set "));
        YEW_ASSERT_EQ_MEM(text.data, "syn.set ", text.len);
        bytebuf_free(&text);
        ps_remove(&f);
    }
}

void test_pickers_symbol_alias_is_live(void)
{
    CmdId id;
    CmdId lsp_id;
    const CmdDesc *desc;

    yew_cmd_shutdown();
    yew_cmd_init();
    id = yew_cmd_lookup("ed.find.symbol", 14U);
    lsp_id = yew_cmd_lookup("ed.lsp.symbols", 14U);
    YEW_ASSERT(id.v != 0U);
    YEW_ASSERT(lsp_id.v != 0U);
    desc = yew_cmd_desc(id);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
    YEW_ASSERT((desc->flags & YEW_CMD_PROMPTS) != 0U);
    YEW_ASSERT((desc->flags & YEW_CMD_NEEDS_WIN) != 0U);
    YEW_ASSERT_EQ_U64(desc->fn == yew_cmd_desc(lsp_id)->fn, true);
    yew_cmd_shutdown();
    yew_cmd_init();
}
