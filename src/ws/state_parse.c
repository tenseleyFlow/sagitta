/*
 * Sprint 25 §6/§7: restoring a workspace, and surviving one that is
 * unreadable.
 *
 * THE ORDER IS THE ALGORITHM.  version -> options -> groups -> tabs ->
 * files -> active_tab -> prune -> layout -> clamp.  Each position is
 * forced by the step after it: groups exist before a tab can join one
 * (§3.1), every tab exists before active_tab can name one, layout
 * happens at the CURRENT terminal size because ratios are permille, and
 * the clamp comes last because it needs the rects layout just produced.
 *
 * STEP 9 IS sag_vp_clamp, NOT sag_vp_follow.  The saved viewport
 * already satisfies scrolloff; follow would move `top` to centre the
 * cursor and the resumed grid would not be the grid the user quit on —
 * which is the entire claim of DoD 2.  Follow resumes at the first
 * cursor motion, where it belongs.
 *
 * A TAB IS NEVER SILENTLY DROPPED FOR A MISSING FILE.  The path is the
 * only record of what someone was working on; deleting it because the
 * file moved is a data loss they cannot undo.  Missing files are
 * counted and reported once (§6), and the tab stays.
 *
 * NOTHING HERE FAILS.  Every §7 row ends with "the editor starts": a
 * corrupt cache is renamed aside, never deleted, and never prompts
 * before the first paint.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/state.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "text/file.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/message.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "util/log.h"
#include "util/sort.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* §7: setting a bad document aside                                 */
/* ---------------------------------------------------------------- */

#define SAG_STATE_CORRUPT_PREFIX "state.fl.corrupt-"

/*
 * Newest-first by name, which is newest-first by time because the
 * stamp is %Y%m%dT%H%M%SZ.  Stable, because raw qsort is banned and
 * because two files can only share a name if one is not there.
 */
static int corrupt_cmp(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    return strcmp(*(char *const *)b, *(char *const *)a);
}

/*
 * Keeps the newest SAG_STATE_CORRUPT_KEEP and deletes the rest.
 *
 * Deleting a *corrupt copy* is not the same as deleting the user's
 * bytes: the copy exists so a bug can be filed against it, and five is
 * enough for that.  Unbounded, a workspace that fails to parse every
 * start accumulates one file per launch forever.
 */
static void prune_corrupt(const char *dir)
{
    char *names[64];
    u32 n = 0U;
    u32 i;
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL && n < SAG_ARRAY_LEN(names)) {
        if (strncmp(ent->d_name, SAG_STATE_CORRUPT_PREFIX,
                    strlen(SAG_STATE_CORRUPT_PREFIX)) != 0)
            continue;
        names[n] = strdup(ent->d_name);
        n++;
    }
    (void)closedir(d);
    sag_sort_stable(names, n, sizeof(names[0]), corrupt_cmp, NULL);
    for (i = 0U; i < n; i++) {
        if (i >= (u32)SAG_STATE_CORRUPT_KEEP) {
            char path[PATH_MAX];

            if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) <
                (int)sizeof(path))
                (void)unlink(path);
        }
        free(names[i]);
    }
}

bool sag_state_set_aside(Ed *ed, char *out, size_t cap)
{
    char stamp[32];
    char dest[PATH_MAX];
    const char *src;
    struct tm tm_utc;
    time_t now;
    int n;

    if (ed == NULL || !ed->state.ready)
        return false;
    src = sag_ws_state_path(&ed->state.key);
    if (src == NULL)
        return false;
    now = time(NULL);
    /* UTC, always.  A local-time stamp sorts wrongly across a DST
     * boundary, and the retention order IS the sort order. */
    if (gmtime_r(&now, &tm_utc) == NULL)
        return false;
    if (strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm_utc) == 0U)
        return false;
    n = snprintf(dest, sizeof(dest), "%s/%s%s", ed->state.key.dir,
                 SAG_STATE_CORRUPT_PREFIX, stamp);
    if (n <= 0 || (size_t)n >= sizeof(dest))
        return false;
    /*
     * MOVED, never unlinked.  s08's stale-journal doctrine: the user's
     * bytes are theirs even when we cannot make sense of them, and a
     * file we deleted is a bug report nobody can file.  The move goes
     * through s08's primitive, which refuses to clobber an existing
     * destination — two failures in one second share a timestamp.
     */
    if (sag_file_move_aside(src, dest) != SAG_SAVE_OK) {
        sag_log(SAG_LOG_WARN, "cannot set aside %s: %s", src,
                strerror(errno));
        return false;
    }
    prune_corrupt(ed->state.key.dir);
    if (out != NULL && cap > 0U)
        (void)snprintf(out, cap, "%s%s", SAG_STATE_CORRUPT_PREFIX, stamp);
    return true;
}

