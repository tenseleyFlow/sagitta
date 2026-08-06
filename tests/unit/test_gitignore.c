/*
 * Sprint 26 §4: the .gitignore matcher.
 *
 * TWO KINDS OF TEST HERE, and the second kind is the point.
 *
 * The supported rows are asserted the ordinary way.  The UNSUPPORTED
 * rows are asserted too — against the behaviour the header documents,
 * not against what git would do.  A feature that silently half-works is
 * worse than one that is absent: it looks right until someone's file
 * goes missing from the finder, and by then nobody remembers that
 * `[[:alpha:]]` was never implemented.  Pinning the divergence means a
 * future change to it has to be deliberate.
 *
 * The pruning tests carry the other risk.  Pruning is what makes a
 * JavaScript checkout usable — one stat instead of 90 000 entries — and
 * it is also the only thing here that can HIDE a file.  So the
 * conservative cases are tested as hard as the aggressive one.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/arena.h"
#include "ws/gitignore.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

typedef struct GiFix {
    Arena a;
} GiFix;

static void gi_make(GiFix *f)
{
    arena_init(&f->a);
}

static void gi_remove(GiFix *f)
{
    arena_free_all(&f->a);
}

static GiSet *gi_rules(GiFix *f, const char *text)
{
    return sag_gi_compile(&f->a, NULL, text, (u64)strlen(text), NULL);
}

/* ---------------------------------------------------------------- */
/* Supported: the ordinary rows                                     */
/* ---------------------------------------------------------------- */

void test_gitignore_blank_lines_and_comments(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f,
                 "\n"
                 "# a comment\n"
                 "   \n"
                 "*.o\n"
                 "\t# indented comment\n");
    SAG_ASSERT(sag_gi_match(g, "a.o", false));
    SAG_ASSERT(!sag_gi_match(g, "a.c", false));
    /* The comment text is not a rule. */
    SAG_ASSERT(!sag_gi_match(g, "a comment", false));
    SAG_ASSERT(!sag_gi_match(g, "# a comment", false));
    gi_remove(&f);
}

/* `\#` is a literal `#`, which is the only way to ignore a file whose
 * name starts with one. */
void test_gitignore_escaped_hash_is_a_literal(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "\\#notacomment\n");
    SAG_ASSERT(sag_gi_match(g, "#notacomment", false));
    SAG_ASSERT(!sag_gi_match(g, "notacomment", false));
    gi_remove(&f);
}

/* Trailing whitespace is stripped — unless it was escaped, which is how
 * you ignore a file whose name really does end in a space. */
void test_gitignore_trailing_whitespace_is_stripped_unless_escaped(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "plain.txt   \n");
    SAG_ASSERT(sag_gi_match(g, "plain.txt", false));
    gi_remove(&f);

    gi_make(&f);
    g = gi_rules(&f, "spaced\\ \n");
    SAG_ASSERT(sag_gi_match(g, "spaced ", false));
    SAG_ASSERT(!sag_gi_match(g, "spaced", false));
    gi_remove(&f);
}

/* A leading `/` anchors to the ignore file's own directory. */
void test_gitignore_leading_slash_anchors(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/build\n");
    SAG_ASSERT(sag_gi_match(g, "build", true));
    /* Anchored, so a `build` further down is NOT ignored. */
    SAG_ASSERT(!sag_gi_match(g, "src/build", true));
    gi_remove(&f);
}

/* Without an anchor, a pattern matches at any depth. */
void test_gitignore_unanchored_matches_any_component(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "build\n");
    SAG_ASSERT(sag_gi_match(g, "build", true));
    SAG_ASSERT(sag_gi_match(g, "src/build", true));
    SAG_ASSERT(sag_gi_match(g, "a/b/c/build", true));
    SAG_ASSERT(!sag_gi_match(g, "rebuild", true));
    gi_remove(&f);
}

/* A trailing `/` matches directories only. */
void test_gitignore_trailing_slash_is_directory_only(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "logs/\n");
    SAG_ASSERT(sag_gi_match(g, "logs", true));
    /* A FILE named `logs` is not ignored by `logs/`. */
    SAG_ASSERT(!sag_gi_match(g, "logs", false));
    gi_remove(&f);
}

/* `*` matches a run within one segment, never across `/`. */
void test_gitignore_star_does_not_cross_a_slash(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/src/*.c\n");
    SAG_ASSERT(sag_gi_match(g, "src/main.c", false));
    /* One level deeper: `*` cannot reach it. */
    SAG_ASSERT(!sag_gi_match(g, "src/ui/draw.c", false));
    gi_remove(&f);
}

