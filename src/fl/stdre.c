/*
 * Sprint 31 deliverable 8: the `re` module, over Sprint 20's engine.
 *
 * `re` TAKES PATTERN STRINGS, NOT A HANDLE.  Spec §4 reserves `regex` as
 * an editor handle constructible only from Sprint 34, and this sprint
 * may not construct one.  A module-private compile cache buys the same
 * performance without touching the frozen type set, and when Sprint 34
 * adds the real handle this API stays as sugar over it -- no call site
 * changes.
 *
 * THE CACHE IS A PURE FUNCTION OF (pattern bytes, flags).  Nothing in a
 * compiled program depends on the VM that asked for it, which is why one
 * static cache can serve every VM in the process without leaking state
 * between them.
 *
 * THE PIN IS THE RE-ENTRANCY GUARD.  re.replace_fn calls back into
 * Fletch, and that callback may call `re` again; if the inner call reset
 * the arena to make room, the outer scan would keep walking a freed
 * program.  So a scan pins the cache, and while pinned the arena is
 * never reset -- an evicted entry loses its table slot and keeps its
 * memory until the next unpinned reset.  Bounded, because the pin depth
 * is the call depth and a pattern is capped at SAG_RE_MAX_PATTERN.
 *
 * BYTE OFFSETS, EVERYWHERE.  A match map's lo/hi are byte offsets into
 * the subject, because that is what the engine reports and converting to
 * cluster indices would make re.find's answer disagree with
 * str.slice_bytes.  Invalid UTF-8 in the subject decodes to U+DC80..DCFF
 * exactly as s20 pinned, so a script can regex a binary file without
 * mangling it.
 */
#include "fl/std.h"

#include <string.h>

#include "fl/gc.h"
#include "search/regex.h"
#include "unicode/utf8.h"
#include "util/arena.h"

enum {
    RE_CACHE_N = 64,
    /* A scan that produces this many matches has stopped being a search
     * and started being a memory problem. */
    RE_MAX_RESULTS = 100000
};

/* ---------------------------------------------------------------- */
/* The compile cache                                                */
/* ---------------------------------------------------------------- */

typedef struct ReEntry {
    char *pat;        /* arena copy; compared by bytes, never by id     */
    u32 patlen;
    u32 flags;
    u64 used;         /* LRU tick                                       */
    SagRe *re;
} ReEntry;

typedef struct ReCache {
    Arena arena;
    bool live;
    ReEntry e[RE_CACHE_N];
    u32 n;
    u64 tick;
    u32 pin;
} ReCache;

static ReCache g_cache;

void fl_re_cache_clear(void)
{
    /* Never while a scan is walking a program out of it.  fl_vm_free is
     * the only caller and nothing is pinned there, but the check is
     * what makes that a property rather than a coincidence. */
    if (g_cache.pin != 0U)
        return;
    if (g_cache.live)
        arena_free_all(&g_cache.arena);
    (void)memset(&g_cache, 0, sizeof(g_cache));
}

static SagRe *cache_get(const char *pat, u32 patlen, u32 flags)
{
    u32 i;

    for (i = 0U; i < g_cache.n; i++) {
        ReEntry *e = &g_cache.e[i];

        if (e->flags == flags && e->patlen == patlen &&
            (patlen == 0U || memcmp(e->pat, pat, (size_t)patlen) == 0)) {
            e->used = ++g_cache.tick;
            return e->re;
        }
    }
    return NULL;
}

static void cache_put(char *pat, u32 patlen, u32 flags, SagRe *re)
{
    ReEntry *slot;

    if (g_cache.n < (u32)RE_CACHE_N) {
        slot = &g_cache.e[g_cache.n++];
    } else {
        u32 i;
        u32 lru = 0U;

        for (i = 1U; i < g_cache.n; i++) {
            if (g_cache.e[i].used < g_cache.e[lru].used)
                lru = i;
        }
        slot = &g_cache.e[lru];
    }
    slot->pat = pat;
    slot->patlen = patlen;
    slot->flags = flags;
    slot->re = re;
    slot->used = ++g_cache.tick;
}

