/*
 * Sprint 26 §6.  See pickers.h.
 */
#define _POSIX_C_SOURCE 200809L

#include "ui/pickers.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "term/grid.h"
#include "ui/cmdline.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "unicode/width.h"
#include "util/log.h"
#include "ws/walk.h"

enum {
    YEW_PICKERS_MAX_ITEMS = 4096,
    YEW_PICKERS_LABEL_MAX = 512,
    /* One capped read per preview; 64 KiB shows far more than 40 lines
     * of anything a human wrote. */
    YEW_PREVIEW_BYTES = 64U * 1024U,
    YEW_PREVIEW_LINES = 40
};

static i64 g_now;
static u64 g_preview_reads;

void yew_pickers_set_now(i64 now)
{
    g_now = now;
}

u64 yew_pickers_preview_reads(void)
{
    return g_preview_reads;
}

void yew_pickers_preview_reset(void)
{
    g_preview_reads = 0U;
}

static i64 pickers_now(void)
{
    const char *pinned;

    if (g_now != 0)
        return g_now;
    /*
     * The pty harness runs the editor as a CHILD, so a setter cannot
     * reach it — the clock has to come through the environment for the
     * undo picker's "3 minutes ago" rows to be the same string on every
     * run.  Unset in normal use, which is the wall clock.
     */
    pinned = getenv("YEW_PICKERS_NOW");
    if (pinned != NULL && pinned[0] != '\0')
        return (i64)strtoll(pinned, NULL, 10);
    return (i64)time(NULL);
}

/* ---------------------------------------------------------------- */
/* Shared item storage                                              */
/* ---------------------------------------------------------------- */

/*
 * One item table, reused by whichever picker is open.
 *
 * The labels are owned here rather than pointing into the model,
 * because the model can change while the picker is up — a tab closed
 * behind the buffer switcher would otherwise leave a dangling label
 * that the next draw walks straight into.
 */
typedef struct ItemStore {
    PickItem items[YEW_PICKERS_MAX_ITEMS];
    char *text;
    u64 text_len;
    u64 text_cap;
    u32 n;
} ItemStore;

static ItemStore store;

static void store_reset(void)
{
    store.n = 0U;
    store.text_len = 0U;
}

static void store_free(void)
{
    yew_xfree(store.text);
    store.text = NULL;
    store.text_cap = 0U;
    store.text_len = 0U;
    store.n = 0U;
}

/* Copies `s` into the store and returns its OFFSET — not a pointer,
 * because the arena may move while more items are added. */
static u64 store_str(const char *s)
{
    u64 len = s == NULL ? 0U : (u64)strlen(s);
    u64 at = store.text_len;

    if (store.text_len + len + 1U > store.text_cap) {
        u64 cap = store.text_cap == 0U ? 4096U : store.text_cap;

        while (cap < store.text_len + len + 1U)
            cap *= 2U;
        store.text = yew_xrealloc(store.text, (size_t)cap);
        store.text_cap = cap;
    }
    if (len > 0U)
        (void)memcpy(store.text + at, s, (size_t)len);
    store.text[at + len] = '\0';
    store.text_len = at + len + 1U;
    return at;
}

/* Resolves every offset to a pointer.  Done once, after the table is
 * complete, so a realloc mid-build cannot dangle. */
static void store_fixup(const u64 *label_off, const u64 *detail_off)
{
    u32 i;

    for (i = 0U; i < store.n; i++) {
        store.items[i].label = store.text + label_off[i];
        store.items[i].detail = detail_off[i] == UINT64_MAX
                                    ? NULL
                                    : store.text + detail_off[i];
    }
}

static const PickItem *store_items(void *ctx, u32 *n)
{
    (void)ctx;
    *n = store.n;
    return store.items;
}

/* ---------------------------------------------------------------- */
/* File finder                                                      */
/* ---------------------------------------------------------------- */

static FileList g_files;
static bool g_files_ready;

