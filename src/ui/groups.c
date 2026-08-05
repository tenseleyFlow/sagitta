/*
 * Sprint 24 §1/§2.  See groups.h for why there is no member list.
 */
#include "ui/groups.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "util/log.h"
#include "util/sort.h"

static char *dup_str(const char *s)
{
    size_t n;
    char *out;

    if (s == NULL)
        return NULL;
    n = strlen(s) + 1U;
    out = sag_xmalloc(n);
    (void)memcpy(out, s, n);
    return out;
}

void sag_groups_init(Groups *g)
{
    if (g == NULL)
        return;
    (void)memset(g, 0, sizeof(*g));
    g->next_group_id = 1U; /* 0 is the invalid id, like tab_id */
}

static void group_dispose(TabGroup *g)
{
    if (g == NULL)
        return;
    free(g->label);
    free(g->dir_path);
    free(g->last_active_member);
    (void)memset(g, 0, sizeof(*g));
}

void sag_groups_free(Ed *ed)
{
    size_t i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->groups.v.len; i++)
        group_dispose(&ed->groups.v.data[i]);
    GroupVec_free(&ed->groups.v);
    sag_groups_init(&ed->groups);
}

/* `basename(dir)/` — the trailing slash is what makes a group entry
 * read as a directory rather than as another file. */
static void default_label(const char *dir, char *out, size_t cap)
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
    if (len == 0U) {
        /* The root directory has no basename to take. */
        (void)snprintf(out, cap, "/");
        return;
    }
    if (len > cap - 2U)
        len = cap - 2U;
    (void)memcpy(out, base, len);
    out[len] = '/';
    out[len + 1U] = '\0';
}

u32 sag_group_create(Ed *ed, const char *dir_path, const char *name)
{
    TabGroup g;
    char label[SAG_TAB_LABEL_MAX];

    if (ed == NULL)
        return 0U;
    (void)memset(&g, 0, sizeof(g));
    g.id = ed->groups.next_group_id++;
    g.dir_path = dup_str(dir_path);
    if (name != NULL && name[0] != '\0') {
        g.label = dup_str(name);
    } else {
        default_label(dir_path, label, sizeof(label));
        g.label = dup_str(label);
    }
    /*
     * Groups may be created EMPTY: the picker names the group first and
     * adds members after, and refusing an empty group here would force
     * the caller to invent a temporary member.
     */
    GroupVec_push(&ed->groups.v, g);
    return g.id;
}

int sag_group_find(const Ed *ed, u32 gid)
{
    size_t i;

    if (ed == NULL || gid == 0U)
        return -1;
    for (i = 0U; i < ed->groups.v.len; i++) {
        if (ed->groups.v.data[i].id == gid)
            return (int)i;
    }
    return -1;
}

TabGroup *sag_group_at(Ed *ed, u32 gid)
{
    int at = sag_group_find(ed, gid);

    return at < 0 ? NULL : &ed->groups.v.data[at];
}

int sag_group_member_count(const Ed *ed, u32 gid)
{
    size_t i;
    int n = 0;

    if (ed == NULL || gid == 0U)
        return 0;
    /* Counted, never remembered.  See the header. */
    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (ed->tabs.v.data[i].group_id == gid)
            n++;
    }
    return n;
}

typedef struct MemberRef {
    u32 ordinal;
    int idx;
} MemberRef;

static int member_cmp(const void *a, const void *b, void *ctx)
{
    const MemberRef *x = a;
    const MemberRef *y = b;

    (void)ctx;
    if (x->ordinal < y->ordinal)
        return -1;
    if (x->ordinal > y->ordinal)
        return 1;
    return 0;
}

int sag_group_members(const Ed *ed, u32 gid, int *out, int cap)
{
    MemberRef refs[SAG_TAB_MAX];
    size_t i;
    int n = 0;
    int j;

    if (ed == NULL || out == NULL || cap <= 0 || gid == 0U)
        return 0;
    for (i = 0U; i < ed->tabs.v.len && n < (int)SAG_ARRAY_LEN(refs); i++) {
        if (ed->tabs.v.data[i].group_id != gid)
            continue;
        refs[n].ordinal = ed->tabs.v.data[i].group_ordinal;
        refs[n].idx = (int)i;
        n++;
    }
    /*
     * SORTED, not indexed.  Mid-removal the ordinals have a hole in
     * them, and `out[ordinal - 1] = idx` writes outside the run it
     * meant to fill.  A stable sort also keeps equal ordinals in array
     * order, so the answer is deterministic (invariant 5).
     */
    sag_sort_stable(refs, (size_t)n, sizeof(refs[0]), member_cmp, NULL);
    if (n > cap)
        n = cap;
    for (j = 0; j < n; j++)
        out[j] = refs[j].idx;
    return n;
}