/*
 * Compiles, or returns the cached program.
 *
 * The arena is reset when the table is full and nothing is pinned: an
 * LRU that only ever evicted table slots would grow the arena without
 * bound, and one full cycle is the natural point to reclaim it.  Every
 * surviving entry is dropped with it, which costs at most 64 recompiles
 * and is why the reset is not attempted mid-scan.
 */
static SagRe *compile_cached(FlVm *vm, const FlStr *pat, u32 flags)
{
    SagRe *re = cache_get(pat->b, pat->len, flags);
    SagReErr err = {0U, NULL};
    char *copy;

    if (re != NULL)
        return re;
    if (pat->len > (u32)SAG_RE_MAX_PATTERN) {
        (void)fl_raise(vm, "limit", "regex: the pattern exceeds %d bytes",
                       SAG_RE_MAX_PATTERN);
        return NULL;
    }
    if (!g_cache.live) {
        arena_init(&g_cache.arena);
        g_cache.live = true;
    } else if (g_cache.n == (u32)RE_CACHE_N && g_cache.pin == 0U) {
        arena_free_all(&g_cache.arena);
        arena_init(&g_cache.arena);
        g_cache.n = 0U;
    }
    re = sag_re_compile(&g_cache.arena, pat->b, (size_t)pat->len, flags,
                        &err);
    if (re == NULL) {
        /*
         * Kind "type" rather than a new kind: a malformed pattern is a
         * bad value, and the campaign has one spec amendment already.
         * The message is s20's own, with its pattern offset, so the
         * wording a user sees here matches the search prompt's.
         */
        (void)fl_raise(vm, "type", "regex: %s at pattern offset %u",
                       err.msg == NULL ? "invalid pattern" : err.msg,
                       (unsigned)err.off);
        return NULL;
    }
    copy = arena_alloc(&g_cache.arena, (size_t)pat->len + 1U, 1U);
    if (pat->len != 0U)
        (void)memcpy(copy, pat->b, (size_t)pat->len);
    copy[pat->len] = '\0';
    cache_put(copy, pat->len, flags, re);
    return re;
}

/* ---------------------------------------------------------------- */
/* Arguments                                                        */
/* ---------------------------------------------------------------- */

static bool read_flags(FlVm *vm, FlValue *a, u32 i, u32 argc, u32 *out)
{
    const FlStr *f;
    u32 k;

    *out = 0U;
    if (argc <= i || a[i].t == (u8)FL_NIL)
        return true;
    if (!fl_arg_str(vm, a, i, &f))
        return false;
    for (k = 0U; k < f->len; k++) {
        switch (f->b[k]) {
        case 'i': *out |= (u32)SAG_RE_ICASE; break;
        case 's': *out |= (u32)SAG_RE_DOTALL; break;
        case 'l': *out |= (u32)SAG_RE_LITERAL; break;
        default:
            /* Named, not ignored: a typo'd flag that silently did
             * nothing would make a case-insensitive search quietly
             * case-sensitive. */
            return fl_raise(vm, "type",
                            "re: unknown flag '%c'; the flags are i, s and l",
                            f->b[k]);
        }
    }
    return true;
}

/* An optional count argument.  Absent, nil or negative means "all". */
static bool read_limit(FlVm *vm, FlValue *a, u32 i, u32 argc, u64 *out)
{
    i64 v;

    *out = (u64)-1;
    if (argc <= i || a[i].t == (u8)FL_NIL)
        return true;
    if (!fl_arg_int(vm, a, i, &v))
        return false;
    if (v >= 0)
        *out = (u64)v;
    return true;
}

/* ---------------------------------------------------------------- */
/* Match maps                                                       */
/* ---------------------------------------------------------------- */

static void map_put(FlVm *vm, FlMap *m, const char *k, FlValue v)
{
    (void)fl_map_set(vm, m, FL_OBJ_V(FL_STR, fl_str_new(vm, k, (u32)strlen(k))),
                     v);
}