const char *sag_state_option_str(const Ed *ed, const char *key,
                                 const char *dflt)
{
    const char *v;
    u64 n = 0U;

    if (ed == NULL || ed->state.options == NULL)
        return dflt;
    v = sag_fl_str_or(sag_fl_get(ed->state.options, key), NULL, &n);
    return v == NULL || n == 0U ? dflt : v;
}

/* One message, exactly once, and never a prompt.  A modal dialog before
 * the first paint, over a CACHE, is user-hostile. */
static SagWsResult recover(Ed *ed, const char *why)
{
    char name[64];

    sag_log(SAG_LOG_WARN, "workspace state unusable: %s", why);
    if (!sag_state_set_aside(ed, name, sizeof(name))) {
        sag_msg(ed, SAG_MSG_WARN, "workspace state was unreadable");
        return SAG_WS_RECOVERED;
    }
    sag_msg(ed, SAG_MSG_WARN, "workspace state was unreadable; saved as %s",
            name);
    return SAG_WS_RECOVERED;
}

/* ---------------------------------------------------------------- */
/* Reading the file                                                 */
/* ---------------------------------------------------------------- */

typedef enum {
    STATE_READ_OK,
    STATE_READ_ABSENT,
    STATE_READ_UNREADABLE,
    STATE_READ_TOO_BIG
} StateReadErr;

static StateReadErr read_state_file(const char *path, Bytebuf *out)
{
    u8 chunk[64U * 1024U];
    FILE *f;
    size_t n;
    struct stat st;

    if (stat(path, &st) != 0)
        return errno == ENOENT ? STATE_READ_ABSENT : STATE_READ_UNREADABLE;
    if (!S_ISREG(st.st_mode))
        return STATE_READ_UNREADABLE;
    /* Checked BEFORE the read, so an 8 GiB file costs a stat rather
     * than 8 GiB of I/O and a rejection afterwards. */
    if ((u64)st.st_size > (u64)SAG_FL_MAX_BYTES)
        return STATE_READ_TOO_BIG;
    f = fopen(path, "rb");
    if (f == NULL)
        return errno == ENOENT ? STATE_READ_ABSENT : STATE_READ_UNREADABLE;
    while ((n = fread(chunk, 1U, sizeof(chunk), f)) > 0U) {
        bytebuf_append(out, chunk, n);
        if (out->len > (u64)SAG_FL_MAX_BYTES) {
            (void)fclose(f);
            return STATE_READ_TOO_BIG;
        }
    }
    if (ferror(f) != 0) {
        (void)fclose(f);
        return STATE_READ_UNREADABLE;
    }
    (void)fclose(f);
    return STATE_READ_OK;
}

/* ---------------------------------------------------------------- */
/* Applying a window record                                         */
/* ---------------------------------------------------------------- */

