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

    yew_cmd_init();
    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);

        if ((desc->flags & YEW_CMD_INTERACTIVE) == 0U)
            continue;
        interactive++;
        YEW_ASSERT_NOT_NULL(yew_batch_command_alternative(desc->name, NULL));
    }
    YEW_ASSERT(interactive >= 10U);
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