/* `?` is exactly one byte, and not a separator. */
void test_gitignore_question_matches_one_byte(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/a?.txt\n");
    SAG_ASSERT(sag_gi_match(g, "ab.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "a.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "abc.txt", false));
    gi_remove(&f);

    gi_make(&f);
    g = gi_rules(&f, "/a?b\n");
    SAG_ASSERT(!sag_gi_match(g, "a/b", false));
    gi_remove(&f);
}

void test_gitignore_bracket_sets(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/f[abc].txt\n");
    SAG_ASSERT(sag_gi_match(g, "fa.txt", false));
    SAG_ASSERT(sag_gi_match(g, "fc.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fd.txt", false));
    gi_remove(&f);

    gi_make(&f);
    g = gi_rules(&f, "/f[a-z].txt\n");
    SAG_ASSERT(sag_gi_match(g, "fm.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fM.txt", false));
    gi_remove(&f);

    /* Both spellings of negation. */
    gi_make(&f);
    g = gi_rules(&f, "/f[!a-z].txt\n");
    SAG_ASSERT(sag_gi_match(g, "fM.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fm.txt", false));
    gi_remove(&f);

    gi_make(&f);
    g = gi_rules(&f, "/f[^a-z].txt\n");
    SAG_ASSERT(sag_gi_match(g, "f0.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fq.txt", false));
    gi_remove(&f);
}

/* Double-star crosses separators, in all three positions. */
void test_gitignore_double_star(void)
{
    GiFix f;
    GiSet *g;

    /* Prefix: at any depth. */
    gi_make(&f);
    g = gi_rules(&f, "**/target\n");
    SAG_ASSERT(sag_gi_match(g, "target", true));
    SAG_ASSERT(sag_gi_match(g, "a/target", true));
    SAG_ASSERT(sag_gi_match(g, "a/b/target", true));
    gi_remove(&f);

    /* Suffix: everything beneath. */
    gi_make(&f);
    g = gi_rules(&f, "/logs/**\n");
    SAG_ASSERT(sag_gi_match(g, "logs/a.txt", false));
    SAG_ASSERT(sag_gi_match(g, "logs/a/b/c.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "other/a.txt", false));
    gi_remove(&f);

    /* Interior: any number of segments between. */
    gi_make(&f);
    g = gi_rules(&f, "/a/**/b\n");
    SAG_ASSERT(sag_gi_match(g, "a/b", false));
    SAG_ASSERT(sag_gi_match(g, "a/x/b", false));
    SAG_ASSERT(sag_gi_match(g, "a/x/y/z/b", false));
    SAG_ASSERT(!sag_gi_match(g, "a/x/c", false));
    gi_remove(&f);
}

/* Negation, and the last matching rule winning. */
void test_gitignore_negation_last_match_wins(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "*.log\n!keep.log\n");
    SAG_ASSERT(sag_gi_match(g, "a.log", false));
    SAG_ASSERT(!sag_gi_match(g, "keep.log", false));
    gi_remove(&f);

    /* ORDER matters: reversing the two makes everything ignored, which
     * is the whole reason the rule is "last match wins" rather than
     * "any negation wins". */
    gi_make(&f);
    g = gi_rules(&f, "!keep.log\n*.log\n");
    SAG_ASSERT(sag_gi_match(g, "a.log", false));
    SAG_ASSERT(sag_gi_match(g, "keep.log", false));
    gi_remove(&f);
}

/* A nested .gitignore is consulted after the ones above it, so it can
 * override them. */
void test_gitignore_nested_sets_deepest_wins(void)
{
    GiFix f;
    GiSet *root;
    GiSet *nested;

    gi_make(&f);
    root = sag_gi_compile(&f.a, NULL, "*.log\n", 6U, NULL);
    nested = sag_gi_compile(&f.a, "sub", "!important.log\n", 15U, root);
    /* The root rule still applies elsewhere... */
    SAG_ASSERT(sag_gi_match(nested, "a.log", false));
    SAG_ASSERT(sag_gi_match(nested, "sub/other.log", false));
    /* ...and the deeper file re-includes only inside its own tree. */
    SAG_ASSERT(!sag_gi_match(nested, "sub/important.log", false));
    gi_remove(&f);
}

/* A nested set's anchored rules anchor to ITS directory, not the root. */
void test_gitignore_nested_anchoring_is_relative_to_its_directory(void)
{
    GiFix f;
    GiSet *root;
    GiSet *nested;

    gi_make(&f);
    root = sag_gi_compile(&f.a, NULL, "\n", 1U, NULL);
    nested = sag_gi_compile(&f.a, "sub", "/build\n", 7U, root);
    SAG_ASSERT(sag_gi_match(nested, "sub/build", true));
    /* Not the root's `build`, and not a deeper one. */
    SAG_ASSERT(!sag_gi_match(nested, "build", true));
    SAG_ASSERT(!sag_gi_match(nested, "sub/x/build", true));
    gi_remove(&f);
}

/* CRLF is not a broken file. */
void test_gitignore_crlf_lines(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "*.o\r\nbuild/\r\n");
    SAG_ASSERT(sag_gi_match(g, "a.o", false));
    SAG_ASSERT(sag_gi_match(g, "build", true));
    gi_remove(&f);
}