static void apply_cursors(Win *w, const FlLit *rec)
{
    const FlLit *list = sag_fl_get(rec, "cursors");
    u32 n = sag_fl_len(list);
    u32 i;
    i64 primary;

    if (n == 0U)
        return; /* the default one cursor at 0 is already there */
    if (n > (u32)SAG_STATE_MAX_CURSORS)
        n = (u32)SAG_STATE_MAX_CURSORS;
    for (i = 0U; i < n; i++) {
        const FlLit *c = sag_fl_at(list, i);
        Cursor cur;

        cur.pos = BYTEOFF((u64)sag_fl_int_or(sag_fl_get(c, "pos"), 0));
        cur.anchor = BYTEOFF((u64)sag_fl_int_or(sag_fl_get(c, "anchor"), 0));
        cur.goal_col.v =
            sag_goal_from_i64(sag_fl_int_or(sag_fl_get(c, "goal"), -1));
        if (i == 0U)
            w->cs.curs.data[w->cs.primary] = cur;
        else
            (void)sag_cset_add(&w->cs, cur);
    }
    primary = sag_fl_int_or(sag_fl_get(rec, "primary"), 0);
    if (primary >= 0 && (u32)primary < w->cs.curs.len)
        w->cs.primary = (u32)primary;
    /*
     * Normalized against the buffer WHEN THERE IS ONE.  A document
     * saved before someone else truncated the file carries offsets past
     * the end, and an out-of-range cursor is not cosmetic — every
     * motion and every edit derives from it.
     *
     * A deferred tab has no text to check against yet, and reading one
     * here to find out would defeat the deferral for every tab at once.
     * sag_tab_hydrate does it at the first moment there IS text.
     */
    if (w->buf != NULL && w->buf->tb != NULL)
        sag_cset_normalize(w->buf->tb, &w->cs);
}

static void apply_view(Win *w, const FlLit *rec)
{
    const FlLit *v = sag_fl_get(rec, "view");

    if (v == NULL)
        return;
    w->vp.top = LINENO((u64)sag_fl_int_or(sag_fl_get(v, "top"), 0));
    w->vp.top_sub = (u32)sag_fl_int_or(sag_fl_get(v, "top_sub"), 0);
    w->vp.left.v = (u64)sag_fl_int_or(sag_fl_get(v, "left"), 0);
    w->vp.wrap = sag_fl_bool_or(sag_fl_get(v, "wrap"), false);
}

/*
 * A ring entry comes back as buf_id + line_hint, with a DEAD mark.
 *
 * That is not a degradation: a mark only means something inside a
 * loaded buffer, and materializing one here would mean reading every
 * file the user has ever jumped through — which is exactly the cost
 * deferral exists to avoid.  s21 keeps line_hint for this case, and
 * a jump into a deferred buffer reopens it there.
 */
static bool ring_entry(Ed *ed, const FlLit *rec, JumpEntry *out)
{
    const char *path;
    u64 plen = 0U;
    Buffer *b;

    (void)memset(out, 0, sizeof(*out));
    path = sag_fl_str_or(sag_fl_get(rec, "path"), NULL, &plen);
    if (path == NULL || plen == 0U)
        return false;
    b = sag_ws_file_buf(ed, path);
    if (b == NULL)
        return false;
    out->buf_id = b->id;
    out->line_hint = LINENO((u64)sag_fl_int_or(sag_fl_get(rec, "line"), 0));
    out->stamp_ms = (u64)sag_fl_int_or(sag_fl_get(rec, "stamp"), 0);
    return true;
}

static void apply_jumps(Ed *ed, Win *w, const FlLit *rec)
{
    const FlLit *j = sag_fl_get(rec, "jumps");
    const FlLit *list;
    u32 n;
    u32 i;
    i64 cur;

    if (j == NULL)
        return;
    list = sag_fl_get(j, "entries");
    n = sag_fl_len(list);
    if (n > (u32)SAG_STATE_MAX_JUMPS)
        n = (u32)SAG_STATE_MAX_JUMPS;
    for (i = 0U; i < n; i++) {
        JumpEntry je;

        if (!ring_entry(ed, sag_fl_at(list, i), &je))
            continue;
        w->jumps.e[w->jumps.head] = je;
        w->jumps.head = (w->jumps.head + 1U) % SAG_JUMPLIST_MAX;
        if (w->jumps.len < (u32)SAG_JUMPLIST_MAX)
            w->jumps.len++;
    }
    /* `cur == len` means "standing at now", and it is also the only
     * safe answer for a value the file disagrees with. */
    cur = sag_fl_int_or(sag_fl_get(j, "cur"), (i64)w->jumps.len);
    w->jumps.cur = cur >= 0 && (u32)cur <= w->jumps.len ? (u32)cur
                                                        : w->jumps.len;
}

