#ifndef YEW_ARGS_H
#define YEW_ARGS_H

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"

typedef struct YewGrantArg {
    const char *text;
    size_t name_len;
} YewGrantArg;

typedef struct {
    bool version;
    bool help;
    bool help_cmds;
    bool clean;
    const char *config_path;
    const char *theme;
    bool no_workspace_config;
    bool trust_workspace;
    const char *batch_script;
    const char **files;
    size_t nfiles;
    const char **batch_args;
    size_t nbatch_args;
    bool test;
    bool quiet;
    YewGrantArg *grants;
    size_t ngrants;
    bool selftest_bug;
} YewArgs;

/* Return an exit code, or -1 when the driver should proceed. */
int yew_args_parse(YewArgs *out, int argc, char **argv, Bytebuf *err);
void yew_args_free(YewArgs *args);

#endif
