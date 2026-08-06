/*
 * Sprint 25 §9 / DoD 13 — and SPRINT 36'S NAMED DoD ITEM.
 *
 * Sprint 36 replaces src/ws/fl_parse.c with the Fletch VM's data path.
 * When it does, the question that matters is not "does the new parser
 * work" but "does it agree with the old one about every byte of the
 * frozen corpus".  This file asks exactly that, and it exists NOW so
 * that s36 turns on a test that already passes rather than writing one
 * against an implementation it is halfway through replacing — which is
 * how a differential test ends up encoding the new behaviour instead of
 * checking it.
 *
 * ==================================================================
 * SPRINT 36: this is your DoD item.  Define SAG_HAVE_FLETCH_STATE and
 * implement sag_fl_parse_fletch() with the same signature as
 * sag_fl_parse().  Everything below then compares the two arms over
 * every corpus document.  Do not weaken an assertion to make it pass;
 * a disagreement here is a format bug in one of the two parsers, and
 * finding out which is the whole point.
 * ==================================================================
 *
 * Until then both arms are the hand-written parser.  That is not a
 * tautology dressed up as a test: it pins the COMPARISON — the
 * structural walk, the byte comparison, the corpus enumeration — so the
 * only thing s36 has to add is a second parse function.  It also
 * catches a real class of bug today, since `parse` is called twice on
 * independent arenas and any state leaking between them shows up here.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/arena.h"
#include "util/buf.h"
#include "util/sort.h"
#include "ws/fllit.h"
#include "ws/state.h"

#define DIFF_CORPUS_DIR "tests/unit/fixtures/wsstate/v1"

/*
 * The second arm.
 *
 * Sprint 36 replaces this body with the Fletch VM data path.  The
 * signature is sag_fl_parse's on purpose: a differential test whose two
 * arms have different shapes ends up comparing the adapters.
 */
#ifdef SAG_HAVE_FLETCH_STATE
extern FlLit *sag_fl_parse_fletch(Arena *a, const u8 *src, u64 len,
                                  FlParseErr *err);
#define DIFF_ARM_B sag_fl_parse_fletch
#define DIFF_ARM_B_NAME "fletch"
#else
#define DIFF_ARM_B sag_fl_parse
#define DIFF_ARM_B_NAME "hand-written (Sprint 36 replaces this arm)"
#endif

/* ---------------------------------------------------------------- */
/* Structural equality                                              */
/* ---------------------------------------------------------------- */

/*
 * Deep equality over the six value kinds, INCLUDING key order.
 *
 * Order is part of the comparison because it is part of the format:
 * maps are insertion-ordered arrays precisely so re-emission is
 * deterministic (fllit.h).  Two parsers that agreed on contents but not
 * on order would produce different bytes from the same document and
 * every golden would disagree on whichever machine ran which.
 */
static bool lit_eq(const FlLit *a, const FlLit *b, char *why, size_t cap)
{
    u32 i;

    if (a == NULL || b == NULL) {
        if (a == b)
            return true;
        (void)snprintf(why, cap, "one side is NULL");
        return false;
    }
    if (a->kind != b->kind) {
        (void)snprintf(why, cap, "kind %d vs %d", (int)a->kind,
                       (int)b->kind);
        return false;
    }
    switch (a->kind) {
    case FL_NIL:
        return true;
    case FL_BOOL:
    case FL_INT:
        if (a->i != b->i) {
            (void)snprintf(why, cap, "%lld vs %lld", (long long)a->i,
                           (long long)b->i);
            return false;
        }
        return true;
    case FL_STR:
        if (a->slen != b->slen) {
            (void)snprintf(why, cap, "string length %llu vs %llu",
                           (unsigned long long)a->slen,
                           (unsigned long long)b->slen);
            return false;
        }
        /* memcmp, not strcmp: these are BYTES, and a path may hold a
         * NUL or an invalid UTF-8 sequence (invariant 2). */
        if (a->slen != 0U && memcmp(a->s, b->s, (size_t)a->slen) != 0) {
            (void)snprintf(why, cap, "string bytes differ");
            return false;
        }
        return true;
    case FL_LIST:
    case FL_MAP:
    default:
        if (a->len != b->len) {
            (void)snprintf(why, cap, "%u vs %u elements", (unsigned)a->len,
                           (unsigned)b->len);
            return false;
        }
        for (i = 0U; i < a->len; i++) {
            if (a->kind == FL_MAP) {
                if (a->keylens[i] != b->keylens[i] ||
                    (a->keylens[i] != 0U &&
                     memcmp(a->keys[i], b->keys[i],
                            (size_t)a->keylens[i]) != 0)) {
                    (void)snprintf(why, cap, "key %u differs", (unsigned)i);
                    return false;
                }
            }
            if (!lit_eq(a->items[i], b->items[i], why, cap))
                return false;
        }
        return true;
    }
}

