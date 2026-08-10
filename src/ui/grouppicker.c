/*
 * Sprint 24 §4.  See grouppicker.h for why ticks are a path set.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ui/grouppicker.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "ui/glyphs.h"
#include "ui/groups.h"
#include "ui/message.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "unicode/width.h"
#include "util/sort.h"
#include "util/strmap.h"

enum {
    GP_ROWS_MAX = 512,
    GP_NAME_ROW = 0
};

typedef struct GpRow {
    char name[256];
    bool is_dir;
    bool is_parent; /* the `../` row */
} GpRow;

/*
 * Module-local: the picker is a modal singleton, and handing its state
 * around would invite a second one.
 */
static struct {
    bool active;
    bool edit_mode;
    GpResult result;
    char dir[YEW_GP_PATH_MAX];
    char name[YEW_GP_NAME_MAX];
    /*
     * THE TICKS.  Keys are canonical paths; the value is unused.  A
     * Strmap because it iterates in INSERTION order, so the confirmed
     * list is deterministic (invariant 5) rather than hash-ordered.
     */
    Strmap ticked;
    Strmap dirty;
    GpRow rows[GP_ROWS_MAX];
    int n_rows;
    int cursor;   /* index into rows */
    int scroll;   /* first visible row */
    bool on_name; /* focus is the name field rather than the list */
    char note[96];
    /* Filled at confirm so the accessors can answer after the dialog
     * has closed. */
    char *result_paths[GP_ROWS_MAX];
    int n_result;
    /* Where the last draw put things, so hit-testing and drawing share
     * one arithmetic (the Sprint 22 law). */
    Rect box;
} gp;

bool yew_gp_active(void)
{
    return gp.active;
}

GpResult yew_gp_result(void)
{
    return gp.result;
}

const char *yew_gp_name(void)
{
    return gp.name;
}

int yew_gp_count(void)
{
    return gp.n_result;
}

const char *yew_gp_path(int i)
{
    if (i < 0 || i >= gp.n_result)
        return NULL;
    return gp.result_paths[i];
}

static void gp_free_results(void)
{
    int i;

    for (i = 0; i < gp.n_result; i++)
        free(gp.result_paths[i]);
    gp.n_result = 0;
}

/* ---------------------------------------------------------------- */
/* Paths                                                            */
/* ---------------------------------------------------------------- */

/*
 * Canonical, and canonical HERE — at the one place a path enters the
 * tick set.  Two spellings of one file would tick twice and open twice,
 * which is two claims on one save destination.
 */
static void gp_canonical(const char *path, char *out, size_t cap)
{
    char *resolved;

    out[0] = '\0';
    if (path == NULL)
        return;
    resolved = realpath(path, NULL);
    if (resolved != NULL) {
        (void)snprintf(out, cap, "%s", resolved);
        free(resolved);
        return;
    }
    /* A path that does not resolve is still a legitimate selection —
     * keep what was asked for rather than dropping it silently. */
    (void)snprintf(out, cap, "%s", path);
}

static void gp_join(const char *dir, const char *name, char *out,
                    size_t cap)
{
    size_t n = strlen(dir);
    int written;

    if (n > 0U && dir[n - 1U] == '/')
        written = snprintf(out, cap, "%s%s", dir, name);
    else
        written = snprintf(out, cap, "%s/%s", dir, name);
    /* A path too long to represent must not become a TRUNCATED path
     * that names a different file — it becomes no path at all. */
    if (written < 0 || (size_t)written >= cap)
        out[0] = '\0';
}

static bool gp_is_ticked(const char *path)
{
    return strmap_has(&gp.ticked, path, strlen(path));
}

static void gp_tick(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return;
    (void)strmap_put(&gp.ticked, path, strlen(path), (void *)(uintptr_t)1U);
}

