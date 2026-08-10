/*
 * Sprint 31 deliverable 4: the `map` module.
 *
 * Insertion-ordered throughout.  Every function here that produces a
 * list or a map produces it in the source map's order, because a
 * config written today must serialize identically tomorrow -- that is
 * invariant 5, and it is why FlMap's dense entry array is the
 * iteration order rather than a hash bucket walk.
 */
#include "fl/std.h"

#include "fl/gc.h"

/*
 * Spec §4 closes the key set to string, int and bool.  Floats are out
 * because 0.1 + 0.2 would not find 0.3, lists and maps because their
 * hash would have to follow mutable contents -- a key that changes
 * where it belongs after insertion is a lookup that silently misses.
 */
static bool check_key(FlVm *vm, FlValue k)
{
    if (fl_hashable(k))
        return true;
    return fl_raise(vm, "key",
                    "map key must be string, int, or bool, found %s",
                    fl_type_name((FlType)k.t));
}

/* Mutating a frozen map raises rather than silently succeeding: the
 * builtin modules are frozen maps, and `math.pi = 3` must be an error
 * a user sees. */
static bool check_writable(FlVm *vm, const FlMap *m, const char *what)
{
    if ((m->h.oflags & (u16)FL_OF_FROZEN) == 0U)
        return true;
    return fl_raise(vm, "type", "%s: the map is frozen", what);
}

/* ---------------------------------------------------------------- */

static bool m_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m))
        return false;
    /* LIVE entries: fl_map_count skips tombstones, which m->n does
     * not -- reporting the raw array length would make len() grow
     * every time a key was deleted and re-added. */
    *out = FL_INT_V((i64)fl_map_count(m));
    return true;
}

static bool m_get(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;
    FlValue v;

    if (!fl_arg_map(vm, a, 0U, &m) || !check_key(vm, a[1]))
        return false;
    if (fl_map_get(m, a[1], &v)) {
        *out = v;
        return true;
    }
    if (n >= 3U) {
        *out = a[2];
        return true;
    }
    return fl_raise(vm, "key", "map.get: no such key");
}

static bool m_set(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m) || !check_key(vm, a[1]) ||
        !check_writable(vm, m, "map.set"))
        return false;
    (void)fl_map_set(vm, m, a[1], a[2]);
    *out = FL_NIL_V;
    return true;
}

static bool m_has(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m) || !check_key(vm, a[1]))
        return false;
    *out = FL_BOOL_V(fl_map_get(m, a[1], NULL));
    return true;
}

static bool m_remove(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;
    FlValue v;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m) || !check_key(vm, a[1]) ||
        !check_writable(vm, m, "map.remove"))
        return false;
    if (!fl_map_get(m, a[1], &v))
        return fl_raise(vm, "key", "map.remove: no such key");
    (void)fl_map_del(m, a[1]);
    *out = v;
    return true;
}

static bool m_clear(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m) || !check_writable(vm, m, "map.clear"))
        return false;
    /*
     * One primitive, NOT a walk that deletes as it goes.
     *
     * This used to be `while (fl_map_iter(...)) fl_map_del(...)` on the
     * theory that a tombstone moves nothing.  It does not, but
     * fl_map_del COMPACTS once more than half the entries are dead,
     * and compaction moves live entries down past the cursor: clearing
     * a three-entry map left one behind.  Sprint 33's suite caught it.
     */
    fl_map_clear(m);
    *out = FL_NIL_V;
    return true;
}

/* keys / values / entries share one walk. */
typedef enum { WANT_KEYS, WANT_VALUES, WANT_ENTRIES } WantKind;