/* ---------------------------------------------------------------- */
/* UNSUPPORTED: the documented divergences                          */
/* ---------------------------------------------------------------- */

/*
 * `[[:alpha:]]` is NOT a POSIX class here.
 *
 * The bracket set closes at the FIRST `]`, so the pattern reads as a
 * set of the bytes `[ : a l p h` followed by a LITERAL `]` — which
 * means `f[[:alpha:]].txt` matches `fa].txt`, a filename nobody has.
 *
 * That is a strange answer, and writing it down is the point.  The
 * first version of this test asserted `fa.txt` because that is what I
 * assumed a byte-set reading would do; the implementation disagreed,
 * and it was the implementation that was right about its own rules.
 * Pinning the real behaviour means implementing classes later is a
 * deliberate change with a failing test to update, rather than a silent
 * swap under anyone relying on today's answer.
 */
void test_gitignore_posix_classes_are_not_supported(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/f[[:alpha:]].txt\n");
    /* A real POSIX class would match every letter.  None of these. */
    SAG_ASSERT(!sag_gi_match(g, "fz.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fa.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "fp.txt", false));
    /* What it actually does: one byte from the set, then a literal `]`. */
    SAG_ASSERT(sag_gi_match(g, "fa].txt", false));
    SAG_ASSERT(sag_gi_match(g, "f:].txt", false));
    SAG_ASSERT(sag_gi_match(g, "f[].txt", false));
    /* And a byte outside the set still does not match. */
    SAG_ASSERT(!sag_gi_match(g, "fz].txt", false));
    gi_remove(&f);
}

/*
 * Matching is always CASE-SENSITIVE, whatever the filesystem says.
 *
 * core.ignorecase would need to know the filesystem's behaviour, and
 * being wrong in either direction hides files.
 */
void test_gitignore_matching_is_case_sensitive(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "*.log\n");
    SAG_ASSERT(sag_gi_match(g, "a.log", false));
    /* git with core.ignorecase=true would ignore this one; we do not. */
    SAG_ASSERT(!sag_gi_match(g, "a.LOG", false));
    SAG_ASSERT(!sag_gi_match(g, "A.Log", false));
    gi_remove(&f);
}

/*
 * A TRACKED file is still ignored here.
 *
 * git never ignores a file it has in the index; we have no index
 * reader, so a tracked-but-ignored file appears in the finder.  This is
 * the most user-visible divergence in the whole matcher, and the
 * assertion exists to say so out loud rather than to endorse it.
 */
void test_gitignore_has_no_index_awareness(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "config.h\n");
    /* Even if `config.h` were `git add`ed, this still reports ignored:
     * the matcher is a pure function of the rules and the path. */
    SAG_ASSERT(sag_gi_match(g, "config.h", false));
    gi_remove(&f);
}

/* An unterminated `[` is a literal `[`, not a parse error and not a
 * dropped rule. */
void test_gitignore_unterminated_bracket_is_a_literal(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "/a[bc.txt\n");
    SAG_ASSERT(sag_gi_match(g, "a[bc.txt", false));
    SAG_ASSERT(!sag_gi_match(g, "ab.txt", false));
    gi_remove(&f);
}

/* ---------------------------------------------------------------- */
/* §4.1: pruning                                                    */
/* ---------------------------------------------------------------- */

/*
 * The aggressive case: an ignored directory with no negation anywhere
 * is prunable, which is what turns node_modules from 90 000 entries
 * into one stat.
 */
