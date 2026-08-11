#include "args.h"

#include <stdlib.h>
#include <string.h>

static int args_error(Bytebuf *err, const char *fmt, const char *arg)
{
    bytebuf_printf(err, "yew: error: ");
    bytebuf_printf(err, fmt, arg);
    bytebuf_push_u8(err, (u8)'\n');
    return YEW_EXIT_ERR;
}

void yew_args_free(YewArgs *args)
{
    free(args->grants);
    args->grants = NULL;
    args->ngrants = 0U;
}

static int parse_error(YewArgs *out, Bytebuf *err, const char *fmt,
                       const char *arg)
{
    yew_args_free(out);
    return args_error(err, fmt, arg);
}

static bool grant_valid(const char *text, size_t *name_len)
{
    const char *colon = strchr(text, ':');

    if (colon == NULL || colon == text || colon[1] == '\0' ||
        strchr(colon + 1, ':') != NULL)
        return false;
    *name_len = (size_t)(colon - text);
    return true;
}

static int add_grant(YewArgs *out, const char *text, Bytebuf *err)
{
    size_t name_len;

    if (!grant_valid(text, &name_len)) {
        return parse_error(out, err,
                           "invalid --grant value '%s' (expected NAME:CAP)",
                           text);
    }
    out->grants = yew_xreallocarray(out->grants, out->ngrants + 1U,
                                    sizeof(*out->grants));
    out->grants[out->ngrants++] = (YewGrantArg){text, name_len};
    return -1;
}

static int require_value(YewArgs *out, int *index, int argc, char **argv,
                         Bytebuf *err, const char **value)
{
    const char *option = argv[*index];

    if (*index + 1 >= argc)
        return parse_error(out, err, "option '%s' requires an argument",
                           option);
    *value = argv[++*index];
    return -1;
}

int yew_args_parse(YewArgs *out, int argc, char **argv, Bytebuf *err)
{
    int i;
    bool batch_requested = false;

    *out = (YewArgs){0};

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "--") == 0) {
            i++;
            if (batch_requested) {
                if (out->batch_script == NULL)
                    return parse_error(out, err,
                                       "option '%s' requires an argument",
                                       "--batch");
                out->batch_args = (const char **)(argv + i);
                out->nbatch_args = (size_t)(argc - i);
                i = argc;
            }
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            if (batch_requested && out->batch_script == NULL) {
                out->batch_script = arg;
                continue;
            }
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
        } else if (strcmp(arg, "--config") == 0) {
            int rc = require_value(out, &i, argc, argv, err, &value);

            if (rc >= 0)
                return rc;
            out->config_path = value;
        } else if (strcmp(arg, "--theme") == 0) {
            int rc = require_value(out, &i, argc, argv, err, &value);

            if (rc >= 0)
                return rc;
            out->theme = value;
        } else if (strcmp(arg, "--no-workspace-config") == 0) {
            out->no_workspace_config = true;
        } else if (strcmp(arg, "--trust-workspace") == 0) {
            out->trust_workspace = true;
        } else if (strcmp(arg, "--batch") == 0) {
            if (batch_requested)
                return parse_error(out, err, "option '%s' specified twice",
                                   arg);
            batch_requested = true;
        } else if (strcmp(arg, "--test") == 0) {
            out->test = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            out->quiet = true;
        } else if (strcmp(arg, "--grant") == 0) {
            int rc = require_value(out, &i, argc, argv, err, &value);

            if (rc >= 0)
                return rc;
            rc = add_grant(out, value, err);
            if (rc >= 0)
                return rc;
        } else if (strcmp(arg, "--selftest-bug") == 0) {
            out->selftest_bug = true;
        } else if (strcmp(arg, "--replay") == 0) {
            return parse_error(out, err,
                               "option '%s' lands in Sprint 38", arg);
        } else if (strcmp(arg, "--batch-strict") == 0) {
            return parse_error(out, err,
                               "option '%s' lands in Sprint 59", arg);
        } else {
            return parse_error(out, err, "unknown option '%s'", arg);
        }
    }

    if (i < argc && batch_requested) {
        int end = i;

        while (end < argc && strcmp(argv[end], "--") != 0)
            end++;
        out->files = (const char **)(argv + i);
        out->nfiles = (size_t)(end - i);
        if (end < argc) {
            out->batch_args = (const char **)(argv + end + 1);
            out->nbatch_args = (size_t)(argc - end - 1);
        }
    } else if (i < argc) {
        out->files = (const char **)(argv + i);
        out->nfiles = (size_t)(argc - i);
    }

    if (out->version || out->help || out->help_cmds) {
        return YEW_EXIT_OK;
    }
    if (batch_requested && out->batch_script == NULL)
        return parse_error(out, err, "option '%s' requires an argument",
                           "--batch");
    if (out->test && !batch_requested)
        return parse_error(out, err, "option '%s' requires --batch", "--test");
    if (out->quiet && !batch_requested)
        return parse_error(out, err, "option '%s' requires --batch", "--quiet");
    if (out->ngrants != 0U && !batch_requested)
        return parse_error(out, err, "option '%s' requires --batch", "--grant");
    return -1;
}
