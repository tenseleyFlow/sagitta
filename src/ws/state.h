#ifndef YEW_WS_STATE_H
#define YEW_WS_STATE_H

/*
 * Sprint 25 §3: the v1 workspace-state schema — FROZEN.
 *
 * Two rules carry the whole document, both facsimile scars:
 *
 * GROUPS ARE WRITTEN BEFORE TABS.  The restore parser materializes a
 * tab as it finishes that tab's record, and yew_group_add_member needs
 * a live group at that moment.  Emitting tabs first would need a fixup
 * pass over ids that have already been renumbered — precisely the class
 * of bug the stable tab_id discipline exists to eliminate.  This is a
 * property of the SCHEMA: a v1 document with tabs before groups is
 * malformed.
 *
 * IDS ARE REMAPPED.  The ids in the file belong to the writing session.
 * yew_group_create and yew_tab_open hand out fresh monotonic ids on
 * restore, so the parser builds a file-id -> live-id map and every
 * reference resolves through it.  Reusing the file's number looks free
 * and is not: next_group_id would have to be reconstructed by a max
 * scan, a plugin-created group could collide within the same session,
 * and any future workspace merge becomes impossible.  The map costs
 * 8 bytes per entry.
 */

#include "edit/loop.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/workspace.h"

typedef struct Ed Ed;

typedef struct WsBoolOption {
    char *key;
    u32 key_len;
    bool value;
} WsBoolOption;

enum {
    YEW_STATE_VERSION = 1,
    /* §5: the debounce.  A state cache that is 2 s stale costs nothing;
     * one written on every keystroke costs an fsync per keystroke. */
    YEW_STATE_SAVE_DEBOUNCE_MS = 2000,
    /* §3's whole-document caps, enforced by the schema layer (the
     * parser owns only syntax and structure). */
    YEW_STATE_MAX_TABS = 512,
    YEW_STATE_MAX_FILES = 256,
    YEW_STATE_MAX_JUMPS = 100,
    YEW_STATE_MAX_CURSORS = 1024,
    /* Ratios travel as permille integers because there are no floats
     * in the format; see fllit.h for why. */
    YEW_STATE_RATIO_MIN = 1,
    YEW_STATE_RATIO_MAX = 999,
    /* §3: the pane tree nests at most this deep.  A deeper `panes` map
     * is corruption, not a tree to recurse into — the parser's depth
     * cap is 32 for the whole document, which is not a bound on this. */
    YEW_STATE_MAX_PANE_DEPTH = 8,
    /*
     * §7 retention.  The oldest is deleted, and "oldest" is decided
     * lexicographically — which is WHY the timestamp is %Y%m%dT%H%M%SZ
     * and not anything friendlier to read.
     */
    YEW_STATE_CORRUPT_KEEP = 5
};

/* Emits the whole v1 document for `ed`. */
void yew_state_emit(const Ed *ed, Bytebuf *out);

/* Sprint 36's production parser: real Fletch pure-literal syntax, adapted
 * to the frozen v1 schema mapper.  The hand-written twin is test-only. */
FlLit *yew_fl_parse_fletch(Arena *a, const u8 *src, u64 len,
                           FlParseErr *err);

/*
 * Sprint 25 §5: saving.
 *
 * Everything hangs off one struct on the Ed so the whole feature can be
 * OFF by default.  A zeroed WsState is stateless: `ready` false means
 * every entry point below returns without touching a filesystem, which
 * is what unit tests, --clean and an unusable state home all want.  The
 * driver opts in with yew_state_open.
 */