/* ---------------------------------------------------------------- */
/* Corpus enumeration                                               */
/* ---------------------------------------------------------------- */

typedef struct DiffList {
    char names[64][64];
    u32 n;
} DiffList;

static int diff_name_cmp(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    return strcmp((const char *)a, (const char *)b);
}

static void diff_list(const char *dir, DiffList *out)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    out->n = 0U;
    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL && out->n < 64U) {
        size_t len = strlen(ent->d_name);

        if (len < 4U || strcmp(ent->d_name + len - 3U, ".fl") != 0)
            continue;
        /* A name that does not fit is not one of ours: every corpus
         * document is `NN-short-name.fl`.  Silently truncating would
         * make the test read a file it did not mean to. */
        if (len >= sizeof(out->names[0]))
            continue;
        (void)memcpy(out->names[out->n], ent->d_name, len + 1U);
        out->n++;
    }
    (void)closedir(d);
    sag_sort_stable(out->names, out->n, sizeof(out->names[0]),
                    diff_name_cmp, NULL);
}

static bool diff_read(const char *path, Bytebuf *out)
{
    u8 chunk[65536];
    FILE *fp = fopen(path, "rb");
    size_t n;

    if (fp == NULL)
        return false;
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) > 0U)
        bytebuf_append(out, chunk, n);
    (void)fclose(fp);
    return true;
}

static void emit_through(const FlLit *lit, Bytebuf *out)
{
    FlEmit e;

    sag_fl_emit_init(&e, out);
    sag_fl_emit_lit(&e, NULL, lit);
    sag_fl_emit_done(&e);
}

/* ---------------------------------------------------------------- */
/* The comparison                                                   */
/* ---------------------------------------------------------------- */

/*
 * For every corpus document: both arms parse it, the two trees are
 * structurally equal, and emitting either produces identical bytes.
 *
 * The byte comparison is the one that matters for s36 — a structural
 * match with different bytes still breaks every golden — but the
 * structural one is what says WHERE they disagree when they do.
 */
