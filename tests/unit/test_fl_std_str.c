/*
 * Sprint 31 deliverable 2: the `str` module.
 *
 * COVERAGE  (function -> test; every row of §2's table, every error kind)
 * --------
 * str.len          clusters       str_indexes_by_grapheme_cluster
 * str.len_bytes                   str_indexes_by_grapheme_cluster
 * str.is_empty                    str_indexes_by_grapheme_cluster
 * str.at           "index"        str_indexes_by_grapheme_cluster
 * str.width        cells          str_indexes_by_grapheme_cluster
 * str.slice        clusters       str_slices_by_cluster
 * str.slice_bytes  "index"        str_slice_bytes_refuses_a_cluster_split
 * str.find         clusters, -1   str_searches_and_tests
 * str.find_bytes   bytes, -1      str_searches_and_tests
 * str.rfind                       str_searches_and_tests
 * str.contains                    str_searches_and_tests
 * str.starts_with                 str_searches_and_tests
 * str.ends_with                   str_searches_and_tests
 * str.upper        Unicode        str_case_and_trim_keep_invalid_bytes
 * str.lower                       str_case_and_trim_keep_invalid_bytes
 * str.trim                        str_case_and_trim_keep_invalid_bytes
 * str.trim_start                  str_case_and_trim_keep_invalid_bytes
 * str.trim_end                    str_case_and_trim_keep_invalid_bytes
 * str.split        limit          str_splits_and_joins
 * str.split_lines  CRLF, trailing str_splits_and_joins
 * str.join                        str_splits_and_joins
 * str.replace      limit          str_builds_new_strings
 * str.repeat       "limit"        str_builds_new_strings
 * str.pad_start    cells, fill    str_builds_new_strings
 * str.pad_end                     str_builds_new_strings
 * str.bytes                       str_converts_to_and_from
 * str.from_bytes   "index"        str_converts_to_and_from
 * str.to_int       base, nil      str_converts_to_and_from
 * str.to_float     nil            str_converts_to_and_from
 * str.cmp          bytewise       str_converts_to_and_from
 * the s02 corpus, cluster law    str_the_s02_corpus_never_splits_a_cluster
 * the s02 corpus, byte round trip str_the_s02_corpus_never_splits_a_cluster
 */
#include "flfix.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P "import str\nimport fmt\n"

/* A ZWJ family: four emoji joined into ONE cluster of eleven codepoints
 * and 25 bytes.  The corpus case for everything cluster-shaped. */
#define FAMILY "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d" \
               "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6"

void test_fl_str_indexes_by_grapheme_cluster(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return str.len(\"abc\")\n", "3");
    FL_EQ(&f, P "return str.len(\"\")\n", "0");
    /*
     * INDEX 0 IS THE FIRST THING A USER CAN SEE.  A ZWJ family is one
     * cluster however many codepoints it is made of, and a user who
     * writes str.at(s, 0) means the family, not a quarter of it.
     */
    FL_EQ(&f, P "return str.len(\"" FAMILY "x\")\n", "2");
    FL_EQ(&f, P "return str.len_bytes(\"" FAMILY "x\")\n", "26");
    FL_EQ(&f, P "return str.at(\"" FAMILY "x\", 0)\n", FAMILY);
    FL_EQ(&f, P "return str.at(\"abc\", 1)\n", "b");
    FL_EQ(&f, P "return str.at(\"abc\", 9)\n",
          "!index: str.at: index 9 out of range");
    FL_EQ(&f, P "return str.is_empty(\"\")\n", "true");
    FL_EQ(&f, P "return str.is_empty(\"a\")\n", "false");
    /* Width is DISPLAY CELLS: the family is one cluster of width 2, and
     * two ideographs are four columns. */
    FL_EQ(&f, P "return str.width(\"" FAMILY "\")\n", "2");
    FL_EQ(&f, P "return str.width(\"\\u{6F22}\\u{5B57}\")\n", "4");
    FL_EQ(&f, P "return str.width(\"abc\")\n", "3");
    flfix_close(&f);
}

void test_fl_str_slices_by_cluster(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return str.slice(\"abcd\", 1, 3)\n", "bc");
    FL_EQ(&f, P "return str.slice(\"abcd\", 2)\n", "cd");
    FL_EQ(&f, P "return str.slice(\"abcd\", 2, 2)\n", "");
    /* One cluster out means the WHOLE family, all 25 bytes of it. */
    FL_EQ(&f, P "return str.slice(\"" FAMILY "x\", 0, 1)\n", FAMILY);
    FL_EQ(&f, P "return str.slice(\"" FAMILY "x\", 1)\n", "x");
    flfix_close(&f);
}

