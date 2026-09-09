/*
 * YEW-F-006 — workspace re-emission drops unknown root and workspace keys.
 *
 * Correct behavior: an older yew retains every unknown state.fl key,
 * including keys nested under workspace, when it parses and later emits a
 * workspace document. A newer yew's data must not be lost on the first save.
 *
 * Baseline failure: yew_state_apply retains only the options subtree.
 * yew_state_emit reconstructs the root and workspace maps from known fields,
 * silently discarding future_root_key and future_workspace_key.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "util/buf.h"
#include "ws/state.h"

static bool bytes_contain(const Bytebuf *buf, const char *needle)
{
    size_t i;
    size_t needle_len;

    if (buf == NULL || needle == NULL)
        return false;
    needle_len = strlen(needle);
    if (needle_len == 0U)
        return true;
    if (buf->len < needle_len)
        return false;
    for (i = 0U; i <= buf->len - needle_len; i++) {
        if (memcmp(buf->data + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

bool test_yew_f_006(char *why, size_t why_cap)
{
    static const char document[] =
        "{\n"
        "  version: 1,\n"
        "  workspace: {\n"
        "    path: \"\",\n"
        "    saved_at: 0,\n"
        "    future_workspace_key: { future_leaf: true, },\n"
        "  },\n"
        "  options: { \"future.option\": \"retain\", },\n"
        "  groups: [],\n"
        "  tabs: [],\n"
        "  active_tab: 0,\n"
        "  files: [],\n"
        "  future_root_key: { future_leaf: true, },\n"
        "}\n";
    Ed ed;
    Bytebuf emitted;
    YewWsResult result;
    bool root_kept;
    bool workspace_kept;
    bool option_kept;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        yew_ed_free(&ed);
        return true; /* Setup failure must be a hard XPASS. */
    }
    result = yew_state_apply(&ed, (const u8 *)document,
                             sizeof(document) - 1U);
    if (result != YEW_WS_FRESH) {
        yew_ed_free(&ed);
        return true; /* This is not the recorded finding. */
    }
    bytebuf_init(&emitted);
    yew_state_emit(&ed, &emitted);
    root_kept = bytes_contain(&emitted, "future_root_key");
    workspace_kept = bytes_contain(&emitted, "future_workspace_key");
    option_kept = bytes_contain(&emitted, "future.option");
    bytebuf_free(&emitted);
    yew_ed_free(&ed);
    if (root_kept && workspace_kept && option_kept)
        return true;
    (void)snprintf(why, why_cap,
                   "unknown keys retained: root=%d workspace=%d options=%d",
                   root_kept ? 1 : 0, workspace_kept ? 1 : 0,
                   option_kept ? 1 : 0);
    return false;
}
