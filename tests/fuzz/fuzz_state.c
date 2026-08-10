#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/*
 * Sprint 25 §9 fuzz: the state parser and the schema layer.
 *
 * WHAT IS BEING DEFENDED.  A workspace state file is the one input
 * yew reads at startup that nobody typed on purpose.  It is written
 * by a previous session, so it is trusted in exactly the way a file on
 * disk is trusted — which is to say not at all, because a truncated
 * write, a filesystem that lost a block, a `>` from the wrong shell, or
 * a hand edit all produce bytes that reach the parser.  Whatever they
 * are, the editor must START.
 *
 * FOUR CLAIMS, checked after every case:
 *
 *   1. Never crashes.  Not on random bytes, not on a mutated corpus
 *      document, not on either while the schema layer walks it.
 *   2. Never loops.  Every cap is bounded by a counter the parser
 *      owns, so a document that nests or repeats forever is rejected
 *      rather than followed.
 *   3. Never allocates past the caps.  8 MiB, a million nodes, depth
 *      32, 4096-byte strings — a document claiming more is corruption,
 *      not an allocation request.
 *   4. Always reaches a YewWsResult.  There is no fifth answer and no
 *      failure return: §7 says every row ends with "the editor starts".
 *
 * WHY THE MUTATED CORPUS MATTERS MORE THAN RANDOM BYTES.  Random bytes
 * are rejected in the first few characters and exercise the error path
 * only.  A corpus document with one byte flipped parses most of the way
 * and then goes wrong somewhere structural — which is the shape of an
 * actual corrupt file, and the shape that finds bugs in the schema
 * layer rather than the tokenizer.
 */
#include "fuzzlib.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"

#define FZ_CORPUS_DIR "tests/unit/fixtures/wsstate/v1"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

/* ---------------------------------------------------------------- */
/* Seeds                                                            */
/* ---------------------------------------------------------------- */

/*
 * The corpus, loaded once.  Only the smaller documents: the 350 KB
 * maximal one would make every mutation cost a megabyte of parsing and
 * buy nothing the 16-leaf document does not already cover.
 */
enum {
    FZ_MAX_SEEDS = 32,
    FZ_SEED_MAX_BYTES = 64U * 1024U
};

typedef struct Seeds {
    Bytebuf doc[FZ_MAX_SEEDS];
    u32 n;
} Seeds;

static Seeds g_seeds;
static bool g_seeds_loaded;

static void seeds_load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL && g_seeds.n < FZ_MAX_SEEDS) {
        char path[512];
        size_t len = strlen(ent->d_name);
        FILE *fp;
        u8 chunk[8192];
        size_t got;
        Bytebuf *into;

        if (len < 4U || strcmp(ent->d_name + len - 3U, ".fl") != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >=
            (int)sizeof(path))
            continue;
        fp = fopen(path, "rb");
        if (fp == NULL)
            continue;
        into = &g_seeds.doc[g_seeds.n];
        bytebuf_init(into);
        while ((got = fread(chunk, 1U, sizeof(chunk), fp)) > 0U) {
            bytebuf_append(into, chunk, got);
            if (into->len > FZ_SEED_MAX_BYTES)
                break;
        }
        (void)fclose(fp);
        if (into->len == 0U || into->len > FZ_SEED_MAX_BYTES) {
            bytebuf_free(into);
            continue;
        }
        g_seeds.n++;
    }
    (void)closedir(d);
}

static void seeds_load(void)
{
    if (g_seeds_loaded)
        return;
    g_seeds_loaded = true;
    seeds_load_dir(FZ_CORPUS_DIR);
    seeds_load_dir(FZ_CORPUS_DIR "/noncanonical");
    seeds_load_dir(FZ_CORPUS_DIR "/invalid");
}

/* ---------------------------------------------------------------- */
/* Mutation                                                         */
/* ---------------------------------------------------------------- */

/*
 * Mutations chosen to break STRUCTURE, not just bytes.
 *
 * A bit flip in the middle of a path only tests the string reader.
 * Deleting a `}`, duplicating a run, or splicing two documents together
 * produces something that tokenizes and then does not mean anything —
 * which is where a parser stops matching its own error handling.
 */
