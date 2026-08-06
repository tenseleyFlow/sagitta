/*
 * Sprint 25 §1.  See workspace.h for why the `path` record exists.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "util/log.h"
#include "util/xdg.h"

u64 sag_fnv1a64(const u8 *bytes, size_t len)
{
    /*
     * The published FNV-1a 64 constants, in HEX.
     *
     * Decimal invites exactly the typo it got the first time — a
     * dropped digit in the 20-digit offset basis, which still hashed,
     * still looked random, and would have keyed every workspace to a
     * directory no other build could find.  The hex forms are
     * checkable against the spec at a glance.
     */
    u64 h = 0xcbf29ce484222325ULL;
    size_t i;

    if (bytes == NULL)
        return h;
    for (i = 0U; i < len; i++) {
        h ^= (u64)bytes[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
 * Scratch for the path accessors.  Returned pointers are valid until
 * the next call for the same key, which is how every other path
 * accessor in the tree behaves; callers copy when they need to keep it.
 */
static char path_scratch[4][PATH_MAX];

/*
 * Concatenation that FAILS rather than truncates.
 *
 * A truncated path is not a shorter path — it is a different path, and
 * writing state to it means writing into a directory nobody chose.  So
 * every join here reports overflow and the caller degrades to
 * stateless, which loses a cache instead of someone's files.
 */
static bool path_cat(char *out, size_t cap, const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);

    if (na + nb + 1U > cap) {
        out[0] = '\0';
        return false;
    }
    (void)memcpy(out, a, na);
    (void)memcpy(out + na, b, nb);
    out[na + nb] = '\0';
    return true;
}

static const char *join_state(const WsKey *k, int slot, const char *tail)
{
    if (k == NULL || k->dir[0] == '\0' ||
        !path_cat(path_scratch[slot], sizeof(path_scratch[slot]), k->dir,
                  tail))
        path_scratch[slot][0] = '\0';
    return path_scratch[slot];
}

const char *sag_ws_state_path(const WsKey *k)
{
    return join_state(k, 0, "state.fl");
}

const char *sag_ws_undo_dir(const WsKey *k)
{
    return join_state(k, 1, "undo/");
}

const char *sag_ws_lock_path(const WsKey *k)
{
    return join_state(k, 2, "lock");
}

/*
 * Reads the `path` record.  Returns:
 *   1  present and it names `want`
 *   0  present and it names something else  (a collision)
 *  -1  absent                               (this probe is free)
 */
static int probe_matches(const char *dir, const char *want)
{
    char record[PATH_MAX];
    char line[PATH_MAX];
    FILE *f;
    size_t n;

    if (!path_cat(record, sizeof(record), dir, "path"))
        return -1;
    f = fopen(record, "r");
    if (f == NULL)
        return -1;
    if (fgets(line, sizeof(line), f) == NULL) {
        (void)fclose(f);
        /* An empty record is a half-written directory from a crash;
         * treat it as free rather than as a collision we can never
         * resolve. */
        return -1;
    }
    (void)fclose(f);
    n = strlen(line);
    while (n > 0U && (line[n - 1U] == '\n' || line[n - 1U] == '\r'))
        line[--n] = '\0';
    return strcmp(line, want) == 0 ? 1 : 0;
}

bool sag_ws_key(WsKey *k, const char *dir)
{
    char *resolved;
    char *state_root;
    u32 probe;

    if (k == NULL)
        return false;
    (void)memset(k, 0, sizeof(*k));
    k->stateless = true;
    if (dir == NULL || dir[0] == '\0')
        return false;

    resolved = realpath(dir, NULL);
    if (resolved == NULL) {
        /* A workspace that does not resolve is not an error here — the
         * caller has already established it is a directory — but there
         * is nothing stable to key on, so we keep no state. */
        sag_log(SAG_LOG_WARN, "workspace %s does not resolve; no state",
                dir);
        return false;
    }
    if (strlen(resolved) >= sizeof(k->realpath)) {
        sag_log(SAG_LOG_WARN, "workspace path too long; no state");
        free(resolved);
        return false;
    }
    (void)snprintf(k->realpath, sizeof(k->realpath), "%s", resolved);
    free(resolved);

    /* Hashed over the BYTES.  Paths are bytes, not text: hashing
     * codepoints would need a decoder that could fail, on input that is
     * allowed to be invalid UTF-8. */
    k->hash = sag_fnv1a64((const u8 *)k->realpath, strlen(k->realpath));

    /* Already ends in `/sagitta` — appending it again produced
     * `.../sagitta/sagitta/workspaces/` and a state tree nothing else
     * could find. */
    state_root = sag_xdg_state_dir();
    if (state_root == NULL) {
        sag_log(SAG_LOG_WARN,
                "no usable state directory; this session keeps no state");
        return false;
    }

    /*
     * The probe walk.  Probe 0 is the bare hash; 1..15 append `-N`.
     * The first probe whose `path` record matches ours, or which has no
     * record at all, is ours.
     */
    for (probe = 0U; probe <= (u32)SAG_WS_PROBE_MAX; probe++) {
        char candidate[PATH_MAX];
        char tail[64];
        int match;

        if (probe == 0U)
            (void)snprintf(tail, sizeof(tail), "/workspaces/%016lx/",
                           (unsigned long)k->hash);
        else
            (void)snprintf(tail, sizeof(tail), "/workspaces/%016lx-%u/",
                           (unsigned long)k->hash, (unsigned)probe);
        if (!path_cat(candidate, sizeof(candidate), state_root, tail)) {
            sag_log(SAG_LOG_WARN, "state path too long; no state");
            free(state_root);
            return false;
        }
        match = probe_matches(candidate, k->realpath);
        if (match == 0)
            continue; /* someone else's directory */
        (void)snprintf(k->dir, sizeof(k->dir), "%s", candidate);
        k->probe = (u8)probe;
        k->stateless = false;
        free(state_root);
        return true;
    }
    /*
     * Sixteen colliding directories is not a situation to guess at.
     * Running stateless loses a cache; writing into a directory that
     * belongs to a different workspace loses that workspace's layout.
     */
    sag_log(SAG_LOG_WARN,
            "workspace key %016lx collided past %d probes; no state",
            (unsigned long)k->hash, SAG_WS_PROBE_MAX);
    free(state_root);
    return false;
}

bool sag_ws_ensure_dir(WsKey *k)
{
    char record[PATH_MAX];
    char line[PATH_MAX];
    int n;

    if (k == NULL || k->stateless || k->dir[0] == '\0')
        return false;
    if (!sag_mkdirs(k->dir, 0700U)) {
        sag_log(SAG_LOG_WARN, "cannot create %s; this session keeps no state",
                k->dir);
        k->stateless = true;
        return false;
    }
    if (!path_cat(record, sizeof(record), k->dir, "path"))
        return false;
    /*
     * Written atomically like everything else: a torn `path` record
     * would read as a collision on the next start and quietly move the
     * whole workspace to probe 1, orphaning its state.
     */
    if (!path_cat(line, sizeof(line), k->realpath, "\n"))
        return false;
    n = (int)strlen(line);
    if (sag_file_write_atomic(record, (const u8 *)line, (size_t)n,
                              0600) != SAG_SAVE_OK) {
        sag_log(SAG_LOG_WARN, "cannot write %s", record);
        return false;
    }
    return true;
}
