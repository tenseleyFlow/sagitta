/*
 * Sprint 23 fuzz: tab operations against a naive oracle.
 *
 * The oracle is a plain array of (id, path) pairs maintained with
 * obvious, slow code — no shared helpers with the implementation, since
 * an oracle that calls the thing it is checking proves nothing.  After
 * every operation the two must agree on the whole set AND on which id
 * is active.
 *
 * That second half is the point.  A close compacts the array, so an
 * implementation that keeps an INDEX rather than resolving an id looks
 * right until the moment it silently starts naming a different tab —
 * which is how facsimile wrote one file's text over another's.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/tabs.h"

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

/* The oracle: ids in visible order, plus which one is active. */
typedef struct Oracle {
    u32 id[SAG_TAB_MAX];
    int n;
    int active;
} Oracle;

static void oracle_insert(Oracle *o, int at, u32 id)
{
    int i;

    if (o->n >= SAG_TAB_MAX)
        return;
    for (i = o->n; i > at; i--)
        o->id[i] = o->id[i - 1];
    o->id[at] = id;
    o->n++;
}

static void oracle_remove(Oracle *o, int at)
{
    int i;

    for (i = at; i < o->n - 1; i++)
        o->id[i] = o->id[i + 1];
    o->n--;
}

static bool oracle_agrees(const Ed *ed, const Oracle *o, char *why,
                          size_t cap)
{
    int i;

    if ((int)sag_tab_count((Ed *)ed) != o->n) {
        (void)snprintf(why, cap, "tab count %d, oracle says %d",
                       (int)sag_tab_count((Ed *)ed), o->n);
        return false;
    }
    for (i = 0; i < o->n; i++) {
        u32 got = sag_tab_at((Ed *)ed, i)->tab_id;

        if (got != o->id[i]) {
            (void)snprintf(why, cap,
                           "tab %d has id %u, oracle says %u", i,
                           (unsigned)got, (unsigned)o->id[i]);
            return false;
        }
    }
    /* The half that catches an index kept where an id was needed. */
    if (o->active >= 0 && o->active < o->n) {
        u32 want = o->id[o->active];
        int at = ((const Ed *)ed)->tabs.active;

        if (at < 0 || at >= o->n) {
            (void)snprintf(why, cap, "active index %d out of range", at);
            return false;
        }
        if (sag_tab_at((Ed *)ed, at)->tab_id != want) {
            (void)snprintf(why, cap,
                           "active tab has id %u, oracle says %u",
                           (unsigned)sag_tab_at((Ed *)ed, at)->tab_id,
                           (unsigned)want);
            return false;
        }
    }
    return true;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Ed ed;
    Oracle o;
    Rng rng;
    size_t op;
    size_t ops;
    bool ok = false;

    rng.s = 0x9E3779B97F4A7C15ULL;
    {
        size_t i;

        for (i = 0U; i < len; i++)
            rng.s = rng.s * 31U + data[i];
    }
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open a buffer");
        return false;
    }
    (void)memset(&o, 0, sizeof(o));
    o.id[0] = sag_tab_at(&ed, 0)->tab_id;
    o.n = 1;
    o.active = 0;

    ops = 80U + (len % 80U);
    for (op = 0U; op < ops; op++) {
        switch (rng_below(&rng, 5U)) {
        case 0:
        case 1: { /* open */
            char path[64];
            int idx;

            (void)snprintf(path, sizeof(path), "/tmp/sag-fz-%u-%u.txt",
                           (unsigned)(rng.s & 0xFFFFU), (unsigned)op);
            idx = sag_tab_open(&ed, path);
            if (idx < 0)
                break; /* refused at the cap; the oracle does not move */
            oracle_insert(&o, idx, sag_tab_at(&ed, idx)->tab_id);
            break;
        }
        case 2: { /* close */
            int at;

            if (o.n <= 1)
                break;
            at = (int)rng_below(&rng, (u32)o.n);
            /*
             * Mirror the survivor rule in the oracle INDEPENDENTLY:
             * not the active tab -> active keeps its id; the active one
             * -> its right neighbour, else its left.
             */
            {
                u32 survivor;

                if (o.active != at)
                    survivor = o.id[o.active];
                else if (at + 1 < o.n)
                    survivor = o.id[at + 1];
                else if (at - 1 >= 0)
                    survivor = o.id[at - 1];
                else
                    survivor = 0U;
                if (!sag_tab_close(&ed, at)) {
                    (void)snprintf(why, why_cap, "close(%d) refused", at);
                    goto done;
                }
                oracle_remove(&o, at);
                o.active = -1;
                {
                    int i;

                    for (i = 0; i < o.n; i++) {
                        if (o.id[i] == survivor) {
                            o.active = i;
                            break;
                        }
                    }
                }
                if (o.active < 0 && o.n > 0)
                    o.active = 0;
            }
            break;
        }
        case 3: { /* switch */
            int at = (int)rng_below(&rng, (u32)o.n);

            sag_tab_switch(&ed, at);
            o.active = at;
            break;
        }
        default: { /* reorder */
            int from;
            int to;

            if (o.n < 2)
                break;
            from = (int)rng_below(&rng, (u32)o.n);
            to = (int)rng_below(&rng, (u32)o.n);
            if (from == to)
                break;
            sag_tab_reorder(&ed, from, to);
            {
                u32 moved = o.id[from];

                oracle_remove(&o, from);
                oracle_insert(&o, to, moved);
                o.active = sag_tab_shifted_index(o.active, from, to);
            }
            break;
        }
        }
        if (!oracle_agrees(&ed, &o, why, why_cap))
            goto done;
    }
    ok = true;
done:
    sag_ed_free(&ed);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_tabs", NULL, run_session);
}