static void mutate(Rng *rng, const Bytebuf *seed, Bytebuf *out)
{
    u32 kind = rng_below(rng, 9U);
    u64 n = seed->len;
    u64 at;

    out->len = 0U;
    if (n == 0U)
        return;
    switch (kind) {
    case 0: /* one byte, anywhere */
        bytebuf_append(out, seed->data, (size_t)n);
        at = rng_below(rng, (u32)n);
        out->data[at] = (u8)rng_below(rng, 256U);
        break;
    case 1: /* truncate — the torn-write shape */
        at = rng_below(rng, (u32)n);
        bytebuf_append(out, seed->data, (size_t)at);
        break;
    case 2: /* delete a run */
        at = rng_below(rng, (u32)n);
        bytebuf_append(out, seed->data, (size_t)at);
        {
            u64 skip = 1U + rng_below(rng, 32U);

            if (at + skip < n)
                bytebuf_append(out, seed->data + at + skip,
                               (size_t)(n - at - skip));
        }
        break;
    case 3: /* duplicate a run */
        at = rng_below(rng, (u32)n);
        bytebuf_append(out, seed->data, (size_t)at);
        {
            u64 run = 1U + rng_below(rng, 64U);

            if (at + run > n)
                run = n - at;
            bytebuf_append(out, seed->data + at, (size_t)run);
            bytebuf_append(out, seed->data + at, (size_t)run);
        }
        bytebuf_append(out, seed->data + at, (size_t)(n - at));
        break;
    case 4: /* drop every closing brace of one kind */
        {
            u8 drop = rng_below(rng, 2U) != 0U ? (u8)'}' : (u8)']';
            u64 i;

            for (i = 0U; i < n; i++) {
                if (seed->data[i] != drop)
                    bytebuf_push_u8(out, seed->data[i]);
            }
        }
        break;
    case 5: /* splice a second document in */
        at = rng_below(rng, (u32)n);
        bytebuf_append(out, seed->data, (size_t)at);
        if (g_seeds.n > 0U) {
            const Bytebuf *other =
                &g_seeds.doc[rng_below(rng, g_seeds.n)];

            if (other->data != NULL)
                bytebuf_append(out, other->data, (size_t)other->len);
        }
        bytebuf_append(out, seed->data + at, (size_t)(n - at));
        break;
    case 6: /* a deeply nested prefix — the depth cap */
        {
            u32 depth = 1U + rng_below(rng, 64U);
            u32 i;

            for (i = 0U; i < depth; i++)
                bytebuf_append(out, (const u8 *)"{ a: ", 5U);
            bytebuf_append(out, seed->data, (size_t)n);
        }
        break;
    case 7: /* a very long string — the 4096-byte cap */
        {
            u32 i;
            u32 len = 4000U + rng_below(rng, 400U);

            bytebuf_append(out, (const u8 *)"{ version: 1, path: \"", 21U);
            for (i = 0U; i < len; i++)
                bytebuf_push_u8(out, (u8)('a' + (i % 26U)));
            bytebuf_append(out, (const u8 *)"\", }\n", 5U);
        }
        break;
    case 8:
    default: /* NUL bytes sprinkled in, which C string handling hates */
        bytebuf_append(out, seed->data, (size_t)n);
        {
            u32 k;
            u32 hits = 1U + rng_below(rng, 8U);

            for (k = 0U; k < hits; k++)
                out->data[rng_below(rng, (u32)n)] = 0U;
        }
        break;
    }
}

/* ---------------------------------------------------------------- */
/* The checks                                                       */
/* ---------------------------------------------------------------- */

/*
 * Claim 3, checked structurally rather than by watching malloc: a tree
 * the parser returned must itself be inside the caps.  A parser that
 * allocated past them and then returned success would pass a crash test
 * and fail this one.
 */
static bool tree_within_caps(const FlLit *v, u32 depth, u64 *nodes,
                             char *why, size_t why_cap)
{
    u32 i;

    if (v == NULL)
        return true;
    (*nodes)++;
    if (*nodes > (u64)YEW_FL_MAX_NODES) {
        (void)snprintf(why, why_cap, "tree exceeds the node cap");
        return false;
    }
    if (depth > (u32)YEW_FL_MAX_DEPTH) {
        (void)snprintf(why, why_cap, "tree exceeds depth %u",
                       (unsigned)YEW_FL_MAX_DEPTH);
        return false;
    }
    if (v->kind == FL_LIT_STR && v->slen > (u64)YEW_FL_MAX_STRING) {
        (void)snprintf(why, why_cap, "string of %llu bytes past the cap",
                       (unsigned long long)v->slen);
        return false;
    }
    if (v->kind != FL_LIT_LIST && v->kind != FL_LIT_MAP)
        return true;
    for (i = 0U; i < v->len; i++) {
        if (!tree_within_caps(v->items[i], depth + 1U, nodes, why, why_cap))
            return false;
    }
    return true;
}

