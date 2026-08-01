#include "harness.h"

#include "args.h"

#include <string.h>

static bool buf_equals(const Bytebuf *buf, const char *expected)
{
    size_t len = strlen(expected);

    return buf->len == len && memcmp(buf->data, expected, len) == 0;
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
    SAG_ASSERT_EQ_U64(err.len, 0U);
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
    bytebuf_free(&err);
}
