#ifndef SAG_UI_GROUPS_H
#define SAG_UI_GROUPS_H

/*
 * Sprint 24 §1/§2: tab groups, ported from the facsimile model whole.
 *
 * THE ONE THING THIS FILE IS ABOUT: a group holds NO MEMBER LIST.
 *
 * Closing a tab compacts the tabs array and renumbers every index above
 * it, so any membership stored on the group side is stale exactly one
 * close later — and a stale membership list does not fail loudly, it
 * quietly names a different file.  So membership lives on the TAB, as
 * `group_id` (0 = ungrouped, and authoritative) plus `group_ordinal`
 * (1-based position within the group).  Counts and member lists are
 * COMPUTED on every ask.  A cached count is a label that can lie.
 *
 * `last_active_member` is a PATH rather than an index for the same
 * reason, with one extra property: it is explicitly non-authoritative.
 * It may dangle — the member it names can be closed while the group
 * lives on — and every reader falls back to the lowest ordinal instead
 * of trusting it.
 */

#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;

typedef struct TabGroup {
    u32 id; /* monotonic per session, never reused; 0 is invalid */
    char *label;    /* display name; heap-owned */
    /*
     * Where the group came from, canonical.  This is a LABEL OF ORIGIN,
     * not a constraint: the picker exists to assemble a group that
     * spans directories, so members may live anywhere.  Nothing may
     * filter membership by this path.
     */
    char *dir_path;
    /*
     * PATH of the member to resume at.  Non-authoritative and allowed
     * to dangle; readers resolve it against the CURRENT members and
     * fall back to the lowest ordinal when it no longer resolves.
     */
    char *last_active_member;
} TabGroup;

VEC_DECL(GroupVec, TabGroup);

typedef struct Groups {
    GroupVec v;
    u32 next_group_id; /* starts at 1 */
} Groups;

void sag_groups_init(Groups *g);
void sag_groups_free(Ed *ed);

/* 0 when refused.  An empty `name` labels the group `basename(dir)/`. */
u32 sag_group_create(Ed *ed, const char *dir_path, const char *name);
/* Stragglers are UNGROUPED, never left pointing at an id that no longer
 * resolves — an orphan reads as "grouped" everywhere it is asked. */
void sag_group_dissolve(Ed *ed, u32 gid);
int sag_group_find(const Ed *ed, u32 gid); /* index into groups; -1 */
TabGroup *sag_group_at(Ed *ed, u32 gid);

/* Both COMPUTED, every time.  See the header comment. */
int sag_group_member_count(const Ed *ed, u32 gid);
/* Tab indices ordered by ordinal.  Ordinals are NOT assumed contiguous
 * — mid-removal they have a hole — so this sorts rather than indexes. */
int sag_group_members(const Ed *ed, u32 gid, int *out, int cap);

/* A tab belongs to exactly one group: an already-grouped tab leaves its
 * old group FIRST, so both groups' ordinals stay consistent. */
void sag_group_add_member(Ed *ed, u32 gid, int tab_idx);
/* Compacts the ordinals above the vacated one, then auto-dissolves the
 * group if that was the last member.  Tab close calls this BEFORE the
 * array compaction, while `tab_idx` still names the right tab. */
void sag_group_remove_member(Ed *ed, int tab_idx);

/* "src/ (4)" — the count is live, never stored. */
void sag_group_label(const Ed *ed, u32 gid, char *buf, size_t n);
/* DERIVED from the active tab, never stored: the active index is
 * assigned raw in several places and a cached copy drifts away from it
 * silently. */
u32 sag_active_group_id(const Ed *ed);
void sag_group_prune_empty(Ed *ed);

/*
 * `pos` is 1-based in the FINAL list the user sees.
 *
 * Pitfall this signature exists to name: "skip the moved member, insert
 * at pos" is off by one whenever the member moves RIGHT, because its
 * vacated slot is still being counted.  Build the final order, then
 * renumber 1..n.
 */
void sag_group_set_ordinal(Ed *ed, int tab_idx, int pos);

/*
 * A group is ONE row-1 entry but several array entries, so moving it
 * moves every member — made contiguous at the destination.
 *
 * Pitfall: compute the entire destination order first AS TAB_IDS, then
 * place position by position.  Moving members one at a time drags
 * already-placed ones back down and interleaves the group with the tabs
 * it passed; recording indices instead of ids goes stale after the
 * first move.
 */
void sag_group_reorder_block(Ed *ed, u32 gid, int to_idx);

/* Records the active tab's path as its group's resume point.  Called
 * when leaving a group, before the switch that changes what "active"
 * means. */
void sag_group_note_position(Ed *ed);

#endif
