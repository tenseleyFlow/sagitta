/*
 * Sprint 30 deliverables 1-4 and 10: values, strings, lists, and the
 * insertion-ordered map.
 *
 * Allocation lives in gc.c (DoD 8: the collector owns the heap and the
 * allocator grep must hit that file and no other).  This file shapes
 * objects the collector hands it and answers questions about them.
 */
#include "fl/value.h"

#include <string.h>

#include "util/log.h"

/* ---------------------------------------------------------------- */
/* Type names                                                       */
/* ---------------------------------------------------------------- */

static const char *const fl_type_names[FL_TYPE_COUNT] = {
    "nil", "bool", "int", "float",
    "str", "list", "map", "fn", "fn", "fn",
    "motion", "upval",
    /*
     * The reserved handles of spec §4.  Naming a type is not
     * constructing one: `type_of` must be able to say "buf" when
     * Sprint 34 starts handing them out, and until then these entries
     * are the only mention outside the enum.
     */
    "buf", "cursor", "span", "win", "regex"
};

const char *fl_type_name(FlType t)
{
    if ((u32)t >= (u32)FL_TYPE_COUNT)
        return "?";
    return fl_type_names[t];
}

/* ---------------------------------------------------------------- */
/* Truth and equality (spec §5.1, §5.2)                             */
/* ---------------------------------------------------------------- */

bool fl_truthy(FlValue v)
{
    /*
     * §5.1: nil and false are falsey and EVERYTHING else is truthy --
     * 0, 0.0 and "" included.  Pinned here because "zero is falsey" is
     * the assumption every reader brings from another language, and a
     * VM that quietly agreed would make `if count { }` mean something
     * different from what the spec says.
     */
    if (v.t == (u8)FL_NIL)
        return false;
    if (v.t == (u8)FL_BOOL)
        return v.as.b;
    return true;
}

bool fl_equal(FlValue a, FlValue b)
{
    /* §5.2: int and float compare across the numeric tower, so
     * `1 == 1.0` holds; every other pair needs the same tag. */
    if (a.t == (u8)FL_INT && b.t == (u8)FL_FLOAT)
        return (double)a.as.i == b.as.f;
    if (a.t == (u8)FL_FLOAT && b.t == (u8)FL_INT)
        return a.as.f == (double)b.as.i;
    if (a.t != b.t)
        return false;
    switch ((FlType)a.t) {
    case FL_NIL:   return true;
    case FL_BOOL:  return a.as.b == b.as.b;
    case FL_INT:   return a.as.i == b.as.i;
    case FL_FLOAT: return a.as.f == b.as.f;
    case FL_STR:   return fl_str_eq((const FlStr *)a.as.o,
                                    (const FlStr *)b.as.o);
    /*
     * Sprint 34's handles are scalars, not objects: the payload is a
     * {slot, gen} pair, so two handles are equal when they name the
     * same slot in the same life.  The default branch below would read
     * the same eight bytes through `as.o` and get the right answer by
     * accident; saying it in the tag the value actually carries means
     * the next person to add a scalar tag does not have to notice.
     */
    case FL_BUF: case FL_CURSOR: case FL_SPAN: case FL_WIN: case FL_REGEX:
        return a.as.i == b.as.i;
    default:
        /* Reference identity for the rest: two distinct lists with
         * equal contents are not the same list. */
        return a.as.o == b.as.o;
    }
}

/* ---------------------------------------------------------------- */
/* Hashing                                                          */
/* ---------------------------------------------------------------- */

u32 fl_hash_bytes(const char *b, u32 n)
{
    u32 h = 2166136261U;   /* FNV-1a offset basis */
    u32 i;

    for (i = 0U; i < n; i++) {
        h ^= (u32)(u8)b[i];
        h *= 16777619U;
    }
    return h;
}

bool fl_hashable(FlValue v)
{
    /* Spec §4: string, int and bool.  A float key is refused at the
     * call site with kind "key" naming the type -- there is
     * deliberately no fl_hash for one, because the error message is
     * the feature and a "just in case" hash would silently accept
     * `{1.0: x}` and then lose the key to rounding. */
    return v.t == (u8)FL_STR || v.t == (u8)FL_INT || v.t == (u8)FL_BOOL;
}

u32 fl_hash_value(FlValue v)
{
    switch ((FlType)v.t) {
    case FL_STR:
        return v.as.o->aux;           /* eager FNV-1a from construction */
    case FL_INT: {
        u64 x = (u64)v.as.i;

        return fl_hash_bytes((const char *)&x, (u32)sizeof(x));
    }
    case FL_BOOL:
        return v.as.b ? 0x9E3779B9U : 0x85EBCA6BU;
    default:
        SAG_BUG("fletch: hashed an unhashable value");
    }
}

/* ---------------------------------------------------------------- */
/* Strings (deliverable 10)                                         */
/* ---------------------------------------------------------------- */

bool fl_str_eq(const FlStr *a, const FlStr *b)
{
    if (a == b)
        return true;
    /*
     * The pointer check above is a FAST PATH ONLY.
     *
     * "Interned strings compare by pointer" holds only if every string
     * is interned, and FL_INTERN_MAX means the long ones are not -- so
     * a pointer-only comparison reports false for two equal 100-byte
     * strings, and the bug reads as "my map lookup misses sometimes".
     * Length and hash reject almost everything before the memcmp.
     */
    if (a == NULL || b == NULL)
        return false;
    if (a->len != b->len || a->h.aux != b->h.aux)
        return false;
    return memcmp(a->b, b->b, a->len) == 0;
}

