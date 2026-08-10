#ifndef SAG_EDIT_BATCH_H
#define SAG_EDIT_BATCH_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef struct Ed Ed;
typedef struct CmdCtx CmdCtx;

typedef struct BatchOpts {
    const char *script;
    const char *const *files;
    size_t nfiles;
    const char *const *args;
    size_t nargs;
    const char *config_path;
    bool clean;
    bool no_workspace_config;
    bool trust_workspace;
    bool test;
    bool quiet;
} BatchOpts;

int sag_batch_run(const BatchOpts *opts);

/* The predicate exists so the seeded unit regression can name the broken
 * invariant.  The bootstrap always calls the fatal wrapper. */
bool sag_batch_selfcheck_ok(const Ed *ed, const char **why);
void sag_batch_selfcheck(Ed *ed);

/* NULL only for a command that is batch-safe. */
const char *sag_batch_command_alternative(const char *name,
                                          const CmdCtx *ctx);

#endif /* SAG_EDIT_BATCH_H */
