/*
 * Sprint 31 deliverable 9: `import`.
 *
 * COVERAGE
 * --------
 * bare name -> builtin        modules_bare_name_is_a_builtin_and_only_that
 * bare name, not a builtin    modules_bare_name_is_a_builtin_and_only_that
 * quoted path, importer-rel   modules_a_quoted_path_resolves_beside_its_importer
 * ..-relative, from a subdir  modules_a_quoted_path_resolves_beside_its_importer
 * not found lists every path  modules_not_found_lists_every_path_tried
 * realpath, not the spelling  modules_load_once_per_realpath
 * one map per (path, origin)  modules_load_once_per_realpath
 * exports exclude _-prefixed  modules_export_every_top_level_name_but_the_private
 * exports are frozen          modules_export_every_top_level_name_but_the_private
 * a module keeps its globals  modules_keep_their_own_globals
 * cycle names the whole chain modules_a_cycle_names_the_whole_chain
 * deferred editor surfaces    modules_deferred_surfaces_name_their_sprint
 * --list-natives, determinism modules_list_natives_is_deterministic
 *
 * The (realpath, origin.kind) key's SECURITY half -- a helper imported
 * by a plugin runs with the plugin's grants -- is tested where it
 * belongs, in test_fl_caps.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "flfix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/buf.h"

/* The library every case here imports. */
static void write_lib(FlFix *f)
{
    flfix_write(f, "lib.fl",
                "let answer = 42\n"
                "let _private = 7\n"
                "fn twice(x) { return x + x }\n");
}

void test_fl_modules_bare_name_is_a_builtin_and_only_that(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, "import fmt\nreturn fmt.str(1)\n", "1");
    FL_EQ(&f, "import str\nreturn str.upper(\"a\")\n", "A");
    /*
     * A bare IDENT is a builtin and NOTHING else.  Resolving it against
     * the filesystem would let a file called str.fl in the wrong
     * directory shadow the standard library, so the miss names the
     * modules rather than listing paths it did not try.
     *
     * The list is GENERATED from vm->builtins now.  It used to be a
     * literal in the message, and Sprint 34's `buf` made it wrong the
     * moment it registered -- the text named seven while the map held
     * eight, so a user was told a module did not exist while importing
     * it on the next line worked.  Asserting the full string here is
     * what keeps the generated version honest.
     */
    FL_EQ(&f, "import nope\nreturn 1\n",
          "!import: there is no builtin module 'nope'; they are str, list, "
          "map, math, fmt, io, re, buf");
    flfix_close(&f);
}

void test_fl_modules_a_quoted_path_resolves_beside_its_importer(void)
{
    FlFix f;

    flfix_open(&f);
    write_lib(&f);
    flfix_mkdir(&f, "sub");
    flfix_write(&f, "sub/uses.fl",
                "import \"../lib.fl\" as l\nlet v = l.answer\n");

    FL_EQ(&f, "import \"lib.fl\" as l\nreturn l.answer\n", "42");
    FL_EQ(&f, "import \"lib.fl\" as l\nreturn l.twice(4)\n", "8");
    /* The importing FILE's directory, not the process cwd -- which is
     * why uses.fl reaches lib.fl through `..` and finds it. */
    FL_EQ(&f, "import \"sub/uses.fl\" as u\nreturn u.v\n", "42");
    /* §11: import is a statement, so it is legal inside a block. */
    FL_EQ(&f, "let n = 0\nif true { import \"lib.fl\" as l\n"
              "n = l.answer }\nreturn n\n", "42");
    flfix_close(&f);
}

void test_fl_modules_not_found_lists_every_path_tried(void)
{
    FlFix f;
    char want[2048];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char cfg[1024];

    flfix_open(&f);
    if (xdg != NULL && xdg[0] != '\0')
        (void)snprintf(cfg, sizeof(cfg), "%s/sagitta", xdg);
    else
        (void)snprintf(cfg, sizeof(cfg), "%s/.config/sagitta",
                       home == NULL ? "" : home);
    /*
     * Every path, one per line.  "cannot find x.fl" without the list is
     * the message that makes a user guess at the search order, and the
     * search order is exactly what they got wrong.
     */
    (void)snprintf(want, sizeof(want),
                   "!import: cannot find 'missing.fl'; tried:\n  %s/missing.fl"
                   "\n  %s/fl/missing.fl", flfix_tmpdir(&f), cfg);
    FL_EQ(&f, "import \"missing.fl\" as m\nreturn 1\n", want);
    flfix_close(&f);
}

