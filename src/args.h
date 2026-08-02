#ifndef SAG_ARGS_H
#define SAG_ARGS_H

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"

typedef struct {
    bool version;
    bool help;
    bool help_cmds;
    bool clean;
    const char *batch_script;
    const char **files;
    size_t nfiles;
    bool selftest_bug;
} SagArgs;

/* Return an exit code, or -1 when the driver should proceed. */
int sag_args_parse(SagArgs *out, int argc, char **argv, Bytebuf *err);

#endif