static FlValue slice(FlVm *vm, const FlStr *s, u64 lo, u64 hi)
{
    if (hi < lo || hi > (u64)s->len)
        return FL_OBJ_V(FL_STR, fl_str_new(vm, "", 0U));
    return FL_OBJ_V(FL_STR,
                    fl_str_new(vm, s->b + lo, (u32)(hi - lo)));
}

/*
 * `{ lo, hi, text }`, plus `groups` at the top level.
 *
 * `groups[i]` is exactly what `$i` expands to, group 0 included -- one
 * numbering for the map and the template beats two that differ by one.
 *
 * A group that did not participate is nil.
 *
 * The engine collapses "absent" and "empty at byte 0" into the same
 * span {0,0} -- pike.c writes it deliberately, and s21's replacer reads
 * it as "expand to nothing" either way.  So {0,0} is read here as
 * absent, which agrees with what `$N` does with it and with what a
 * reader of `x(q)?(b)` expects.  The residual corner is a capture that
 * genuinely matched EMPTY AT BYTE 0 of the subject, which reports nil;
 * fixing that needs a real sentinel out of s20 and is not this
 * sprint's to change.
 */
static FlValue match_map(FlVm *vm, const FlStr *s, const SagReMatch *m,
                         bool with_groups)
{
    FlMap *r = fl_map_new(vm);
    u32 g;

    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, r));
    map_put(vm, r, "lo", FL_INT_V((i64)m->g[0].lo));
    map_put(vm, r, "hi", FL_INT_V((i64)m->g[0].hi));
    map_put(vm, r, "text", slice(vm, s, m->g[0].lo, m->g[0].hi));
    if (with_groups) {
        FlList *gs = fl_list_new(vm);

        fl_gc_protect(vm, FL_OBJ_V(FL_LIST, gs));
        for (g = 0U; g < m->ngroups; g++) {
            bool absent = g != 0U && m->g[g].lo == 0U && m->g[g].hi == 0U;

            if (absent) {
                (void)fl_list_push(vm, gs, FL_NIL_V);
            } else {
                SagReMatch one;

                (void)memset(&one, 0, sizeof(one));
                one.g[0] = m->g[g];
                one.ngroups = 1U;
                (void)fl_list_push(vm, gs, match_map(vm, s, &one, false));
            }
        }
        fl_gc_release(vm, 1U);
        map_put(vm, r, "groups", FL_OBJ_V(FL_LIST, gs));
    }
    fl_gc_release(vm, 1U);
    return FL_OBJ_V(FL_MAP, r);
}

/* ---------------------------------------------------------------- */
/* Scanning                                                         */
/* ---------------------------------------------------------------- */

/*
 * Where a scan resumes after a match ending at `hi`.
 *
 * A ZERO-WIDTH match advances one CODEPOINT, never one byte: resuming
 * mid-sequence would let the next match start inside a character and
 * report a span that splits it.  A zero-width match at the very end
 * ends the scan, which is what makes re.find_all("abc", "x*")
 * terminate.
 */
static u64 advance(const FlStr *s, u64 lo, u64 hi, bool *done)
{
    u32 cp;
    size_t k;

    *done = false;
    if (hi > lo)
        return hi;
    if (hi >= (u64)s->len) {
        *done = true;
        return hi;
    }
    k = sag_utf8_decode((const u8 *)s->b + hi, (size_t)s->len - hi, &cp);
    return hi + (u64)(k == 0U ? 1U : k);
}

typedef struct Scan {
    FlVm *vm;
    const FlStr *s;
    const SagRe *re;
    SagReInput in;
    u64 at;
    u64 count;
    u64 limit;
} Scan;

static bool scan_open(FlVm *vm, FlValue *a, u32 argc, u32 flagi, u32 limiti,
                      Scan *sc)
{
    const FlStr *s;
    const FlStr *pat;
    u32 flags;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &pat))
        return false;
    if (!read_flags(vm, a, flagi, argc, &flags))
        return false;
    if (limiti != 0U && !read_limit(vm, a, limiti, argc, &sc->limit))
        return false;
    if (limiti == 0U)
        sc->limit = (u64)-1;
    sc->re = compile_cached(vm, pat, flags);
    if (sc->re == NULL)
        return false;
    sc->vm = vm;
    sc->s = s;
    sc->in = sag_re_input_bytes((const u8 *)s->b, (u64)s->len);
    sc->at = 0U;
    sc->count = 0U;
    /* Pinned for the whole scan: `sc->re` must stay valid even if a
     * callback compiles something else.  Every exit path unpins. */
    g_cache.pin++;
    return true;
}

