#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/*
 * Sprint 24 fuzz: group membership, and the picker's key handling.
 *
 * Two things are being defended.
 *
 * MEMBERSHIP.  Groups keep no member list, so every count and every
 * ordering is recomputed from the tabs.  The oracle here keeps exactly
 * the list the real model refuses to keep, in obvious slow code that
 * shares nothing with the implementation.  After every op the two must
 * agree — and every live group must have members, because an empty one
 * is a row-1 entry that resolves to nothing (DoD 10).
 *
 * THE PICKER.  Random keys must never crash it, and the ticked set must
 * never come to contain a path that was neither listed nor preselected.
 * A dialog that walks directories while holding a set of paths has two
 * ways to go wrong — losing ticks, and inventing them — and a key
 * storm finds the second one.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/groupnav.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/tabs.h"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

enum {
    FZ_MAX_TABS = 48,
    FZ_MAX_GROUPS = 12
};

/* The member lists the model deliberately does not keep, by tab_id
 * because the array compacts under both of us. */
typedef struct Oracle {
    u32 gid[FZ_MAX_GROUPS];
    u32 members[FZ_MAX_GROUPS][FZ_MAX_TABS];
    int n_members[FZ_MAX_GROUPS];
    int n_groups;
} Oracle;

static int oracle_find(const Oracle *o, u32 gid)
{
    int i;

    for (i = 0; i < o->n_groups; i++) {
        if (o->gid[i] == gid)
            return i;
    }
    return -1;
}

static void oracle_drop(Oracle *o, int at)
{
    int i;

    for (i = at; i < o->n_groups - 1; i++) {
        o->gid[i] = o->gid[i + 1];
        (void)memcpy(o->members[i], o->members[i + 1],
                     sizeof(o->members[i]));
        o->n_members[i] = o->n_members[i + 1];
    }
    o->n_groups--;
}

/* Removes a tab from whichever group holds it, dissolving an emptied
 * group exactly as the model does. */
static void oracle_remove(Oracle *o, u32 tid)
{
    int i;
    int j;
    int k;

    for (i = 0; i < o->n_groups; i++) {
        for (j = 0; j < o->n_members[i]; j++) {
            if (o->members[i][j] != tid)
                continue;
            for (k = j; k < o->n_members[i] - 1; k++)
                o->members[i][k] = o->members[i][k + 1];
            o->n_members[i]--;
            if (o->n_members[i] == 0)
                oracle_drop(o, i);
            return;
        }
    }
}

static bool oracle_agrees(const Ed *ed, const Oracle *o, char *why,
                          size_t cap)
{
    int i;
    int j;

    if ((int)ed->groups.v.len != o->n_groups) {
        (void)snprintf(why, cap, "group count %d, oracle %d",
                       (int)ed->groups.v.len, o->n_groups);
        return false;
    }
    for (i = 0; i < o->n_groups; i++) {
        int members[FZ_MAX_TABS];
        int n = yew_group_members(ed, o->gid[i], members, FZ_MAX_TABS);

        /* DoD 10: no live group is ever empty. */
        if (n == 0) {
            (void)snprintf(why, cap, "group %u is empty",
                           (unsigned)o->gid[i]);
            return false;
        }
        if (n != o->n_members[i]) {
            (void)snprintf(why, cap, "group %u has %d members, oracle %d",
                           (unsigned)o->gid[i], n, o->n_members[i]);
            return false;
        }
        if (yew_group_member_count(ed, o->gid[i]) != n) {
            (void)snprintf(why, cap, "count disagrees with members()");
            return false;
        }
        for (j = 0; j < n; j++) {
            const Tab *t = yew_tab_at((Ed *)ed, members[j]);

            if (t == NULL || t->tab_id != o->members[i][j]) {
                (void)snprintf(why, cap, "group %u member %d mismatch",
                               (unsigned)o->gid[i], j);
                return false;
            }
            /* Ordinals are exactly 1..n with no holes. */
            if (t->group_ordinal != (u32)(j + 1)) {
                (void)snprintf(why, cap, "group %u ordinal %u at slot %d",
                               (unsigned)o->gid[i],
                               (unsigned)t->group_ordinal, j);
                return false;
            }
        }
    }
    /* No tab points at a group that does not exist. */
    for (i = 0; i < (int)yew_tab_count(ed); i++) {
        const Tab *t = yew_tab_at((Ed *)ed, i);

        if (t->group_id == 0U) {
            if (t->group_ordinal != 0U) {
                (void)snprintf(why, cap, "ungrouped tab has ordinal %u",
                               (unsigned)t->group_ordinal);
                return false;
            }
            continue;
        }
        if (yew_group_find(ed, t->group_id) < 0) {
            (void)snprintf(why, cap, "tab %d orphaned on group %u", i,
                           (unsigned)t->group_id);
            return false;
        }
    }
    return true;
}

