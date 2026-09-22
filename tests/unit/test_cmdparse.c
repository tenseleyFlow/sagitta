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
    return YEW_CMD_OK;
}

static void register_parse_command(const char *name, u8 arity,
                                   const char *argspec, u8 policy,
                                   const char *abbrev)
{
    CmdEntry entry = {
        {name, parse_nop, arity, 0U, "Parser test command", NULL},
        argspec, policy, abbrev};

    (void)yew_cmd_register_entry(&entry);
}

static void parse_fixture_init(ParseFixture *f)
{
    Cursor cursor = {BYTEOFF(4U), {4U}, BYTEOFF(4U)};

    memset(f, 0, sizeof(*f));
    f->tb = yew_textbuf_from_bytes((const u8 *)"one two\nthree\nlast", 18U);
    f->ed.buffer.tb = f->tb;
    f->ed.buffer.path = "/home/u/proj/main.c";
    f->ed.buffer.meta.realpath = "/home/u/proj/main.c";
    f->ed.single_win.buf = &f->ed.buffer;
    yew_cset_init(&f->ed.single_win.cs, cursor);
    f->ed.win = &f->ed.single_win;
    f->ed.ws.dir = "/home/u/proj";
    arena_init(&f->arena);
    yew_cmd_shutdown();
    yew_cmd_init();
    register_parse_command("ed.ui.open", YEW_ARITY_STR, "s", YEW_RP_OPT,
                           "topen");
    register_parse_command("ed.ui.grow", YEW_ARITY_NONE, "",
                           YEW_RP_REQUIRED, "sort");
    register_parse_command("ed.ui.shrink", YEW_ARITY_NONE, "",
                           YEW_RP_FORBID, "q");
}

static void parse_fixture_free(ParseFixture *f)
{
    yew_cmd_shutdown();
    arena_free_all(&f->arena);
    yew_cset_free(&f->ed.single_win.cs);
    yew_textbuf_free(f->tb);
}

static void assert_arg(ParseFixture *f, const char *line,
                       const char *expected)
{
    CmdParse parsed;

    YEW_ASSERT(yew_cmd_parse(&f->ed, line, strlen(line), &f->arena,
                             &parsed));
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.write");
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], expected);
}

static void assert_error(ParseFixture *f, const char *line,
                         const char *expected)
{
    CmdParse parsed;
    size_t len = strlen(line);

    YEW_ASSERT(!yew_cmd_parse(&f->ed, line, len, &f->arena, &parsed));
    YEW_ASSERT_EQ_STR(parsed.err.msg, expected);
    YEW_ASSERT(parsed.err.tok_lo < parsed.err.tok_hi);
    YEW_ASSERT(parsed.err.tok_hi <= len);
}

static void assert_error_span(ParseFixture *f, const char *line,
                              const char *expected, u32 lo, u32 hi)
{
    CmdParse parsed;

    YEW_ASSERT(!yew_cmd_parse(&f->ed, line, strlen(line), &f->arena,
                              &parsed));
    YEW_ASSERT_EQ_STR(parsed.err.msg, expected);
    YEW_ASSERT_EQ_U64(parsed.err.tok_lo, lo);
    YEW_ASSERT_EQ_U64(parsed.err.tok_hi, hi);
}

void test_cmdparse_tokenizer_expansion_matrix(void)
{
    static const char nul_line[] = ":w a\0b";
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
    YEW_ASSERT(YEW_ARRAY_LEN(rows) >= 40U);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++)
        assert_arg(&f, rows[i].line, rows[i].arg);

    f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(0U);
    f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(2U);
    assert_arg(&f, ":w %s", "on");
    {
        static const u8 binary[] = {'a', '\0', 'b'};

        yew_textbuf_free(f.tb);
        f.tb = yew_textbuf_from_bytes(binary, sizeof(binary));
        f.ed.buffer.tb = f.tb;
        f.ed.single_win.buf = &f.ed.buffer;
        f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(0U);
        f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(sizeof(binary));
        assert_error(&f, ":w %s",
                     "NUL byte is not valid in a command line");
    }

    assert_error(&f, ":w \"abc", "unterminated \"");
    assert_error(&f, ":w 'abc", "unterminated '");
    assert_error(&f, ":w \"\\q\"", "unknown escape '\\q'");
    assert_error(&f, ":w %z", "unknown expansion '%z'");
    f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(4U);
    f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(4U);
    assert_error(&f, ":w %s", "%s needs a selection");
    f.ed.buffer.path = NULL;
    assert_error(&f, ":w %", "buffer has no file name");
    {
        CmdParse parsed;

        YEW_ASSERT(!yew_cmd_parse(&f.ed, nul_line, sizeof(nul_line) - 1U,
                                  &f.arena, &parsed));
        YEW_ASSERT_EQ_STR(parsed.err.msg,
                          "NUL byte is not valid in a command line");
        YEW_ASSERT_EQ_U64(parsed.err.tok_lo, 4U);
        YEW_ASSERT_EQ_U64(parsed.err.tok_hi, 5U);
    }
    parse_fixture_free(&f);
}

