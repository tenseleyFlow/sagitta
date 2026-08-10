#include "harness.h"

#include "args.h"

#include <string.h>

static bool buf_equals(const Bytebuf *buf, const char *expected)
{
    size_t len = strlen(expected);

    return buf->len == len && memcmp(buf->data, expected, len) == 0;
}

static bool buf_contains(const Bytebuf *buf, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (n > buf->len)
        return false;
    for (i = 0U; i + n <= buf->len; i++) {
        if (memcmp(buf->data + i, needle, n) == 0)
            return true;
    }
    return false;
}

void test_args_parse_version(void)
{
    char *argv[] = {"sagitta", "--version"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 2, argv, &err);
    SAG_ASSERT_EQ_I64(rc, SAG_EXIT_OK);
    SAG_ASSERT(args.version);
    SAG_ASSERT(!args.help);
    SAG_ASSERT(!args.help_cmds);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_help_cmds(void)
{
    char *argv[] = {"sagitta", "--help-cmds"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 2, argv, &err);
    SAG_ASSERT_EQ_I64(rc, SAG_EXIT_OK);
    SAG_ASSERT(args.help_cmds);
    SAG_ASSERT(!args.help);
    SAG_ASSERT(!args.version);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_help(void)
{
    char *argv[] = {"sagitta", "--help", "file.txt"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 3, argv, &err);
    SAG_ASSERT_EQ_I64(rc, SAG_EXIT_OK);
    SAG_ASSERT(args.help);
    SAG_ASSERT_EQ_U64(args.nfiles, 1U);
    SAG_ASSERT_EQ_STR(args.files[0], "file.txt");
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_unknown(void)
{
    char *argv[] = {"sagitta", "--no-such-flag"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 2, argv, &err);
    SAG_ASSERT_EQ_I64(rc, SAG_EXIT_ERR);
    SAG_ASSERT(buf_equals(&err,
        "sagitta: error: unknown option '--no-such-flag'\n"));
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_file(void)
{
    char *argv[] = {"sagitta", "one.txt", "two.txt"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 3, argv, &err);
    SAG_ASSERT_EQ_I64(rc, -1);
    SAG_ASSERT_EQ_U64(args.nfiles, 2U);
    SAG_ASSERT_EQ_STR(args.files[0], "one.txt");
    SAG_ASSERT_EQ_STR(args.files[1], "two.txt");
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch(void)
{
    char *argv[] = {"sagitta", "--batch", "edit.fl", "one.txt"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 4, argv, &err);
    SAG_ASSERT_EQ_I64(rc, -1);
    SAG_ASSERT_EQ_STR(args.batch_script, "edit.fl");
    SAG_ASSERT_EQ_U64(args.nfiles, 1U);
    SAG_ASSERT_EQ_STR(args.files[0], "one.txt");
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_missing(void)
{
    char *argv[] = {"sagitta", "--batch"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 2, argv, &err);
    SAG_ASSERT_EQ_I64(rc, SAG_EXIT_ERR);
    SAG_ASSERT(buf_equals(&err,
        "sagitta: error: option '--batch' requires an argument\n"));
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_end_options(void)
{
    char *argv[] = {"sagitta", "--clean", "--", "--version"};
    SagArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = sag_args_parse(&args, 4, argv, &err);
    SAG_ASSERT_EQ_I64(rc, -1);
    SAG_ASSERT(args.clean);
    SAG_ASSERT(!args.version);
    SAG_ASSERT_EQ_U64(args.nfiles, 1U);
    SAG_ASSERT_EQ_STR(args.files[0], "--version");
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_config_controls(void)
{
    char *argv[] = {"sagitta", "--config", "mine.fl",
                    "--no-workspace-config", "--trust-workspace"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 5, argv, &err), -1);
    SAG_ASSERT_EQ_STR(args.config_path, "mine.fl");
    SAG_ASSERT(args.no_workspace_config);
    SAG_ASSERT(args.trust_workspace);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_config_missing(void)
{
    char *argv[] = {"sagitta", "--config"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 2, argv, &err), SAG_EXIT_ERR);
    SAG_ASSERT(buf_equals(&err,
        "sagitta: error: option '--config' requires an argument\n"));
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_full_model(void)
{
    char *argv[] = {
        "sagitta", "--clean", "--config", "user.fl",
        "--no-workspace-config", "--trust-workspace", "--quiet", "--test",
        "--batch", "tools/fmt.fl", "one.c", "-", "two.c", "--",
        "--write", "value with spaces", ""
    };
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, (int)SAG_ARRAY_LEN(argv), argv,
                                     &err), -1);
    SAG_ASSERT(args.clean);
    SAG_ASSERT_EQ_STR(args.config_path, "user.fl");
    SAG_ASSERT(args.no_workspace_config);
    SAG_ASSERT(args.trust_workspace);
    SAG_ASSERT(args.quiet);
    SAG_ASSERT(args.test);
    SAG_ASSERT_EQ_STR(args.batch_script, "tools/fmt.fl");
    SAG_ASSERT_EQ_U64(args.nfiles, 3U);
    SAG_ASSERT_EQ_STR(args.files[0], "one.c");
    SAG_ASSERT_EQ_STR(args.files[1], "-");
    SAG_ASSERT_EQ_STR(args.files[2], "two.c");
    SAG_ASSERT_EQ_U64(args.nbatch_args, 3U);
    SAG_ASSERT_EQ_STR(args.batch_args[0], "--write");
    SAG_ASSERT_EQ_STR(args.batch_args[1], "value with spaces");
    SAG_ASSERT_EQ_STR(args.batch_args[2], "");
    SAG_ASSERT_EQ_U64(args.ngrants, 0U);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_runner_order(void)
{
    char *argv[] = {"sagitta", "--batch", "--test", "--clean", "--quiet",
                    "case.fl"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 6, argv, &err), -1);
    SAG_ASSERT_EQ_STR(args.batch_script, "case.fl");
    SAG_ASSERT(args.test);
    SAG_ASSERT(args.clean);
    SAG_ASSERT(args.quiet);
    SAG_ASSERT_EQ_U64(args.nfiles, 0U);
    SAG_ASSERT_NULL(args.files);
    SAG_ASSERT_EQ_U64(args.nbatch_args, 0U);
    SAG_ASSERT_NULL(args.batch_args);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_empty_arg_tail(void)
{
    char *argv[] = {"sagitta", "--batch", "case.fl", "--"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 4, argv, &err), -1);
    SAG_ASSERT_EQ_STR(args.batch_script, "case.fl");
    SAG_ASSERT_EQ_U64(args.nfiles, 0U);
    SAG_ASSERT_EQ_U64(args.nbatch_args, 0U);
    SAG_ASSERT_NOT_NULL(args.batch_args);
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_grants(void)
{
    char *argv[] = {"sagitta", "--grant", "fmt:fs.read", "--batch",
                    "--grant", "lint:shell", "run.fl"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 7, argv, &err), -1);
    SAG_ASSERT_EQ_STR(args.batch_script, "run.fl");
    SAG_ASSERT_EQ_U64(args.ngrants, 2U);
    SAG_ASSERT_EQ_STR(args.grants[0].text, "fmt:fs.read");
    SAG_ASSERT_EQ_U64(args.grants[0].name_len, 3U);
    SAG_ASSERT_EQ_STR(args.grants[0].text + args.grants[0].name_len + 1U,
                      "fs.read");
    SAG_ASSERT_EQ_STR(args.grants[1].text, "lint:shell");
    SAG_ASSERT_EQ_U64(args.grants[1].name_len, 4U);
    SAG_ASSERT_EQ_STR(args.grants[1].text + args.grants[1].name_len + 1U,
                      "shell");
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_only_options_require_batch(void)
{
    const char *options[] = {"--test", "--quiet", "--grant"};
    const char *messages[] = {
        "sagitta: error: option '--test' requires --batch\n",
        "sagitta: error: option '--quiet' requires --batch\n",
        "sagitta: error: option '--grant' requires --batch\n"
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(options); i++) {
        char *argv[3] = {"sagitta", (char *)options[i], "plug:net"};
        int argc = strcmp(options[i], "--grant") == 0 ? 3 : 2;
        SagArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        SAG_ASSERT_EQ_I64(sag_args_parse(&args, argc, argv, &err),
                          SAG_EXIT_ERR);
        SAG_ASSERT(buf_equals(&err, messages[i]));
        SAG_ASSERT_NULL(args.grants);
        SAG_ASSERT_EQ_U64(args.ngrants, 0U);
        sag_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_grant_errors(void)
{
    const char *values[] = {"plug", ":fs.read", "plug:", "a:b:c"};
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(values); i++) {
        char *argv[] = {"sagitta", "--batch", "run.fl", "--grant",
                        (char *)values[i]};
        SagArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        SAG_ASSERT_EQ_I64(sag_args_parse(&args, 5, argv, &err),
                          SAG_EXIT_ERR);
        SAG_ASSERT(buf_contains(&err, "invalid --grant value"));
        SAG_ASSERT(buf_contains(&err, "expected NAME:CAP"));
        SAG_ASSERT_NULL(args.grants);
        SAG_ASSERT_EQ_U64(args.ngrants, 0U);
        sag_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_misuse(void)
{
    char *duplicate[] = {"sagitta", "--batch", "--batch", "run.fl"};
    char *missing_grant[] = {"sagitta", "--batch", "run.fl", "--grant"};
    char *missing_script[] = {"sagitta", "--batch", "--test", "--"};
    char **cases[] = {duplicate, missing_grant, missing_script};
    int counts[] = {4, 4, 4};
    const char *messages[] = {
        "sagitta: error: option '--batch' specified twice\n",
        "sagitta: error: option '--grant' requires an argument\n",
        "sagitta: error: option '--batch' requires an argument\n"
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        SagArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        SAG_ASSERT_EQ_I64(sag_args_parse(&args, counts[i], cases[i], &err),
                          SAG_EXIT_ERR);
        SAG_ASSERT(buf_equals(&err, messages[i]));
        SAG_ASSERT_NULL(args.grants);
        SAG_ASSERT_EQ_U64(args.ngrants, 0U);
        sag_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_stops_options_at_first_file(void)
{
    char *argv[] = {"sagitta", "--batch", "run.fl", "one", "--quiet",
                    "--", "tail"};
    SagArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    SAG_ASSERT_EQ_I64(sag_args_parse(&args, 7, argv, &err), -1);
    SAG_ASSERT(!args.quiet);
    SAG_ASSERT_EQ_U64(args.nfiles, 2U);
    SAG_ASSERT_EQ_STR(args.files[0], "one");
    SAG_ASSERT_EQ_STR(args.files[1], "--quiet");
    SAG_ASSERT_EQ_U64(args.nbatch_args, 1U);
    SAG_ASSERT_EQ_STR(args.batch_args[0], "tail");
    SAG_ASSERT_EQ_U64(err.len, 0U);
    sag_args_free(&args);
    bytebuf_free(&err);
}
