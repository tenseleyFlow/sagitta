/*
 * Sprint 21 §2 / DoD 4: the smartcase table.
 *
 * The rule that matters is the one about what "has an uppercase
 * literal" means.  The obvious implementation is a strpbrk for A-Z over
 * the pattern text, and it is wrong in a way users hit immediately:
 * `\Wfoo` contains a capital W, contains no capital the user typed as
 * text, and must go on matching `Foo`.  So the non-triggering escapes
 * get rows of their own, and each asserts the actual match rather than
 * only the flag.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "search/searchui.h"
#include "util/arena.h"

typedef struct ScCase {
    bool ignorecase;
    bool smartcase;
    const char *pat;
    bool want_icase;
} ScCase;

static bool sc_icase(const ScCase *c)
{
    Arena arena;
    SagRe *re;
    SearchOpts o;
    bool got;

    arena_init(&arena);
    sag_search_opts_init(&o);
    o.ignorecase = c->ignorecase;
    o.smartcase = c->smartcase;
    re = sag_re_compile(&arena, c->pat, strlen(c->pat), 0U, NULL);
    SAG_ASSERT_NOT_NULL(re);
    got = sag_search_wants_icase(re, &o);
    arena_free_all(&arena);
    return got;
}

/* Does `pat` match `text` under these options, end to end? */
static bool sc_matches(bool ignorecase, bool smartcase, const char *pat,
                       const char *text)
{
    Arena arena;
    SearchOpts o;
    SagRe *re;
    SagReInput in;
    bool hit;

    arena_init(&arena);
    sag_search_opts_init(&o);
    o.ignorecase = ignorecase;
    o.smartcase = smartcase;
    re = sag_search_compile(&arena, pat, strlen(pat), &o, NULL);
    SAG_ASSERT_NOT_NULL(re);
    in = sag_re_input_bytes((const u8 *)text, (u64)strlen(text));
    hit = sag_re_search(re, &in, BYTEOFF(0U), NULL);
    arena_free_all(&arena);
    return hit;
}

void test_search_smartcase_table(void)
{
    static const ScCase rows[] = {
        /* ignorecase off: always case-sensitive, whatever else is set. */
        {false, false, "foo", false},
        {false, true, "foo", false},
        {false, true, "Foo", false},
        /* ignorecase on, smartcase off: always insensitive. */
        {true, false, "foo", true},
        {true, false, "Foo", true},
        /* ignorecase on, smartcase on: the pattern decides. */
        {true, true, "foo", true},
        {true, true, "Foo", false},
        /* \c and \C outrank every combination above. */
        {false, false, "\\cfoo", true},
        {false, true, "\\cFoo", true},
        {true, false, "\\Cfoo", false},
        {true, true, "\\Cfoo", false},
        {true, true, "\\CFoo", false}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        bool got = sc_icase(&rows[i]);

        if (got != rows[i].want_icase)
            (void)fprintf(stderr,
                          "smartcase /%s/ ic=%d sc=%d: wanted %s, got %s\n",
                          rows[i].pat, (int)rows[i].ignorecase,
                          (int)rows[i].smartcase,
                          rows[i].want_icase ? "icase" : "case",
                          got ? "icase" : "case");
        SAG_ASSERT_EQ_U64((u64)got, (u64)rows[i].want_icase);
    }
}

/*
 * DoD 4's other half.  Every one of these contains an uppercase BYTE
 * and none of them is an uppercase literal, so all must stay
 * case-insensitive under ignorecase+smartcase.
 */
void test_search_smartcase_ignores_uppercase_inside_escapes(void)
{
    static const char *const patterns[] = {
        "\\Wfoo",       /* the canonical case                          */
        "\\Sfoo",
        "\\Dfoo",
        "\\x41foo",     /* an uppercase letter, spelled as an escape   */
        "\\Afoo",       /* start-of-text anchor                        */
        "\\bfoo",       /* word boundary                               */
        "[[:upper:]]foo"
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(patterns); i++) {
        ScCase c;

        c.ignorecase = true;
        c.smartcase = true;
        c.pat = patterns[i];
        c.want_icase = true;
        if (!sc_icase(&c))
            (void)fprintf(stderr,
                          "/%s/ flipped to case-sensitive: an uppercase "
                          "byte inside an escape was counted as a "
                          "literal\n", patterns[i]);
        SAG_ASSERT(sc_icase(&c));
    }
}

/* The end-to-end statement of the same rule, which is what a user
 * would actually notice. */
void test_search_smartcase_backslash_w_still_matches_capitalized(void)
{
    /* " Foo" — \W matches the space, foo must match Foo. */
    SAG_ASSERT(sc_matches(true, true, "\\Wfoo", " Foo"));
    /* A real uppercase literal does flip it. */
    SAG_ASSERT(!sc_matches(true, true, "\\WFoo", " foo"));
    SAG_ASSERT(sc_matches(true, true, "\\WFoo", " Foo"));
}

void test_search_smartcase_directives_match_end_to_end(void)
{
    /* \c wins even with ignorecase off and an uppercase literal. */
    SAG_ASSERT(sc_matches(false, true, "\\cFoo", "FOO"));
    SAG_ASSERT(sc_matches(false, true, "\\cfoo", "FOO"));
    /* \C wins even with ignorecase on. */
    SAG_ASSERT(!sc_matches(true, false, "\\Cfoo", "FOO"));
    SAG_ASSERT(sc_matches(true, false, "\\Cfoo", "foo"));
    /* The directive itself consumes no input: /\cfoo/ is three
     * characters wide, not five. */
    SAG_ASSERT(sc_matches(false, false, "\\cfoo", "foo"));
}

void test_search_smartcase_uppercase_is_unicode_not_ascii(void)
{
    ScCase c;

    /* Cyrillic Ж is uppercase and is not in A-Z; an ASCII-only check
     * would leave this insensitive. */
    c.ignorecase = true;
    c.smartcase = true;
    c.pat = "\xD0\x96";
    c.want_icase = false;
    SAG_ASSERT_EQ_U64((u64)sc_icase(&c), 0U);

    /* Its lowercase form does not trip smartcase. */
    c.pat = "\xD0\xB6";
    SAG_ASSERT_EQ_U64((u64)sc_icase(&c), 1U);
}

/* Options default to the sprint's stated values: exact case by default,
 * wrap and highlight on. */
void test_search_opts_defaults(void)
{
    SearchOpts o;

    sag_search_opts_init(&o);
    SAG_ASSERT(!o.ignorecase);
    SAG_ASSERT(o.smartcase);
    SAG_ASSERT(o.wrapscan);
    SAG_ASSERT(o.hlsearch);
    /* Defaults alone mean case-sensitive, whatever the pattern says. */
    SAG_ASSERT(!sc_matches(false, true, "foo", "FOO"));
}