typedef struct WsState {
    WsKey key;
    /* Key computed and <dir> usable.  False = this session keeps no
     * state, and that is never an error the caller handles. */
    bool ready;
    bool dirty;
    /* We hold <dir>/lock.  A second yew in the same workspace
     * restores normally and never writes: last-writer-wins would
     * silently discard whichever session quit first. */
    bool writer;
    /* The ownership notice is shown exactly ONCE, and at the first
     * change rather than at startup — before the first paint it would
     * be a warning about something the user had not done yet. */
    bool owner_told;
    long owner_pid;
    TimerId timer;
    /* Test hook: completed atomic writes.  Counting is the only way to
     * assert that a burst of ten changes debounces into one write. */
    u64 writes;

    /*
     * §6 step 2: the options map, held VERBATIM.
     *
     * The option model is Sprint 36's.  Until it exists this build must
     * not delete settings it does not understand — an older yew
     * opening a newer session's workspace would silently drop every key
     * it had never heard of, and the next save would make that
     * permanent.  So the parsed subtree is kept in its own arena and
     * re-emitted unchanged.
     */
    Arena doc;
    const FlLit *options;
    bool doc_ready;
    /* Mutable known booleans overlay the immutable retained literal map.
     * Rows stay key-sorted so newly introduced options emit deterministically
     * without disturbing the order or values of unknown retained literals. */
    WsBoolOption *bool_options;
    u32 bool_options_len;
    u32 bool_options_cap;

    /* §6: files that were not on disk at restore.  Counted so exactly
     * ONE summary message is shown, never one per tab. */
    u32 missing_count;
} WsState;

/* Computes the key for `ed`'s workspace root, creates the state dir and
 * claims the lock.  Never fails loudly: a bad state home leaves `ready`
 * false and the editor runs stateless. */
void yew_state_open(Ed *ed);
/* Final unconditional save (if dirty and we are the writer), then
 * dispose.  The clean-quit path; NOT the crash path. */
void yew_state_close(Ed *ed);
/* Cancels the timer and releases our lock.  Idempotent, saves nothing. */
void yew_state_dispose(Ed *ed);

void yew_state_mark_dirty(Ed *ed); /* from state-changing events */
bool yew_state_save(Ed *ed);       /* false = not the writer, or I/O   */

/*
 * Sprint 25 §6/§7: restoring.
 *
 * FRESH     nothing to restore — no state file, or none was readable
 *           and the bad one has been set aside.  Not an error.
 * RESTORED  the document applied.
 * RECOVERED the document was unusable; it was RENAMED ASIDE (never
 *           deleted) and the editor starts fresh.
 *
 * There is no failure return, on purpose.  A corrupt cache is not a
 * failed edit: §7's every row ends with "the editor starts", exit 0,
 * exactly one message, and no prompt before the first paint.
 */
typedef enum {
    YEW_WS_FRESH,
    YEW_WS_RESTORED,
    YEW_WS_RECOVERED
} YewWsResult;

YewWsResult yew_ws_restore(Ed *ed);

/* Applies a document already in memory.  Split out so the tests and the
 * fuzzer can drive the schema layer without a filesystem. */
YewWsResult yew_state_apply(Ed *ed, const u8 *bytes, u64 len);

/*
 * §7: sets `state.fl` aside as state.fl.corrupt-YYYYMMDDTHHMMSSZ and
 * prunes to the newest YEW_STATE_CORRUPT_KEEP.  Exposed for the
 * recovery tests, which assert the retention rather than trusting it.
 */
bool yew_state_set_aside(Ed *ed, char *out, size_t cap);

/*
 * Reads a workspace option out of the retained `options` map.
 *
 * The option MODEL is Sprint 36's; this is the one bridge to it, so
 * that §8's `history.scope` has somewhere real to come from instead of
 * a second parallel store that would have to be reconciled later.  The
 * returned pointer lives in the document arena and dies with
 * yew_state_dispose.
 */
const char *yew_state_option_str(const Ed *ed, const char *key,
                                 const char *dflt);
bool yew_state_option_bool(const Ed *ed, const char *key, bool dflt);
/* Returns true only when the retained effective value changed. */
bool yew_state_option_bool_set(Ed *ed, const char *key, bool value);

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

void yew_idmap_init(IdMapVec *m);
void yew_idmap_free(IdMapVec *m);
void yew_idmap_put(IdMapVec *m, u32 file_id, u32 live_id);
u32 yew_idmap_get(const IdMapVec *m, u32 file_id);

/* permille <-> ratio, the ONE conversion site (s22's single-rounding
 * rule, extended to the format). */
float yew_permille_to_ratio(i64 permille);
i64 yew_ratio_to_permille(float ratio);

/* `goal: -1` in the file means YEW_GCOL_EOL, which is UINT64_MAX
 * internally and does not fit i64 — writing it raw would force every
 * reader into an unsigned special case. */
i64 yew_goal_to_i64(u64 goal);
u64 yew_goal_from_i64(i64 v);

#endif