/* ---------------------------------------------------------------- */
/* Rebuilding a pane tree                                           */
/* ---------------------------------------------------------------- */

typedef struct WinSlots {
    Win *v[SAG_PANE_MAX_LEAVES];
    u32 n;
    u32 leaves; /* leaves materialized so far, against the s22 cap */
} WinSlots;

static Pane *build_panes(const FlLit *m, WinSlots *slots, u32 depth)
{
    const FlLit *split;
    const char *dir;
    u64 dlen = 0U;
    Pane *p;

    if (m == NULL || depth > (u32)SAG_STATE_MAX_PANE_DEPTH)
        return NULL;
    split = sag_fl_get(m, "split");
    dir = sag_fl_str_or(split, NULL, &dlen);
    if (dir == NULL) {
        i64 idx = sag_fl_int_or(sag_fl_get(m, "win"), 0);
        Win *w;

        if (slots->leaves >= (u32)SAG_PANE_MAX_LEAVES)
            return NULL;
        /*
         * An out-of-range `win` takes slot 0 rather than failing the
         * whole tab.  The index is positional, not an id, so a document
         * whose lists disagree is corrupt in exactly one field — and a
         * pane showing the wrong window beats a tab that vanished.
         */
        if (idx < 0 || (u32)idx >= slots->n)
            idx = 0;
        w = slots->n == 0U ? NULL : slots->v[idx];
        if (w == NULL)
            return NULL;
        slots->leaves++;
        return sag_pane_new_leaf(w);
    }
    p = sag_xcalloc(1U, sizeof(*p));
    p->is_leaf = false;
    p->dir = dlen > 0U && dir[0] == 'v' ? SAG_SPLIT_V : SAG_SPLIT_H;
    p->ratio = sag_permille_to_ratio(
        sag_fl_int_or(sag_fl_get(m, "ratio_permille"), 500));
    p->a = build_panes(sag_fl_get(m, "a"), slots, depth + 1U);
    p->b = build_panes(sag_fl_get(m, "b"), slots, depth + 1U);
    if (p->a == NULL || p->b == NULL) {
        /*
         * A split with one child is not a tree.  Collapse to whichever
         * side survived rather than leaving a node the layout walker
         * would dereference through NULL.
         */
        Pane *keep = p->a != NULL ? p->a : p->b;

        free(p);
        return keep;
    }
    p->a->parent = p;
    p->b->parent = p;
    return p;
}

/* ---------------------------------------------------------------- */
/* Applying one tab record                                          */
/* ---------------------------------------------------------------- */

/*
 * §6: the disk check happens HERE, once per tab at restore — never at
 * save, which would stat 500 paths on every debounce, and never on a
 * draw path.
 */