/* ---------------------------------------------------------------- */
/* Map (deliverable 3)                                              */
/* ---------------------------------------------------------------- */

/*
 * THE ITERATION ORDER IS THE DENSE ARRAY.  `idx` only accelerates
 * lookup, and every rule below exists because the obvious alternative
 * breaks invariant 5 -- same state, same bytes, on any machine.
 *
 *   - delete marks `dead` and tombstones the index slot; it never moves
 *     an entry.  Swapping the last entry into the hole is the usual
 *     trick and it makes order depend on delete history.
 *   - re-inserting a deleted key APPENDS.  Reviving in place makes
 *     order depend on whether a key was ever deleted, which is
 *     invisible in the source and untestable from it.
 *   - compaction runs only when ndead > n/2, and preserves live order.
 *     One predictable order change -- dead rows vanish -- rather than
 *     an unpredictable one at every delete.
 */

enum { FL_MAP_MIN_ICAP = 8U };

u32 fl_map_probe(const FlMap *m, FlValue k, u32 hash, bool *found)
{
    u32 mask = m->icap - 1U;
    u32 i = hash & mask;

    *found = false;
    if (m->icap == 0U)
        return 0U;
    for (;;) {
        u32 slot = m->idx[i];

        if (slot == 0U)
            return i;                  /* empty: insertion point */
        {
            const FlMapEnt *e = &m->ent[slot - 1U];

            if (!e->dead && e->hash == hash && fl_equal(e->k, k)) {
                *found = true;
                return i;
            }
        }
        i = (i + 1U) & mask;           /* linear probing */
    }
}

bool fl_map_get(const FlMap *m, FlValue k, FlValue *out)
{
    bool found = false;
    u32 i;

    if (m == NULL || m->icap == 0U || !fl_hashable(k))
        return false;
    i = fl_map_probe(m, k, fl_hash_value(k), &found);
    if (!found)
        return false;
    if (out != NULL)
        *out = m->ent[m->idx[i] - 1U].v;
    return true;
}

/* Rebuilds `idx` from the live rows of `ent`.  Used after growth and
 * after compaction; never reorders `ent`. */
void fl_map_reindex(FlMap *m)
{
    u32 mask;
    u32 i;

    if (m->icap == 0U)
        return;
    mask = m->icap - 1U;
    (void)memset(m->idx, 0, (size_t)m->icap * sizeof(*m->idx));
    for (i = 0U; i < m->n; i++) {
        u32 at;

        if (m->ent[i].dead)
            continue;
        at = m->ent[i].hash & mask;
        while (m->idx[at] != 0U)
            at = (at + 1U) & mask;
        m->idx[at] = i + 1U;
    }
}

void fl_map_compact(FlMap *m)
{
    u32 w = 0U;
    u32 i;

    if (m == NULL || m->ndead == 0U)
        return;
    for (i = 0U; i < m->n; i++) {
        if (m->ent[i].dead)
            continue;
        if (w != i)
            m->ent[w] = m->ent[i];
        w++;
    }
    m->n = w;
    m->ndead = 0U;
    fl_map_reindex(m);
}

/*
 * Empty the map in one pass.
 *
 * NOT expressible as "iterate and delete": fl_map_del compacts once
 * more than half the entries are dead, and compaction MOVES live
 * entries down, so a caller's cursor lands past the end and the tail
 * survives.  Sprint 31's map.clear did exactly that and left
 * floor(n/2) entries behind for n >= 3; the Sprint 33 conformance
 * suite is what caught it.
 *
 * `mods` moves once, which is enough: an iteration in progress must
 * notice that the container was reshaped, and it cannot care how many
 * times.
 */
void fl_map_clear(FlMap *m)
{
    if (m == NULL)
        return;
    m->n = 0U;
    m->ndead = 0U;
    m->mods++;
    if (m->idx != NULL && m->icap != 0U)
        (void)memset(m->idx, 0, (size_t)m->icap * sizeof(m->idx[0]));
}

bool fl_map_del(FlMap *m, FlValue k)
{
    bool found = false;
    u32 i;

    if (m == NULL || m->icap == 0U || !fl_hashable(k))
        return false;
    i = fl_map_probe(m, k, fl_hash_value(k), &found);
    if (!found)
        return false;
    m->ent[m->idx[i] - 1U].dead = true;
    m->idx[i] = 0U;
    m->ndead++;
    m->mods++;
    /*
     * Tombstoning the index slot to 0 ends the probe chain, so a key
     * that collided with this one and was inserted later becomes
     * unreachable.  Reindexing costs a walk but keeps lookup correct;
     * the alternative is a second "deleted" sentinel and a probe loop
     * that must distinguish it from empty, which is where linear-probe
     * tables usually go wrong.
     */
    fl_map_reindex(m);
    if (m->ndead > m->n / 2U)
        fl_map_compact(m);
    return true;
}

u32 fl_map_count(const FlMap *m)
{
    return m == NULL ? 0U : m->n - m->ndead;
}

bool fl_map_iter(const FlMap *m, u32 *cursor, FlValue *k, FlValue *v)
{
    if (m == NULL || cursor == NULL)
        return false;
    while (*cursor < m->n) {
        const FlMapEnt *e = &m->ent[*cursor];

        (*cursor)++;
        if (e->dead)
            continue;
        if (k != NULL)
            *k = e->k;
        if (v != NULL)
            *v = e->v;
        return true;
    }
    return false;
}