/*
 * A preview that creates NOTHING.
 *
 * DoD 11 counts this: 20 previews must be 20 reads and zero buffer
 * allocations.  Going through yew_ws_file_buf would make a deferred tab
 * look resident, which is s24's pitfall 1 — residency is asked of the
 * allocation, so allocating IS the lie.
 */
void yew_pickers_preview_file(Ed *ed, void *ctx, i32 payload, Rect r)
{
    static u8 buf[YEW_PREVIEW_BYTES];
    YewColor dim = {YEW_COLOR_RGB, 140U, 140U, 140U};
    YewColor bg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    char path[PATH_MAX];
    const char *rel;
    ssize_t got;
    int fd;
    u16 row = 0U;
    u64 at = 0U;
    bool binary = false;
    u64 i;

    (void)ctx;
    if (ed == NULL || payload < 0 || (u32)payload >= g_files.paths.len)
        return;
    if (r.w == 0U || r.h == 0U)
        return;
    rel = g_files.paths.data[payload];
    if (snprintf(path, sizeof(path), "%s/%s", yew_ws_root(ed), rel) >=
        (int)sizeof(path))
        return;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    g_preview_reads++;
    got = pread(fd, buf, sizeof(buf), 0);
    (void)close(fd);
    if (got <= 0)
        return;
    /* s08's NUL heuristic: a file with a zero byte in its first block
     * is not text, and rendering it would spray control codes. */
    for (i = 0U; i < (u64)got && i < 8192U; i++) {
        if (buf[i] == 0U) {
            binary = true;
            break;
        }
    }
    if (binary) {
        char line[64];

        (void)snprintf(line, sizeof(line), "binary file, %lld bytes",
                       (long long)got);
        (void)yew_grid_puts(&ed->grid, r.y, r.x, (const u8 *)line,
                            strlen(line), dim, bg, YEW_ATTR_DIM);
        return;
    }
    while (at < (u64)got && row < r.h && row < (u16)YEW_PREVIEW_LINES) {
        u64 eol = at;
        size_t fit;
        int cells = 0;

        while (eol < (u64)got && buf[eol] != (u8)'\n')
            eol++;
        fit = yew_str_clip(buf + at, (size_t)(eol - at), (int)r.w, &cells);
        (void)yew_grid_puts(&ed->grid, (u16)(r.y + row), r.x, buf + at,
                            fit, dim, bg, YEW_ATTR_DIM);
        row++;
        at = eol + 1U;
    }
}

static bool find_file_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    char path[PATH_MAX];

    (void)ctx;
    (void)how;
    if (ed == NULL || payload < 0 || (u32)payload >= g_files.paths.len)
        return false;
    if (snprintf(path, sizeof(path), "%s/%s", yew_ws_root(ed),
                 g_files.paths.data[payload]) >= (int)sizeof(path))
        return false;
    return yew_tab_open(ed, path) >= 0;
}