void test_fl_str_slice_bytes_refuses_a_cluster_split(void)
{
    FlFix f;
    char src[512];
    int i;

    flfix_open(&f);
    /* The honest byte API still refuses to split a cluster. */
    FL_EQ(&f, P "return str.slice_bytes(\"abcd\", 1, 3)\n", "bc");
    FL_EQ(&f, P "return str.slice_bytes(\"" FAMILY "\", 0, 25)\n", FAMILY);
    /*
     * EVERY interior byte of the family, refused by name.
     *
     * A byte-indexed slice that splits a cluster produces a string that
     * still "works" and renders as a broken accent three screens later,
     * with nothing in between to blame -- so the message teaches the
     * fix rather than merely failing.
     */
    for (i = 1; i < 25; i++) {
        char want[128];

        (void)snprintf(src, sizeof(src),
                       P "return str.slice_bytes(\"" FAMILY "\", 0, %d)\n", i);
        /* The sprint pins this wording: it names the offset and the
         * rule, so the message teaches the fix. */
        (void)snprintf(want, sizeof(want),
                       "!index: str.slice_bytes: offset %d splits a "
                       "grapheme cluster", i);
        FL_EQ(&f, src, want);
    }
    flfix_close(&f);
}

void test_fl_str_searches_and_tests(void)
{
    FlFix f;

    flfix_open(&f);
    /* CLUSTER indices out of find, byte offsets out of find_bytes --
     * the names are the whole difference and each says which. */
    FL_EQ(&f, P "return str.find(\"" FAMILY "x\", \"x\")\n", "1");
    FL_EQ(&f, P "return str.find_bytes(\"" FAMILY "x\", \"x\")\n", "25");
    FL_EQ(&f, P "return str.find(\"abcabc\", \"b\")\n", "1");
    FL_EQ(&f, P "return str.find(\"abcabc\", \"b\", 2)\n", "4");
    FL_EQ(&f, P "return str.find(\"abc\", \"z\")\n", "-1");
    FL_EQ(&f, P "return str.find_bytes(\"abc\", \"z\")\n", "-1");
    FL_EQ(&f, P "return str.rfind(\"abcabc\", \"b\")\n", "4");
    FL_EQ(&f, P "return str.rfind(\"abc\", \"z\")\n", "-1");
    FL_EQ(&f, P "return str.contains(\"abc\", \"bc\")\n", "true");
    FL_EQ(&f, P "return str.contains(\"abc\", \"cb\")\n", "false");
    FL_EQ(&f, P "return str.starts_with(\"abc\", \"ab\")\n", "true");
    FL_EQ(&f, P "return str.starts_with(\"abc\", \"bc\")\n", "false");
    FL_EQ(&f, P "return str.ends_with(\"abc\", \"bc\")\n", "true");
    FL_EQ(&f, P "return str.ends_with(\"abc\", \"ab\")\n", "false");
    /* The empty needle is found at the start, which is what every
     * substring search agrees on and what keeps find/slice composable. */
    FL_EQ(&f, P "return str.find(\"abc\", \"\")\n", "0");
    flfix_close(&f);
}

void test_fl_str_case_and_trim_keep_invalid_bytes(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return str.upper(\"aBc\")\n", "ABC");
    FL_EQ(&f, P "return str.lower(\"AbC\")\n", "abc");
    /* Full Unicode case mapping, so the sharp s grows a letter. */
    FL_EQ(&f, P "return str.upper(\"stra\\u{00DF}e\")\n", "STRASSE");
    FL_EQ(&f, P "return str.lower(\"\\u{0130}\") != \"\"\n", "true");
    FL_EQ(&f, P "return str.trim(\"  a b  \")\n", "a b");
    FL_EQ(&f, P "return str.trim_start(\"  a  \")\n", "a  ");
    FL_EQ(&f, P "return str.trim_end(\"  a  \")\n", "  a");
    FL_EQ(&f, P "return str.trim(\"\\t\\n a \\r\\n\")\n", "a");
    /*
     * INVALID BYTES SURVIVE EVERYTHING, byte for byte.  A string
     * builtin that "cleans" its input is one that loses data, and a
     * path that is not UTF-8 is still a path.
     */
    FL_EQ(&f, P "return str.upper(\"a\\x80b\")\n", "A\x80" "B");
    FL_EQ(&f, P "return str.lower(\"A\\x80B\")\n", "a\x80" "b");
    FL_EQ(&f, P "return str.trim(\" \\x80 \")\n", "\x80");
    FL_EQ(&f, P "return str.len_bytes(str.upper(\"\\xff\\xfe\"))\n", "2");
    flfix_close(&f);
}