static void diff_over(const char *dir, u32 expect_min)
{
    DiffList list;
    u32 i;

    diff_list(dir, &list);
    SAG_ASSERT(list.n >= expect_min);
    for (i = 0U; i < list.n; i++) {
        char path[256];
        char why[128];
        Bytebuf raw;
        Bytebuf out_a;
        Bytebuf out_b;
        Arena arena_a;
        Arena arena_b;
        FlParseErr err_a;
        FlParseErr err_b;
        FlLit *lit_a;
        FlLit *lit_b;

        (void)snprintf(path, sizeof(path), "%s/%s", dir, list.names[i]);
        bytebuf_init(&raw);
        bytebuf_init(&out_a);
        bytebuf_init(&out_b);
        /* SEPARATE arenas.  One shared arena would let the second parse
         * reuse the first's allocations, which is exactly the sharing a
         * differential test must not have. */
        arena_init(&arena_a);
        arena_init(&arena_b);
        SAG_ASSERT(diff_read(path, &raw));
        (void)memset(&err_a, 0, sizeof(err_a));
        (void)memset(&err_b, 0, sizeof(err_b));

        lit_a = sag_fl_parse(&arena_a, raw.data, raw.len, &err_a);
        lit_b = DIFF_ARM_B(&arena_b, raw.data, raw.len, &err_b);

        /* Agreeing that a document is BAD is agreement too. */
        if (lit_a == NULL || lit_b == NULL) {
            if ((lit_a == NULL) != (lit_b == NULL))
                (void)fprintf(stderr,
                              "%s: hand-written %s, " DIFF_ARM_B_NAME
                              " %s\n",
                              path, lit_a == NULL ? "rejected" : "accepted",
                              lit_b == NULL ? "rejected" : "accepted");
            SAG_ASSERT((lit_a == NULL) == (lit_b == NULL));
            /* And they must fault in the same PLACE, or one of them is
             * accepting a prefix the other is not. */
            if (lit_a == NULL) {
                SAG_ASSERT_EQ_U64(err_a.line, err_b.line);
                SAG_ASSERT_EQ_U64(err_a.col, err_b.col);
            }
            goto next;
        }

        why[0] = '\0';
        if (!lit_eq(lit_a, lit_b, why, sizeof(why)))
            (void)fprintf(stderr, "%s: trees differ: %s\n", path, why);
        SAG_ASSERT(lit_eq(lit_a, lit_b, why, sizeof(why)));

        emit_through(lit_a, &out_a);
        emit_through(lit_b, &out_b);
        if (out_a.len != out_b.len ||
            memcmp(out_a.data, out_b.data, out_a.len) != 0)
            (void)fprintf(stderr, "%s: re-emitted bytes differ\n", path);
        SAG_ASSERT_EQ_U64(out_a.len, out_b.len);
        SAG_ASSERT_EQ_MEM(out_a.data, out_b.data, out_a.len);

    next:
        arena_free_all(&arena_a);
        arena_free_all(&arena_b);
        bytebuf_free(&raw);
        bytebuf_free(&out_a);
        bytebuf_free(&out_b);
    }
}

/* The canonical corpus: the documents s36 must reproduce byte for
 * byte. */
void test_state_differential_canonical_corpus(void)
{
    diff_over(DIFF_CORPUS_DIR, 20U);
}

/* And the ones that are not canonical, because a parser that only
 * agrees on its own output has not been compared to anything. */
void test_state_differential_noncanonical_corpus(void)
{
    diff_over(DIFF_CORPUS_DIR "/noncanonical", 4U);
}

/*
 * The adversarial set, where the two arms must agree about REJECTION —
 * including where.  A parser that accepts a document the other rejects
 * is the worst kind of disagreement: it comes up with tabs that only
 * one of the two implementations believes in.
 */
void test_state_differential_invalid_corpus(void)
{
    diff_over(DIFF_CORPUS_DIR "/invalid", 20U);
}

/*
 * The comparison itself has teeth.
 *
 * A differential test whose equality function returns true for
 * everything passes forever and proves nothing, and there is no way to
 * tell from a green run which it is.  So lit_eq is checked against
 * documents that genuinely differ, one per way of differing.
 */