void sag_group_add_member(Ed *ed, u32 gid, int tab_idx)
{
    Tab *t = sag_tab_at(ed, tab_idx);
    size_t i;
    u32 max = 0U;

    if (t == NULL || gid == 0U || sag_group_find(ed, gid) < 0)
        return;
    /* Already here: its ordinal is valid and re-adding would renumber
     * it to the end for no reason. */
    if (t->group_id == gid)
        return;
    /*
     * Leaves the old group FIRST.  A tab in two groups at once is not
     * representable — there is one `group_id` — but skipping the
     * removal leaves the old group's ordinals with a permanent hole,
     * and every later compaction there works from a list one longer
     * than the truth.
     */
    if (t->group_id != 0U)
        sag_group_remove_member(ed, tab_idx);
    for (i = 0U; i < ed->tabs.v.len; i++) {
        const Tab *o = &ed->tabs.v.data[i];

        if (o->group_id == gid && o->group_ordinal > max)
            max = o->group_ordinal;
    }
    t->group_id = gid;
    t->group_ordinal = max + 1U;
}

void sag_group_remove_member(Ed *ed, int tab_idx)
{
    Tab *t = sag_tab_at(ed, tab_idx);
    u32 gid;
    u32 vacated;
    size_t i;

    if (t == NULL || t->group_id == 0U)
        return;
    gid = t->group_id;
    vacated = t->group_ordinal;
    t->group_id = 0U;
    t->group_ordinal = 0U;
    for (i = 0U; i < ed->tabs.v.len; i++) {
        Tab *o = &ed->tabs.v.data[i];

        if (o->group_id == gid && o->group_ordinal > vacated)
            o->group_ordinal--;
    }
    /*
     * Auto-dissolve.  An empty group is a row-1 entry that resolves to
     * nothing: clicking it enters a group with no members and the walk
     * steps into a hole.  DoD 10 asserts one cannot exist after any op
     * sequence, and this is the only place that guarantee comes from.
     */
    if (sag_group_member_count(ed, gid) == 0)
        sag_group_dissolve(ed, gid);
}

void sag_group_dissolve(Ed *ed, u32 gid)
{
    int at;
    size_t i;

    if (ed == NULL || gid == 0U)
        return;
    at = sag_group_find(ed, gid);
    if (at < 0)
        return;
    /*
     * Stragglers become ungrouped rather than orphaned.  A tab still
     * naming a freed id answers "yes, grouped" to every question and
     * resolves to nothing when asked which group — so the row-1
     * renderer skips it as a member and row 2 never lists it, and the
     * file becomes unreachable by any navigation.
     */
    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (ed->tabs.v.data[i].group_id != gid)
            continue;
        ed->tabs.v.data[i].group_id = 0U;
        ed->tabs.v.data[i].group_ordinal = 0U;
    }
    group_dispose(&ed->groups.v.data[at]);
    (void)memmove(&ed->groups.v.data[at], &ed->groups.v.data[at + 1],
                  (ed->groups.v.len - (size_t)at - 1U) * sizeof(TabGroup));
    ed->groups.v.len--;
}

void sag_group_label(const Ed *ed, u32 gid, char *buf, size_t n)
{
    int at;

    if (buf == NULL || n == 0U)
        return;
    buf[0] = '\0';
    at = sag_group_find(ed, gid);
    if (at < 0)
        return;
    /* The count is computed HERE, at the moment of display, so a label
     * cannot survive the close that made it wrong. */
    (void)snprintf(buf, n, "%s (%d)", ed->groups.v.data[at].label,
                   sag_group_member_count(ed, gid));
}

u32 sag_active_group_id(const Ed *ed)
{
    if (ed == NULL || ed->tabs.active < 0 ||
        (size_t)ed->tabs.active >= ed->tabs.v.len)
        return 0U;
    return ed->tabs.v.data[ed->tabs.active].group_id;
}