CmdStatus yew_find_cmd_file(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    PickerSpec spec;
    WalkOpts o;
    WalkState *w;
    u64 *label_off;
    u64 *detail_off;
    u32 i;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (g_files_ready)
        yew_filelist_free(&g_files);
    yew_filelist_init(&g_files);
    g_files_ready = true;
    (void)memset(&o, 0, sizeof(o));
    o.use_gitignore = true;
    w = yew_walk_begin(yew_ws_root(ed), &o, &g_files);
    if (w == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot read %s", yew_ws_root(ed));
        return YEW_CMD_ERR_IO;
    }
    /*
     * Walked to completion here rather than sliced.  §7 makes this
     * incremental; until then a blocking walk is honest about what it
     * is, and the perf gate measures it.
     */
    while (yew_walk_step(w, 0))
        ;
    yew_walk_end(w);
    if (g_files.truncated) {
        yew_msg(ed, YEW_MSG_WARN,
                "showing the first %llu files; narrow the workspace",
                (unsigned long long)g_files.paths.len);
    }

    store_reset();
    label_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                  sizeof(*label_off));
    detail_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                   sizeof(*detail_off));
    for (i = 0U; i < g_files.paths.len && store.n < YEW_PICKERS_MAX_ITEMS;
         i++) {
        const char *rel = g_files.paths.data[i];
        const char *slash = strrchr(rel, '/');

        label_off[store.n] = store_str(rel);
        if (slash == NULL) {
            detail_off[store.n] = UINT64_MAX;
        } else {
            char dir[PATH_MAX];
            size_t dlen = (size_t)(slash - rel);

            if (dlen >= sizeof(dir))
                dlen = sizeof(dir) - 1U;
            (void)memcpy(dir, rel, dlen);
            dir[dlen] = '\0';
            detail_off[store.n] = store_str(dir);
        }
        store.items[store.n].payload = (i32)i;
        store.items[store.n].flags = 0U;
        store.n++;
    }
    store_fixup(label_off, detail_off);
    yew_xfree(label_off);
    yew_xfree(detail_off);

    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Find file";
    spec.items = store_items;
    spec.preview = yew_pickers_preview_file;
    spec.accept = find_file_accept;
    spec.path_mode = true;
    yew_picker_open(ed, &spec);
    return YEW_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* Buffer / tab switcher                                            */
/* ---------------------------------------------------------------- */

static bool find_buffer_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    int idx;

    (void)ctx;
    (void)how;
    if (ed == NULL)
        return false;
    /*
     * Resolved from the TAB ID, not from a stored index: the list is
     * built once and a tab can close while the picker is open (s23's
     * law).  A stale index would switch to whoever slid into the slot.
     */
    idx = yew_tab_index_of_id(ed, (u32)payload);
    if (idx < 0)
        return false;
    /* The switch hydrates a deferred tab — accepting one is exactly
     * when its read becomes worth paying for (s24 D3). */
    yew_tab_switch(ed, idx);
    return true;
}

CmdStatus yew_find_cmd_buffer(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    PickerSpec spec;
    u64 *label_off;
    u64 *detail_off;
    u32 i;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    store_reset();
    label_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                  sizeof(*label_off));
    detail_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                   sizeof(*detail_off));
    for (i = 0U; i < ed->tabs.v.len && store.n < YEW_PICKERS_MAX_ITEMS;
         i++) {
        const Tab *t = &ed->tabs.v.data[i];
        const char *display = yew_tab_display_path(t);
        const char *slash;
        u8 flags = 0U;

        label_off[store.n] = store_str(display == NULL ? "untitled" :
                                                        display);
        slash = display == NULL ? NULL : strrchr(display, '/');
        if (slash == NULL) {
            detail_off[store.n] = UINT64_MAX;
        } else {
            char dir[PATH_MAX];
            size_t dlen = (size_t)(slash - display);

            if (dlen >= sizeof(dir))
                dlen = sizeof(dir) - 1U;
            (void)memcpy(dir, display, dlen);
            dir[dlen] = '\0';
            /* A group member carries its group's label, so the switcher
             * shows which group a file belongs to. */
            if (t->group_id != 0U) {
                char label[64];
                char joined[PATH_MAX + 96];

                /* Sized past PATH_MAX on purpose: `dir` may already be
                 * that long, and a truncating join would silently show
                 * a different directory. */
                yew_group_label(ed, t->group_id, label, sizeof(label));
                (void)snprintf(joined, sizeof(joined), "%s  %s", label,
                               dir);
                detail_off[store.n] = store_str(joined);
            } else {
                detail_off[store.n] = store_str(dir);
            }
        }
        /* Derived, never stored: modified comes from undo (s23). */
        if (yew_tab_modified(ed, (int)i))
            flags |= (u8)YEW_PICK_MODIFIED;
        if (!yew_tab_is_resident(ed, (int)i))
            flags |= (u8)YEW_PICK_DEFERRED;
        if (t->missing_at_restore)
            flags |= (u8)YEW_PICK_ORPHAN;
        store.items[store.n].payload = (i32)t->tab_id;
        store.items[store.n].flags = flags;
        store.n++;
    }
    store_fixup(label_off, detail_off);
    yew_xfree(label_off);
    yew_xfree(detail_off);

    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Switch buffer";
    spec.items = store_items;
    spec.accept = find_buffer_accept;
    spec.path_mode = true;
    yew_picker_open(ed, &spec);
    return YEW_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* Command palette                                                  */