static void gp_untick(const char *path)
{
    Strmap fresh;
    StrmapIter it;
    const char *key;
    size_t key_len;
    void *value;

    if (!gp_is_ticked(path))
        return;
    /*
     * Rebuilt rather than deleted in place: Strmap has no remove, and
     * the rebuild preserves insertion order for every surviving key,
     * which is what keeps the confirmed list deterministic.
     */
    strmap_init(&fresh);
    it = strmap_iter(&gp.ticked);
    while (strmap_iter_next(&it, &key, &key_len, &value)) {
        if (key_len == strlen(path) && memcmp(key, path, key_len) == 0)
            continue;
        (void)strmap_put(&fresh, key, key_len, value);
    }
    strmap_free(&gp.ticked);
    gp.ticked = fresh;
}

void yew_gp_preselect(const char *path)
{
    char canon[YEW_GP_PATH_MAX];

    gp_canonical(path, canon, sizeof(canon));
    if (canon[0] != '\0')
        gp_tick(canon);
}

void yew_gp_mark_dirty(const char *path)
{
    char canon[YEW_GP_PATH_MAX];

    gp_canonical(path, canon, sizeof(canon));
    if (canon[0] != '\0')
        (void)strmap_put(&gp.dirty, canon, strlen(canon),
                         (void *)(uintptr_t)1U);
}

/* ---------------------------------------------------------------- */
/* Listing                                                          */
/* ---------------------------------------------------------------- */

static int gp_ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int gp_casecmp(const char *a, const char *b)
{
    size_t i = 0U;

    for (;;) {
        int ca = gp_ascii_lower((unsigned char)a[i]);
        int cb = gp_ascii_lower((unsigned char)b[i]);

        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == 0)
            return 0;
        i++;
    }
}

/* Directories first, then files; each case-insensitively sorted.  The
 * `../` row is not sorted — it is pinned by the lister. */
static int gp_row_cmp(const void *a, const void *b, void *ctx)
{
    const GpRow *x = a;
    const GpRow *y = b;

    (void)ctx;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return gp_casecmp(x->name, y->name);
}

static void gp_list_dir(void)
{
    DIR *d;
    struct dirent *e;

    gp.n_rows = 0;
    gp.cursor = 0;
    gp.scroll = 0;
    /*
     * The `../` row is pinned on top and absent at `/`.  It is what
     * makes a group able to span directories, which is why the tick set
     * is a set of paths and not a per-row flag.
     */
    if (strcmp(gp.dir, "/") != 0) {
        (void)memset(&gp.rows[0], 0, sizeof(gp.rows[0]));
        (void)snprintf(gp.rows[0].name, sizeof(gp.rows[0].name), "..");
        gp.rows[0].is_dir = true;
        gp.rows[0].is_parent = true;
        gp.n_rows = 1;
    }
    d = opendir(gp.dir);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL && gp.n_rows < GP_ROWS_MAX) {
        GpRow *r;
        char full[YEW_GP_PATH_MAX];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (e->d_name[0] == '.')
            continue; /* dotfiles stay out of the way */
        gp_join(gp.dir, e->d_name, full, sizeof(full));
        /* stat rather than d_type: d_type is DT_UNKNOWN on several
         * filesystems, and a directory listed as a file would be
         * tickable and then never openable. */
        if (stat(full, &st) != 0)
            continue;
        /*
         * Directories to walk, REGULAR FILES to tick, and nothing else.
         *
         * Without the S_ISREG half, everything that is not a directory
         * became tickable — the fuzzer walked to /dev and selected a
         * block device, which the group would then have opened as a
         * document.  A picker for choosing files must only offer files.
         */
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;
        r = &gp.rows[gp.n_rows];
        (void)memset(r, 0, sizeof(*r));
        (void)snprintf(r->name, sizeof(r->name), "%s", e->d_name);
        r->is_dir = S_ISDIR(st.st_mode);
        gp.n_rows++;
    }
    (void)closedir(d);
    {
        int base = gp.n_rows > 0 && gp.rows[0].is_parent ? 1 : 0;

        yew_sort_stable(&gp.rows[base], (size_t)(gp.n_rows - base),
                        sizeof(gp.rows[0]), gp_row_cmp, NULL);
    }
}

