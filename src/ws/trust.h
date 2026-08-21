#ifndef YEW_WS_TRUST_H
#define YEW_WS_TRUST_H

/*
 * Workspace trust is keyed by the workspace directory's realpath, resolved
 * once per probe.  A symlink grant therefore belongs to its current target;
 * repointing the symlink cannot carry that grant to another directory.
 */

#include <limits.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#include "util/base.h"
#include "util/buf.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum { YEW_TRUST_PRUNE_DAYS_DEFAULT = 365 };

typedef enum AiWsGrant {
    YEW_AI_WS_UNSET = 0,
    YEW_AI_WS_ALLOW,
    YEW_AI_WS_DENY
} AiWsGrant;

typedef enum YewTrustDecision {
    YEW_TRUST_NO_CONFIG,
    YEW_TRUST_GRANTED,
    YEW_TRUST_DENIED,
    YEW_TRUST_PROMPT_NEW,
    YEW_TRUST_PROMPT_CHANGED,
    YEW_TRUST_PROMPT_REPLACED,
    YEW_TRUST_SKIP_NO_TTY,
    YEW_TRUST_ERROR
} YewTrustDecision;

typedef enum YewTrustAnswer {
    YEW_TRUST_ALWAYS,
    YEW_TRUST_ONCE,
    YEW_TRUST_NEVER,
    YEW_TRUST_VIEW,
    YEW_TRUST_SKIP
} YewTrustAnswer;

typedef struct YewTrustProbe {
    char workspace[PATH_MAX];
    char config_path[PATH_MAX];
    Bytebuf bytes;              /* exact bytes whose hash was checked */
    u64 hash;
    dev_t dev;
    ino_t ino;
    bool has_config;
} YewTrustProbe;

typedef struct YewTrustDb {
    void *impl;
} YewTrustDb;

void yew_trust_db_init(YewTrustDb *db);
void yew_trust_db_free(YewTrustDb *db);

/* Explicit paths are the unit-test and embedding seam.  The un-suffixed
 * forms use $XDG_STATE_HOME/yew/trust.fl.  A failed load leaves the
 * previous in-memory database untouched. */
bool yew_trust_db_load_path(YewTrustDb *db, const char *path);
bool yew_trust_db_write_path(YewTrustDb *db, const char *path, time_t now,
                             u32 prune_days);
bool yew_trust_db_load(YewTrustDb *db);
bool yew_trust_db_write(YewTrustDb *db, time_t now, u32 prune_days);

/* AI disclosure grants are independent of workspace-config trust.  These
 * DB-level seams canonicalize workspace through realpath and fingerprint the
 * directory itself; Ed-facing policy wrappers live in the AI module.  A
 * replacement at the same path invalidates both grants. */
AiWsGrant yew_trust_ai_grant(YewTrustDb *db, const char *workspace);
bool yew_trust_ai_set(YewTrustDb *db, const char *workspace,
                      AiWsGrant grant, time_t now);
bool yew_trust_ai_forget(YewTrustDb *db, const char *workspace);

void yew_trust_probe_init(YewTrustProbe *probe);
void yew_trust_probe_free(YewTrustProbe *probe);

/* No-TTY is a decision input, not an invitation to read stdin: this module
 * never prompts or spins an event loop.  The caller presents PROMPT_* through
 * the normal cmdline machinery. */
YewTrustDecision yew_trust_check(YewTrustDb *db, const char *workspace,
                                 bool has_tty, bool pregrant,
                                 YewTrustProbe *probe);
const char *yew_trust_decision_reason(YewTrustDecision decision);
bool yew_trust_answer(YewTrustDb *db, const YewTrustProbe *probe,
                      YewTrustAnswer answer, time_t now);

#endif /* YEW_WS_TRUST_H */