void test_cmdparse_fl_preserves_source_as_one_argument(void)
{
    ParseFixture f;
    CmdParse parsed;

    parse_fixture_init(&f);
    YEW_ASSERT(yew_cmd_parse(&f.ed, ":fl answer + 1", 14U, &f.arena,
                             &parsed));
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.fl.eval");
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], "answer + 1");
    parse_fixture_free(&f);
}

void test_cmdparse_group_close_full_name_and_abbrev(void)
{
    static const char *const lines[] = {
        ":group.close", ":ed.group.close", ":gclose"
    };
    ParseFixture f;

    parse_fixture_init(&f);
    for (u32 i = 0U; i < YEW_ARRAY_LEN(lines); i++) {
        CmdParse parsed;

        YEW_ASSERT(yew_cmd_parse(&f.ed, lines[i], strlen(lines[i]),
                                 &f.arena, &parsed));
        YEW_ASSERT_EQ_U64(parsed.argv.n, 1U);
        YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.group.close");
    }
    parse_fixture_free(&f);
}

void test_cmdparse_prof_subcommands_route_to_registry(void)
{
    static const struct {
        const char *line;
        const char *command;
        const char *arg;
    } rows[] = {
        {":prof", "ed.prof.report", NULL},
        {":prof report", "ed.prof.report", NULL},
        {":prof reset", "ed.prof.reset", NULL},
        {":prof dump '/tmp/prof report'", "ed.prof.dump",
         "/tmp/prof report"},
        {":prof mark typing", "ed.prof.mark", "typing"},
        {":prof frames", "ed.prof.frames", NULL},
        {":prof frames 12", "ed.prof.frames", "12"}
    };
    ParseFixture f;
    size_t i;

    parse_fixture_init(&f);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        CmdParse parsed;

        YEW_ASSERT(yew_cmd_parse(&f.ed, rows[i].line, strlen(rows[i].line),
                                 &f.arena, &parsed));
        YEW_ASSERT_EQ_STR(parsed.argv.v[0], rows[i].command);
        YEW_ASSERT_EQ_U64(parsed.argv.n, rows[i].arg == NULL ? 1U : 2U);
        if (rows[i].arg != NULL)
            YEW_ASSERT_EQ_STR(parsed.argv.v[1], rows[i].arg);
    }
    assert_error(&f, ":prof unknown", "unknown prof action 'unknown'");
    assert_error(&f, ":prof report extra",
                 ":prof.report takes no arguments");
    parse_fixture_free(&f);
}

