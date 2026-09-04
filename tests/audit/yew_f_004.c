/*
 * YEW-F-004 — full Fletch parser rejects bare dotted map keys.
 *
 * Correct behavior: the frozen map-entry grammar accepts option names such
 * as `clipboard.sync:` and `search.smartcase:` without requiring quotes,
 * and set() applies both values.
 *
 * Baseline failure: the full parser consumes `clipboard` as the map key,
 * then expects `:` and rejects the following dot.  The pure-literal parser
 * accepts the same key shape, and runtime/init.fl avoids the defect by
 * quoting every dotted option name.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"

bool test_yew_f_004(char *why, size_t why_cap)
{
    static const char source[] =
        "set({ clipboard.sync: \"none\", search.smartcase: false })";
    Ed ed;
    OptVal clipboard;
    OptVal smartcase;
    CmdStatus status;
    bool ok;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        (void)snprintf(why, why_cap, "could not open scratch buffer");
        return false;
    }
    status = yew_fl_eval(&ed, source, sizeof(source) - 1U);
    ok = status == YEW_CMD_OK &&
         yew_opt_get(&ed, ed.win->buf, ed.win, "clipboard.sync", 14U,
                     &clipboard) &&
         yew_opt_get(&ed, ed.win->buf, ed.win, "search.smartcase", 16U,
                     &smartcase) &&
         clipboard.type == (u8)YEW_OPT_ENUM &&
         clipboard.as.str.len == 4U &&
         memcmp(clipboard.as.str.s, "none", 4U) == 0 &&
         smartcase.type == (u8)YEW_OPT_BOOL && !smartcase.as.b;
    if (!ok) {
        const char *message = ed.msg.full == NULL ? ed.msg.text : ed.msg.full;

        (void)snprintf(why, why_cap, "status=%d: %s", (int)status,
                       message == NULL || message[0] == '\0' ?
                           "bare dotted key was not applied" : message);
    }
    yew_ed_free(&ed);
    return ok;
}