void sag_group_prune_empty(Ed *ed)
{
    size_t i = 0U;

    if (ed == NULL)
        return;
    while (i < ed->groups.v.len) {
        u32 gid = ed->groups.v.data[i].id;

        if (sag_group_member_count(ed, gid) == 0) {
            /* The vec compacts under `i`, so do not advance. */
            sag_group_dissolve(ed, gid);
            continue;
        }
        i++;
    }
}

void sag_group_set_ordinal(Ed *ed, int tab_idx, int pos)
{
    int members[SAG_TAB_MAX];
    int final_order[SAG_TAB_MAX];
    Tab *t = sag_tab_at(ed, tab_idx);
    int n;
    int at = -1;
    int i;
    int k = 0;

    if (t == NULL || t->group_id == 0U)
        return;
    n = sag_group_members(ed, t->group_id, members,
                          (int)SAG_ARRAY_LEN(members));
    for (i = 0; i < n; i++) {
        if (members[i] == tab_idx)
            at = i;
    }
    if (at < 0)
        return;
    if (pos < 1)
        pos = 1;
    if (pos > n)
        pos = n;
    /*
     * Build the FINAL list, then renumber 1..n.
     *
     * Removing the member BEFORE choosing the insertion point is what
     * makes `pos` mean the same thing in both directions.  Leaving it
     * in place while inserting counts its own vacated slot, so every
     * rightward move lands one short of where the user pointed.
     */
    for (i = 0; i < n; i++) {
        if (i == at)
            continue;
        final_order[k++] = members[i];
    }
    for (i = k; i > pos - 1; i--)
        final_order[i] = final_order[i - 1];
    final_order[pos - 1] = tab_idx;
    k++;
    for (i = 0; i < k; i++)
        ed->tabs.v.data[final_order[i]].group_ordinal = (u32)(i + 1);
}

void sag_group_reorder_block(Ed *ed, u32 gid, int to_idx)
{
    u32 order[SAG_TAB_MAX]; /* tab_IDS, in the final array order */
    u32 others[SAG_TAB_MAX];
    int members[SAG_TAB_MAX];
    int nm;
    int no = 0;
    int n;
    int i;
    int k = 0;
    u32 active_id;

    if (ed == NULL || gid == 0U)
        return;
    n = (int)ed->tabs.v.len;
    nm = sag_group_members(ed, gid, members, (int)SAG_ARRAY_LEN(members));
    if (nm == 0)
        return;
    for (i = 0; i < n; i++) {
        if (ed->tabs.v.data[i].group_id != gid)
            others[no++] = ed->tabs.v.data[i].tab_id;
    }
    if (to_idx < 0)
        to_idx = 0;
    if (to_idx > no)
        to_idx = no;
    /*
     * The ENTIRE destination order, computed first, as ids.
     *
     * Moving members one at a time drags already-placed ones back down
     * and interleaves the group with the tabs it passed; recording
     * indices instead of ids goes stale the moment the first move
     * shifts the array under them.
     */
    for (i = 0; i < to_idx; i++)
        order[k++] = others[i];
    for (i = 0; i < nm; i++)
        order[k++] = ed->tabs.v.data[members[i]].tab_id;
    for (i = to_idx; i < no; i++)
        order[k++] = others[i];

    active_id = ed->tabs.active >= 0 && ed->tabs.active < n
                    ? ed->tabs.v.data[ed->tabs.active].tab_id
                    : 0U;
    for (i = 0; i < k; i++) {
        int q = sag_tab_index_of_id(ed, order[i]);
        Tab tmp;

        if (q < 0 || q == i)
            continue;
        tmp = ed->tabs.v.data[i];
        ed->tabs.v.data[i] = ed->tabs.v.data[q];
        ed->tabs.v.data[q] = tmp;
    }
    /* Active is a POSITION and the positions all just changed; it is
     * re-resolved from the id it named before the move. */
    if (active_id != 0U)
        ed->tabs.active = sag_tab_index_of_id(ed, active_id);
}

void sag_group_note_position(Ed *ed)
{
    u32 gid = sag_active_group_id(ed);
    TabGroup *g;
    const Tab *t;

    if (gid == 0U)
        return;
    g = sag_group_at(ed, gid);
    t = sag_tab_at(ed, ed->tabs.active);
    if (g == NULL || t == NULL || t->path == NULL)
        return;
    free(g->last_active_member);
    g->last_active_member = dup_str(t->path);
}