void test_cmdparse_resolution_bang_errors_and_parse_point(void)
{
    ParseFixture f;
    CmdParse parsed;
    CmdParsePoint point;

    parse_fixture_init(&f);
    YEW_ASSERT(yew_cmd_parse(&f.ed, ":ui.open value", 14U, &f.arena,
                             &parsed));
    YEW_ASSERT(yew_cmd_parse(&f.ed, ":ui.o value", 11U, &f.arena,
                             &parsed));
    YEW_ASSERT(yew_cmd_parse(&f.ed, ":w! value", 9U, &f.arena, &parsed));
    YEW_ASSERT(parsed.bang);
    assert_error(&f, ":ui.shrink extra",
                 ":ui.shrink takes no arguments");
    assert_error_span(&f, ":quit extra", ":quit takes no arguments",
                      6U, 11U);
    assert_error(&f, ":1quit", ":quit takes no range");
    assert_error(&f, ":sort", ":ui.grow requires a range");
    assert_error(&f, ":not_a_command",
                 "unknown command 'not_a_command' (try Tab)");
    {
        CmdParse parsed;

        YEW_ASSERT(yew_cmd_parse(&f.ed, ":source", 7U, &f.arena,
                                 &parsed));
        YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.config.reload");
    }
    assert_error(&f, ":source old.vim",
                 ":config.reload takes no arguments");
    assert_error(&f, ":cmdline.accept",
                 "unknown command 'cmdline.accept' (try Tab)");
    /* Sprint 19 opened these: a bang takes the rest of the line verbatim
     * as one argument, because a shell command is not a token list. */
    assert_error(&f, ":!", "ed.shell.run needs a command");
    assert_error(&f, ":r !", "ed.shell.read needs a command");
    /*
     * Sprint 21 opened `:s` and `:g`, so a bare one is an arity error
     * rather than a deferral.  Their bodies take the rest of the line
     * verbatim — `:s/pat/rep/` has no space after the name, and the
     * tokenizer must not try to understand a `/` inside a regex.
     */
    assert_error(&f, ":s", ":search.replace requires 1 argument");
    {
        CmdParse parsed;

        YEW_ASSERT(yew_cmd_parse(&f.ed, ":s/a/b/g", 8U, &f.arena,
                                 &parsed));
        YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
        YEW_ASSERT_EQ_STR(parsed.argv.v[1], "/a/b/g");
        YEW_ASSERT(yew_cmd_parse(&f.ed, ":%s#a#b#", 8U, &f.arena,
                                 &parsed));
        YEW_ASSERT_EQ_STR(parsed.argv.v[1], "#a#b#");
        YEW_ASSERT_EQ_U64(parsed.range.kind, YEW_RANGE_BUFFER);
        YEW_ASSERT(yew_cmd_parse(&f.ed, ":g/re/d", 7U, &f.arena, &parsed));
        YEW_ASSERT_EQ_STR(parsed.argv.v[1], "/re/d");
    }
    assert_error(&f, ":fl", ":fl needs Fletch source");

    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":w \"my fi", 9U, 9U,
                                   &f.arena, &point));
    YEW_ASSERT(point.command_known);
    YEW_ASSERT_EQ_U64(point.token_index, 1U);
    YEW_ASSERT_EQ_STR(point.stem, "my fi");
    YEW_ASSERT_EQ_U64(point.token.lo, 3U);
    YEW_ASSERT_EQ_U64(point.token.hi, 9U);
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":w one ", 7U, 7U,
                                   &f.arena, &point));
    YEW_ASSERT_EQ_U64(point.token_index, 2U);
    YEW_ASSERT_EQ_U64(point.token.lo, 7U);
    YEW_ASSERT_EQ_U64(point.token.hi, 7U);

    assert_error(&f, ":file.w",
                 "ambiguous: file.write, file.write_quit");
    assert_error(&f, ":file.open",
                 ":file.open requires 1 argument");
    parse_fixture_free(&f);
}

/*
 * Sprint 57.18 §1 / DoD 3: the EXECUTING parser is byte-identical.
 *
 * The pinned property, asserted rather than assumed.  Every `:!` form
 * this table names still produces one verbatim argument spanning the
 * whole body -- pipes, quotes, redirection, `%` and the second bang of
 * `:!!` included -- and the argument span still runs to the end of the
 * line.  The point parser learned to split that same text in this
 * sprint; if that split ever leaks into yew_cmd_parse, this fails.
 */