static void gp_walk_to(const char *dir)
{
    char canon[YEW_GP_PATH_MAX];

    gp_canonical(dir, canon, sizeof(canon));
    if (canon[0] == '\0')
        return;
    (void)snprintf(gp.dir, sizeof(gp.dir), "%s", canon);
    /* The ticks survive: they are paths, and this only changed which
     * ones happen to be on screen. */
    gp_list_dir();
}

/* ---------------------------------------------------------------- */
/* Show / close                                                     */
/* ---------------------------------------------------------------- */

static void gp_default_name(const char *dir, char *out, size_t cap)
{
    const char *base;
    const char *end;
    size_t len;

    if (dir == NULL || dir[0] == '\0') {
        (void)snprintf(out, cap, "group/");
        return;
    }
    end = dir + strlen(dir);
    while (end > dir && end[-1] == '/')
        end--;
    base = end;
    while (base > dir && base[-1] != '/')
        base--;
    len = (size_t)(end - base);
    if (len == 0U || len > cap - 2U) {
        (void)snprintf(out, cap, "group/");
        return;
    }
    (void)memcpy(out, base, len);
    out[len] = '/';
    out[len + 1U] = '\0';
}

static bool gp_open(Ed *ed, const char *dir, const char *name, bool edit)
{
    if (ed == NULL)
        return false;
    yew_gp_close(ed);
    /* A fresh dialog invalidates the last one's answer.  Leaving the
     * old result standing would let an accessor report paths the user
     * chose in a dialog they have since dismissed. */
    gp_free_results();
    strmap_init(&gp.ticked);
    strmap_init(&gp.dirty);
    gp.active = true;
    gp.edit_mode = edit;
    gp.result = YEW_GP_PENDING;
    gp.on_name = false;
    gp.note[0] = '\0';
    gp_walk_to(dir != NULL && dir[0] != '\0' ? dir : yew_ws_root(ed));
    if (name != NULL && name[0] != '\0')
        (void)snprintf(gp.name, sizeof(gp.name), "%s", name);
    else
        gp_default_name(gp.dir, gp.name, sizeof(gp.name));
    ed->full_damage = true;
    return true;
}

bool yew_gp_show(Ed *ed, const char *dir)
{
    return gp_open(ed, dir, NULL, false);
}

bool yew_gp_show_edit(Ed *ed, const char *dir, const char *name)
{
    return gp_open(ed, dir, name, true);
}

void yew_gp_close(Ed *ed)
{
    strmap_free(&gp.ticked);
    strmap_free(&gp.dirty);
    gp.active = false;
    gp.n_rows = 0;
    gp.cursor = 0;
    gp.scroll = 0;
    gp.note[0] = '\0';
    if (ed != NULL)
        ed->full_damage = true;
}

/* ---------------------------------------------------------------- */
/* Confirm                                                          */
/* ---------------------------------------------------------------- */

static int gp_tick_count(void)
{
    return (int)strmap_len(&gp.ticked);
}

static void gp_confirm(Ed *ed)
{
    StrmapIter it;
    const char *key;
    size_t key_len;
    void *value;

    /* Refused, not silently accepted: a group with no members would
     * auto-dissolve the moment it was created, so the user would watch
     * their work vanish with no explanation. */
    if (gp_tick_count() == 0) {
        (void)snprintf(gp.note, sizeof(gp.note),
                       "tick at least one file (space)");
        return;
    }
    if (gp.name[0] == '\0') {
        (void)snprintf(gp.note, sizeof(gp.note), "the group needs a name");
        gp.on_name = true;
        return;
    }
    gp_free_results();
    it = strmap_iter(&gp.ticked);
    while (strmap_iter_next(&it, &key, &key_len, &value) &&
           gp.n_result < GP_ROWS_MAX) {
        char *copy = yew_xmalloc(key_len + 1U);

        (void)memcpy(copy, key, key_len);
        copy[key_len] = '\0';
        gp.result_paths[gp.n_result++] = copy;
    }
    gp.result = YEW_GP_CONFIRMED;
    gp.active = false;
    if (ed != NULL)
        ed->full_damage = true;
}

