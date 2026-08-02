#include "args.h"

#include <string.h>

static int args_error(Bytebuf *err, const char *fmt, const char *arg)
{
    bytebuf_printf(err, "sagitta: error: ");
    bytebuf_printf(err, fmt, arg);
    bytebuf_push_u8(err, (u8)'\n');
    return SAG_EXIT_ERR;
}

int sag_args_parse(SagArgs *out, int argc, char **argv, Bytebuf *err)
{
    int i;

    *out = (SagArgs){0};

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (strcmp(arg, "--version") == 0) {
            out->version = true;
        } else if (strcmp(arg, "--help") == 0) {
            out->help = true;
        } else if (strcmp(arg, "--help-cmds") == 0) {
            out->help_cmds = true;
        } else if (strcmp(arg, "--clean") == 0) {
            out->clean = true;
        } else if (strcmp(arg, "--batch") == 0) {
            if (i + 1 >= argc) {
                return args_error(err, "option '%s' requires an argument", arg);
            }
            i++;
            out->batch_script = argv[i];
        } else if (strcmp(arg, "--selftest-bug") == 0) {
            out->selftest_bug = true;
        } else {
            return args_error(err, "unknown option '%s'", arg);
        }
    }

    if (i < argc) {
        out->files = (const char **)(argv + i);
        out->nfiles = (size_t)(argc - i);
    }

    if (out->version || out->help || out->help_cmds) {
        return SAG_EXIT_OK;
    }
    return -1;
}
