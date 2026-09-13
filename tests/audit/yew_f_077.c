/*
 * YEW-F-077 — rectangular yank omits required short-row padding.
 *
 * Correct behavior: Sprint 12 section 5 requires an ordinary, non-ragged
 * rectangular register to right-pad every short row to its CCol width at
 * store time.  Selecting columns [0, 2) across "a" and "bb" must therefore
 * store the two rows "a " and "bb".
 *
 * Baseline failure: the selection path copies each clipped source span
 * verbatim, stores "a" and "bb", and still labels the register non-ragged.
 * A later block paste consequently loses the selected rectangle's geometry.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/sel_actions.h"

bool test_yew_f_077(char *why, size_t why_cap)
{
    static const u8 source[] = "a\nbb\n";
    static const u8 expected[] = "a bb";
    Ed ed;
    EditCtx ec;
    CmdCtx cx = {0};
    Cursor *cursor;
    RegVal *reg;
    CmdStatus status;
    bool correct;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "could not open scratch buffer");
        yew_ed_free(&ed);
        return false;
    }
    ec = yew_ed_edit_ctx(&ed);
    if (!yew_edit_insert(&ec, BYTEOFF(0U), source, sizeof(source) - 1U)) {
        (void)snprintf(why, why_cap, "could not populate scratch buffer");
        yew_ed_finish_edit(&ed, &ec);
        yew_ed_free(&ed);
        return false;
    }
    yew_ed_finish_edit(&ed, &ec);
    ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    ed.win->h.kind = YEW_SEL_RECT;
    cursor = yew_ed_cursor(&ed);
    cursor->anchor = BYTEOFF(0U);
    cursor->pos = BYTEOFF(4U);
    cursor->goal_col = (GCol){2U};
    yew_cset_normalize(ed.buffer.tb, &ed.win->cs);

    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    status = yew_sel_cmd_yank(&cx);
    reg = yew_reg_get(&ed.regs, '"');
    correct = status == YEW_CMD_OK && reg != NULL &&
              reg->type == YEW_REG_BLOCKWISE && !reg->ragged &&
              reg->width == 2U && reg->bytes.len == sizeof(expected) - 1U &&
              memcmp(reg->bytes.data, expected, sizeof(expected) - 1U) == 0 &&
              reg->rows.len == 2U && reg->rows.data[0].lo == 0U &&
              reg->rows.data[0].hi == 2U && reg->rows.data[1].lo == 2U &&
              reg->rows.data[1].hi == 4U;
    if (!correct) {
        (void)snprintf(why, why_cap,
                       "status=%u width=%u ragged=%u bytes=%zu rows=%zu; "
                       "expected non-ragged width-2 rows 'a '/'bb'",
                       (u32)status, reg == NULL ? 0U : reg->width,
                       reg != NULL && reg->ragged ? 1U : 0U,
                       reg == NULL ? 0U : reg->bytes.len,
                       reg == NULL ? 0U : reg->rows.len);
    }
    yew_ed_free(&ed);
    return correct;
}