void test_cmdparse_bang_execution_is_unchanged(void)
{
    static const struct {
        const char *line;
        const char *command;
        const char *arg;
        u32 arg_lo;
    } rows[] = {
        {":!ls", "ed.shell.run", "ls", 2U},
        {":!  ls -l", "ed.shell.run", "ls -l", 4U},
        {":!ls | grep x > out", "ed.shell.run", "ls | grep x > out", 2U},
        {":!echo \"a b\"", "ed.shell.run", "echo \"a b\"", 2U},
        {":!echo 'a b'", "ed.shell.run", "echo 'a b'", 2U},
        {":!echo a\\ b", "ed.shell.run", "echo a\\ b", 2U},
        {":!echo %", "ed.shell.run", "echo %", 2U},
        {":!!top", "ed.shell.run", "!top", 2U},
        {":!!", "ed.shell.run", "!", 2U},
        {":%!sort", "ed.shell.run", "sort", 3U},
        {":1,2!fmt -w 40", "ed.shell.run", "fmt -w 40", 5U},
        {":r !date", "ed.shell.read", "date", 4U},
        {":r !!date", "ed.shell.read", "!date", 4U}
    };
    ParseFixture f;
    size_t i;

    parse_fixture_init(&f);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        CmdParse parsed;
        size_t len = strlen(rows[i].line);

        YEW_ASSERT(yew_cmd_parse(&f.ed, rows[i].line, len, &f.arena,
                                 &parsed));
        YEW_ASSERT_EQ_STR(parsed.argv.v[0], rows[i].command);
        YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
        YEW_ASSERT_EQ_STR(parsed.argv.v[1], rows[i].arg);
        YEW_ASSERT_EQ_U64(parsed.arg_tok[1].lo, rows[i].arg_lo);
        YEW_ASSERT_EQ_U64(parsed.arg_tok[1].hi, len);
    }
    /* And the refusals are still refusals, at the same spans. */
    assert_error(&f, ":!", "ed.shell.run needs a command");
    assert_error(&f, ":r !", "ed.shell.read needs a command");
    parse_fixture_free(&f);
}

/*
 * Sprint 57.18 §1: the POINT parser locates the caret's word inside a
 * bang body, which is what `yew_comp_query_at` needs and never had.
 *
 * `token_index` counts SHELL words here: 0 is the command the shell
 * will run and 1+ are its operands, which is the whole reason
 * `bang_body` is reported alongside it.
 */
void test_cmdparse_bang_point_splits_the_shell_body(void)
{
    static const struct {
        const char *line;
        size_t cursor;
        u32 index;
        const char *stem;
        u32 lo;
        u32 hi;
    } rows[] = {
        /* Word 0 is the command word, at the caret or at the body start. */
        {":!", 2U, 0U, "", 2U, 2U},
        {":!ch", 4U, 0U, "ch", 2U, 4U},
        {":!  ch", 6U, 0U, "ch", 4U, 6U},
        {":!ch", 3U, 0U, "c", 2U, 3U},
        /* A range or the read spelling opens the same body. */
        {":%!so", 5U, 0U, "so", 3U, 5U},
        {":1,2!so", 7U, 0U, "so", 5U, 7U},
        {":r !da", 6U, 0U, "da", 4U, 6U},
        /* §4's `:!!` prefix is not part of the command word. */
        {":!!to", 5U, 0U, "to", 3U, 5U},
        /* Operands. */
        {":!ls ", 5U, 1U, "", 5U, 5U},
        {":!ls sr", 7U, 1U, "sr", 5U, 7U},
        {":!ls a b", 8U, 2U, "b", 7U, 8U},
        /* Quoting: the stem comes back without its quotes. */
        {":!cat \"my fi", 12U, 1U, "my fi", 6U, 12U},
        {":!cat 'my fi", 12U, 1U, "my fi", 6U, 12U},
        {":!cat \"my file\" ne", 18U, 2U, "ne", 16U, 18U},
        {":!cat my\\ fi", 12U, 1U, "my fi", 6U, 12U},
        {":!cat \"a\\\"b", 11U, 1U, "a\"b", 6U, 11U},
        /* SHELL rules, not Sprint 18's: `%` is a percent sign. */
        {":!echo %", 8U, 1U, "%", 7U, 8U},
        /* Sprint 57.23 lifts 57.18's named deferral: the word after a
         * `|` is a COMMAND word again (index 0), not operand 2. */
        {":!ls | gr", 9U, 0U, "gr", 7U, 9U},
        /* A caret in the whitespace run before a word is a fresh word. */
        {":!ls   x", 5U, 1U, "", 5U, 5U}
    };
    ParseFixture f;
    CmdParsePoint point;
    size_t i;

    parse_fixture_init(&f);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        size_t len = strlen(rows[i].line);

        YEW_ASSERT(yew_cmd_parse_point(&f.ed, rows[i].line, len,
                                       rows[i].cursor, &f.arena, &point));
        YEW_ASSERT(point.bang_body);
        YEW_ASSERT_EQ_U64(point.token_index, rows[i].index);
        YEW_ASSERT_EQ_STR(point.stem, rows[i].stem);
        YEW_ASSERT_EQ_U64(point.token.lo, rows[i].lo);
        YEW_ASSERT_EQ_U64(point.token.hi, rows[i].hi);
    }
    parse_fixture_free(&f);
}