void test_gitignore_prunable_when_nothing_could_re_include(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "node_modules/\n*.o\n");
    SAG_ASSERT(sag_gi_match(g, "node_modules", true));
    SAG_ASSERT(sag_gi_prunable(g, "node_modules"));
    gi_remove(&f);
}

/*
 * The conservative case: a negation reaching INSIDE the directory
 * blocks the prune, because pruning would hide `keep.js`.
 *
 * git's own rule says a file under an excluded directory cannot be
 * re-included — so this negation is arguably dead — but acting on that
 * means reasoning about git's precedence rather than about our own
 * rules, and being wrong hides a file. Descend instead.
 */
void test_gitignore_not_prunable_when_a_negation_reaches_inside(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "node_modules/\n!node_modules/keep.js\n");
    SAG_ASSERT(sag_gi_match(g, "node_modules", true));
    SAG_ASSERT(!sag_gi_prunable(g, "node_modules"));
    gi_remove(&f);
}

/* An UNANCHORED negation could name something at any depth, so it
 * blocks pruning everywhere. */
void test_gitignore_unanchored_negation_blocks_pruning(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "build/\n!keep.txt\n");
    SAG_ASSERT(sag_gi_match(g, "build", true));
    SAG_ASSERT(!sag_gi_prunable(g, "build"));
    gi_remove(&f);
}

/* A negation beginning with a wildcard could match anything. */
void test_gitignore_wildcard_negation_blocks_pruning(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "vendor/\n!*.md\n");
    SAG_ASSERT(!sag_gi_prunable(g, "vendor"));
    gi_remove(&f);
}

/* A directory that is not ignored at all is never prunable — pruning is
 * a decision about ignored subtrees only. */
void test_gitignore_unignored_directory_is_not_prunable(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    g = gi_rules(&f, "node_modules/\n");
    SAG_ASSERT(!sag_gi_prunable(g, "src"));
    gi_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Loading from disk                                                */
/* ---------------------------------------------------------------- */

void test_gitignore_load_reads_a_file(void)
{
    GiFix f;
    GiSet *g;
    char dir[128];
    char path[256];
    FILE *fp;

    gi_make(&f);
    (void)snprintf(dir, sizeof(dir), "/tmp/sag-gi-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(dir));
    SAG_ASSERT(snprintf(path, sizeof(path), "%s/.gitignore", dir) > 0);
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("*.tmp\nbuild/\n", fp);
    (void)fclose(fp);

    g = sag_gi_load(&f.a, dir, NULL);
    SAG_ASSERT_NOT_NULL(g);
    SAG_ASSERT(sag_gi_match(g, "x.tmp", false));
    SAG_ASSERT(sag_gi_match(g, "build", true));
    SAG_ASSERT(!sag_gi_match(g, "x.c", false));

    (void)unlink(path);
    (void)rmdir(dir);
    gi_remove(&f);
}

/*
 * No ignore file returns the PARENT unchanged, so a deep tree of
 * directories without one costs no allocation and no chain depth.
 */
void test_gitignore_absent_file_returns_the_parent(void)
{
    GiFix f;
    GiSet *parent;
    GiSet *g;
    char dir[128];

    gi_make(&f);
    (void)snprintf(dir, sizeof(dir), "/tmp/sag-gi-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(dir));
    parent = gi_rules(&f, "*.o\n");
    g = sag_gi_load(&f.a, dir, parent);
    SAG_ASSERT(g == parent);
    /* And a set whose every line is a comment is not a set either. */
    SAG_ASSERT(sag_gi_compile(&f.a, NULL, "# nothing\n", 10U, parent) ==
               parent);
    (void)rmdir(dir);
    gi_remove(&f);
}

/* Degenerate inputs do not crash. */
void test_gitignore_degenerate_inputs(void)
{
    GiFix f;
    GiSet *g;

    gi_make(&f);
    SAG_ASSERT_NULL(sag_gi_load(&f.a, NULL, NULL));
    SAG_ASSERT_NULL(sag_gi_compile(&f.a, NULL, NULL, 0U, NULL));
    SAG_ASSERT(!sag_gi_match(NULL, "a.txt", false));
    SAG_ASSERT(!sag_gi_prunable(NULL, "a"));
    g = gi_rules(&f, "*.o\n");
    SAG_ASSERT(!sag_gi_match(g, "", false));
    SAG_ASSERT(!sag_gi_match(g, NULL, false));
    SAG_ASSERT(!sag_gi_prunable(g, ""));
    gi_remove(&f);
}