/* ---------------------------------------------------------------- */

static bool command_search_part(void *ctx, i32 payload, u32 part,
                                const u8 **text, size_t *len)
{
    const CmdDesc *desc;

    (void)ctx;
    if (payload <= 0 || text == NULL || len == NULL || part > 1U)
        return false;
    desc = yew_cmd_desc((CmdId){(u32)payload});
    if (desc == NULL)
        return false;
    *text = (const u8 *)(part == 0U ? desc->name : desc->help);
    *len = strlen((const char *)*text);
    return true;
}

static bool find_command_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    const CmdDesc *desc;
    CmdId id;

    (void)ctx;
    (void)how;
    if (ed == NULL || payload <= 0)
        return false;
    id = (CmdId){(u32)payload};
    desc = yew_cmd_desc(id);
    if (desc == NULL || (desc->flags & YEW_CMD_INTERNAL) != 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "command is no longer available");
        return false;
    }

    /* Commands needing an argument stay on the ordinary E-mode path, where
     * their argspec, completion and diagnostics already live.  Deferred
     * rows use the same path so accepting one produces the pinned Sprint
     * refusal instead of a registry-only log line. */
    if (desc->arity != YEW_ARITY_NONE ||
        (desc->flags & YEW_CMD_DEFERRED) != 0U) {
        char seed[256];
        const char *name = strncmp(desc->name, "ed.", 3U) == 0
                               ? desc->name + 3U
                               : desc->name;

        if (snprintf(seed, sizeof(seed), "%s%s", name,
                     desc->arity == YEW_ARITY_NONE ? "" : " ") >=
            (int)sizeof(seed)) {
            yew_msg(ed, YEW_MSG_ERROR, "command name is too long");
            return false;
        }
        yew_cmdline_open(ed, YEW_PROMPT_CMD, seed);
        return true;
    }
    {
        CmdCtx cx = {0};
        CmdStatus status;

        cx.ed = ed;
        cx.win = ed->win;
        cx.count = 1U;
        cx.source = YEW_SRC_KEY;
        status = yew_ed_invoke(ed, id, &cx);
        if (status != YEW_CMD_OK && !ed->msg.active)
            yew_msg(ed, YEW_MSG_ERROR, "command failed: %s", desc->name);
        return status == YEW_CMD_OK;
    }
}

CmdStatus yew_find_cmd_command(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    PickerSpec spec;
    u64 *label_off;
    u64 *detail_off;
    u32 n;
    u32 i;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    store_reset();
    label_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                  sizeof(*label_off));
    detail_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                   sizeof(*detail_off));
    n = yew_cmd_count();
    for (i = 0U; i < n && store.n < YEW_PICKERS_MAX_ITEMS; i++) {
        CmdId id = {i + 1U};
        const CmdDesc *desc = yew_cmd_at(i);

        /* yew_cmd_at() preserves registry order, including inactive plugin
         * tombstones.  The palette is an executable inventory, so it keeps
         * only active, user-facing commands. */
        if (desc == NULL || yew_cmd_desc(id) == NULL ||
            (desc->flags & YEW_CMD_INTERNAL) != 0U)
            continue;
        label_off[store.n] = store_str(desc->name);
        detail_off[store.n] = store_str(desc->help);
        store.items[store.n].payload = (i32)id.v;
        store.items[store.n].flags =
            (desc->flags & YEW_CMD_DEFERRED) != 0U
                ? (u8)YEW_PICK_DEFERRED
                : 0U;
        store.n++;
    }
    store_fixup(label_off, detail_off);
    yew_xfree(label_off);
    yew_xfree(detail_off);

    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Command palette";
    spec.items = store_items;
    spec.accept = find_command_accept;
    spec.search_part = command_search_part;
    spec.search_parts_independent = true;
    spec.path_mode = false;
    yew_picker_open(ed, &spec);
    return YEW_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* Undo branch picker — closes Sprint 10 §11                        */
