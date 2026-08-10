/*
 * Sprint 31 deliverable 7: the `io` module.
 *
 * COVERAGE  (function -> test; every row of §7's table, every error kind)
 * --------
 * io.read        bytes verbatim   io_read_returns_the_bytes_verbatim
 * io.read        "io"             io_read_names_the_errno_and_the_path
 * io.read_lines  split_lines of   io_read_lines_is_split_lines_of_the_file
 * io.write       atomic           io_write_then_read_round_trips
 * io.append      O_APPEND         io_write_then_read_round_trips
 * io.exists      false on error   io_interrogates_without_raising
 * io.is_dir                       io_interrogates_without_raising
 * io.size        "io"             io_size_reports_bytes_or_raises
 * io.remove      file or emptydir io_remove_takes_a_file_or_an_empty_dir
 * io.mkdir       parents          io_mkdir_makes_one_level_or_all_of_them
 * io.glob        * ? [] ** dots   io_glob_matches_the_documented_syntax
 * io.glob        order, symlinks  io_glob_is_sorted_and_never_follows_a_link
 * io.glob        root defaults    io_glob_roots_at_the_importing_module
 * io.env                          io_env_is_the_one_getenv
 * io.print       no capability    io_print_needs_no_capability
 * io.eprint      no capability    io_print_needs_no_capability
 *
 * The `capability` kind belongs to every row above and is covered once,
 * properly, in test_fl_caps.c -- a per-function repetition of it would
 * be sixteen copies of one assertion.  The "limit" kinds are the 64 MiB
 * and 100 000-match caps, which a unit test cannot reach without
 * building a fixture bigger than the suite; they are asserted by
 * inspection at the DoD walk.
 */
#define _POSIX_C_SOURCE 200809L

#include "flfix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P "import io\n"

/* Every case here runs as a config that may read and write, so the
 * capability is never the thing under test. */
