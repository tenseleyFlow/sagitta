/*
 * YEW-F-079 — workspace re-emission drops unknown entity-record keys.
 *
 * Correct behavior: Sprint 25's forward-compatibility law applies inside
 * identity-bearing group and tab records as well as singleton root maps.
 * An older yew must preserve fields written by a newer yew when saving the
 * restored workspace.
 *
 * Baseline failure: apply_groups/apply_tabs copy only known fields into live
 * objects, and yew_state_emit reconstructs both records from those objects.
 * The parsed literals carrying future_group_key and future_tab_key are lost.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "util/buf.h"
#include "ws/state.h"

static bool f079_contains(const Bytebuf *buf, const char *needle)
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
    for (i = 0U; i <= buf->len - needle_len; i++)
        if (memcmp(buf->data + i, needle, needle_len) == 0)
            return true;
    return false;
}

bool test_yew_f_079(char *why, size_t why_cap)
{
    static const char body[] = "audit fixture\n";
    char path[] = "/tmp/yew-f079-XXXXXX";
    char document[4096];
    Ed ed;
    Bytebuf emitted;
    FILE *stream;
    int fd;
    int count;
    bool initialized = false;
    bool emitted_ready = false;
    bool group_kept = false;
    bool tab_kept = false;
    bool setup_failed = false;

    fd = mkstemp(path);
    if (fd < 0)
        return true; /* Infrastructure failure must be a hard XPASS. */
    stream = fdopen(fd, "wb");
    if (stream == NULL) {
        (void)close(fd);
        (void)unlink(path);
        return true;
    }
    if (fwrite(body, 1U, sizeof(body) - 1U, stream) != sizeof(body) - 1U ||
        fclose(stream) != 0) {
        (void)unlink(path);
        return true;
    }

    count = snprintf(
        document, sizeof(document),
        "{\n"
        "  version: 1,\n"
        "  workspace: { path: \"\", saved_at: 0, },\n"
        "  options: {},\n"
        "  groups: [{ id: 1, label: \"audit\", dir_path: \"\", "
        "future_group_key: { leaf: true, }, },],\n"
        "  tabs: [{ id: 1, path: \"%s\", group: 1, group_ordinal: 1, "
        "deferred: true, future_tab_key: { leaf: true, }, },],\n"
        "  active_tab: 1,\n"
        "  files: [],\n"
        "}\n",
        path);
    if (count < 0 || (size_t)count >= sizeof(document)) {
        (void)unlink(path);
        return true;
    }

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&ed);
    initialized = true;
    if (!yew_ed_open_scratch(&ed) ||
        yew_state_apply(&ed, (const u8 *)document, (u64)count) !=
            YEW_WS_RESTORED) {
        setup_failed = true;
        goto done;
    }
    bytebuf_init(&emitted);
    emitted_ready = true;
    yew_state_emit(&ed, &emitted);
    group_kept = f079_contains(&emitted, "future_group_key");
    tab_kept = f079_contains(&emitted, "future_tab_key");
    if ((!group_kept || !tab_kept) && why != NULL && why_cap > 0U)
        (void)snprintf(why, why_cap,
                       "unknown entity keys retained: group=%d tab=%d",
                       group_kept ? 1 : 0, tab_kept ? 1 : 0);

done:
    if (emitted_ready)
        bytebuf_free(&emitted);
    if (initialized)
        yew_ed_free(&ed);
    (void)unlink(path);
    return setup_failed ? true : group_kept && tab_kept;
}
