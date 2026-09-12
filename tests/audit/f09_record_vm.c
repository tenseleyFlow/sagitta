/*
 * Sprint 58 F09 Q5 — replay reaches the VM through exactly one call.
 *
 * The Makefile compiles record.c for this binary with fl_call_chunk renamed
 * to the inert function below.  A macro whose real body would insert bytes
 * must therefore leave the document, cursor set, and register source intact.
 * This makes the source-level side-door scan executable without adding a
 * test hook to product code.
 */
#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/record.h"

static u32 vm_calls;

bool yew_audit_fl_call_chunk(FlRuntime *rt, FlFn *fn, CmdSource source)
{
    (void)rt;
    (void)fn;
    if (source != YEW_SRC_REPLAY)
        return false;
    vm_calls++;
    return true;
}

int main(void)
{
    static const u8 source[] = "@[ i\"changed\" ]\n";
    Ed ed;
    const RegVal *stored;
    Cursor before;
    bool ok;

    yew_cmd_init();
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)fprintf(stderr, "f09-record-vm: scratch open failed\n");
        yew_ed_free(&ed);
        yew_cmd_shutdown();
        return 1;
    }
    before = *yew_ed_cursor(&ed);
    if (yew_flapi_reg_write(&ed, (u8)'a', source,
                            (u32)(sizeof(source) - 1U), false) != YEW_CMD_OK) {
        (void)fprintf(stderr, "f09-record-vm: register seed failed\n");
        yew_ed_free(&ed);
        yew_cmd_shutdown();
        return 1;
    }
    ok = yew_macro_replay(&ed, (u8)'a', 1U) == YEW_CMD_OK;
    stored = yew_reg_get(&ed.regs, (u8)'a');
    ok = ok && vm_calls == 1U && yew_textbuf_len(ed.buffer.tb) == 0U &&
         ed.win != NULL && ed.win->cs.curs.len == 1U &&
         ed.win->cs.primary == 0U &&
         ed.win->cs.curs.data[0].pos.v == before.pos.v &&
         ed.win->cs.curs.data[0].anchor.v == before.anchor.v &&
         ed.win->cs.curs.data[0].goal_col.v == before.goal_col.v &&
         stored != NULL && stored->bytes.len == sizeof(source) - 1U &&
         memcmp(stored->bytes.data, source, sizeof(source) - 1U) == 0;
    if (!ok)
        (void)fprintf(stderr,
                      "f09-record-vm: calls=%u bytes=%llu cursors=%u\n",
                      (unsigned)vm_calls,
                      (unsigned long long)yew_textbuf_len(ed.buffer.tb),
                      ed.win == NULL ? 0U : (unsigned)ed.win->cs.curs.len);
    yew_ed_free(&ed);
    yew_cmd_shutdown();
    if (!ok)
        return 1;
    (void)printf("f09-record-vm: one inert VM call, zero editor mutation\n");
    return 0;
}