/* One document through the parser and the schema layer. */
static bool run_one(const u8 *bytes, u64 len, char *why, size_t why_cap)
{
    Arena a;
    FlParseErr err;
    FlLit *lit;
    Ed ed;
    YewWsResult r;
    bool ok = false;

    arena_init(&a);
    (void)memset(&err, 0, sizeof(err));
    lit = yew_fl_parse(&a, bytes, len, &err);
    if (lit != NULL) {
        u64 nodes = 0U;

        if (!tree_within_caps(lit, 0U, &nodes, why, why_cap))
            goto done;
    } else if (err.line == 0U && len != 0U) {
        /* A rejection with no position is one nobody can act on, and
         * §7's log line promises one. */
        (void)snprintf(why, why_cap, "rejected without a line number");
        goto done;
    }

    /*
     * And through the SCHEMA layer, on a real editor.  The parser
     * returning cleanly is half the claim; the half that matters is
     * that applying whatever it returned leaves a usable editor.
     */
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open a scratch buffer");
        yew_ed_free(&ed);
        goto done;
    }
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    r = yew_state_apply(&ed, bytes, len);
    if (r != YEW_WS_FRESH && r != YEW_WS_RESTORED && r != YEW_WS_RECOVERED) {
        (void)snprintf(why, why_cap, "result %d is not a YewWsResult",
                       (int)r);
        yew_ed_free(&ed);
        goto done;
    }
    /* Claim 4's teeth: the editor is still usable. */
    if (yew_tab_count(&ed) < 1) {
        (void)snprintf(why, why_cap, "no tabs left after apply");
        yew_ed_free(&ed);
        goto done;
    }
    if (ed.quit) {
        (void)snprintf(why, why_cap, "apply asked the editor to quit");
        yew_ed_free(&ed);
        goto done;
    }
    if (ed.pane_root == NULL || ed.win == NULL) {
        (void)snprintf(why, why_cap, "apply left no pane tree or window");
        yew_ed_free(&ed);
        goto done;
    }
    yew_ed_free(&ed);
    ok = true;
done:
    arena_free_all(&a);
    return ok;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Rng rng;
    Bytebuf mutated;
    u32 i;
    size_t k;
    /* Enough per case to reach the schema layer repeatedly without
     * making a corpus entry cost a second. */
    const u32 rounds = 24U;
    bool ok = true;

    seeds_load();
    /*
     * A fuzzer that silently tests nothing is worse than no fuzzer: it
     * reports "ok" forever.  The seeds are read relative to the repo
     * root, so running from anywhere else would leave the mutation half
     * doing no work at all, and the only symptom would be a suspiciously
     * fast green run.
     */
    if (g_seeds.n < 20U) {
        (void)snprintf(why, why_cap,
                       "only %u corpus seeds loaded from %s (run from the "
                       "repo root)",
                       (unsigned)g_seeds.n, FZ_CORPUS_DIR);
        return false;
    }
    rng.s = 0x9E3779B97F4A7C15ULL;
    for (k = 0U; k < len; k++)
        rng.s = rng.s * 31U + data[k];

    /* The raw case bytes first: random input is its own test, and it is
     * the one that reaches the tokenizer's error paths. */
    if (!run_one(data, (u64)len, why, why_cap))
        return false;

    bytebuf_init(&mutated);
    for (i = 0U; i < rounds && g_seeds.n > 0U; i++) {
        const Bytebuf *seed = &g_seeds.doc[rng_below(&rng, g_seeds.n)];

        mutate(&rng, seed, &mutated);
        if (mutated.len > (u64)YEW_FL_MAX_BYTES)
            continue;
        if (!run_one(mutated.data, mutated.len, why, why_cap)) {
            ok = false;
            break;
        }
    }
    bytebuf_free(&mutated);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_state", NULL, run_session);
}