/* ---------------------------------------------------------------- */

static bool undo_branch_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    EditCtx ec;
    Buffer *doc;
    bool ok;

    (void)ctx;
    (void)how;
    if (ed == NULL)
        return false;
    doc = yew_ed_doc(ed);
    if (doc == NULL || doc->undo == NULL)
        return false;
    ec = yew_ed_edit_ctx(ed);
    ok = yew_undo_to(&ec, (u32)payload);
    yew_ed_finish_edit(ed, &ec);
    if (!ok)
        yew_msg(ed, YEW_MSG_ERROR, "cannot reach that undo state");
    return ok;
}

CmdStatus yew_undo_cmd_branches(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    PickerSpec spec;
    UndoNodeInfo *info;
    Buffer *doc;
    u64 *label_off;
    u64 *detail_off;
    u32 n;
    u32 i;
    i64 now = pickers_now();

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    doc = yew_ed_doc(ed);
    if (doc == NULL || doc->undo == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "no undo history here");
        return YEW_CMD_ERR_STATE;
    }
    info = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS, sizeof(*info));
    n = yew_undo_list(doc->undo, info, YEW_PICKERS_MAX_ITEMS);
    store_reset();
    label_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                  sizeof(*label_off));
    detail_off = yew_xreallocarray(NULL, YEW_PICKERS_MAX_ITEMS,
                                   sizeof(*detail_off));
    for (i = 0U; i < n && store.n < YEW_PICKERS_MAX_ITEMS; i++) {
        char desc[192];
        char label[YEW_PICKERS_LABEL_MAX];
        char detail[64];
        u32 indent = info[i].depth < 16U ? info[i].depth : 16U;
        u32 k;
        size_t at = 0U;

        /*
         * `now` is PASSED IN, never read from a clock in here — that is
         * what makes "3 minutes ago" reproducible and lets the
         * determinism lane pin these rows (invariant 5).
         */
        yew_undo_describe(doc->undo, info[i].id, now, desc, sizeof(desc));
        /* Tree structure as indentation: the list is flat, the shape is
         * the depth. */
        for (k = 0U; k < indent && at + 2U < sizeof(label); k++) {
            label[at++] = ' ';
            label[at++] = ' ';
        }
        (void)snprintf(label + at, sizeof(label) - at, "%s%s%s",
                       info[i].is_current ? "> " : "", desc,
                       info[i].is_saved ? " *" : "");
        label_off[store.n] = store_str(label);
        (void)snprintf(detail, sizeof(detail), "+%llu -%llu%s",
                       (unsigned long long)info[i].bytes_ins,
                       (unsigned long long)info[i].bytes_del,
                       info[i].is_trimmed ? " (trimmed)" : "");
        detail_off[store.n] = store_str(detail);
        store.items[store.n].payload = (i32)info[i].id;
        /* On the current path is the bright row; everything else is a
         * branch you left. */
        store.items[store.n].flags =
            info[i].on_current_path ? 0U : (u8)YEW_PICK_DEFERRED;
        store.n++;
    }
    store_fixup(label_off, detail_off);
    yew_xfree(label_off);
    yew_xfree(detail_off);
    yew_xfree(info);

    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Undo branches";
    spec.items = store_items;
    spec.accept = undo_branch_accept;
    /* NOT path_mode: an undo description is prose, and scoring its last
     * `.`-or-`/` segment would rank every row on a fragment. */
    spec.path_mode = false;
    yew_picker_open(ed, &spec);
    return YEW_CMD_OK;
}

void yew_pickers_dispose(void)
{
    store_free();
    if (g_files_ready) {
        yew_filelist_free(&g_files);
        g_files_ready = false;
    }
}