void test_fl_modules_load_once_per_realpath(void)
{
    FlFix f;

    flfix_open(&f);
    write_lib(&f);
    /*
     * `./lib.fl` and `lib.fl` are ONE module.  Caching the spelling
     * loads the file twice inside one origin -- two copies of its
     * globals, and a registration list that silently doubles.  Identity
     * is the assertion because equality of two equal-looking maps would
     * pass either way.
     */
    FL_EQ(&f, "import \"lib.fl\" as x\nimport \"./lib.fl\" as y\n"
              "return x == y\n", "true");
    flfix_mkdir(&f, "sub");
    FL_EQ(&f, "import \"lib.fl\" as x\nimport \"sub/../lib.fl\" as y\n"
              "return x == y\n", "true");
    flfix_close(&f);
}

void test_fl_modules_export_every_top_level_name_but_the_private(void)
{
    FlFix f;

    flfix_open(&f);
    write_lib(&f);
    FL_EQ(&f, "import \"lib.fl\" as l\nreturn l.answer\n", "42");
    /* §11: `_`-prefixed top-level names are not exported, and reading
     * one is a miss rather than a nil. */
    FL_EQ(&f, "import \"lib.fl\" as l\nreturn l._private\n",
          "!key: no field '_private' on map");
    /* A module is a value other code holds; `m.x = 1` from outside
     * would rewrite it for every holder. */
    FL_EQ(&f, "import \"lib.fl\" as l\nl.answer = 1\nreturn 1\n",
          "!type: object is frozen");
    flfix_close(&f);
}

void test_fl_modules_keep_their_own_globals(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * A module body runs against its OWN globals, and a function it
     * defines keeps reading them however far from home it is called.
     *
     * Both halves are load-bearing.  With one shared map, `helper`
     * below would overwrite the importer's `helper` the moment the
     * import ran; and with the globals read from the CALLER, `peek`
     * would look `secret` up in the importer's map and find nothing.
     */
    flfix_write(&f, "own.fl",
                "import str\n"
                "let secret = \"inside\"\n"
                "fn peek() { return str.upper(secret) }\n");
    FL_EQ(&f, "let secret = \"outside\"\n"
              "import \"own.fl\" as o\n"
              "return o.peek() + \"/\" + secret\n", "INSIDE/outside");
    flfix_close(&f);
}

void test_fl_modules_a_cycle_names_the_whole_chain(void)
{
    FlFix f;
    char want[2048];
    const char *dir;

    flfix_open(&f);
    dir = flfix_tmpdir(&f);
    flfix_write(&f, "a.fl", "import \"b.fl\" as b\nlet from_a = 1\n");
    flfix_write(&f, "b.fl", "import \"c.fl\" as c\nlet from_b = 2\n");
    flfix_write(&f, "c.fl", "import \"a.fl\" as a\nlet from_c = 3\n");
    /*
     * All four lines, in LOAD ORDER, with the re-entered file at both
     * ends.  "cyclic import of b.fl" is the version nobody can act on:
     * in a tree of any size the question is which file reached it.
     */
    (void)snprintf(want, sizeof(want),
                   "!import: import cycle:\n  %s/a.fl\n  -> %s/b.fl\n"
                   "  -> %s/c.fl\n  -> %s/a.fl", dir, dir, dir, dir);
    FL_EQ(&f, "import \"a.fl\" as a\nreturn 1\n", want);
    flfix_close(&f);
}