void test_fl_str_splits_and_joins(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.repr(str.split(\"a,b,c\", \",\"))\n",
          "[\"a\", \"b\", \"c\"]");
    FL_EQ(&f, P "return fmt.repr(str.split(\"a,b,c\", \",\", 2))\n",
          "[\"a\", \"b,c\"]");
    FL_EQ(&f, P "return fmt.repr(str.split(\"abc\", \",\"))\n", "[\"abc\"]");
    /*
     * A trailing terminator produces a final empty line, and one \r per
     * line is dropped so a CRLF file reads the same as an LF one.  This
     * is the ONE definition of line splitting; io.read_lines is
     * literally this function applied to a file's bytes.
     */
    FL_EQ(&f, P "return fmt.repr(str.split_lines(\"a\\nb\\n\"))\n",
          "[\"a\", \"b\", \"\"]");
    FL_EQ(&f, P "return fmt.repr(str.split_lines(\"a\\r\\nb\\r\\n\"))\n",
          "[\"a\", \"b\", \"\"]");
    FL_EQ(&f, P "return fmt.repr(str.split_lines(\"a\\nb\"))\n",
          "[\"a\", \"b\"]");
    /* Only the terminator is ours to remove: a line ending "\r\r"
     * keeps the first one. */
    FL_EQ(&f, P "return str.len_bytes(str.split_lines(\"a\\r\\r\\n\")[0])\n",
          "2");
    FL_EQ(&f, P "return str.join([\"a\", \"b\"], \"-\")\n", "a-b");
    FL_EQ(&f, P "return str.join([], \"-\")\n", "");
    FL_EQ(&f, P "return str.join(str.split(\"a-b-c\", \"-\"), \"-\")\n",
          "a-b-c");
    flfix_close(&f);
}

void test_fl_str_builds_new_strings(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return str.replace(\"aXbXc\", \"X\", \"-\")\n", "a-b-c");
    FL_EQ(&f, P "return str.replace(\"aXbXc\", \"X\", \"-\", 1)\n", "a-bXc");
    FL_EQ(&f, P "return str.replace(\"abc\", \"z\", \"-\")\n", "abc");
    FL_EQ(&f, P "return str.repeat(\"ab\", 3)\n", "ababab");
    FL_EQ(&f, P "return str.repeat(\"ab\", 0)\n", "");
    /* The 64 MiB cap, which is where a repeat stops being a formatting
     * request and starts being an out-of-memory. */
    FL_EQ(&f, P "return str.repeat(\"ab\", 99999999)\n",
          "!limit: str.repeat: result exceeds 64 MiB");
    /* Padding is in CELLS, so a wide character counts for two. */
    FL_EQ(&f, P "return str.pad_start(\"ab\", 5)\n", "   ab");
    FL_EQ(&f, P "return str.pad_end(\"ab\", 5, \".\")\n", "ab...");
    FL_EQ(&f, P "return str.pad_start(\"\\u{6F22}\", 4, \".\")\n",
          "..\xe6\xbc\xa2");
    FL_EQ(&f, P "return str.pad_end(\"abcd\", 2)\n", "abcd");
    flfix_close(&f);
}

void test_fl_str_converts_to_and_from(void)
{
    FlFix f;

    flfix_open(&f);
    FL_EQ(&f, P "return fmt.repr(str.bytes(\"ab\"))\n", "[97, 98]");
    FL_EQ(&f, P "return str.from_bytes([97, 98])\n", "ab");
    /* Round-trips an invalid byte, which is the reason these two exist
     * rather than pretending to be about text. */
    FL_EQ(&f, P "return str.len_bytes(str.from_bytes(str.bytes(\"a\\x80\")))\n",
          "2");
    FL_EQ(&f, P "return str.from_bytes([999])\n",
          "!index: str.from_bytes: item 0 is not a byte");
    FL_EQ(&f, P "return str.to_int(\"42\")\n", "42");
    FL_EQ(&f, P "return str.to_int(\"ff\", 16)\n", "255");
    FL_EQ(&f, P "return str.to_int(\"-7\")\n", "-7");
    /* nil rather than a raise: "is this a number" is a question, and
     * asking it should not need a try. */
    FL_EQ(&f, P "return str.to_int(\"nope\")\n", "nil");
    FL_EQ(&f, P "return str.to_int(\"12x\")\n", "nil");
    FL_EQ(&f, P "return fmt.str(str.to_float(\"1.5\"))\n", "1.5");
    FL_EQ(&f, P "return str.to_float(\"nope\")\n", "nil");
    /* Bytewise, so it is a total order and not a locale's opinion. */
    FL_EQ(&f, P "return str.cmp(\"a\", \"b\")\n", "-1");
    FL_EQ(&f, P "return str.cmp(\"b\", \"a\")\n", "1");
    FL_EQ(&f, P "return str.cmp(\"a\", \"a\")\n", "0");
    FL_EQ(&f, P "return str.cmp(\"a\", \"ab\")\n", "-1");
    flfix_close(&f);
}

/* Every byte as \xNN, so arbitrary bytes -- NULs and invalid sequences
 * included -- can be written into a Fletch source string. */
static void escape_bytes(char *out, size_t cap, const u8 *b, size_t n)
{
    size_t i;
    size_t at = 0U;

    for (i = 0U; i < n && at + 5U < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, "\\x%02x", (unsigned)b[i]);
    out[at] = '\0';
}

/*
 * DoD 4, over the WHOLE s02 golden corpus rather than one family.
 *
 * Two laws, per case:
 *
 *   - str.len is the cluster count, and str.slice_bytes succeeds at
 *     exactly the cluster boundaries and raises "index" at every other
 *     offset.  A byte-indexed slice that splits a cluster produces a
 *     string that still "works" and renders as a broken glyph three
 *     screens later; the corpus is where the shapes that do it live.
 *   - the byte APIs round-trip every case exactly, which is the
 *     invalid-byte promise in the form that holds for ALL input.
 *     (upper/lower legitimately rewrite valid cased bytes, so their
 *     invalid-byte behaviour is asserted on targeted cases in
 *     str_case_and_trim_keep_invalid_bytes rather than here.)
 */
void test_fl_str_the_s02_corpus_never_splits_a_cluster(void)
{
    FlFix f;
    FILE *fp = fopen("tests/unit/fixtures/unicode/corpus.txt", "r");
    char line[2048];
    size_t cases = 0U;

    SAG_ASSERT_NOT_NULL(fp);
    flfix_open(&f);
    while (fgets(line, sizeof(line), fp) != NULL) {
        u8 bytes[256];
        size_t bounds[256];
        char esc[2048];
        char src[8192];
        char want[256];
        char *p = line;
        char *hex;
        char *lens;
        size_t nbytes = 0U;
        size_t nclusters = 0U;
        size_t at = 0U;
        size_t off;

        while (isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;
        hex = strchr(p, '|');
        if (hex == NULL)
            continue;
        *hex++ = '\0';
        lens = strchr(hex, '|');
        if (lens == NULL)
            continue;
        *lens++ = '\0';
        {
            char *end = strchr(lens, '|');

            if (end != NULL)
                *end = '\0';
        }
        /* The bytes. */
        {
            char *tok = strtok(hex, " \t\n");

            while (tok != NULL && nbytes < sizeof(bytes)) {
                bytes[nbytes++] = (u8)strtoul(tok, NULL, 16);
                tok = strtok(NULL, " \t\n");
            }
        }
        /* The cluster boundaries, as cumulative byte offsets. */
        {
            char *tok = strtok(lens, ", \t\n");

            while (tok != NULL && nclusters < SAG_ARRAY_LEN(bounds)) {
                at += (size_t)strtoul(tok, NULL, 10);
                bounds[nclusters++] = at;
                tok = strtok(NULL, ", \t\n");
            }
        }
        if (nbytes == 0U || at != nbytes)
            continue;          /* a malformed row is the corpus's problem */
        escape_bytes(esc, sizeof(esc), bytes, nbytes);

        (void)snprintf(src, sizeof(src),
                       "import str\nreturn str.len(\"%s\")\n", esc);
        (void)snprintf(want, sizeof(want), "%zu", nclusters);
        FL_EQ(&f, src, want);

        /* The byte APIs carry every case through unchanged. */
        (void)snprintf(src, sizeof(src),
                       "import str\n"
                       "return str.from_bytes(str.bytes(\"%s\")) == \"%s\"\n",
                       esc, esc);
        FL_EQ(&f, src, "true");

        /* Boundaries succeed; everything between them is refused. */
        for (off = 1U; off < nbytes; off++) {
            bool is_bound = false;
            size_t i;

            for (i = 0U; i < nclusters; i++) {
                if (bounds[i] == off)
                    is_bound = true;
            }
            (void)snprintf(src, sizeof(src),
                           "import str\n"
                           "return str.len_bytes(str.slice_bytes(\"%s\", 0, "
                           "%zu))\n", esc, off);
            if (is_bound) {
                (void)snprintf(want, sizeof(want), "%zu", off);
            } else {
                (void)snprintf(want, sizeof(want),
                               "!index: str.slice_bytes: offset %zu splits a "
                               "grapheme cluster", off);
            }
            FL_EQ(&f, src, want);
        }
        cases++;
    }
    (void)fclose(fp);
    /* A corpus that stopped being read would pass every assertion it
     * never made. */
    SAG_ASSERT(cases >= 20U);
    flfix_close(&f);
}