/*
 * The other half of §1: what is NOT a bang body.
 *
 * `:w! file` is Sprint 18's bang FLAG glued to a command name, and
 * reading it as a shell body would break every `:x!` in the editor.  A
 * caret on or before the bang is still the command name.
 */
void test_cmdparse_bang_body_does_not_swallow_the_bang_flag(void)
{
    ParseFixture f;
    CmdParsePoint point;

    parse_fixture_init(&f);
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":w! fil", 7U, 7U, &f.arena,
                                   &point));
    YEW_ASSERT(!point.bang_body);
    YEW_ASSERT_EQ_U64(point.token_index, 1U);
    YEW_ASSERT_EQ_STR(point.stem, "fil");
    /* A caret before the bang completes commands, not executables. */
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":!ls", 4U, 1U, &f.arena,
                                   &point));
    YEW_ASSERT(!point.bang_body);
    YEW_ASSERT_EQ_U64(point.token_index, 0U);
    YEW_ASSERT_EQ_STR(point.stem, "");
    /* And an ordinary command's arguments are untouched. */
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":ui.open val", 12U, 12U,
                                   &f.arena, &point));
    YEW_ASSERT(!point.bang_body);
    YEW_ASSERT_EQ_U64(point.token_index, 1U);
    YEW_ASSERT_EQ_STR(point.stem, "val");
    parse_fixture_free(&f);
}

/*
 * Sprint 57.23 §5: the point parser now reads the body through the shell
 * context lexer -- and execution still does not.  `:!ls | gr` completes
 * `gr` as a COMMAND while yew_cmd_parse hands `ls | gr` to the shell as
 * one verbatim argument.
 */
void test_cmdparse_bang_pipe_body_stays_verbatim(void)
{
    ParseFixture f;
    CmdParse parsed;
    CmdParsePoint point;

    parse_fixture_init(&f);
    YEW_ASSERT(yew_cmd_parse(&f.ed, ":!ls | gr", 9U, &f.arena, &parsed));
    YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.shell.run");
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], "ls | gr");
    YEW_ASSERT_EQ_U64(parsed.arg_tok[1].lo, 2U);
    YEW_ASSERT_EQ_U64(parsed.arg_tok[1].hi, 9U);

    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":!ls | gr", 9U, 9U, &f.arena,
                                   &point));
    YEW_ASSERT(point.bang_body);
    YEW_ASSERT_NOT_NULL(point.shell);
    YEW_ASSERT_EQ_U64(point.shell->pos, YEW_SH_POS_COMMAND);
    YEW_ASSERT_EQ_U64(point.shell->replace.lo, 7U);
    YEW_ASSERT_EQ_U64(point.shell->replace.hi, 9U);
    YEW_ASSERT_EQ_STR(point.shell->stem, "gr");
    YEW_ASSERT_EQ_U64(point.token.lo, 7U);
    YEW_ASSERT_EQ_U64(point.token_index, 0U);
    /* The range and read spellings rebase onto their own body start. */
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":r !cat > ou", 12U, 12U,
                                   &f.arena, &point));
    YEW_ASSERT_NOT_NULL(point.shell);
    YEW_ASSERT_EQ_U64(point.shell->pos, YEW_SH_POS_REDIRECT);
    YEW_ASSERT_EQ_U64(point.token.lo, 10U);
    YEW_ASSERT_EQ_STR(point.shell->argv[0], "cat");
    /* Not a bang body: no shell context. */
    YEW_ASSERT(yew_cmd_parse_point(&f.ed, ":w fi", 5U, 5U, &f.arena,
                                   &point));
    YEW_ASSERT_NULL(point.shell);
    parse_fixture_free(&f);
}