static bool path_is_readable_file(const char *path)
{
    struct stat st;

    if (path == NULL)
        return false;
    if (stat(path, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    return access(path, R_OK) == 0;
}

static void apply_wins(Ed *ed, Tab *t, const FlLit *rec, Buffer *buf)
{
    const FlLit *wins = sag_fl_get(rec, "wins");
    WinSlots slots;
    u32 n = sag_fl_len(wins);
    u32 i;
    Pane *root;

    (void)memset(&slots, 0, sizeof(slots));
    if (n == 0U)
        return; /* keep the single window sag_tab_open already made */
    if (n > (u32)SAG_PANE_MAX_LEAVES)
        n = (u32)SAG_PANE_MAX_LEAVES;
    for (i = 0U; i < n; i++) {
        Win *w = sag_ed_win_clone(ed, ed->win);

        if (w == NULL)
            break;
        sag_ed_win_set_buffer(ed, w, buf);
        /* set_buffer is a no-op when the window already shows `buf`,
         * so the view is applied after it either way. */
        apply_cursors(w, sag_fl_at(wins, i));
        apply_view(w, sag_fl_at(wins, i));
        apply_jumps(ed, w, sag_fl_at(wins, i));
        slots.v[slots.n++] = w;
    }
    root = build_panes(sag_fl_get(rec, "panes"), &slots, 0U);
    if (root == NULL) {
        /* Nothing usable: release what we built and keep the tab's
         * original single-leaf tree rather than leaving it rootless. */
        for (i = 0U; i < slots.n; i++)
            sag_ed_win_release(ed, slots.v[i]);
        return;
    }
    /* Any window the tree did not claim is released here; leaking one
     * would leak its cursor set and viewport with it. */
    {
        Pane *claimed[SAG_PANE_MAX_LEAVES * 2];
        u32 nclaimed = 0U;
        u32 k;

        sag_pane_collect_leaves(root, claimed, SAG_ARRAY_LEN(claimed),
                                &nclaimed);
        for (i = 0U; i < slots.n; i++) {
            bool used = false;

            for (k = 0U; k < nclaimed; k++) {
                if (claimed[k]->win == slots.v[i])
                    used = true;
            }
            if (!used)
                sag_ed_win_release(ed, slots.v[i]);
        }
    }
    /* The placeholder tree goes only once the replacement is known
     * good — an early free would strand the tab on a dangling root if
     * build_panes had refused. */
    if (t->root != NULL) {
        Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
        u32 nl = 0U;
        /*
         * Is the editor LOOKING at the tree we are about to free?
         *
         * It is whenever restore reuses an already-open tab:
         * sag_tab_open finds the existing one and switches to it, which
         * points ed->pane_root at this very root.  Freeing it and
         * walking away leaves the next sag_ed_layout writing rects into
         * freed memory — a use-after-free that surfaces as a segfault
         * inside calloc several hundred tests later, nowhere near here.
         */
        bool was_live = ed->pane_root == t->root;

        sag_pane_collect_leaves(t->root, leaves, SAG_ARRAY_LEN(leaves),
                                &nl);
        for (i = 0U; i < nl; i++)
            sag_ed_win_release(ed, leaves[i]->win);
        sag_pane_free(t->root);
        t->root = root;
        t->focus = sag_pane_first_leaf(root);
        if (was_live) {
            ed->pane_root = t->root;
            ed->focus = t->focus;
            if (ed->focus != NULL && ed->focus->win != NULL)
                ed->win = ed->focus->win;
            ed->layout_dirty = true;
            ed->full_damage = true;
        }
        return;
    }
    t->root = root;
    t->focus = sag_pane_first_leaf(root);
}

/* ---------------------------------------------------------------- */
/* Applying the document                                            */
/* ---------------------------------------------------------------- */

static void apply_groups(Ed *ed, const FlLit *doc, IdMapVec *gids)
{
    const FlLit *list = sag_fl_get(doc, "groups");
    u32 n = sag_fl_len(list);
    u32 i;

    for (i = 0U; i < n; i++) {
        const FlLit *g = sag_fl_at(list, i);
        const char *label;
        const char *dir;
        u64 llen = 0U;
        u64 dlen = 0U;
        u32 file_id = (u32)sag_fl_int_or(sag_fl_get(g, "id"), 0);
        u32 live;

        if (file_id == 0U)
            continue;
        dir = sag_fl_str_or(sag_fl_get(g, "dir_path"), "", &dlen);
        label = sag_fl_str_or(sag_fl_get(g, "label"), NULL, &llen);
        live = sag_group_create(ed, dir, label);
        if (live == 0U)
            continue;
        /* The FILE id is the key and the LIVE id is the value; the two
         * are never conflated.  DoD 4 greps for exactly that. */
        sag_idmap_put(gids, file_id, live);
        {
            const char *last;
            u64 nlen = 0U;

            last = sag_fl_str_or(sag_fl_get(g, "last_active_member"), NULL,
                                 &nlen);
            if (last != NULL && nlen > 0U)
                sag_group_set_last_member(ed, live, last);
        }
    }
}

/* `first_out` receives the index of the first tab this document
 * accounted for, whether it was opened here or already present — see
 * the fallback in sag_state_apply for why length cannot answer that. */
static void apply_tabs(Ed *ed, const FlLit *doc, const IdMapVec *gids,
                       IdMapVec *tids, int *first_out)
{
    const FlLit *list = sag_fl_get(doc, "tabs");
    u32 n = sag_fl_len(list);
    u32 i;
    u32 dropped = 0U;

    if (n > (u32)SAG_STATE_MAX_TABS) {
        sag_log(SAG_LOG_WARN, "workspace state lists %u tabs; keeping %d",
                (unsigned)n, SAG_STATE_MAX_TABS);
        n = (u32)SAG_STATE_MAX_TABS;
    }
    for (i = 0U; i < n; i++) {
        const FlLit *rec = sag_fl_at(list, i);
        const char *path;
        u64 plen = 0U;
        u32 file_id;
        int idx;
        Tab *t;
        Buffer *buf;

        path = sag_fl_str_or(sag_fl_get(rec, "path"), NULL, &plen);
        if (path == NULL || plen == 0U) {
            /* nil path = an untitled scratch tab.  Dropped rather than
             * recreated: there is nothing to put in it (§3 table). */
            dropped++;
            continue;
        }
        idx = sag_tab_open(ed, path);
        if (idx < 0) {
            dropped++;
            continue;
        }
        t = sag_tab_at(ed, idx);
        if (t == NULL) {
            dropped++;
            continue;
        }
        if (*first_out < 0)
            *first_out = idx;
        file_id = (u32)sag_fl_int_or(sag_fl_get(rec, "id"), 0);
        sag_idmap_put(tids, file_id, t->tab_id);
        /*
         * The file is checked once, here.  A tab is KEPT when its file
         * is gone — the path is the only record of what was being
         * worked on, and dropping it is a loss nobody can undo.
         */
        if (!path_is_readable_file(t->path)) {
            t->missing_at_restore = true;
            ed->state.missing_count++;
        }
        {
            u32 gid = sag_idmap_get(gids, (u32)sag_fl_int_or(
                                              sag_fl_get(rec, "group"), 0));

            if (gid != 0U) {
                i64 ord = sag_fl_int_or(sag_fl_get(rec, "group_ordinal"), 0);

                sag_group_add_member(ed, gid, idx);
                if (ord > 0)
                    sag_group_set_ordinal(ed, idx, (int)ord);
            }
        }
        buf = sag_ws_buf_by_id(ed, t->buffer_id);
        if (buf != NULL)
            apply_wins(ed, t, rec, buf);
    }
    if (dropped > 0U)
        sag_log(SAG_LOG_INFO, "workspace state: %u tab record(s) dropped",
                (unsigned)dropped);
}

static void apply_files(Ed *ed, const FlLit *doc)
{
    const FlLit *list = sag_fl_get(doc, "files");
    u32 n = sag_fl_len(list);
    u32 i;

    if (n > (u32)SAG_STATE_MAX_FILES)
        n = (u32)SAG_STATE_MAX_FILES;
    for (i = 0U; i < n; i++) {
        const FlLit *rec = sag_fl_at(list, i);
        const FlLit *marks;
        const FlLit *changes;
        const char *path;
        u64 plen = 0U;
        Buffer *b;
        u32 m;

        path = sag_fl_str_or(sag_fl_get(rec, "path"), NULL, &plen);
        if (path == NULL || plen == 0U)
            continue;
        /*
         * A record with no matching tab is kept anyway: marks for a
         * file you closed are still yours, and the buffer this creates
         * is non-resident, so keeping them costs no read.
         */
        b = sag_ws_file_buf(ed, path);
        if (b == NULL)
            continue;
        marks = sag_fl_get(rec, "marks");
        for (m = 0U; m < sag_fl_len(marks); m++) {
            const FlLit *mk = sag_fl_at(marks, m);
            const char *name;
            u64 nlen = 0U;

            name = sag_fl_str_or(sag_fl_get(mk, "name"), NULL, &nlen);
            if (name == NULL || nlen != 1U || name[0] < 'a' ||
                name[0] > 'z')
                continue;
            /*
             * Recorded as a PENDING offset, not set through
             * sag_ed_mark_set: that needs a MarkSet, which needs a
             * loaded buffer, which is the read deferral exists to
             * avoid.  Hydration materializes them (see ed.c).
             */
            b->pending_marks[name[0] - 'a'] =
                (u64)sag_fl_int_or(sag_fl_get(mk, "pos"), 0);
            b->pending_mark_set[name[0] - 'a'] = true;
        }
        changes = sag_fl_get(rec, "changes");
        if (changes != NULL) {
            const FlLit *entries = sag_fl_get(changes, "entries");
            u32 c;
            u32 nc = sag_fl_len(entries);
            i64 cur;

            if (nc > (u32)SAG_STATE_MAX_JUMPS)
                nc = (u32)SAG_STATE_MAX_JUMPS;
            for (c = 0U; c < nc; c++) {
                JumpEntry je;

                if (!ring_entry(ed, sag_fl_at(entries, c), &je))
                    continue;
                b->changes.e[b->changes.head] = je;
                b->changes.head =
                    (b->changes.head + 1U) % SAG_CHANGELIST_MAX;
                if (b->changes.len < (u32)SAG_CHANGELIST_MAX)
                    b->changes.len++;
            }
            cur = sag_fl_int_or(sag_fl_get(changes, "cur"),
                                (i64)b->changes.len);
            b->changes.cur = cur >= 0 && (u32)cur <= b->changes.len
                                 ? (u32)cur
                                 : b->changes.len;
        }
    }
}

/* Step 9, on every window of every tab. */
static void clamp_all(Ed *ed)
{
    u32 i;

    for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        u32 k;

        if (ed->tabs.v.data[i].root == NULL)
            continue;
        sag_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                SAG_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++) {
            Win *w = leaves[k]->win;

            /*
             * Only RESIDENT windows.  Clamping asks the buffer how many
             * lines it has, and a deferred one has no text to ask —
             * reading it here to find out would defeat the deferral for
             * every tab at once.  sag_tab_hydrate clamps at the first
             * moment there is something to clamp against.
             */
            if (w != NULL && w->buf != NULL && w->buf->tb != NULL)
                sag_vp_clamp(w);
        }
    }
}

