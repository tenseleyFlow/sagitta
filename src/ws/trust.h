#ifndef SAG_WS_TRUST_H
#define SAG_WS_TRUST_H

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

enum { SAG_TRUST_PRUNE_DAYS_DEFAULT = 365 };

typedef enum SagTrustDecision {
    SAG_TRUST_NO_CONFIG,
    SAG_TRUST_GRANTED,
    SAG_TRUST_DENIED,
    SAG_TRUST_PROMPT_NEW,
    SAG_TRUST_PROMPT_CHANGED,
    SAG_TRUST_PROMPT_REPLACED,
    SAG_TRUST_SKIP_NO_TTY,
    SAG_TRUST_ERROR
} SagTrustDecision;

typedef enum SagTrustAnswer {
    SAG_TRUST_ALWAYS,
    SAG_TRUST_ONCE,
    SAG_TRUST_NEVER,
    SAG_TRUST_VIEW,
    SAG_TRUST_SKIP
} SagTrustAnswer;

typedef struct SagTrustProbe {
    char workspace[PATH_MAX];
    char config_path[PATH_MAX];
    Bytebuf bytes;              /* exact bytes whose hash was checked */
    u64 hash;
    dev_t dev;
    ino_t ino;
    bool has_config;
} SagTrustProbe;

typedef struct SagTrustDb {
    void *impl;
} SagTrustDb;

void sag_trust_db_init(SagTrustDb *db);
void sag_trust_db_free(SagTrustDb *db);

/* Explicit paths are the unit-test and embedding seam.  The un-suffixed
 * forms use $XDG_STATE_HOME/sagitta/trust.fl.  A failed load leaves the
 * previous in-memory database untouched. */
bool sag_trust_db_load_path(SagTrustDb *db, const char *path);
bool sag_trust_db_write_path(SagTrustDb *db, const char *path, time_t now,
                             u32 prune_days);
bool sag_trust_db_load(SagTrustDb *db);
bool sag_trust_db_write(SagTrustDb *db, time_t now, u32 prune_days);

void sag_trust_probe_init(SagTrustProbe *probe);
void sag_trust_probe_free(SagTrustProbe *probe);

/* No-TTY is a decision input, not an invitation to read stdin: this module
 * never prompts or spins an event loop.  The caller presents PROMPT_* through
 * the normal cmdline machinery. */
SagTrustDecision sag_trust_check(SagTrustDb *db, const char *workspace,
                                 bool has_tty, bool pregrant,
                                 SagTrustProbe *probe);
const char *sag_trust_decision_reason(SagTrustDecision decision);
bool sag_trust_answer(SagTrustDb *db, const SagTrustProbe *probe,
                      SagTrustAnswer answer, time_t now);

#endif /* SAG_WS_TRUST_H */
