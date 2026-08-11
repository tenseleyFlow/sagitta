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
    char *argv[] = {"yew", "--version"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 2, argv, &err);
    YEW_ASSERT_EQ_I64(rc, YEW_EXIT_OK);
    YEW_ASSERT(args.version);
    YEW_ASSERT(!args.help);
    YEW_ASSERT(!args.help_cmds);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_help_cmds(void)
{
    char *argv[] = {"yew", "--help-cmds"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 2, argv, &err);
    YEW_ASSERT_EQ_I64(rc, YEW_EXIT_OK);
    YEW_ASSERT(args.help_cmds);
    YEW_ASSERT(!args.help);
    YEW_ASSERT(!args.version);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_help(void)
{
    char *argv[] = {"yew", "--help", "file.txt"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 3, argv, &err);
    YEW_ASSERT_EQ_I64(rc, YEW_EXIT_OK);
    YEW_ASSERT(args.help);
    YEW_ASSERT_EQ_U64(args.nfiles, 1U);
    YEW_ASSERT_EQ_STR(args.files[0], "file.txt");
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_unknown(void)
{
    char *argv[] = {"yew", "--no-such-flag"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 2, argv, &err);
    YEW_ASSERT_EQ_I64(rc, YEW_EXIT_ERR);
    YEW_ASSERT(buf_equals(&err,
        "yew: error: unknown option '--no-such-flag'\n"));
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_file(void)
{
    char *argv[] = {"yew", "one.txt", "two.txt"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 3, argv, &err);
    YEW_ASSERT_EQ_I64(rc, -1);
    YEW_ASSERT_EQ_U64(args.nfiles, 2U);
    YEW_ASSERT_EQ_STR(args.files[0], "one.txt");
    YEW_ASSERT_EQ_STR(args.files[1], "two.txt");
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch(void)
{
    char *argv[] = {"yew", "--batch", "edit.fl", "one.txt"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 4, argv, &err);
    YEW_ASSERT_EQ_I64(rc, -1);
    YEW_ASSERT_EQ_STR(args.batch_script, "edit.fl");
    YEW_ASSERT_EQ_U64(args.nfiles, 1U);
    YEW_ASSERT_EQ_STR(args.files[0], "one.txt");
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_missing(void)
{
    char *argv[] = {"yew", "--batch"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 2, argv, &err);
    YEW_ASSERT_EQ_I64(rc, YEW_EXIT_ERR);
    YEW_ASSERT(buf_equals(&err,
        "yew: error: option '--batch' requires an argument\n"));
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_end_options(void)
{
    char *argv[] = {"yew", "--clean", "--", "--version"};
    YewArgs args;
    Bytebuf err;
    int rc;

    bytebuf_init(&err);
    rc = yew_args_parse(&args, 4, argv, &err);
    YEW_ASSERT_EQ_I64(rc, -1);
    YEW_ASSERT(args.clean);
    YEW_ASSERT(!args.version);
    YEW_ASSERT_EQ_U64(args.nfiles, 1U);
    YEW_ASSERT_EQ_STR(args.files[0], "--version");
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_config_controls(void)
{
    char *argv[] = {"yew", "--config", "mine.fl",
                    "--theme", "quiver-light",
                    "--no-workspace-config", "--trust-workspace"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 7, argv, &err), -1);
    YEW_ASSERT_EQ_STR(args.config_path, "mine.fl");
    YEW_ASSERT_EQ_STR(args.theme, "quiver-light");
    YEW_ASSERT(args.no_workspace_config);
    YEW_ASSERT(args.trust_workspace);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_config_missing(void)
{
    char *argv[] = {"yew", "--config"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 2, argv, &err), YEW_EXIT_ERR);
    YEW_ASSERT(buf_equals(&err,
        "yew: error: option '--config' requires an argument\n"));
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_full_model(void)
{
    char *argv[] = {
        "yew", "--clean", "--config", "user.fl",
        "--no-workspace-config", "--trust-workspace", "--quiet", "--test",
        "--batch", "tools/fmt.fl", "one.c", "-", "two.c", "--",
        "--write", "value with spaces", ""
    };
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, (int)YEW_ARRAY_LEN(argv), argv,
                                     &err), -1);
    YEW_ASSERT(args.clean);
    YEW_ASSERT_EQ_STR(args.config_path, "user.fl");
    YEW_ASSERT(args.no_workspace_config);
    YEW_ASSERT(args.trust_workspace);
    YEW_ASSERT(args.quiet);
    YEW_ASSERT(args.test);
    YEW_ASSERT_EQ_STR(args.batch_script, "tools/fmt.fl");
    YEW_ASSERT_EQ_U64(args.nfiles, 3U);
    YEW_ASSERT_EQ_STR(args.files[0], "one.c");
    YEW_ASSERT_EQ_STR(args.files[1], "-");
    YEW_ASSERT_EQ_STR(args.files[2], "two.c");
    YEW_ASSERT_EQ_U64(args.nbatch_args, 3U);
    YEW_ASSERT_EQ_STR(args.batch_args[0], "--write");
    YEW_ASSERT_EQ_STR(args.batch_args[1], "value with spaces");
    YEW_ASSERT_EQ_STR(args.batch_args[2], "");
    YEW_ASSERT_EQ_U64(args.ngrants, 0U);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_runner_order(void)
{
    char *argv[] = {"yew", "--batch", "--test", "--clean", "--quiet",
                    "case.fl"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 6, argv, &err), -1);
    YEW_ASSERT_EQ_STR(args.batch_script, "case.fl");
    YEW_ASSERT(args.test);
    YEW_ASSERT(args.clean);
    YEW_ASSERT(args.quiet);
    YEW_ASSERT_EQ_U64(args.nfiles, 0U);
    YEW_ASSERT_NULL(args.files);
    YEW_ASSERT_EQ_U64(args.nbatch_args, 0U);
    YEW_ASSERT_NULL(args.batch_args);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_empty_arg_tail(void)
{
    char *argv[] = {"yew", "--batch", "case.fl", "--"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 4, argv, &err), -1);
    YEW_ASSERT_EQ_STR(args.batch_script, "case.fl");
    YEW_ASSERT_EQ_U64(args.nfiles, 0U);
    YEW_ASSERT_EQ_U64(args.nbatch_args, 0U);
    YEW_ASSERT_NOT_NULL(args.batch_args);
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_grants(void)
{
    char *argv[] = {"yew", "--grant", "fmt:fs.read", "--batch",
                    "--grant", "lint:shell", "run.fl"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 7, argv, &err), -1);
    YEW_ASSERT_EQ_STR(args.batch_script, "run.fl");
    YEW_ASSERT_EQ_U64(args.ngrants, 2U);
    YEW_ASSERT_EQ_STR(args.grants[0].text, "fmt:fs.read");
    YEW_ASSERT_EQ_U64(args.grants[0].name_len, 3U);
    YEW_ASSERT_EQ_STR(args.grants[0].text + args.grants[0].name_len + 1U,
                      "fs.read");
    YEW_ASSERT_EQ_STR(args.grants[1].text, "lint:shell");
    YEW_ASSERT_EQ_U64(args.grants[1].name_len, 4U);
    YEW_ASSERT_EQ_STR(args.grants[1].text + args.grants[1].name_len + 1U,
                      "shell");
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}

void test_args_parse_batch_only_options_require_batch(void)
{
    const char *options[] = {"--test", "--quiet", "--grant"};
    const char *messages[] = {
        "yew: error: option '--test' requires --batch\n",
        "yew: error: option '--quiet' requires --batch\n",
        "yew: error: option '--grant' requires --batch\n"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(options); i++) {
        char *argv[3] = {"yew", (char *)options[i], "plug:net"};
        int argc = strcmp(options[i], "--grant") == 0 ? 3 : 2;
        YewArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        YEW_ASSERT_EQ_I64(yew_args_parse(&args, argc, argv, &err),
                          YEW_EXIT_ERR);
        YEW_ASSERT(buf_equals(&err, messages[i]));
        YEW_ASSERT_NULL(args.grants);
        YEW_ASSERT_EQ_U64(args.ngrants, 0U);
        yew_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_grant_errors(void)
{
    const char *values[] = {"plug", ":fs.read", "plug:", "a:b:c"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(values); i++) {
        char *argv[] = {"yew", "--batch", "run.fl", "--grant",
                        (char *)values[i]};
        YewArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        YEW_ASSERT_EQ_I64(yew_args_parse(&args, 5, argv, &err),
                          YEW_EXIT_ERR);
        YEW_ASSERT(buf_contains(&err, "invalid --grant value"));
        YEW_ASSERT(buf_contains(&err, "expected NAME:CAP"));
        YEW_ASSERT_NULL(args.grants);
        YEW_ASSERT_EQ_U64(args.ngrants, 0U);
        yew_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_misuse(void)
{
    char *duplicate[] = {"yew", "--batch", "--batch", "run.fl"};
    char *missing_grant[] = {"yew", "--batch", "run.fl", "--grant"};
    char *missing_script[] = {"yew", "--batch", "--test", "--"};
    char *replay[] = {"yew", "--batch", "--replay", "q", "run.fl"};
    char *strict[] = {"yew", "--batch-strict"};
    char **cases[] = {duplicate, missing_grant, missing_script, replay,
                      strict};
    int counts[] = {4, 4, 4, 5, 2};
    const char *messages[] = {
        "yew: error: option '--batch' specified twice\n",
        "yew: error: option '--grant' requires an argument\n",
        "yew: error: option '--batch' requires an argument\n",
        "yew: error: option '--replay' lands in Sprint 38\n",
        "yew: error: option '--batch-strict' lands in Sprint 59\n"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        YewArgs args;
        Bytebuf err;

        bytebuf_init(&err);
        YEW_ASSERT_EQ_I64(yew_args_parse(&args, counts[i], cases[i], &err),
                          YEW_EXIT_ERR);
        YEW_ASSERT(buf_equals(&err, messages[i]));
        YEW_ASSERT_NULL(args.grants);
        YEW_ASSERT_EQ_U64(args.ngrants, 0U);
        yew_args_free(&args);
        bytebuf_free(&err);
    }
}

void test_args_parse_batch_stops_options_at_first_file(void)
{
    char *argv[] = {"yew", "--batch", "run.fl", "one", "--quiet",
                    "--", "tail"};
    YewArgs args;
    Bytebuf err;

    bytebuf_init(&err);
    YEW_ASSERT_EQ_I64(yew_args_parse(&args, 7, argv, &err), -1);
    YEW_ASSERT(!args.quiet);
    YEW_ASSERT_EQ_U64(args.nfiles, 2U);
    YEW_ASSERT_EQ_STR(args.files[0], "one");
    YEW_ASSERT_EQ_STR(args.files[1], "--quiet");
    YEW_ASSERT_EQ_U64(args.nbatch_args, 1U);
    YEW_ASSERT_EQ_STR(args.batch_args[0], "tail");
    YEW_ASSERT_EQ_U64(err.len, 0U);
    yew_args_free(&args);
    bytebuf_free(&err);
}