/* ---------------------------------------------------------------- */
/* Keys                                                             */
/* ---------------------------------------------------------------- */

static void gp_cursor_to(int at)
{
    if (gp.n_rows == 0)
        return;
    if (at < 0)
        at = 0;
    if (at >= gp.n_rows)
        at = gp.n_rows - 1;
    gp.cursor = at;
    /* Minimal scroll, like the tab strip: moving the list further than
     * needed shifts rows the user was reading. */
    if (gp.cursor < gp.scroll)
        gp.scroll = gp.cursor;
    if (gp.cursor >= gp.scroll + YEW_GP_VISIBLE_ROWS)
        gp.scroll = gp.cursor - YEW_GP_VISIBLE_ROWS + 1;
    if (gp.scroll < 0)
        gp.scroll = 0;
}

/*
 * Sprint 27 §2: the wheel over the dialog.
 *
 * It moves the CURSOR rather than an independent offset, because
 * gp.scroll is derived from gp.cursor and always has been — a second
 * writer would be reverted by the next cursor move and the list would
 * appear to snap back for no reason.  Moving focus is also the honest
 * reading here: unlike the completion menu, this dialog's cursor is
 * what a click focuses too.
 */
void yew_gp_scroll(Ed *ed, int rows)
{
    if (!gp.active || rows == 0)
        return;
    gp.on_name = false;
    gp_cursor_to(gp.cursor + rows);
    if (ed != NULL)
        ed->full_damage = true;
}

static void gp_enter_row(Ed *ed, const GpRow *r)
{
    char full[YEW_GP_PATH_MAX];

    if (r->is_parent) {
        gp_join(gp.dir, "..", full, sizeof(full));
        gp_walk_to(full);
        return;
    }
    gp_join(gp.dir, r->name, full, sizeof(full));
    gp_walk_to(full);
    (void)ed;
}

/* Space on a file toggles and ADVANCES one row, so a run of files can
 * be ticked without moving the other hand. */
static void gp_toggle_row(Ed *ed, const GpRow *r)
{
    char full[YEW_GP_PATH_MAX];
    char canon[YEW_GP_PATH_MAX];

    /* Directories are WALKED, not ticked: a group is a set of files,
     * and ticking a directory would beg the question of what happens
     * when its contents change. */
    if (r->is_dir) {
        gp_enter_row(ed, r);
        return;
    }
    gp_join(gp.dir, r->name, full, sizeof(full));
    gp_canonical(full, canon, sizeof(canon));
    if (gp_is_ticked(canon))
        gp_untick(canon);
    else
        gp_tick(canon);
    gp.note[0] = '\0';
    gp_cursor_to(gp.cursor + 1);
}

static void gp_name_key(Ed *ed, Key key)
{
    size_t n = strlen(gp.name);

    (void)ed;
    if (key.code == YEW_KEY_BACKSPACE) {
        if (n > 0U)
            gp.name[n - 1U] = '\0';
        return;
    }
    if (key.ntext == 0U || key.code < 0x20U)
        return;
    if (n + key.ntext + 1U >= sizeof(gp.name))
        return;
    (void)memcpy(gp.name + n, key.text, key.ntext);
    gp.name[n + key.ntext] = '\0';
    gp.note[0] = '\0';
}

