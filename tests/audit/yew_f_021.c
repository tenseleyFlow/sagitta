/*
 * YEW-F-021 — plugin teardown retains raw hook and ledger lengths.
 *
 * Correct behavior: Sprint 54 section 4 and Sprint 58 F14 q3 require every
 * registry length to return to its pre-enable value after plugin teardown.
 *
 * Baseline failure: removal clears the callback and marks both rows inactive,
 * but FlHookTable.n and FlRegLedger.n retain tombstone slots.  The integrated
 * 20-plugin by 20-cycle control proves those slots plateau and the closures
 * are reclaimed; this reproducer pins the narrower raw-length mismatch.
 */
#include "audit.h"

#include <stdio.h>

#include "fl/flhook.h"

bool test_yew_f_021(char *why, size_t why_cap)
{
    FlHookTable hooks;
    FlNative callback = {0};
    u32 id;
    bool restored;

    fl_hook_table_init(&hooks, NULL, NULL);
    id = fl_hook_add(&hooks, 54U, (u32)FL_EV_ED_IDLE,
                     FL_OBJ_V(FL_NATIVE, &callback));
    if (!fl_hook_remove(&hooks, id)) {
        fl_hook_table_free(&hooks);
        return false;
    }
    restored = hooks.n == 0U && hooks.ledger.n == 0U;
    if (!restored)
        (void)snprintf(why, why_cap,
                       "teardown retains hook length %u and ledger length %u",
                       (unsigned)hooks.n, (unsigned)hooks.ledger.n);
    fl_hook_table_free(&hooks);
    return restored;
}
