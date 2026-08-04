#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/cmdparse.h"

typedef struct ParseFixture {
    Ed ed;
    TextBuf *tb;
    Arena arena;
} ParseFixture;

static CmdStatus parse_nop(CmdCtx *cx)
{
    (void)cx;
    return SAG_CMD_OK;
}

static void register_parse_command(const char *name, u8 arity,
                                   const char *argspec, u8 policy,
                                   const char *abbrev)
{
    CmdEntry entry = {
        {name, parse_nop, arity, 0U, "Parser test command"},
        argspec, policy, abbrev};

    (void)sag_cmd_register_entry(&entry);
}

static void parse_fixture_init(ParseFixture *f)
{
    Cursor cursor = {BYTEOFF(4U), {4U}, BYTEOFF(4U)};

    memset(f, 0, sizeof(*f));
    f->tb = sag_textbuf_from_bytes((const u8 *)"one two\nthree\nlast", 18U);
    f->ed.buffer.tb = f->tb;
    f->ed.buffer.path = "/home/u/proj/main.c";
    f->ed.buffer.meta.realpath = "/home/u/proj/main.c";
    f->ed.single_win.buf = &f->ed.buffer;
    sag_cset_init(&f->ed.single_win.cs, cursor);
    f->ed.win = &f->ed.single_win;
    f->ed.ws.dir = "/home/u/proj";
    arena_init(&f->arena);
    sag_cmd_shutdown();
    sag_cmd_init();
    register_parse_command("ed.ui.open", SAG_ARITY_STR, "s", SAG_RP_OPT,
                           "topen");
    register_parse_command("ed.ui.grow", SAG_ARITY_NONE, "",
                           SAG_RP_REQUIRED, "sort");
    register_parse_command("ed.ui.shrink", SAG_ARITY_NONE, "",
                           SAG_RP_FORBID, "q");
}

static void parse_fixture_free(ParseFixture *f)
{
    sag_cmd_shutdown();
    arena_free_all(&f->arena);
    sag_cset_free(&f->ed.single_win.cs);
    sag_textbuf_free(f->tb);
}

static void assert_arg(ParseFixture *f, const char *line,
                       const char *expected)
{
    CmdParse parsed;

    SAG_ASSERT(sag_cmd_parse(&f->ed, line, strlen(line), &f->arena,
                             &parsed));
    SAG_ASSERT_EQ_U64(parsed.argv.n, 2U);
    SAG_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.write");
    SAG_ASSERT_EQ_STR(parsed.argv.v[1], expected);
}

static void assert_error(ParseFixture *f, const char *line,
                         const char *expected)
{
    CmdParse parsed;
    size_t len = strlen(line);

    SAG_ASSERT(!sag_cmd_parse(&f->ed, line, len, &f->arena, &parsed));
    SAG_ASSERT_EQ_STR(parsed.err.msg, expected);
    SAG_ASSERT(parsed.err.tok_lo < parsed.err.tok_hi);
    SAG_ASSERT(parsed.err.tok_hi <= len);
}