static void scan_close(void)
{
    if (g_cache.pin != 0U)
        g_cache.pin--;
}

/* ---------------------------------------------------------------- */
/* The template language                                            */
/* ---------------------------------------------------------------- */

/*
 * `$0`..`$9`, `${12}` for two digits, `$$` for a literal `$`.
 *
 * A `$` followed by anything else RAISES rather than emitting a literal
 * dollar.  Silently passing it through is how a `$1` typo becomes
 * invisible data loss in a whole-file replace: the run reports success
 * and the text is wrong.
 */
static bool expand(FlVm *vm, Bytebuf *out, const FlStr *tpl, const FlStr *s,
                   const SagReMatch *m, u32 ngroups)
{
    size_t i = 0U;

    while (i < (size_t)tpl->len) {
        u32 g;
        size_t start = i;

        if (tpl->b[i] != '$') {
            bytebuf_push_u8(out, (u8)tpl->b[i]);
            i++;
            continue;
        }
        i++;
        if (i >= (size_t)tpl->len)
            return fl_raise(vm, "type",
                            "re: the template ends in '$' at byte %zu",
                            start);
        if (tpl->b[i] == '$') {
            bytebuf_push_u8(out, (u8)'$');
            i++;
            continue;
        }
        if (tpl->b[i] >= '0' && tpl->b[i] <= '9') {
            g = (u32)(tpl->b[i] - '0');
            i++;
        } else if (tpl->b[i] == '{') {
            bool any = false;

            g = 0U;
            i++;
            while (i < (size_t)tpl->len && tpl->b[i] >= '0' &&
                   tpl->b[i] <= '9' && g < (u32)SAG_RE_MAX_GROUPS) {
                g = g * 10U + (u32)(tpl->b[i] - '0');
                any = true;
                i++;
            }
            if (!any || i >= (size_t)tpl->len || tpl->b[i] != '}')
                return fl_raise(vm, "type",
                                "re: expected ${N} at byte %zu", start);
            i++;
        } else {
            return fl_raise(vm, "type",
                            "re: '$' is followed by '%c' at byte %zu; write "
                            "'$$' for a literal dollar",
                            tpl->b[i], start);
        }
        if (g >= ngroups)
            return fl_raise(vm, "index",
                            "re: the template names group %u, the pattern "
                            "has %u", (unsigned)g, (unsigned)ngroups);
        /* A group that did not participate expands to nothing -- the
         * engine reports it as an empty span, and an empty expansion is
         * what every replace dialect does with it. */
        if (m != NULL && m->g[g].hi > m->g[g].lo)
            bytebuf_append(out, s->b + m->g[g].lo,
                           (size_t)(m->g[g].hi - m->g[g].lo));
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* The natives                                                      */
/* ---------------------------------------------------------------- */

static bool re_test(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *pat;
    const SagRe *re;
    SagReInput in;
    u32 flags;

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_str(vm, a, 1U, &pat))
        return false;
    if (!read_flags(vm, a, 2U, n, &flags))
        return false;
    /* NOCAPTURE: the caller asked a yes/no question, and promising not
     * to read groups lets the engine skip the slot bookkeeping. */
    re = compile_cached(vm, pat, flags | (u32)SAG_RE_NOCAPTURE);
    if (re == NULL)
        return false;
    in = sag_re_input_bytes((const u8 *)s->b, (u64)s->len);
    *out = FL_BOOL_V(sag_re_test(re, &in, BYTEOFF(0)));
    return true;
}

static bool re_find(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Scan sc;
    SagReMatch m;
    i64 from = 0;

    if (n >= 4U && a[3].t != (u8)FL_NIL && !fl_arg_int(vm, a, 3U, &from))
        return false;
    if (!scan_open(vm, a, n, 2U, 0U, &sc))
        return false;
    if (from < 0 || (u64)from > (u64)sc.s->len) {
        scan_close();
        return fl_raise(vm, "index",
                        "re.find: byte %lld is outside a subject of %u bytes",
                        (long long)from, (unsigned)sc.s->len);
    }
    if (sag_re_search(sc.re, &sc.in, BYTEOFF((u64)from), &m))
        *out = match_map(vm, sc.s, &m, true);
    else
        *out = FL_NIL_V;
    scan_close();
    return true;
}

static bool re_find_all(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Scan sc;
    FlList *r;
    SagReMatch m;
    bool done = false;

    if (!scan_open(vm, a, n, 2U, 3U, &sc))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    while (!done && sc.count < sc.limit &&
           sag_re_search(sc.re, &sc.in, BYTEOFF(sc.at), &m)) {
        if (r->n >= (u32)RE_MAX_RESULTS) {
            fl_gc_release(vm, 1U);
            scan_close();
            return fl_raise(vm, "limit", "re.find_all: more than %d matches",
                            RE_MAX_RESULTS);
        }
        (void)fl_list_push(vm, r, match_map(vm, sc.s, &m, true));
        sc.count++;
        sc.at = advance(sc.s, m.g[0].lo, m.g[0].hi, &done);
    }
    fl_gc_release(vm, 1U);
    scan_close();
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool re_replace(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Scan sc;
    const FlStr *tpl;
    SagReMatch m;
    Bytebuf o;
    bool done = false;
    u32 ngroups;

    if (!fl_arg_str(vm, a, 2U, &tpl))
        return false;
    if (!scan_open(vm, a, n, 3U, 4U, &sc))
        return false;
    ngroups = sag_re_group_count(sc.re);
    bytebuf_init(&o);
    while (!done && sc.count < sc.limit &&
           sag_re_search(sc.re, &sc.in, BYTEOFF(sc.at), &m)) {
        bytebuf_append(&o, sc.s->b + sc.at, (size_t)(m.g[0].lo - sc.at));
        if (!expand(vm, &o, tpl, sc.s, &m, ngroups)) {
            bytebuf_free(&o);
            scan_close();
            return false;
        }
        sc.count++;
        sc.at = advance(sc.s, m.g[0].lo, m.g[0].hi, &done);
        /*
         * A zero-width match copies the codepoint it stepped over, or
         * the subject would lose a character at every empty match.
         */
        if (m.g[0].hi == m.g[0].lo && sc.at > m.g[0].hi)
            bytebuf_append(&o, sc.s->b + m.g[0].hi,
                           (size_t)(sc.at - m.g[0].hi));
    }
    if (sc.at < (u64)sc.s->len)
        bytebuf_append(&o, sc.s->b + sc.at, (size_t)((u64)sc.s->len - sc.at));
    scan_close();
    *out = FL_OBJ_V(FL_STR, fl_str_take(vm, &o));
    bytebuf_free(&o);
    return true;
}

static bool re_replace_fn(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Scan sc;
    FlValue f;
    SagReMatch m;
    Bytebuf o;
    bool done = false;

    if (!fl_arg_fn(vm, a, 2U, &f))
        return false;
    if (!scan_open(vm, a, n, 3U, 4U, &sc))
        return false;
    bytebuf_init(&o);
    while (!done && sc.count < sc.limit &&
           sag_re_search(sc.re, &sc.in, BYTEOFF(sc.at), &m)) {
        FlValue arg = match_map(vm, sc.s, &m, true);
        FlValue got = FL_NIL_V;

        bytebuf_append(&o, sc.s->b + sc.at, (size_t)(m.g[0].lo - sc.at));
        fl_gc_protect(vm, arg);
        /* The callback may itself use `re`; the pin taken by scan_open
         * is what keeps sc.re alive across it. */
        if (!fl_call(vm, f, &arg, 1U, &got)) {
            fl_gc_release(vm, 1U);
            bytebuf_free(&o);
            scan_close();
            return false;
        }
        fl_gc_release(vm, 1U);
        if (got.t != (u8)FL_STR) {
            bytebuf_free(&o);
            scan_close();
            return fl_raise(vm, "type",
                            "re.replace_fn: the function must return str, "
                            "found %s", fl_type_name((FlType)got.t));
        }
        bytebuf_append(&o, ((const FlStr *)got.as.o)->b,
                       (size_t)((const FlStr *)got.as.o)->len);
        sc.count++;
        sc.at = advance(sc.s, m.g[0].lo, m.g[0].hi, &done);
        if (m.g[0].hi == m.g[0].lo && sc.at > m.g[0].hi)
            bytebuf_append(&o, sc.s->b + m.g[0].hi,
                           (size_t)(sc.at - m.g[0].hi));
    }
    if (sc.at < (u64)sc.s->len)
        bytebuf_append(&o, sc.s->b + sc.at, (size_t)((u64)sc.s->len - sc.at));
    scan_close();
    *out = FL_OBJ_V(FL_STR, fl_str_take(vm, &o));
    bytebuf_free(&o);
    return true;
}

static bool re_split(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Scan sc;
    FlList *r;
    SagReMatch m;
    bool done = false;

    if (!scan_open(vm, a, n, 2U, 3U, &sc))
        return false;
    r = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, r));
    while (!done && sc.count < sc.limit &&
           sag_re_search(sc.re, &sc.in, BYTEOFF(sc.at), &m)) {
        u64 next = advance(sc.s, m.g[0].lo, m.g[0].hi, &done);

        /* A zero-width match at the very end adds nothing: the tail
         * push below already covers it, and emitting both is how the
         * same subject came back with two empty pieces. */
        if (done && m.g[0].hi == m.g[0].lo)
            break;
        /* A zero-width separator would otherwise emit an empty piece
         * for every position; the piece runs to where the scan
         * resumes, so "abc" split on "x*" yields a, b, c. */
        (void)fl_list_push(vm, r,
                           slice(vm, sc.s, sc.at,
                                 m.g[0].hi > m.g[0].lo ? m.g[0].lo : next));
        sc.at = next;
        sc.count++;
        if (r->n >= (u32)RE_MAX_RESULTS) {
            fl_gc_release(vm, 1U);
            scan_close();
            return fl_raise(vm, "limit", "re.split: more than %d pieces",
                            RE_MAX_RESULTS);
        }
    }
    (void)fl_list_push(vm, r, slice(vm, sc.s, sc.at, (u64)sc.s->len));
    fl_gc_release(vm, 1U);
    scan_close();
    *out = FL_OBJ_V(FL_LIST, r);
    return true;
}