SagWsResult sag_state_apply(Ed *ed, const u8 *bytes, u64 len)
{
    FlParseErr err;
    FlLit *doc;
    IdMapVec gids;
    IdMapVec tids;
    i64 version;
    u32 before;
    int first;

    static const u8 nothing = 0U;

    if (ed == NULL)
        return SAG_WS_FRESH;
    /*
     * An EMPTY file is not "nothing to do" — it is a document that does
     * not parse, and §7 sets it aside like any other.  Treating a NULL
     * buffer as absent here made a zero-byte state.fl come back FRESH
     * and stay on disk forever, failing the same way every start.
     */
    if (bytes == NULL) {
        bytes = &nothing;
        len = 0U;
    }
    if (!ed->state.doc_ready) {
        arena_init(&ed->state.doc);
        ed->state.doc_ready = true;
    }
    (void)memset(&err, 0, sizeof(err));
    doc = sag_fl_parse(&ed->state.doc, bytes, len, &err);
    if (doc == NULL) {
        char why[192];

        (void)snprintf(why, sizeof(why), "%u:%u: %s", err.line, err.col,
                       err.msg == NULL ? "parse error" : err.msg);
        return recover(ed, why);
    }
    if (doc->kind != FL_MAP)
        return recover(ed, "root is not a map");
    version = sag_fl_int_or(sag_fl_get(doc, "version"), 0);
    if (version != (i64)SAG_STATE_VERSION) {
        char why[64];

        (void)snprintf(why, sizeof(why), "version %lld, expected %d",
                       (long long)version, SAG_STATE_VERSION);
        return recover(ed, why);
    }
    /*
     * workspace.path mismatch LOGS and continues.  A moved checkout is
     * not corruption — the key is the realpath, so a mismatch means the
     * directory was renamed under us, and refusing to restore would
     * punish someone for moving their own project.
     */
    {
        const char *saved;
        u64 slen = 0U;
        const char *root = sag_ws_root(ed);

        saved = sag_fl_str_or(sag_fl_get(sag_fl_get(doc, "workspace"),
                                         "path"),
                              NULL, &slen);
        if (saved != NULL && root != NULL && strcmp(saved, root) != 0)
            sag_log(SAG_LOG_INFO,
                    "workspace state was written for %s; restoring into %s",
                    saved, root);
    }
    /* Step 2: options kept verbatim for Sprint 36. */
    ed->state.options = sag_fl_get(doc, "options");

    sag_idmap_init(&gids);
    sag_idmap_init(&tids);
    before = (u32)ed->tabs.v.len;
    first = -1;
    apply_groups(ed, doc, &gids);              /* step 3 */
    apply_tabs(ed, doc, &gids, &tids, &first); /* step 4 */
    apply_files(ed, doc);                      /* step 5 */
    {                                          /* step 6 */
        u32 live = sag_idmap_get(&tids,
                                 (u32)sag_fl_int_or(
                                     sag_fl_get(doc, "active_tab"), 0));
        int idx = live == 0U ? -1 : sag_tab_index_of_id(ed, live);

        /*
         * The fallback is the FIRST TAB THIS DOCUMENT NAMED, not the
         * first one past the old length.  active_tab can legitimately
         * resolve to nothing — it names an untitled scratch tab, which
         * is never emitted — and when restore reuses tabs that are
         * already open, the length does not grow at all, so inferring
         * the index from it lands on a tab the document never mentioned
         * or on no tab whatsoever.
         */
        if (idx < 0)
            idx = first;
        if (idx >= 0)
            sag_tab_switch(ed, idx);
    }
    sag_idmap_free(&gids);
    sag_idmap_free(&tids);
    sag_group_prune_empty(ed);               /* step 7 */
    /*
     * Steps 8 and 9 are the caller's, because layout needs the terminal
     * size and this function is also driven by tests with no terminal.
     * sag_ws_restore does both; see there.
     */
    /*
     * RESTORED means the document named at least one tab we could
     * account for — not that the tab count grew.  Restoring into a
     * session that already has those files open changes nothing about
     * the length and is still a restore.
     */
    (void)before;
    return first < 0 ? SAG_WS_FRESH : SAG_WS_RESTORED;
}