bool yew_gp_key(Ed *ed, Key key)
{
    const GpRow *r;

    if (!gp.active || ed == NULL)
        return false;
    if (key.ev == YEW_KEY_RELEASE)
        return true;
    /*
     * Any key the dialog takes repaints it.  Without this the picker
     * consumed the keystroke and nothing redrew — the cursor moved in
     * the model and the screen never said so, which the pty harness
     * sees as a frame that never arrives.
     */
    ed->full_damage = true;

    if (key.code == YEW_KEY_ESCAPE) {
        gp.result = YEW_GP_CANCELLED;
        yew_gp_close(ed);
        return true;
    }
    if (key.code == YEW_KEY_TAB) {
        gp.on_name = !gp.on_name;
        return true;
    }
    if (gp.on_name) {
        if (key.code == YEW_KEY_ENTER) {
            gp_confirm(ed);
            return true;
        }
        if (key.code == YEW_KEY_DOWN) {
            gp.on_name = false;
            return true;
        }
        gp_name_key(ed, key);
        return true;
    }

    if (key.code == YEW_KEY_UP) {
        /* Up off the top returns focus to the name field rather than
         * stopping dead — the field is above the list on screen. */
        if (gp.cursor == 0)
            gp.on_name = true;
        else
            gp_cursor_to(gp.cursor - 1);
        return true;
    }
    if (key.code == YEW_KEY_DOWN) {
        gp_cursor_to(gp.cursor + 1);
        return true;
    }
    if (key.code == YEW_KEY_LEFT) {
        char full[YEW_GP_PATH_MAX];

        gp_join(gp.dir, "..", full, sizeof(full));
        gp_walk_to(full);
        return true;
    }
    r = gp.cursor >= 0 && gp.cursor < gp.n_rows ? &gp.rows[gp.cursor]
                                                : NULL;
    if (key.code == (u32)' ') {
        if (r != NULL)
            gp_toggle_row(ed, r);
        return true;
    }
    if (key.code == YEW_KEY_ENTER || key.code == YEW_KEY_RIGHT) {
        if (r != NULL && r->is_dir)
            gp_enter_row(ed, r);
        else
            gp_confirm(ed);
        return true;
    }
    /* Everything else is swallowed: a modal dialog that let unhandled
     * keys through would edit the document behind it. */
    return true;
}

/* ---------------------------------------------------------------- */
/* Drawing                                                          */
/* ---------------------------------------------------------------- */

/*
 * ONE function computes row -> listing index, and both the renderer and
 * the click router call it.  Deriving the mapping twice is the Sprint
 * 22 drift, and here it would mean clicking one filename and ticking
 * another.
 */
static int gp_row_at(u16 y)
{
    int rel;

    if (gp.box.h == 0U)
        return -1;
    /* box.y + 0 border, +1 title, +2 name field, +3 separator, then
     * rows. */
    rel = (int)y - (int)gp.box.y - 4;
    if (rel < 0 || rel >= YEW_GP_VISIBLE_ROWS)
        return -1;
    rel += gp.scroll;
    return rel < gp.n_rows ? rel : -1;
}