static bool collect(FlVm *vm, FlValue *a, FlValue *out, WantKind want)
{
    FlMap *m;
    FlList *r;
    u32 cursor = 0U;
    FlValue k;
    FlValue v;

    if (!fl_arg_map(vm, a, 0U, &m))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    while (fl_map_iter(m, &cursor, &k, &v)) {
        if (want == WANT_KEYS) {
            (void)fl_list_push(vm, r, k);
        } else if (want == WANT_VALUES) {
            (void)fl_list_push(vm, r, v);
        } else {
            FlList *pair = fl_list_new(vm);

            /* The pair is protected too: pushing it into `r` allocates,
             * and until that push lands nothing else points at it. */
            fl_gc_protect(vm, FL_OBJ_V(FL_LIST, pair));
            (void)fl_list_push(vm, pair, k);
            (void)fl_list_push(vm, pair, v);
            (void)fl_list_push(vm, r, FL_OBJ_V(FL_LIST, pair));
            fl_gc_release(vm, 1U);
        }
    }
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool m_keys(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return collect(vm, a, out, WANT_KEYS);
}

static bool m_values(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return collect(vm, a, out, WANT_VALUES);
}

static bool m_entries(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    return collect(vm, a, out, WANT_ENTRIES);
}

static bool m_merge(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *x;
    FlMap *y;
    FlMap *r;
    u32 cursor = 0U;
    FlValue k;
    FlValue v;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &x) || !fl_arg_map(vm, a, 1U, &y))
        return false;
    r = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, r));
    /* a's order first, then b's NEW keys: a key in both keeps a's
     * position and takes b's value, which is what makes merge usable
     * for layering defaults under overrides without reshuffling. */
    while (fl_map_iter(x, &cursor, &k, &v))
        (void)fl_map_set(vm, r, k, v);
    cursor = 0U;
    while (fl_map_iter(y, &cursor, &k, &v))
        (void)fl_map_set(vm, r, k, v);
    fl_gc_release(vm, 1U);
    *out = FL_OBJ_V(FL_MAP, r);
    return true;
}

static bool m_copy(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;
    FlMap *r;
    u32 cursor = 0U;
    FlValue k;
    FlValue v;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m))
        return false;
    r = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, r));
    while (fl_map_iter(m, &cursor, &k, &v))
        (void)fl_map_set(vm, r, k, v);
    fl_gc_release(vm, 1U);
    /* Shallow, and NOT frozen even when the source was: a copy exists
     * to be modified, and inheriting the flag would make copying a
     * module produce another unwritable map. */
    *out = FL_OBJ_V(FL_MAP, r);
    return true;
}

static bool m_freeze(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlMap *m;

    (void)n;
    if (!fl_arg_map(vm, a, 0U, &m))
        return false;
    /* In place, and returns the same map rather than a frozen copy:
     * freezing a value someone else already holds a reference to is
     * the point -- a defensive copy would leave the original writable
     * and the caller none the wiser. */
    m->h.oflags |= (u16)FL_OF_FROZEN;
    *out = a[0];
    return true;
}

/* ---------------------------------------------------------------- */

static const FlNativeDef MAP_DEFS[] = {
    {"len",     m_len,     1U, 1U, 0U, "(m) -> int"},
    {"get",     m_get,     2U, 3U, 0U, "(m, k, [default]) -> v"},
    {"set",     m_set,     3U, 3U, 0U, "(m, k, v) -> nil"},
    {"has",     m_has,     2U, 2U, 0U, "(m, k) -> bool"},
    {"remove",  m_remove,  2U, 2U, 0U, "(m, k) -> v"},
    {"clear",   m_clear,   1U, 1U, 0U, "(m) -> nil"},
    {"keys",    m_keys,    1U, 1U, 0U, "(m) -> list"},
    {"values",  m_values,  1U, 1U, 0U, "(m) -> list"},
    {"entries", m_entries, 1U, 1U, 0U, "(m) -> list"},
    {"merge",   m_merge,   2U, 2U, 0U, "(a, b) -> map"},
    {"copy",    m_copy,    1U, 1U, 0U, "(m) -> map"},
    {"freeze",  m_freeze,  1U, 1U, 0U, "(m) -> map"}
};

const FlModuleDef fl_mod_map = {
    "map", MAP_DEFS, (u32)YEW_ARRAY_LEN(MAP_DEFS), NULL, 0U
};
