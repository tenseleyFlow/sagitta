#ifndef SAG_WS_STATE_H
#define SAG_WS_STATE_H

/*
 * Sprint 25 §3: the v1 workspace-state schema — FROZEN.
 *
 * Two rules carry the whole document, both facsimile scars:
 *
 * GROUPS ARE WRITTEN BEFORE TABS.  The restore parser materializes a
 * tab as it finishes that tab's record, and sag_group_add_member needs
 * a live group at that moment.  Emitting tabs first would need a fixup
 * pass over ids that have already been renumbered — precisely the class
 * of bug the stable tab_id discipline exists to eliminate.  This is a
 * property of the SCHEMA: a v1 document with tabs before groups is
 * malformed.
 *
 * IDS ARE REMAPPED.  The ids in the file belong to the writing session.
 * sag_group_create and sag_tab_open hand out fresh monotonic ids on
 * restore, so the parser builds a file-id -> live-id map and every
 * reference resolves through it.  Reusing the file's number looks free
 * and is not: next_group_id would have to be reconstructed by a max
 * scan, a plugin-created group could collide within the same session,
 * and any future workspace merge becomes impossible.  The map costs
 * 8 bytes per entry.
 */

#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"

typedef struct Ed Ed;

enum {
    SAG_STATE_VERSION = 1,
    /* §3's whole-document caps, enforced by the schema layer (the
     * parser owns only syntax and structure). */
    SAG_STATE_MAX_TABS = 512,
    SAG_STATE_MAX_FILES = 256,
    SAG_STATE_MAX_JUMPS = 100,
    SAG_STATE_MAX_CURSORS = 1024,
    /* Ratios travel as permille integers because there are no floats
     * in the format; see fllit.h for why. */
    SAG_STATE_RATIO_MIN = 1,
    SAG_STATE_RATIO_MAX = 999
};

/* Emits the whole v1 document for `ed`. */
void sag_state_emit(const Ed *ed, Bytebuf *out);

/*
 * The file-id -> live-id map (§3.1).
 *
 * A missing group record resolves to 0 — UNGROUPED — never to whatever
 * id happens to exist now.  Attaching a tab to an unrelated group
 * because its own record was dropped is silent and wrong; being
 * ungrouped is visible and correct.
 */
typedef struct IdMap {
    u32 file_id;
    u32 live_id;
} IdMap;

typedef struct IdMapVec {
    IdMap *data;
    u32 len, cap;
} IdMapVec;

void sag_idmap_init(IdMapVec *m);
void sag_idmap_free(IdMapVec *m);
void sag_idmap_put(IdMapVec *m, u32 file_id, u32 live_id);
u32 sag_idmap_get(const IdMapVec *m, u32 file_id);

/* permille <-> ratio, the ONE conversion site (s22's single-rounding
 * rule, extended to the format). */
float sag_permille_to_ratio(i64 permille);
i64 sag_ratio_to_permille(float ratio);

/* `goal: -1` in the file means SAG_GCOL_EOL, which is UINT64_MAX
 * internally and does not fit i64 — writing it raw would force every
 * reader into an unsigned special case. */
i64 sag_goal_to_i64(u64 goal);
u64 sag_goal_from_i64(i64 v);

#endif
