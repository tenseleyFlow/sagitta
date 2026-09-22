/*
 * YEW-F-079 — workspace re-emission drops unknown entity-record keys.
 *
 * Correct behavior: Sprint 25's forward-compatibility law applies inside
 * every entity record as well as singleton root maps.  An older yew must
 * preserve fields written by a newer yew when saving the restored workspace,
 * and a retained field must follow its entity when live arrays are reordered.
 *
 * Baseline failure: apply_groups/apply_tabs copy only known fields into live
 * objects, and yew_state_emit reconstructs records from those objects.  The
 * same pattern existed in panes, windows, cursors, views, history rings,
 * files, marks, changes, and undo metadata.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
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

static bool f079_make_file(char path[], const char *body)
{
    FILE *stream;
    int fd = mkstemp(path);

    if (fd < 0)
        return false;
    stream = fdopen(fd, "wb");
    if (stream == NULL) {
        (void)close(fd);
        (void)unlink(path);
        return false;
    }
    if (fwrite(body, 1U, strlen(body), stream) != strlen(body) ||
        fclose(stream) != 0) {
        (void)unlink(path);
        return false;
    }
    return true;
}

static const FlLit *f079_tab_for_path(const FlLit *root, const char *path)
{
    const FlLit *tabs = yew_fl_get(root, "tabs");
    u32 i;

    for (i = 0U; i < yew_fl_len(tabs); i++) {
        const FlLit *tab = yew_fl_at(tabs, i);
        const char *got;
        u64 got_len = 0U;
        size_t want_len = strlen(path);

        got = yew_fl_str_or(yew_fl_get(tab, "path"), NULL, &got_len);
        if (got != NULL && got_len == (u64)want_len &&
            memcmp(got, path, want_len) == 0)
            return tab;
    }
    return NULL;
}

static bool f079_has_key(const FlLit *map, const char *key, u64 key_len)
{
    u32 i;

    if (map == NULL || map->kind != FL_LIT_MAP)
        return false;
    for (i = 0U; i < map->len; i++)
        if (map->keylens[i] == key_len &&
            memcmp(map->keys[i], key, (size_t)key_len) == 0)
            return true;
    return false;
}

bool test_yew_f_079(char *why, size_t why_cap)
{
    static const char body[] = "audit fixture\nsecond line\n";
    static const char *const markers[] = {
        "future_group_key", "future_pane_split_key",
        "future_pane_leaf_key", "future_win_key", "future_cursor_key",
        "future_view_key", "future_jumps_key", "future_jump_entry_key",
        "future_file_key", "future_mark_key", "future_changes_key",
        "future_change_entry_key", "future_undo_key"
    };
    char path_a[] = "/tmp/yew-f079-a-XXXXXX";
    char path_b[] = "/tmp/yew-f079-b-XXXXXX";
    char *real_a = NULL;
    char *real_b = NULL;
    char document[16384];
    Ed ed;
    Bytebuf emitted;
    Arena parsed_arena;
    FlParseErr parse_err;
    FlLit *parsed = NULL;
    int count;
    size_t retained = 0U;
    size_t i;
    bool initialized = false;
    bool emitted_ready = false;
    bool parsed_ready = false;
    bool tab_identity = false;
    bool pending_mark = false;
    bool binary_key = false;
    bool setup_failed = false;
    bool found_a = false;
    bool found_b = false;
    i64 tag_a = -1;
    i64 tag_b = -1;

    if (!f079_make_file(path_a, body) || !f079_make_file(path_b, body)) {
        (void)unlink(path_a);
        (void)unlink(path_b);
        return true;
    }
    /* glibc fortify requires a caller-supplied realpath buffer to hold
     * PATH_MAX bytes even for these short fixture paths.  Let realpath
     * allocate the exact result instead of relying on a 512-byte array. */
    real_a = realpath(path_a, NULL);
    real_b = realpath(path_b, NULL);
    if (real_a == NULL || real_b == NULL) {
        free(real_a);
        free(real_b);
        (void)unlink(path_a);
        (void)unlink(path_b);
        return true;
    }

    count = snprintf(
        document, sizeof(document),
        "{\n"
        "  version: 1,\n"
        "  workspace: { path: \"\", saved_at: 0, },\n"
        "  options: {},\n"
        "  groups: [{ id: 1, label: \"audit\", dir_path: \"\", "
        "future_group_key: { leaf: true, }, "
        "\"future\\0tail\": { leaf: true, }, },],\n"
        "  tabs: [{ id: 1, path: \"%s\", group: 1, group_ordinal: 1, "
        "deferred: true, focus: 0, future_tab_key: 101,\n"
        "    panes: { split: \"h\", ratio_permille: 500, "
        "future_pane_split_key: { leaf: true, },\n"
        "      a: { win: 0, future_pane_leaf_key: { leaf: true, }, },\n"
        "      b: { win: 1, }, },\n"
        "    wins: [{\n"
        "      cursors: [{ pos: 1, anchor: 1, goal: -1, "
        "future_cursor_key: { leaf: true, }, },], primary: 0,\n"
        "      view: { top: 0, top_sub: 0, left: 0, wrap: false, "
        "future_view_key: { leaf: true, }, },\n"
        "      jumps: { cur: 1, future_jumps_key: { leaf: true, }, "
        "entries: [{ path: \"%s\", line: 1, col: 0, stamp: 7, "
        "future_jump_entry_key: { leaf: true, }, },], },\n"
        "      future_win_key: { leaf: true, },\n"
        "    }, { cursors: [{ pos: 2, anchor: 2, goal: -1, },], "
        "primary: 0, view: { top: 0, top_sub: 0, left: 0, wrap: false, }, "
        "jumps: { cur: 0, entries: [], }, },],\n"
        "  }, { id: 2, path: \"%s\", group: 1, group_ordinal: 2, "
        "deferred: true, future_tab_key: 202, },],\n"
        "  active_tab: 2,\n"
        "  files: [{ path: \"%s\", future_file_key: { leaf: true, },\n"
        "    marks: [{ name: \"a\", pos: 2, "
        "future_mark_key: { leaf: true, }, },],\n"
        "    changes: { cur: 1, future_changes_key: { leaf: true, }, "
        "entries: [{ path: \"%s\", line: 1, col: 0, stamp: 9, "
        "future_change_entry_key: { leaf: true, }, },], },\n"
        "    undo: { file: \"future.yewu\", version: 1, "
        "future_undo_key: { leaf: true, }, },\n"
        "  },],\n"
        "}\n",
        real_a, real_a, real_b, real_a, real_a);
    if (count < 0 || (size_t)count >= sizeof(document)) {
        free(real_a);
        free(real_b);
        (void)unlink(path_a);
        (void)unlink(path_b);
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
    {
        int a_idx = yew_tab_find_by_path(&ed, real_a);
        Tab *ta;
        Pane *added;

        if (a_idx < 0) {
            setup_failed = true;
            goto done;
        }
        yew_tab_switch(&ed, a_idx);
        ta = yew_tab_at(&ed, a_idx);
        yew_layout_compute(ta->root, (Rect){0U, 0U, 120U, 40U});
        added = yew_pane_split(&ed, ta->root->a, YEW_SPLIT_H);
        if (added == NULL || !yew_pane_close(&ed, added)) {
            setup_failed = true;
            goto done;
        }
    }
    /* tab_id, not the compacted array position, owns retained tab data. */
    yew_tab_reorder(&ed, 1, 2);
    bytebuf_init(&emitted);
    emitted_ready = true;
    yew_state_emit(&ed, &emitted);
    for (i = 0U; i < YEW_ARRAY_LEN(markers); i++)
        if (f079_contains(&emitted, markers[i]))
            retained++;

    arena_init(&parsed_arena);
    parsed_ready = true;
    (void)memset(&parse_err, 0, sizeof(parse_err));
    parsed = yew_fl_parse_fletch(&parsed_arena, emitted.data, emitted.len,
                                 &parse_err);
    if (parsed != NULL) {
        const FlLit *ta = f079_tab_for_path(parsed, real_a);
        const FlLit *tb = f079_tab_for_path(parsed, real_b);
        const FlLit *files = yew_fl_get(parsed, "files");
        const FlLit *marks = yew_fl_get(yew_fl_at(files, 0U), "marks");
        const FlLit *mark = yew_fl_at(marks, 0U);
        static const char binary_name[] = "future\0tail";

        found_a = ta != NULL;
        found_b = tb != NULL;
        tag_a = yew_fl_int_or(yew_fl_get(ta, "future_tab_key"), -1);
        tag_b = yew_fl_int_or(yew_fl_get(tb, "future_tab_key"), -1);
        tab_identity = found_a && found_b && tag_a == 101 && tag_b == 202;
        pending_mark = mark != NULL &&
            yew_fl_int_or(yew_fl_get(mark, "pos"), -1) == 2 &&
            yew_fl_get(mark, "future_mark_key") != NULL;
        binary_key = f079_has_key(yew_fl_at(yew_fl_get(parsed, "groups"),
                                            0U),
                                  binary_name, sizeof(binary_name) - 1U);
    }
    if ((retained != YEW_ARRAY_LEN(markers) || !tab_identity ||
         !pending_mark || !binary_key) && why != NULL && why_cap > 0U)
        (void)snprintf(why, why_cap,
                       "record keys retained=%zu/%zu tabs=%d/%lld,%d/%lld "
                       "pending_mark=%d binary_key=%d",
                       retained, YEW_ARRAY_LEN(markers),
                       found_a ? 1 : 0, (long long)tag_a,
                       found_b ? 1 : 0, (long long)tag_b,
                       pending_mark ? 1 : 0, binary_key ? 1 : 0);

done:
    if (parsed_ready)
        arena_free_all(&parsed_arena);
    if (emitted_ready)
        bytebuf_free(&emitted);
    if (initialized)
        yew_ed_free(&ed);
    (void)unlink(path_a);
    (void)unlink(path_b);
    free(real_a);
    free(real_b);
    return setup_failed ? true :
        retained == YEW_ARRAY_LEN(markers) && tab_identity && pending_mark &&
        binary_key;
}