static const char *io_open(FlFix *f)
{
    flfix_open(f);
    flfix_as(f, (u8)FL_ORIGIN_CONFIG,
             (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE);
    return flfix_tmpdir(f);
}

/*
 * A program with up to three `%s` filled in by the fixture's directory.
 *
 * The format goes through a function rather than straight into
 * snprintf so the compiler does not count the conversions: several
 * cases name the directory twice, and passing three copies to a
 * one-conversion format is a varargs no-op rather than a mismatch.
 */
static void io_src(char *out, size_t cap, const char *fmt, const char *dir)
{
    (void)snprintf(out, cap, fmt, dir, dir, dir);
}

#define IO_EQ(f, dir, fmt_, want)                                             \
    do {                                                                      \
        char io_src_[2048];                                                   \
                                                                              \
        io_src(io_src_, sizeof(io_src_), P fmt_, (dir));                      \
        FL_EQ((f), io_src_, (want));                                          \
    } while (0)

void test_fl_io_read_returns_the_bytes_verbatim(void)
{
    FlFix f;
    const char *dir = io_open(&f);

    flfix_write(&f, "a.txt", "hello\nworld\n");
    IO_EQ(&f, dir, "return io.read(\"%s/a.txt\")\n", "hello\nworld\n");
    /* No UTF-8 validation anywhere: a file that is not text is still a
     * file, and invariant 2 is about the user's bytes. */
    flfix_write(&f, "bin", "a\x80\xff" "b");
    IO_EQ(&f, dir, "return io.read(\"%s/bin\")\n", "a\x80\xff" "b");
    flfix_close(&f);
}

void test_fl_io_read_names_the_errno_and_the_path(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[1024];

    flfix_mkdir(&f, "sub");
    /* The errno NAME, from our own table: strerror moves with LANG and
     * a message that changes with the environment breaks a golden. */
    (void)snprintf(want, sizeof(want), "!io: ENOENT: %s/nope", dir);
    IO_EQ(&f, dir, "return io.read(\"%s/nope\")\n", want);
    (void)snprintf(want, sizeof(want), "!io: EISDIR: %s/sub", dir);
    IO_EQ(&f, dir, "return io.read(\"%s/sub\")\n", want);
    /*
     * A path with an embedded NUL is REFUSED, not truncated.  The
     * kernel would stop at the NUL and act on a shorter path than the
     * script named, which is the byte confusion invariant 2 forbids.
     */
    FL_EQ(&f, P "return io.read(\"a\\0b\")\n",
          "!io: path contains a NUL byte at offset 1");
    FL_EQ(&f, P "return io.read(\"\")\n", "!io: the path is empty");
    flfix_close(&f);
}

void test_fl_io_read_lines_is_split_lines_of_the_file(void)
{
    FlFix f;
    const char *dir = io_open(&f);

    flfix_write(&f, "u.txt", "a\nb\n");
    flfix_write(&f, "w.txt", "a\r\nb\r\n");
    flfix_write(&f, "n.txt", "a\nb");
    /*
     * Literally str.split_lines of the content -- one implementation,
     * shared -- so a trailing terminator yields a final empty line and
     * a CRLF file reads the same as an LF one.
     */
    IO_EQ(&f, dir,
          "import fmt\nreturn fmt.repr(io.read_lines(\"%s/u.txt\"))\n",
          "[\"a\", \"b\", \"\"]");
    IO_EQ(&f, dir,
          "import fmt\nreturn fmt.repr(io.read_lines(\"%s/w.txt\"))\n",
          "[\"a\", \"b\", \"\"]");
    IO_EQ(&f, dir,
          "import fmt\nreturn fmt.repr(io.read_lines(\"%s/n.txt\"))\n",
          "[\"a\", \"b\"]");
    flfix_close(&f);
}

void test_fl_io_write_then_read_round_trips(void)
{
    FlFix f;
    const char *dir = io_open(&f);

    IO_EQ(&f, dir,
          "io.write(\"%s/w.txt\", \"one\")\n"
          "return io.read(\"%s/w.txt\")\n", "one");
    IO_EQ(&f, dir,
          "io.write(\"%s/w.txt\", \"one\")\n"
          "io.append(\"%s/w.txt\", \"two\")\n"
          "return io.read(\"%s/w.txt\")\n", "onetwo");
    /* Bytes, not text, on the way out as well as in. */
    IO_EQ(&f, dir,
          "io.write(\"%s/b\", \"x\\x00y\")\nreturn io.size(\"%s/b\")\n", "3");
    flfix_close(&f);
}

void test_fl_io_interrogates_without_raising(void)
{
    FlFix f;
    const char *dir = io_open(&f);

    flfix_write(&f, "a.txt", "hi");
    flfix_mkdir(&f, "sub");
    IO_EQ(&f, dir, "return io.exists(\"%s/a.txt\")\n", "true");
    /* FALSE on any error, per the table: `exists` answers a question,
     * and a permission problem upstream is still "I cannot see it". */
    IO_EQ(&f, dir, "return io.exists(\"%s/nope\")\n", "false");
    IO_EQ(&f, dir, "return io.exists(\"%s/nope/deeper\")\n", "false");
    IO_EQ(&f, dir, "return io.is_dir(\"%s/sub\")\n", "true");
    IO_EQ(&f, dir, "return io.is_dir(\"%s/a.txt\")\n", "false");
    IO_EQ(&f, dir, "return io.is_dir(\"%s/nope\")\n", "false");
    flfix_close(&f);
}

void test_fl_io_size_reports_bytes_or_raises(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[1024];

    flfix_write(&f, "a.txt", "hello\nworld\n");
    IO_EQ(&f, dir, "return io.size(\"%s/a.txt\")\n", "12");
    (void)snprintf(want, sizeof(want), "!io: ENOENT: %s/nope", dir);
    IO_EQ(&f, dir, "return io.size(\"%s/nope\")\n", want);
    flfix_close(&f);
}

void test_fl_io_remove_takes_a_file_or_an_empty_dir(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[1024];

    flfix_write(&f, "a.txt", "hi");
    flfix_mkdir(&f, "empty");
    flfix_mkdir(&f, "full");
    flfix_write(&f, "full/x", "x");
    IO_EQ(&f, dir,
          "io.remove(\"%s/a.txt\")\nreturn io.exists(\"%s/a.txt\")\n", "false");
    IO_EQ(&f, dir,
          "io.remove(\"%s/empty\")\nreturn io.exists(\"%s/empty\")\n", "false");
    (void)snprintf(want, sizeof(want), "!io: ENOTEMPTY: %s/full", dir);
    IO_EQ(&f, dir, "return io.remove(\"%s/full\")\n", want);
    (void)snprintf(want, sizeof(want), "!io: ENOENT: %s/nope", dir);
    IO_EQ(&f, dir, "return io.remove(\"%s/nope\")\n", want);
    flfix_close(&f);
}

void test_fl_io_mkdir_makes_one_level_or_all_of_them(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[1024];

    IO_EQ(&f, dir, "io.mkdir(\"%s/one\")\nreturn io.is_dir(\"%s/one\")\n",
          "true");
    /* Without `parents`, a missing intermediate is an error rather
     * than a directory tree appearing by surprise. */
    (void)snprintf(want, sizeof(want), "!io: ENOENT: %s/q/r", dir);
    IO_EQ(&f, dir, "return io.mkdir(\"%s/q/r\")\n", want);
    IO_EQ(&f, dir,
          "io.mkdir(\"%s/x/y/z\", true)\nreturn io.is_dir(\"%s/x/y/z\")\n",
          "true");
    /* "make sure this path exists" is what the flag means, so an
     * existing directory is not an error under it. */
    IO_EQ(&f, dir,
          "io.mkdir(\"%s/x/y/z\", true)\nreturn io.is_dir(\"%s/x/y/z\")\n",
          "true");
    flfix_close(&f);
}

/* The tree every glob case below matches against. */
static void glob_tree(FlFix *f)
{
    char link[1024];

    flfix_write(f, "a.txt", "a");
    flfix_write(f, "b.md", "b");
    flfix_write(f, ".hidden", "h");
    flfix_mkdir(f, "sub");
    flfix_mkdir(f, "sub/deep");
    flfix_write(f, "sub/b.txt", "sb");
    flfix_write(f, "sub/deep/c.txt", "dc");
    /* A symlink pointing at the root: descending it would loop. */
    (void)snprintf(link, sizeof(link), "%s/sub/loop", flfix_tmpdir(f));
    YEW_ASSERT_EQ_I64(symlink(flfix_tmpdir(f), link), 0);
}

void test_fl_io_glob_matches_the_documented_syntax(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[4096];

    glob_tree(&f);
    (void)snprintf(want, sizeof(want), "[\"%s/a.txt\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"*.txt\"))\n", want);
    (void)snprintf(want, sizeof(want), "[\"%s/b.md\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"?.md\"))\n", want);
    (void)snprintf(want, sizeof(want), "[\"%s/a.txt\", \"%s/b.md\"]", dir, dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"[ab]*\"))\n", want);
    (void)snprintf(want, sizeof(want), "[\"%s/sub/b.txt\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"sub/*.txt\"))\n", want);
    /* `**` crosses directories and matches zero levels too. */
    (void)snprintf(want, sizeof(want), "[\"%s/sub/deep/c.txt\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"**/c.txt\"))\n", want);
    (void)snprintf(want, sizeof(want),
                   "[\"%s/a.txt\", \"%s/sub/b.txt\", \"%s/sub/deep/c.txt\"]",
                   dir, dir, dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"**/*.txt\"))\n", want);
    /*
     * A dot-file matches only when the component asks for one.  `*`
     * skipping .bashrc is what every shell does and what every user
     * expects.
     */
    (void)snprintf(want, sizeof(want), "[\"%s/.hidden\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\".*\"))\n", want);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"nothing/*\"))\n", "[]");
    FL_EQ(&f, P "return io.glob(\"\")\n", "!type: io.glob: the pattern is "
                                          "empty");
    flfix_close(&f);
}

void test_fl_io_glob_is_sorted_and_never_follows_a_link(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[4096];

    glob_tree(&f);
    /*
     * The symlinked directory is MATCHED but never descended: a loop is
     * then impossible without a visited set, and a glob that follows
     * links is one that eventually walks /proc.  It appears exactly
     * once, and the scan terminates.
     */
    (void)snprintf(want, sizeof(want),
                   "[\"%s/sub/b.txt\", \"%s/sub/deep\", \"%s/sub/loop\"]",
                   dir, dir, dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"sub/*\"))\n", want);
    /*
     * Sorted BYTEWISE over the whole result, not per directory.  A
     * depth-first walk emits `a/z` before `a.txt` and bytewise `.`
     * sorts before `/`, so per-directory sorting is not enough --
     * `sub` below sorts after `sub.txt` for exactly that reason.
     */
    flfix_mkdir(&f, "zz");
    flfix_write(&f, "zz/q.txt", "q");
    flfix_write(&f, "zz.txt", "z");
    (void)snprintf(want, sizeof(want),
                   "[\"%s/a.txt\", \"%s/sub/b.txt\", \"%s/sub/deep/c.txt\", "
                   "\"%s/zz.txt\", \"%s/zz/q.txt\"]", dir, dir, dir, dir, dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"**/*.txt\"))\n", want);
    flfix_close(&f);
}

void test_fl_io_glob_roots_at_the_importing_module(void)
{
    FlFix f;
    const char *dir = io_open(&f);
    char want[4096];
    char src[2048];

    glob_tree(&f);
    /*
     * The default root is the importing FILE's directory, never the
     * process cwd.  A config that globbed relative to the cwd would
     * find its snippets when yew was launched from the config
     * directory and find nothing anywhere else, and the user would
     * report it as "plugins load sometimes".
     */
    (void)snprintf(want, sizeof(want), "[\"%s/a.txt\"]", dir);
    FL_EQ(&f, P "import fmt\nreturn fmt.repr(io.glob(\"*.txt\"))\n", want);
    /* An explicit root wins over it. */
    (void)snprintf(src, sizeof(src),
                   P "import fmt\n"
                     "return fmt.repr(io.glob(\"*.txt\", \"%s/sub\"))\n", dir);
    (void)snprintf(want, sizeof(want), "[\"%s/sub/b.txt\"]", dir);
    FL_EQ(&f, src, want);
    flfix_close(&f);
}

void test_fl_io_env_is_the_one_getenv(void)
{
    FlFix f;

    flfix_open(&f);
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    /* An environment read is ambient authority by another name, so it
     * goes through a capability and through one function. */
    FL_EQ(&f, P "return io.env(\"YEW_UNSET_FOR_THIS_TEST\")\n", "nil");
    YEW_ASSERT_EQ_I64(setenv("YEW_SET_FOR_THIS_TEST", "yes", 1), 0);
    FL_EQ(&f, P "return io.env(\"YEW_SET_FOR_THIS_TEST\")\n", "yes");
    YEW_ASSERT_EQ_I64(unsetenv("YEW_SET_FOR_THIS_TEST"), 0);
    flfix_close(&f);
}

void test_fl_io_print_needs_no_capability(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * The host already chose to run this script; denying it output
     * produces silent failures rather than safety.  Asserted with NO
     * grants at all, which is the whole point.
     */
    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    FL_EQ(&f, P "io.print()\nreturn 1\n", "1");
    FL_EQ(&f, P "io.eprint()\nreturn 1\n", "1");
    /* And it is still denied everything else. */
    FL_EQ(&f, P "return io.read(\"/nope\")\n",
          "!capability: fs.read denied to a plugin");
    flfix_close(&f);
}
