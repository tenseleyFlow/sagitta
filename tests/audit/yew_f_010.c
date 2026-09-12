/*
 * YEW-F-010 — macro store accepts source that fails on its first replay.
 *
 * Correct behavior: Sprint 38 says store-time validation rejects a macro that
 * would fail at replay, leaves the register unchanged, and reports the error
 * while the user is still editing the macro scratch buffer.
 *
 * Baseline failure: yew_macro_store checks only whether fl_compile_str returns
 * a function.  An unresolved call compiles, is stored with a success message,
 * and then fails immediately when the ordinary replay VM resolves the name.
 * The replay transaction does roll back, so this reproducer changes no file
 * and leaves no document bytes behind.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/record.h"
#include "ui/macrobrowse.h"

bool test_yew_f_010(char *why, size_t why_cap)
{
    static const u8 source[] =
        "@[ i\"changed\" ]\n"
        "missing_function()\n";
    Ed ed;
    CmdStatus edit_status;
    CmdStatus store_status = YEW_CMD_ERR_STATE;
    CmdStatus replay_status = YEW_CMD_ERR_STATE;
    const RegVal *stored;
    bool register_unchanged = false;
    bool correct = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "could not open scratch buffer");
        yew_ed_free(&ed);
        return false;
    }
    if (yew_flapi_reg_write(&ed, (u8)'a', source,
                            (u32)(sizeof(source) - 1U), false) != YEW_CMD_OK) {
        (void)snprintf(why, why_cap, "could not seed macro register");
        yew_ed_free(&ed);
        return false;
    }
    edit_status = yew_macro_edit(&ed, (u8)'a');
    if (edit_status == YEW_CMD_OK && ed.win != NULL && ed.win->buf != NULL) {
        store_status = yew_macro_store(&ed, ed.win->buf);
        stored = yew_reg_get(&ed.regs, (u8)'a');
        register_unchanged = stored != NULL &&
                             stored->bytes.len == sizeof(source) - 1U &&
                             memcmp(stored->bytes.data, source,
                                    sizeof(source) - 1U) == 0;
        correct = store_status != YEW_CMD_OK && register_unchanged;
        if (store_status == YEW_CMD_OK)
            replay_status = yew_macro_replay(&ed, (u8)'a', 1U);
    }
    if (!correct) {
        (void)snprintf(why, why_cap,
                       "edit=%d store=%d replay=%d unchanged=%u; invalid macro was stored before replay failed",
                       (int)edit_status, (int)store_status,
                       (int)replay_status,
                       register_unchanged ? 1U : 0U);
    }
    yew_ed_free(&ed);
    return correct;
}