void yew_gp_draw(Ed *ed)
{
    u16 w;
    u16 h;
    u16 x0;
    u16 y0;
    int i;
    YewColor fg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    YewColor bg = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};
    YewColor accent = {YEW_COLOR_RGB, 120U, 180U, 255U};
    YewColor dim = {YEW_COLOR_RGB, 120U, 120U, 120U};
    char line[YEW_GP_WIDTH_MAX + 64];

    if (!gp.active || ed == NULL || ed->grid.cols == 0U)
        return;
    w = ed->grid.cols < YEW_GP_WIDTH_MAX ? ed->grid.cols
                                         : (u16)YEW_GP_WIDTH_MAX;
    h = (u16)(YEW_GP_VISIBLE_ROWS + 7);
    if (h > ed->grid.rows)
        h = ed->grid.rows;
    x0 = (u16)((ed->grid.cols - w) / 2U);
    y0 = (u16)((ed->grid.rows > h) ? (ed->grid.rows - h) / 2U : 0U);
    gp.box = (Rect){x0, y0, w, h};

    {
        Cell blank;

        (void)memset(&blank, 0, sizeof(blank));
        for (i = 0; i < (int)h; i++)
            yew_grid_fill(&ed->grid, (u16)(y0 + i), x0, (u16)(x0 + w),
                          blank);
    }
    /* The dialog owns its rectangle, so a click on it never falls
     * through to the pane underneath. */
    yew_region_add(YEW_REGION_BLOCK, gp.box, 0);

    (void)snprintf(line, sizeof(line), " %s ",
                   gp.edit_mode ? "Edit Tab Group" : "New Tab Group");
    (void)yew_grid_puts(&ed->grid, y0, x0, (const u8 *)line, strlen(line),
                        fg, bg, YEW_ATTR_BOLD);

    (void)snprintf(line, sizeof(line), " name: %s%s", gp.name,
                   gp.on_name ? "_" : "");
    (void)yew_grid_puts(&ed->grid, (u16)(y0 + 1U), x0, (const u8 *)line,
                        strlen(line), fg, bg,
                        gp.on_name ? YEW_ATTR_REVERSE : 0U);
    yew_region_add(YEW_REGION_GP_NAME,
                   (Rect){x0, (u16)(y0 + 1U), w, 1U}, 0);

    {
        /* The directory line is the one field that can exceed the box,
         * so it is clipped through the width tables rather than by a
         * byte count — a path ending in a multibyte name must not be
         * cut mid-sequence. */
        size_t fit;
        int cells = 0;

        fit = yew_str_clip((const u8 *)gp.dir, strlen(gp.dir),
                           w > 1U ? (int)(w - 1U) : 0, &cells);
        (void)yew_grid_puts(&ed->grid, (u16)(y0 + 2U), (u16)(x0 + 1U),
                            (const u8 *)gp.dir, fit, dim, bg,
                            YEW_ATTR_DIM);
    }

    for (i = 0; i < YEW_GP_VISIBLE_ROWS; i++) {
        int idx = gp.scroll + i;
        u16 y = (u16)(y0 + 4U + i);
        const GpRow *r;
        char full[YEW_GP_PATH_MAX];
        char canon[YEW_GP_PATH_MAX];
        bool ticked = false;
        bool is_dirty = false;

        if (idx >= gp.n_rows)
            break;
        r = &gp.rows[idx];
        if (!r->is_dir) {
            gp_join(gp.dir, r->name, full, sizeof(full));
            gp_canonical(full, canon, sizeof(canon));
            ticked = gp_is_ticked(canon);
            is_dirty = strmap_has(&gp.dirty, canon, strlen(canon));
        }
        /*
         * `[ ]` is the affordance: an unticked row must still show a
         * box, or nothing on screen says the rows can be ticked at all.
         * Directories show no box, because they are walked.
         */
        (void)snprintf(line, sizeof(line), " %s %s%s%s%s",
                       r->is_dir ? "   "
                                 : yew_glyph(ticked ? YEW_GLYPH_TICKED
                                                    : YEW_GLYPH_UNTICKED),
                       r->is_parent ? "../" : r->name,
                       r->is_dir && !r->is_parent ? "/" : "",
                       ticked && is_dirty ? " " : "",
                       ticked && is_dirty
                           ? yew_glyph(YEW_GLYPH_DIRTY_TICK) : "");
        (void)yew_grid_puts(&ed->grid, y, x0, (const u8 *)line,
                            strlen(line), ticked ? accent : fg, bg,
                            idx == gp.cursor && !gp.on_name
                                ? YEW_ATTR_REVERSE
                                : 0U);
        yew_region_add(YEW_REGION_GP_ROW, (Rect){x0, y, w, 1U}, idx);
    }

    /*
     * The count is ALWAYS visible.  Ticks in other directories have
     * scrolled out of sight, and without the count the `../` row looks
     * like it loses them.
     */
    (void)snprintf(line, sizeof(line), " %d selected", gp_tick_count());
    (void)yew_grid_puts(&ed->grid, (u16)(y0 + h - 2U), x0,
                        (const u8 *)line, strlen(line), fg, bg,
                        YEW_ATTR_BOLD);
    if (gp.note[0] != '\0')
        (void)snprintf(line, sizeof(line), " %s", gp.note);
    else
        (void)snprintf(line, sizeof(line),
                       " tab focus · space tick · enter %s · esc cancel",
                       gp.edit_mode ? "save" : "create");
    (void)yew_grid_puts(&ed->grid, (u16)(y0 + h - 1U), x0,
                        (const u8 *)line, strlen(line), dim, bg,
                        YEW_ATTR_DIM);
}

