#ifndef YEW_WS_WORKSPACE_H
#define YEW_WS_WORKSPACE_H

/*
 * Sprint 25 §1: workspace identity.
 *
 * THE DIRECTORY IS THE WORKSPACE.  There is no project file, no
 * registry, no "open workspace" verb — the process's cwd at startup (or
 * `--workspace`) canonicalizes to a realpath, and that realpath's hash
 * names a directory under $XDG_STATE_HOME.
 *
 * NOTHING THIS SPRINT WRITES LANDS IN THE WORKSPACE ITSELF.  State goes
 * under the state home; the repo stays clean, and a user who never
 * looks at ~/.local/state sees no trace of us.  The one file yew
 * ever reads from a workspace is `.yew.fl`, which is user-authored
 * and belongs to Sprint 36.
 *
 * The `path` record inside each state directory is what keeps the
 * scheme auditable: without it the state tree is a pile of hex names
 * that no human can garbage-collect or file a bug about, and a 64-bit
 * collision would be undetectable rather than merely rare.  With it,
 * one open in the common case is the entire collision mitigation.
 */

#include <limits.h>

#include "util/base.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum {
    /* Past this many colliding directories we stop guessing and run
     * without state rather than write into someone else's. */
    YEW_WS_PROBE_MAX = 15
};

typedef struct WsKey {
    char realpath[PATH_MAX]; /* canonical workspace dir              */
    u64 hash;                /* FNV-1a 64 over the realpath BYTES    */
    u8 probe;                /* collision probe index, 0 = base      */
    char dir[PATH_MAX];      /* $XDG_STATE_HOME/yew/workspaces/… */
    /* True = never read, never write.  --clean and --batch set it
     * before any I/O happens, and so does an unusable state home. */
    bool stateless;
} WsKey;

/*
 * False means "run stateless" — never an error the caller must handle.
 * A missing state home is a reason to keep no state, not a reason to
 * refuse to edit.
 */
bool yew_ws_key(WsKey *k, const char *dir);
const char *yew_ws_state_path(const WsKey *k); /* <dir>/state.fl */
const char *yew_ws_undo_dir(const WsKey *k);   /* <dir>/undo/    */
const char *yew_ws_lock_path(const WsKey *k);  /* <dir>/lock     */

/*
 * FNV-1a 64 over BYTES.  Exposed because the undo sidecar (Sprint 10)
 * keys files the same way, and two implementations of one hash is two
 * chances to disagree about where a file lives.
 *
 * No seed and no randomization: the same directory must produce the
 * same key forever, or every restore after an upgrade silently starts
 * from a fresh workspace (invariant 5).
 */
u64 yew_fnv1a64(const u8 *bytes, size_t len);

/* Creates <dir> (0700, mkdir -p) and writes the `path` record.  False
 * when the directory could not be made — the caller goes stateless. */
bool yew_ws_ensure_dir(WsKey *k);

#endif
