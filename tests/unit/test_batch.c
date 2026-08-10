#include "harness.h"

#include <string.h>

#include "edit/batch.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "term/tty.h"
#include "text/piece.h"

static void batch_fixture(Ed *ed)
{
    sag_ed_init(ed);
    ed->headless = true;
    sag_tty_poison(&ed->tty);
}

void test_batch_selfcheck_pins_every_uninitialized_subsystem(void)
{
    Ed ed;
    const char *why = NULL;

    batch_fixture(&ed);
    SAG_ASSERT(sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_NULL(why);
    SAG_ASSERT_NULL(ed.grid.front);
    SAG_ASSERT_NULL(ed.grid.back);
    SAG_ASSERT_NULL(ed.grid.dmg);
    SAG_ASSERT_NULL(ed.in.buf.data);
    SAG_ASSERT_EQ_U64(ed.in.buf.len, 0U);
    SAG_ASSERT_EQ_U64(ed.in.buf.cap, 0U);
    SAG_ASSERT(ed.tty.poisoned);
    SAG_ASSERT(ed.tty.rfd < 0);
    SAG_ASSERT_EQ_U64(ed.timers.len, 0U);
    SAG_ASSERT(!ed.tty_ready);
    SAG_ASSERT(!ed.input_ready);
    SAG_ASSERT(!ed.grid_ready);
    SAG_ASSERT(!ed.render_ready);
    sag_ed_free(&ed);
}

void test_batch_selfcheck_rejects_seeded_grid_input_tty_and_timer(void)
{
    Ed ed;
    const char *why = NULL;
    Cell fake_cell;
    u8 fake_byte;

    batch_fixture(&ed);
    ed.grid.front = &fake_cell;
    SAG_ASSERT(!sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_EQ_STR(why, "grid initialized");
    ed.grid.front = NULL;
    ed.in.buf.data = &fake_byte;
    ed.in.buf.cap = 1U;
    SAG_ASSERT(!sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_EQ_STR(why, "input initialized");
    ed.in.buf = (Bytebuf){0};
    ed.tty.poisoned = false;
    SAG_ASSERT(!sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_EQ_STR(why, "terminal not poisoned");
    ed.tty.poisoned = true;
    ed.timers.len = 1U;
    SAG_ASSERT(!sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_EQ_STR(why, "timer installed");
    ed.timers.len = 0U;
    sag_ed_free(&ed);
}

void test_batch_selfcheck_rejects_interactive_installed_binding(void)
{
    Ed ed;
    const char *why = NULL;
    const BindRow row = {"x", "ed.search.open", 0, NULL};
    Keymap interactive = {0};

    batch_fixture(&ed);
    SAG_ASSERT(sag_keymap_build(&interactive, "batch-negative", &row, 1U));
    ed.keys.n = 1U;
    ed.keys.l[0] = &interactive;
    SAG_ASSERT(!sag_batch_selfcheck_ok(&ed, &why));
    SAG_ASSERT_EQ_STR(why, "interactive command bound");
    ed.keys.n = 0U;
    sag_keymap_free(&interactive);
    sag_ed_free(&ed);
}

void test_batch_refusal_table_covers_every_interactive_command(void)
{
    u32 i;
    u32 interactive = 0U;

    sag_cmd_init();
    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);

        if ((desc->flags & SAG_CMD_INTERACTIVE) == 0U)
            continue;
        interactive++;
        SAG_ASSERT_NOT_NULL(sag_batch_command_alternative(desc->name, NULL));
    }
    SAG_ASSERT(interactive >= 10U);
}

void test_batch_memory_buffer_is_byte_exact_named_and_initially_clean(void)
{
    static const u8 bytes[] = {'a', '\r', '\n', 0x80U, 0xffU, 'z'};
    Ed ed;
    TextIter it;
    const u8 *chunk = NULL;
    u64 len = 0U;

    batch_fixture(&ed);
    SAG_ASSERT(sag_ed_open_memory(&ed, bytes, sizeof(bytes), "[stdin]"));
    SAG_ASSERT_EQ_STR(sag_buf_label(&ed.buffer), "[stdin]");
    SAG_ASSERT((ed.buffer.flags & SAG_BUF_SCRATCH) != 0U);
    SAG_ASSERT(!sag_buf_dirty(&ed.buffer));
    SAG_ASSERT_EQ_U64(sag_buf_len(&ed.buffer), sizeof(bytes));
    SAG_ASSERT(sag_textiter_begin(&it, ed.buffer.tb, BYTEOFF(0U)));
    SAG_ASSERT(sag_textiter_chunk(&it, ed.buffer.tb, &chunk, &len));
    SAG_ASSERT_EQ_U64(len, sizeof(bytes));
    SAG_ASSERT_EQ_MEM(chunk, bytes, sizeof(bytes));
    sag_ed_free(&ed);
}