bool yew_gp_click(Ed *ed, u16 x, u16 y)
{
    Region hit;

    if (!gp.active || ed == NULL)
        return false;
    hit = yew_region_hit(x, y);
    ed->full_damage = true;
    if (hit.kind == YEW_REGION_GP_NAME) {
        gp.on_name = true;
        return true;
    }
    if (hit.kind == YEW_REGION_GP_ROW) {
        /* The same row->index mapping the renderer used, asked for by
         * the same function. */
        int idx = gp_row_at(y);

        if (idx >= 0) {
            gp.on_name = false;
            gp_cursor_to(idx);
            gp_toggle_row(ed, &gp.rows[idx]);
        }
        return true;
    }
    /* Anything else inside the dialog is swallowed by its BLOCK. */
    return hit.kind == YEW_REGION_BLOCK;
}

/* ---------------------------------------------------------------- */
/* Commands: where a confirmed result MEANS something                */
/* ---------------------------------------------------------------- */

/* The group the Edit-mode dialog was opened against, so apply knows
 * which membership to diff.  0 in New mode. */
static u32 gp_edit_gid;

/*
 * New: create the group, then adopt or open each ticked path.
 *
 * `find_by_path` FIRST — an already-open file joins the group as the
 * tab it already is.  Opening it again would be a second tab on one
 * path, which is two claims on one save destination.
 */
static void gp_apply_new(Ed *ed)
{
    u32 gid;
    int i;
    int n = yew_gp_count();

    gid = yew_group_create(ed, gp.dir, yew_gp_name());
    if (gid == 0U)
        return;
    for (i = 0; i < n; i++) {
        const char *path = yew_gp_path(i);
        int idx;

        if (path == NULL)
            continue;
        idx = yew_tab_find_by_path(ed, path);
        if (idx < 0) {
            /* Deferred: opening a 40-file group costs one read, not
             * forty (§3). */
            idx = yew_tab_open(ed, path);
        }
        if (idx >= 0)
            yew_group_add_member(ed, gid, idx);
    }
    /* Empty can only happen if every open failed; prune rather than
     * leave a group that resolves to nothing (DoD 10). */
    yew_group_prune_empty(ed);
}

/*
 * Edit: diff the ticked set against the current members.
 *
 * Added paths open deferred and join; removed members LEAVE the group
 * and their tabs close.  The close is why the dirty bullet exists on
 * the row — the warning has to be readable before Enter.
 */