void test_fl_modules_deferred_surfaces_name_their_sprint(void)
{
    FlFix f;

    flfix_open(&f);
    /*
     * DoD 11 and the no-silent-stubs rule.  Reporting `bind` as a typo
     * sends the author of a config looking for a spelling mistake that
     * is not there.
     */
    FL_EQ(&f, "return bind\n", "!name: bind lands in Sprint 34");
    FL_EQ(&f, "return set\n", "!name: set lands in Sprint 34");
    FL_EQ(&f, "return on\n", "!name: on lands in Sprint 34");
    FL_EQ(&f, "return win\n", "!name: win lands in Sprint 34");
    /*
     * `buf` LEFT this list when flapi.c registered it.  It is now an
     * ordinary builtin module, so a bare mention is an undefined name --
     * the fix is `import buf`, and saying "lands in Sprint 34" would
     * send the reader away from something that is sitting right there.
     * A deferral that outlives the deferred thing is its own kind of
     * silent stub.
     */
    FL_EQ(&f, "return buf\n", "!name: undefined name 'buf'");
    FL_EQ(&f, "import buf\nreturn buf.current()\n",
          "!handle: no editor: this build of the prompt has none");
    /* And an ordinary typo still reads as one. */
    FL_EQ(&f, "return nope\n", "!name: undefined name 'nope'");
    /* There is no io.run and no io.http in 1.0; the shell and net bits
     * exist for Sprint 54 to prompt for. */
    FL_EQ(&f, "import io\nreturn io.run\n", "!key: no field 'run' on map");
    FL_EQ(&f, "import io\nreturn io.http\n", "!key: no field 'http' on map");
    flfix_close(&f);
}

void test_fl_modules_list_natives_is_deterministic(void)
{
    FlFix f;
    Bytebuf a;
    Bytebuf b;
    u32 na;
    u32 nb;
    size_t i;
    u32 lines = 0U;

    flfix_open(&f);
    bytebuf_init(&a);
    bytebuf_init(&b);
    /*
     * DoD 2's hidden flag.  Sprint 33's coverage ledger DIFFS against
     * this output, so the order has to be a property rather than a
     * habit: the tables are static and walked in registration order,
     * which is spec §11's own listing order.
     */
    na = fl_std_list_natives(&f.vm, &a);
    nb = fl_std_list_natives(&f.vm, &b);
    SAG_ASSERT_EQ_U64((u64)na, (u64)nb);
    SAG_ASSERT_EQ_U64((u64)a.len, (u64)b.len);
    SAG_ASSERT_EQ_I64(memcmp(a.data, b.data, a.len), 0);
    /*
     * §11's order: str first, re last OF THE STDLIB -- and then the
     * editor API after it.  `buf` is spec §4, not §11, so it is
     * appended rather than folded into the stdlib's listing order; the
     * seven keep the order §11 gives them and the tail is what moved.
     */
    SAG_ASSERT_EQ_I64(memcmp(a.data, "str.len\n", 8U), 0);
    SAG_ASSERT_NOT_NULL(strstr((const char *)a.data, "re.escape\n"));
    SAG_ASSERT_EQ_I64(memcmp(a.data + a.len - 9U, "buf.text\n", 9U), 0);
    /* Every line is `module.name`, once. */
    for (i = 0U; i < a.len; i++) {
        if (a.data[i] == (u8)'\n')
            lines++;
    }
    SAG_ASSERT_EQ_U64((u64)lines, (u64)na);
    /*
     * 121 = the stdlib's 117 (30 str + 19 list + 12 map + 29 math +
     * 7 fmt + 13 io + 7 re) plus Sprint 34's 4 on `buf`.  Pinned so a
     * function added or lost shows up here rather than in s33's ledger
     * three sprints later -- which is exactly what happened when the
     * four landed: the conformance gate reported 117/121 natives and
     * named each one missing a COVERS token.
     *
     * s31's DoD 2 asks for 150 and its own tables define 117; see the
     * DoD-walk note.  The number below is the tables, not the target.
     */
    SAG_ASSERT_EQ_U64((u64)na, 121U);
    bytebuf_free(&a);
    bytebuf_free(&b);
    flfix_close(&f);
}