static bool re_escape(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    Bytebuf o;

    (void)n;
    if (!fl_arg_str(vm, a, 0U, &s))
        return false;
    bytebuf_init(&o);
    /* s21's own escaper, not a second one: `*`, `#`, `:s//` and this
     * all have to agree about what a metacharacter is. */
    sag_re_quote(&o, (const u8 *)s->b, (size_t)s->len);
    *out = FL_OBJ_V(FL_STR, fl_str_take(vm, &o));
    bytebuf_free(&o);
    return true;
}

/* ---------------------------------------------------------------- */

static const FlNativeDef RE_DEFS[] = {
    {"test",       re_test,       2U, 3U, 0U, "(s, pat, [flags]) -> bool"},
    {"find",       re_find,       2U, 4U, 0U,
     "(s, pat, [flags], [from]) -> map|nil"},
    {"find_all",   re_find_all,   2U, 4U, 0U,
     "(s, pat, [flags], [limit]) -> list"},
    {"replace",    re_replace,    3U, 5U, 0U,
     "(s, pat, tmpl, [flags], [limit]) -> str"},
    {"replace_fn", re_replace_fn, 3U, 5U, 0U,
     "(s, pat, f, [flags], [limit]) -> str"},
    {"split",      re_split,      2U, 4U, 0U,
     "(s, pat, [flags], [limit]) -> list"},
    {"escape",     re_escape,     1U, 1U, 0U, "(s) -> str"}
};

const FlModuleDef fl_mod_re = {
    "re", RE_DEFS, (u32)SAG_ARRAY_LEN(RE_DEFS), NULL, 0U
};