static void gp_apply_edit(Ed *ed)
{
    int members[YEW_TAB_MAX];
    u32 keep_ids[YEW_TAB_MAX];
    int n_keep = 0;
    int n;
    int i;
    int j;
    u32 gid = gp_edit_gid;

    if (yew_group_find(ed, gid) < 0)
        return;
    /* Additions first, so the group never passes through empty and
     * auto-dissolves out from under the edit. */
    for (i = 0; i < yew_gp_count(); i++) {
        const char *path = yew_gp_path(i);
        int idx;

        if (path == NULL)
            continue;
        idx = yew_tab_find_by_path(ed, path);
        if (idx < 0)
            idx = yew_tab_open(ed, path);
        if (idx < 0)
            continue;
        yew_group_add_member(ed, gid, idx);
        keep_ids[n_keep++] = yew_tab_at(ed, idx)->tab_id;
    }
    /*
     * Removals by tab_ID.  Closing a tab compacts the array, so a list
     * of indices gathered beforehand would name different tabs by the
     * second close.
     */
    n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    {
        u32 drop_ids[YEW_TAB_MAX];
        int n_drop = 0;

        for (i = 0; i < n; i++) {
            const Tab *t = yew_tab_at(ed, members[i]);
            bool keep = false;

            if (t == NULL)
                continue;
            for (j = 0; j < n_keep; j++) {
                if (keep_ids[j] == t->tab_id) {
                    keep = true;
                    break;
                }
            }
            if (!keep)
                drop_ids[n_drop++] = t->tab_id;
        }
        for (i = 0; i < n_drop; i++) {
            int idx = yew_tab_index_of_id(ed, drop_ids[i]);

            if (idx < 0)
                continue;
            /* Never close the last tab out from under the editor. */
            if (yew_tab_count(ed) <= 1U)
                break;
            yew_group_remove_member(ed, idx);
            (void)yew_tab_close(ed, idx);
        }
    }
    yew_group_prune_empty(ed);
}

void yew_gp_apply(Ed *ed)
{
    if (ed == NULL || yew_gp_result() != YEW_GP_CONFIRMED)
        return;
    /* Consumed once: apply runs after every key the dialog took, and a
     * result left CONFIRMED would create the group again on the next
     * keystroke. */
    gp.result = YEW_GP_PENDING;
    if (gp.edit_mode)
        gp_apply_edit(ed);
    else
        gp_apply_new(ed);
    gp_free_results();
    ed->layout_dirty = true;
    ed->full_damage = true;
}

CmdStatus yew_gp_cmd_new(CmdCtx *cx)
{
    const char *dir;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    dir = cx->sarg;
    if (dir == NULL || dir[0] == '\0') {
        const Tab *t = yew_tab_at(cx->ed, cx->ed->tabs.active);
        static char here[YEW_GP_PATH_MAX];

        /* The active tab's directory, else the workspace root: the
         * files you want are almost always beside the one you are
         * looking at. */
        here[0] = '\0';
        if (t != NULL && t->path != NULL) {
            const char *slash = strrchr(t->path, '/');

            if (slash != NULL && slash != t->path) {
                size_t n = (size_t)(slash - t->path);

                if (n < sizeof(here)) {
                    (void)memcpy(here, t->path, n);
                    here[n] = '\0';
                }
            }
        }
        dir = here[0] != '\0' ? here : yew_ws_root(cx->ed);
    }
    gp_edit_gid = 0U;
    return yew_gp_show(cx->ed, dir) ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_gp_cmd_edit(CmdCtx *cx)
{
    Ed *ed;
    u32 gid;
    TabGroup *g;
    int members[YEW_TAB_MAX];
    int n;
    int i;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    gid = yew_active_group_id(ed);
    if (gid == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "not in a tab group");
        return YEW_CMD_ERR_STATE;
    }
    g = yew_group_at(ed, gid);
    if (g == NULL)
        return YEW_CMD_ERR_STATE;
    if (!yew_gp_show_edit(ed, g->dir_path, g->label))
        return YEW_CMD_ERR_STATE;
    gp_edit_gid = gid;
    /*
     * Every current member arrives ticked — including the ones living
     * outside `dir_path`, which is exactly why preselect takes a PATH
     * and not an index into the listing.
     */
    n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    for (i = 0; i < n; i++) {
        const Tab *t = yew_tab_at(ed, members[i]);

        if (t == NULL || t->path == NULL)
            continue;
        yew_gp_preselect(t->path);
        if (yew_tab_modified(ed, members[i]))
            yew_gp_mark_dirty(t->path);
    }
    return YEW_CMD_OK;
}