void test_state_differential_comparison_detects_differences(void)
{
    static const char *const pairs[][2] = {
        {"{ a: 1, }", "{ a: 2, }"},                 /* value      */
        {"{ a: 1, }", "{ b: 1, }"},                 /* key        */
        {"{ a: 1, b: 2, }", "{ b: 2, a: 1, }"},     /* ORDER      */
        {"{ a: 1, }", "{ a: \"1\", }"},             /* kind       */
        {"{ a: [ 1, ], }", "{ a: [ 1, 1, ], }"},    /* length     */
        {"{ a: true, }", "{ a: false, }"},          /* bool       */
        {"{ a: nil, }", "{ a: 0, }"},               /* nil vs 0   */
        {"{ a: \"x\", }", "{ a: \"y\", }"}          /* string     */
    };
    u32 i;

    for (i = 0U; i < SAG_ARRAY_LEN(pairs); i++) {
        Arena arena_a;
        Arena arena_b;
        FlParseErr err;
        FlLit *lit_a;
        FlLit *lit_b;
        char why[128];

        arena_init(&arena_a);
        arena_init(&arena_b);
        lit_a = sag_fl_parse(&arena_a, (const u8 *)pairs[i][0],
                             (u64)strlen(pairs[i][0]), &err);
        lit_b = sag_fl_parse(&arena_b, (const u8 *)pairs[i][1],
                             (u64)strlen(pairs[i][1]), &err);
        SAG_ASSERT_NOT_NULL(lit_a);
        SAG_ASSERT_NOT_NULL(lit_b);
        why[0] = '\0';
        SAG_ASSERT(!lit_eq(lit_a, lit_b, why, sizeof(why)));
        /* And it says WHY, so a real disagreement is diagnosable. */
        SAG_ASSERT(why[0] != '\0');
        /* Reflexive, or it would report differences that are not
         * there. */
        SAG_ASSERT(lit_eq(lit_a, lit_a, why, sizeof(why)));
        SAG_ASSERT(lit_eq(lit_b, lit_b, why, sizeof(why)));
        arena_free_all(&arena_a);
        arena_free_all(&arena_b);
    }
}

/*
 * A byte-level difference the STRUCTURAL comparison would miss, kept
 * separate because it is the failure mode s36 is most likely to hit:
 * two trees that are equal and two emissions that are not.
 */
void test_state_differential_byte_comparison_is_not_redundant(void)
{
    static const char *const same_tree[] = {
        "{ a: 1, }",
        "{\n  a: 1,\n}\n",
        "{   a:   1,   }",
        "# comment\n{ a: 1, }"
    };
    Bytebuf canonical;
    u32 i;

    bytebuf_init(&canonical);
    for (i = 0U; i < SAG_ARRAY_LEN(same_tree); i++) {
        Arena a;
        FlParseErr err;
        FlLit *lit;
        Bytebuf out;

        arena_init(&a);
        bytebuf_init(&out);
        lit = sag_fl_parse(&a, (const u8 *)same_tree[i],
                           (u64)strlen(same_tree[i]), &err);
        SAG_ASSERT_NOT_NULL(lit);
        emit_through(lit, &out);
        /*
         * Four different byte sequences, one tree, ONE canonical
         * emission.  That is what makes the byte comparison in
         * diff_over meaningful: the emitter has a single output shape,
         * so if two parsers produce equal trees they must produce equal
         * bytes — and if they do not, one of them built a tree that is
         * only nearly equal.
         */
        if (i == 0U) {
            bytebuf_append(&canonical, out.data, out.len);
        } else {
            SAG_ASSERT_EQ_U64(out.len, canonical.len);
            SAG_ASSERT_EQ_MEM(out.data, canonical.data, canonical.len);
        }
        arena_free_all(&a);
        bytebuf_free(&out);
    }
    bytebuf_free(&canonical);
}

/*
 * Names Sprint 36 in the binary, so `strings build/unit_tests | grep`
 * finds the handover and the arm that is still a placeholder is
 * visible from outside the source.
 */
void test_state_differential_names_sprint_36(void)
{
    static const char *const handover =
        "Sprint 36 replaces the hand-written state parser with the "
        "Fletch VM data path; this test compares the two arms over the "
        "frozen v1 corpus.";

    SAG_ASSERT_NOT_NULL(strstr(handover, "Sprint 36"));
#ifdef SAG_HAVE_FLETCH_STATE
    /* s36 is here: both arms are real. */
    SAG_ASSERT(strcmp(DIFF_ARM_B_NAME, "fletch") == 0);
#else
    /* Not yet: the second arm is deliberately the first one. */
    SAG_ASSERT_NOT_NULL(strstr(DIFF_ARM_B_NAME, "Sprint 36"));
#endif
}