void test_cmdparse_tokenizer_expansion_matrix(void)
{
    static const struct {
        const char *line;
        const char *arg;
    } rows[] = {
        {":w alpha", "alpha"},
        {"w alpha", "alpha"},
        {"  :  w alpha  ", "alpha"},
        {":w alpha\\ beta", "alpha beta"},
        {":w 'alpha beta'", "alpha beta"},
        {":w \"alpha beta\"", "alpha beta"},
        {":w a'b c'd", "ab cd"},
        {":w a\"b c\"d", "ab cd"},
        {":w ''", ""},
        {":w \"\"", ""},
        {":w '\\n'", "\\n"},
        {":w \"\\n\"", "\n"},
        {":w \"\\t\"", "\t"},
        {":w \"\\\\\"", "\\"},
        {":w \"\\\"\"", "\""},
        {":w \"\\%\"", "%"},
        {":w %%", "%"},
        {":w %", "/home/u/proj/main.c"},
        {":w \"%\"", "/home/u/proj/main.c"},
        {":w %p", "/home/u/proj/main.c"},
        {":w %d", "/home/u/proj"},
        {":w %h", "/home/u/proj"},
        {":w %b", "main.c"},
        {":w %l", "1"},
        {":w %c", "5"},
        {":w '%d/lit.c'", "%d/lit.c"},
        {":w %d/backup.txt", "/home/u/proj/backup.txt"},
        {":w \"%d/my file.c\"", "/home/u/proj/my file.c"},
        {":w one\\\ttwo", "one\ttwo"},
        {":w '$HOME'", "$HOME"},
        {":w \"$HOME\"", "$HOME"},
        {":w a\\\"b", "a\"b"},
        {":w a\\'b", "a'b"},
        {":w a\\\\b", "a\\b"},
        {":w 'it''s'", "its"},
        {":w x'y'z", "xyz"},
        {":w x\"y\"z", "xyz"},
        {":w 日本語", "日本語"},
        {":w 👨‍👩‍👧‍👦", "👨‍👩‍👧‍👦"},
        {":w a-b_c.1", "a-b_c.1"},
        {":w ./relative", "./relative"},
        {":w a=b", "a=b"},
    };
    ParseFixture f;
    size_t i;

    parse_fixture_init(&f);
    SAG_ASSERT(SAG_ARRAY_LEN(rows) >= 40U);
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++)
        assert_arg(&f, rows[i].line, rows[i].arg);

    f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(0U);
    f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(2U);
    assert_arg(&f, ":w %s", "on");

    assert_error(&f, ":w \"abc", "unterminated \"");
    assert_error(&f, ":w 'abc", "unterminated '");
    assert_error(&f, ":w \"\\q\"", "unknown escape '\\q'");
    assert_error(&f, ":w %z", "unknown expansion '%z'");
    f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(4U);
    f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(4U);
    assert_error(&f, ":w %s", "%s needs a selection");
    f.ed.buffer.path = NULL;
    assert_error(&f, ":w %", "buffer has no file name");
    parse_fixture_free(&f);
}

void test_cmdparse_resolution_bang_errors_and_parse_point(void)
{
    ParseFixture f;
    CmdParse parsed;
    CmdParsePoint point;

    parse_fixture_init(&f);
    SAG_ASSERT(sag_cmd_parse(&f.ed, ":ui.open value", 14U, &f.arena,
                             &parsed));
    SAG_ASSERT(sag_cmd_parse(&f.ed, ":ui.o value", 11U, &f.arena,
                             &parsed));
    SAG_ASSERT(sag_cmd_parse(&f.ed, ":w! value", 9U, &f.arena, &parsed));
    SAG_ASSERT(parsed.bang);
    assert_error(&f, ":ui.shrink extra",
                 ":ui.shrink takes no arguments");
    assert_error(&f, ":sort", ":ui.grow requires a range");
    assert_error(&f, ":not_a_command", 
                 "unknown command 'not_a_command' (try Tab)");
    assert_error(&f, ":!printf x", ":! runs shell commands: Sprint 19");
    assert_error(&f, ":1,2!sort", ":! runs shell commands: Sprint 19");
    assert_error(&f, ":r !printf x",
                 ":r ! reads shell output: Sprint 19");
    assert_error(&f, ":jobs", ":jobs lists shell jobs: Sprint 19");
    assert_error(&f, ":s", ":s substitutes text: Sprint 21");
    assert_error(&f, ":g",
                 ":g search surface: Sprint 21; Fletch queries: Sprint 34");
    assert_error(&f, ":fl", ":fl evaluates Fletch: Sprint 32");

    SAG_ASSERT(sag_cmd_parse_point(&f.ed, ":w \"my fi", 9U, 9U,
                                   &f.arena, &point));
    SAG_ASSERT(point.command_known);
    SAG_ASSERT_EQ_U64(point.token_index, 1U);
    SAG_ASSERT_EQ_STR(point.stem, "my fi");
    SAG_ASSERT_EQ_U64(point.token.lo, 3U);
    SAG_ASSERT_EQ_U64(point.token.hi, 9U);
    SAG_ASSERT(sag_cmd_parse_point(&f.ed, ":w one ", 7U, 7U,
                                   &f.arena, &point));
    SAG_ASSERT_EQ_U64(point.token_index, 2U);
    SAG_ASSERT_EQ_U64(point.token.lo, 7U);
    SAG_ASSERT_EQ_U64(point.token.hi, 7U);
    parse_fixture_free(&f);
}