SagWsResult sag_ws_restore(Ed *ed)
{
    Bytebuf raw;
    StateReadErr rd;
    SagWsResult result;
    const char *path;

    if (ed == NULL || !ed->state.ready)
        return SAG_WS_FRESH;
    path = sag_ws_state_path(&ed->state.key);
    if (path == NULL)
        return SAG_WS_FRESH;
    bytebuf_init(&raw);
    rd = read_state_file(path, &raw);
    switch (rd) {
    case STATE_READ_ABSENT:
        /* Silent.  A first run is not an event. */
        bytebuf_free(&raw);
        return SAG_WS_FRESH;
    case STATE_READ_UNREADABLE:
        /*
         * NOT set aside.  We could not read it, so we have no grounds
         * to call it corrupt — renaming a file we merely lack
         * permission for would move someone else's data.
         */
        bytebuf_free(&raw);
        sag_log(SAG_LOG_WARN, "cannot read %s: %s", path, strerror(errno));
        sag_msg(ed, SAG_MSG_WARN, "workspace state could not be read");
        return SAG_WS_FRESH;
    case STATE_READ_TOO_BIG:
        bytebuf_free(&raw);
        return recover(ed, "larger than the 8 MiB cap");
    case STATE_READ_OK:
    default:
        break;
    }
    result = sag_state_apply(ed, raw.data, raw.len);
    bytebuf_free(&raw);

    /* Step 8: at the CURRENT size.  Ratios are permille, so a smaller
     * terminal restores proportionally rather than in stale cells. */
    sag_ed_layout(ed);
    clamp_all(ed); /* step 9 — clamp, never follow */

    /*
     * ONE message for all of them.  A wall of per-file warnings trains
     * people to dismiss the next one, which will matter.
     */
    if (ed->state.missing_count == 1U) {
        sag_msg(ed, SAG_MSG_WARN,
                "1 file in this workspace is no longer on disk");
    } else if (ed->state.missing_count > 1U) {
        sag_msg(ed, SAG_MSG_WARN,
                "%u files in this workspace are no longer on disk",
                (unsigned)ed->state.missing_count);
    }
    /*
     * A restore is not a change.  Marking dirty here would rewrite the
     * document on every start, so the first REAL edit is what schedules
     * the next save.
     */
    ed->state.dirty = false;
    return result;
}
