#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "args.h"
#include "edit/batch.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "term/tty.h"
#include "text/piece.h"

static void batch_fixture(Ed *ed)
{
    yew_ed_init(ed);
    ed->headless = true;
    yew_tty_poison(&ed->tty);
}

void test_batch_selfcheck_pins_every_uninitialized_subsystem(void)
{
    Ed ed;
    const char *why = NULL;

    batch_fixture(&ed);
    YEW_ASSERT(yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_NULL(why);
    YEW_ASSERT_NULL(ed.grid.front);
    YEW_ASSERT_NULL(ed.grid.back);
    YEW_ASSERT_NULL(ed.grid.dmg);
    YEW_ASSERT_NULL(ed.in.buf.data);
    YEW_ASSERT_EQ_U64(ed.in.buf.len, 0U);
    YEW_ASSERT_EQ_U64(ed.in.buf.cap, 0U);
    YEW_ASSERT(ed.tty.poisoned);
    YEW_ASSERT(ed.tty.rfd < 0);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    YEW_ASSERT(!ed.tty_ready);
    YEW_ASSERT(!ed.input_ready);
    YEW_ASSERT(!ed.grid_ready);
    YEW_ASSERT(!ed.render_ready);
    yew_ed_free(&ed);
}

void test_batch_selfcheck_rejects_seeded_grid_input_tty_and_timer(void)
{
    Ed ed;
    const char *why = NULL;
    Cell fake_cell;
    u8 fake_byte;

    batch_fixture(&ed);
    ed.grid.front = &fake_cell;
    YEW_ASSERT(!yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_EQ_STR(why, "grid initialized");
    ed.grid.front = NULL;
    ed.in.buf.data = &fake_byte;
    ed.in.buf.cap = 1U;
    YEW_ASSERT(!yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_EQ_STR(why, "input initialized");
    ed.in.buf = (Bytebuf){0};
    ed.tty.poisoned = false;
    YEW_ASSERT(!yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_EQ_STR(why, "terminal not poisoned");
    ed.tty.poisoned = true;
    ed.timers.len = 1U;
    YEW_ASSERT(!yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_EQ_STR(why, "timer installed");
    ed.timers.len = 0U;
    yew_ed_free(&ed);
}

void test_batch_selfcheck_rejects_interactive_installed_binding(void)
{
    Ed ed;
    const char *why = NULL;
    const BindRow row = {"x", "ed.search.open", 0, NULL};
    Keymap interactive = {0};

    batch_fixture(&ed);
    YEW_ASSERT(yew_keymap_build(&interactive, "batch-negative", &row, 1U));
    ed.keys.n = 1U;
    ed.keys.l[0] = &interactive;
    YEW_ASSERT(!yew_batch_selfcheck_ok(&ed, &why));
    YEW_ASSERT_EQ_STR(why, "interactive command bound");
    ed.keys.n = 0U;
    yew_keymap_free(&interactive);
    yew_ed_free(&ed);
}

void test_batch_refusal_table_covers_every_interactive_command(void)
{
    u32 i;
    u32 interactive = 0U;
    u32 git_interactive = 0U;

    yew_cmd_init();
    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);

        if ((desc->flags & YEW_CMD_INTERACTIVE) == 0U)
            continue;
        interactive++;
        YEW_ASSERT_NOT_NULL(yew_batch_command_alternative(desc->name, NULL));
        if (strncmp(desc->name, "ed.git.", 7U) == 0) {
            git_interactive++;
            YEW_ASSERT_EQ_STR(yew_batch_command_alternative(desc->name, NULL),
                              "no batch alternative");
        }
    }
    YEW_ASSERT(interactive >= 39U);
    YEW_ASSERT_EQ_U64(git_interactive, 18U);
}

void test_batch_memory_buffer_is_byte_exact_named_and_initially_clean(void)
{
    static const u8 bytes[] = {'a', '\r', '\n', 0x80U, 0xffU, 'z'};
    Ed ed;
    TextIter it;
    const u8 *chunk = NULL;
    u64 len = 0U;

    batch_fixture(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, bytes, sizeof(bytes), "[stdin]"));
    YEW_ASSERT_EQ_STR(yew_buf_label(&ed.buffer), "[stdin]");
    YEW_ASSERT((ed.buffer.flags & YEW_BUF_SCRATCH) != 0U);
    YEW_ASSERT(!yew_buf_dirty(&ed.buffer));
    YEW_ASSERT_EQ_U64(yew_buf_len(&ed.buffer), sizeof(bytes));
    YEW_ASSERT(yew_textiter_begin(&it, ed.buffer.tb, BYTEOFF(0U)));
    YEW_ASSERT(yew_textiter_chunk(&it, ed.buffer.tb, &chunk, &len));
    YEW_ASSERT_EQ_U64(len, sizeof(bytes));
    YEW_ASSERT_EQ_MEM(chunk, bytes, sizeof(bytes));
    yew_ed_free(&ed);
}

static char *batch_env_copy(const char *name)
{
    const char *value = getenv(name);

    return value == NULL ? NULL : strdup(value);
}

static void batch_env_restore(const char *name, char *saved)
{
    if (saved == NULL)
        (void)unsetenv(name);
    else {
        (void)setenv(name, saved, 1);
        free(saved);
    }
}

void test_batch_profiler_dumps_one_frame_per_executed_statement(void)
{
    static const char source[] =
        "let a = 1\nlet b = 2\nreturn a + b\nlet skipped = 4\n";
    char root[] = "/tmp/yew-batch-prof-XXXXXX";
    char script[512];
    char report_path[512];
    char report[4096];
    char *saved_prof = batch_env_copy("YEW_PROF");
    char *saved_out = batch_env_copy("YEW_PROF_OUT");
    FILE *fp;
    size_t n;
    struct stat st;
    BatchOpts opts = {0};

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    (void)snprintf(script, sizeof(script), "%s/run.fl", root);
    (void)snprintf(report_path, sizeof(report_path), "%s/report.txt", root);
    fp = fopen(script, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, sizeof(source) - 1U, fp),
                      sizeof(source) - 1U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    fp = fopen(report_path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite("stale", 1U, 5U, fp), 5U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);

    YEW_ASSERT_EQ_I64(setenv("YEW_PROF", "1", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_PROF_OUT", report_path, 1), 0);
    opts.script = script;
    opts.clean = true;
    opts.quiet = true;
    YEW_ASSERT_EQ_I64(yew_batch_run(&opts), YEW_EXIT_OK);

    fp = fopen(report_path, "rb");
    YEW_ASSERT_NOT_NULL(fp);
    n = fread(report, 1U, sizeof(report) - 1U, fp);
    report[n] = '\0';
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_NOT_NULL(strstr(report, "frames=3 dropped=0"));
    YEW_ASSERT_NOT_NULL(strstr(report,
                               "mode=batch phases=dispatch,jobs,syn"));
    YEW_ASSERT_NOT_NULL(strstr(report, "render             0"));
    YEW_ASSERT_NOT_NULL(strstr(report, "write              0"));
    YEW_ASSERT_EQ_I64(stat(report_path, &st), 0);
    YEW_ASSERT_EQ_U64(st.st_mode & 0777U, 0600U);

    batch_env_restore("YEW_PROF_OUT", saved_out);
    batch_env_restore("YEW_PROF", saved_prof);
    YEW_ASSERT_EQ_I64(unlink(report_path), 0);
    YEW_ASSERT_EQ_I64(unlink(script), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
}