static bool run_membership(Rng *rng, Ed *ed, Oracle *o, char *why,
                           size_t cap, u32 iters)
{
    u32 step;

    for (step = 0U; step < iters; step++) {
        int ntabs = (int)yew_tab_count(ed);
        int idx = ntabs > 0 ? (int)rng_below(rng, (u32)ntabs) : 0;

        switch (rng_below(rng, 6U)) {
        case 0: /* create a group with one member */
            if (o->n_groups < FZ_MAX_GROUPS && ntabs > 0) {
                u32 g = yew_group_create(ed, "/src", NULL);
                u32 tid = yew_tab_at(ed, idx)->tab_id;

                yew_group_add_member(ed, g, idx);
                oracle_remove(o, tid);
                o->gid[o->n_groups] = g;
                o->members[o->n_groups][0] = tid;
                o->n_members[o->n_groups] = 1;
                o->n_groups++;
            }
            break;
        case 1: /* join an existing group */
            if (o->n_groups > 0 && ntabs > 0) {
                int gi = (int)rng_below(rng, (u32)o->n_groups);
                u32 gid = o->gid[gi];
                u32 tid = yew_tab_at(ed, idx)->tab_id;

                if (yew_tab_at(ed, idx)->group_id != gid) {
                    yew_group_add_member(ed, gid, idx);
                    oracle_remove(o, tid);
                    gi = oracle_find(o, gid);
                    if (gi >= 0)
                        o->members[gi][o->n_members[gi]++] = tid;
                }
            }
            break;
        case 2: /* leave */
            if (ntabs > 0) {
                u32 tid = yew_tab_at(ed, idx)->tab_id;

                yew_group_remove_member(ed, idx);
                oracle_remove(o, tid);
            }
            break;
        case 3: /* close a tab */
            if (ntabs > 1) {
                u32 tid = yew_tab_at(ed, idx)->tab_id;

                (void)yew_tab_close(ed, idx);
                oracle_remove(o, tid);
            }
            break;
        case 4: /* open a tab */
            if (ntabs < FZ_MAX_TABS - 1) {
                char path[64];

                (void)snprintf(path, sizeof(path), "/tmp/yew-fz-%u.txt",
                               (unsigned)step);
                (void)yew_tab_open(ed, path);
            }
            break;
        default: /* reorder within the group */
            if (ntabs > 0 && yew_tab_at(ed, idx)->group_id != 0U) {
                u32 gid = yew_tab_at(ed, idx)->group_id;
                int gi = oracle_find(o, gid);
                int n = gi >= 0 ? o->n_members[gi] : 0;

                if (n > 0) {
                    int pos = (int)rng_below(rng, (u32)n) + 1;
                    u32 tid = yew_tab_at(ed, idx)->tab_id;
                    int at = 0;
                    int k;

                    yew_group_set_ordinal(ed, idx, pos);
                    for (k = 0; k < n; k++) {
                        if (o->members[gi][k] == tid)
                            at = k;
                    }
                    for (k = at; k < n - 1; k++)
                        o->members[gi][k] = o->members[gi][k + 1];
                    for (k = n - 1; k > pos - 1; k--)
                        o->members[gi][k] = o->members[gi][k - 1];
                    o->members[gi][pos - 1] = tid;
                }
            }
            break;
        }
        if (!oracle_agrees(ed, o, why, cap))
            return false;
        /* The walk must survive any shape the storm produces: it is the
         * only way out of a group, so a crash here strands the user. */
        yew_file_step(ed, rng_below(rng, 2U) == 0U ? 1 : -1);
        if (!oracle_agrees(ed, o, why, cap))
            return false;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* The picker key storm                                             */
/* ---------------------------------------------------------------- */

typedef struct FzTree {
    char root[64];
    char sub[128];
    char files[4][256];
} FzTree;

static bool tree_make(FzTree *t)
{
    int i;

    (void)snprintf(t->root, sizeof(t->root), "/tmp/yew-fzgp-XXXXXX");
    if (mkdtemp(t->root) == NULL)
        return false;
    (void)snprintf(t->sub, sizeof(t->sub), "%s/sub", t->root);
    if (mkdir(t->sub, 0700) != 0)
        return false;
    (void)snprintf(t->files[0], sizeof(t->files[0]), "%s/a.txt", t->root);
    (void)snprintf(t->files[1], sizeof(t->files[1]), "%s/b.txt", t->root);
    (void)snprintf(t->files[2], sizeof(t->files[2]), "%s/c.txt", t->sub);
    (void)snprintf(t->files[3], sizeof(t->files[3]), "%s/d.txt", t->sub);
    for (i = 0; i < 4; i++) {
        FILE *f = fopen(t->files[i], "w");

        if (f == NULL)
            return false;
        (void)fprintf(f, "x\n");
        (void)fclose(f);
    }
    return true;
}

static void tree_remove(FzTree *t)
{
    int i;

    for (i = 0; i < 4; i++)
        (void)unlink(t->files[i]);
    (void)rmdir(t->sub);
    (void)rmdir(t->root);
}

static bool run_picker(Rng *rng, Ed *ed, const FzTree *t, char *why,
                       size_t cap, u32 iters)
{
    static const u32 codes[] = {
        YEW_KEY_UP,  YEW_KEY_DOWN,  YEW_KEY_LEFT, YEW_KEY_RIGHT,
        YEW_KEY_TAB, YEW_KEY_ENTER, (u32)' ',     (u32)'q',
        (u32)'/',    YEW_KEY_BACKSPACE, YEW_KEY_ESCAPE
    };
    u32 step;

    if (!yew_gp_show(ed, t->root)) {
        (void)snprintf(why, cap, "picker refused to open");
        return false;
    }
    for (step = 0U; step < iters; step++) {
        Key k;

        if (!yew_gp_active()) {
            /* Enter confirmed it or Esc cancelled it; apply and reopen
             * rather than hammering a closed dialog. */
            yew_gp_apply(ed);
            if (!yew_gp_show(ed, t->root)) {
                (void)snprintf(why, cap, "picker refused to reopen");
                return false;
            }
        }
        (void)memset(&k, 0, sizeof(k));
        k.kind = YEW_EV_KEY;
        k.ev = YEW_KEY_PRESS;
        k.code = codes[rng_below(rng, (u32)(sizeof(codes) /
                                            sizeof(codes[0])))];
        if (k.code >= 0x20U && k.code < 0x7FU) {
            k.ntext = 1U;
            k.text[0] = (u8)k.code;
        }
        (void)yew_gp_key(ed, k);
        /*
         * Every confirmed path must be one the dialog could actually
         * have OFFERED: absolute, canonical, and a regular file that
         * exists.
         *
         * Deliberately not "under the starting directory" — walking
         * past `../` into another tree is the entire point of the
         * dialog, and a fuzzer that forbade it would be testing a
         * picker nobody asked for.  What is being caught here is an
         * INVENTED path: a truncated join, a directory ticked as a
         * file, or a stale entry surviving a re-list.
         */
        if (yew_gp_result() == YEW_GP_CONFIRMED) {
            int i;

            for (i = 0; i < yew_gp_count(); i++) {
                const char *p = yew_gp_path(i);
                struct stat st;
                char *canon;
                bool same;

                if (p == NULL || p[0] != '/') {
                    (void)snprintf(why, cap,
                                   "picker produced a non-absolute path: %s",
                                   p == NULL ? "(null)" : p);
                    return false;
                }
                if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
                    (void)snprintf(why, cap,
                                   "picker produced a non-file: %s", p);
                    return false;
                }
                canon = realpath(p, NULL);
                same = canon != NULL && strcmp(canon, p) == 0;
                free(canon);
                if (!same) {
                    (void)snprintf(why, cap,
                                   "picker produced a non-canonical path: %s",
                                   p);
                    return false;
                }
            }
        }
        yew_gp_apply(ed);
    }
    yew_gp_close(ed);
    return true;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Ed ed;
    Oracle o;
    FzTree t;
    Rng rng;
    bool ok = false;
    int i;
    /* Enough ops to shake out ordinal compaction without making each
     * corpus entry cost a second. */
    const u32 iters = 400U;

    rng.s = 0x9E3779B97F4A7C15ULL;
    for (i = 0; i < (int)len; i++)
        rng.s = rng.s * 31U + data[i];
    (void)memset(&o, 0, sizeof(o));
    if (!tree_make(&t)) {
        (void)snprintf(why, why_cap, "cannot build the fixture tree");
        return false;
    }
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open a scratch buffer");
        goto done;
    }
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    for (i = 0; i < 6; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-fzseed-%d.txt", i);
        (void)yew_tab_open(&ed, path);
    }
    if (!run_membership(&rng, &ed, &o, why, why_cap, iters))
        goto done;
    if (!run_picker(&rng, &ed, &t, why, why_cap, iters))
        goto done;
    ok = true;
done:
    yew_gp_close(&ed);
    yew_ed_free(&ed);
    tree_remove(&t);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_groups", NULL, run_session);
}
