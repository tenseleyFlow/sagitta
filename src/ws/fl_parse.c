/*
 * Sprint 25 §4: the parser.
 *
 * TOTAL over the §2 subset, and never trusting.  A state file is a
 * CACHE that arbitrary bytes may have replaced — corruption, a
 * truncated write from an older build, a user hand-editing to unstick
 * something.  Every failure mode ends with the editor starting.
 *
 * Syntax and the structural caps live here.  Ranges, enum spellings and
 * required-field checks live in the schema layer, where a failure can
 * name a field in the log.  Merging the two would mean rewriting both
 * in Sprint 36 instead of just this one.
 */
#include "ws/fllit.h"

#include <string.h>

#include "util/log.h"

typedef struct P {
    Arena *a;
    const u8 *src;
    u64 len;
    u64 at;
    u32 line, col;
    u32 nodes;
    u32 depth;
    FlParseErr *err;
    bool failed;
} P;

static void fail(P *p, const char *msg)
{
    if (p->failed)
        return; /* keep the FIRST failure: it is the cause */
    p->failed = true;
    if (p->err != NULL) {
        p->err->off = p->at;
        p->err->line = p->line;
        p->err->col = p->col;
        p->err->msg = msg;
    }
}

static bool at_end(const P *p)
{
    return p->at >= p->len;
}

static u8 peek(const P *p)
{
    return p->at < p->len ? p->src[p->at] : 0U;
}

static u8 advance(P *p)
{
    u8 c = p->src[p->at++];

    /* line:col is for the LOG, not a caret dialog — this is not
     * user-authored source (Sprint 36's config is, and that is where
     * s29's diagnostics get reused). */
    if (c == (u8)'\n') {
        p->line++;
        p->col = 1U;
    } else {
        p->col++;
    }
    return c;
}

/*
 * Whitespace and comments.
 *
 * `#` to end of line is ACCEPTED and never emitted.  A user hand-
 * editing their state file to unstick something must not hit a parse
 * error; the notes are lost on the next write, and the manual says so
 * rather than the parser discovering it for them.
 */
static void skip_ws(P *p)
{
    while (!at_end(p)) {
        u8 c = peek(p);

        if (c == (u8)' ' || c == (u8)'\t' || c == (u8)'\n' ||
            c == (u8)'\r') {
            (void)advance(p);
            continue;
        }
        if (c == (u8)'#') {
            while (!at_end(p) && peek(p) != (u8)'\n')
                (void)advance(p);
            continue;
        }
        return;
    }
}

static FlLit *node(P *p, WsFlLitKind kind)
{
    FlLit *v;

    if (++p->nodes > (u32)SAG_FL_MAX_NODES) {
        fail(p, "node cap exceeded");
        return NULL;
    }
    v = arena_alloc(p->a, sizeof(*v), sizeof(void *));
    (void)memset(v, 0, sizeof(*v));
    v->kind = kind;
    return v;
}

static void push_item(P *p, FlLit *c, const char *key, u64 keylen,
                      FlLit *item)
{
    if (c->len == c->cap) {
        u32 cap = c->cap == 0U ? 4U : c->cap * 2U;
        FlLit **items = arena_alloc(p->a, (size_t)cap * sizeof(*items),
                                    sizeof(void *));

        if (c->items != NULL)
            (void)memcpy(items, c->items, (size_t)c->len * sizeof(*items));
        c->items = items;
        if (c->kind == FL_LIT_MAP) {
            const char **keys =
                arena_alloc(p->a, (size_t)cap * sizeof(*keys),
                            sizeof(void *));
            u64 *lens = arena_alloc(p->a, (size_t)cap * sizeof(*lens),
                                    sizeof(u64));

            if (c->keys != NULL) {
                (void)memcpy(keys, c->keys, (size_t)c->len * sizeof(*keys));
                (void)memcpy(lens, c->keylens,
                             (size_t)c->len * sizeof(*lens));
            }
            c->keys = keys;
            c->keylens = lens;
        }
        c->cap = cap;
    }
    c->items[c->len] = item;
    if (c->kind == FL_LIT_MAP) {
        c->keys[c->len] = key;
        c->keylens[c->len] = keylen;
    }
    c->len++;
}

static int hexval(u8 c)
{
    if (c >= (u8)'0' && c <= (u8)'9')
        return c - (u8)'0';
    if (c >= (u8)'a' && c <= (u8)'f')
        return c - (u8)'a' + 10;
    if (c >= (u8)'A' && c <= (u8)'F')
        return c - (u8)'A' + 10;
    return -1;
}

/*
 * Strings are BYTES.
 *
 * Never validated as UTF-8, never normalized, never replaced.  A path
 * that is not valid UTF-8 is still a path, and rejecting it here would
 * lose exactly the files invariant 2 exists to protect.
 */
static FlLit *parse_string(P *p)
{
    FlLit *v;
    u8 *buf;
    u64 n = 0U;
    u64 cap;
    u64 start;

    (void)advance(p); /* the opening quote */
    start = p->at;
    /* One pass to size, one to fill: the arena has no realloc, and a
     * string is bounded by the cap anyway. */
    {
        u64 scan = p->at;
        u64 count = 0U;

        while (scan < p->len && p->src[scan] != (u8)'"') {
            if (p->src[scan] == (u8)'\\') {
                scan++;
                if (scan >= p->len)
                    break;
                if (p->src[scan] == (u8)'x')
                    scan += 2U;
            }
            scan++;
            count++;
        }
        cap = count + 1U;
    }
    if (cap - 1U > (u64)SAG_FL_MAX_STRING) {
        fail(p, "string exceeds the 4096-byte cap");
        return NULL;
    }
    buf = arena_alloc(p->a, (size_t)cap, 1U);
    p->at = start;
    for (;;) {
        u8 c;

        if (at_end(p)) {
            fail(p, "unterminated string");
            return NULL;
        }
        c = advance(p);
        if (c == (u8)'"')
            break;
        if (c != (u8)'\\') {
            buf[n++] = c;
            continue;
        }
        if (at_end(p)) {
            fail(p, "unterminated escape");
            return NULL;
        }
        c = advance(p);
        switch (c) {
        case (u8)'"':
            buf[n++] = (u8)'"';
            break;
        case (u8)'\\':
            buf[n++] = (u8)'\\';
            break;
        case (u8)'n':
            buf[n++] = (u8)'\n';
            break;
        case (u8)'t':
            buf[n++] = (u8)'\t';
            break;
        case (u8)'r':
            buf[n++] = (u8)'\r';
            break;
        case (u8)'0':
            buf[n++] = 0U;
            break;
        case (u8)'x': {
            int hi;
            int lo;

            if (p->at + 1U >= p->len) {
                fail(p, "truncated \\xNN escape");
                return NULL;
            }
            hi = hexval(advance(p));
            lo = hexval(advance(p));
            if (hi < 0 || lo < 0) {
                fail(p, "bad \\xNN escape");
                return NULL;
            }
            buf[n++] = (u8)((hi << 4) | lo);
            break;
        }
        default:
            /* Anything outside the pinned table is rejected rather
             * than passed through: a format with an open-ended escape
             * set cannot be reimplemented compatibly. */
            fail(p, "unknown escape");
            return NULL;
        }
    }
    v = node(p, FL_LIT_STR);
    if (v == NULL)
        return NULL;
    buf[n] = 0U;
    v->s = (const char *)buf;
    v->slen = n;
    return v;
}

static FlLit *parse_value(P *p);

/* A bare identifier key, or a quoted one.  Option keys carry dots, so
 * `"tabs.group_hover_preview"` has to be expressible. */
static bool parse_key(P *p, const char **key, u64 *keylen)
{
    u64 start;

    if (peek(p) == (u8)'"') {
        FlLit *s = parse_string(p);

        if (s == NULL)
            return false;
        *key = s->s;
        *keylen = s->slen;
        return true;
    }
    start = p->at;
    while (!at_end(p)) {
        u8 c = peek(p);

        if ((c >= (u8)'a' && c <= (u8)'z') ||
            (c >= (u8)'A' && c <= (u8)'Z') ||
            (c >= (u8)'0' && c <= (u8)'9') || c == (u8)'_' ||
            c == (u8)'.') {
            (void)advance(p);
            continue;
        }
        break;
    }
    if (p->at == start) {
        fail(p, "expected a key");
        return false;
    }
    {
        u64 n = p->at - start;
        char *copy = arena_alloc(p->a, (size_t)n + 1U, 1U);

        (void)memcpy(copy, p->src + start, (size_t)n);
        copy[n] = '\0';
        *key = copy;
        *keylen = n;
    }
    return true;
}

static FlLit *parse_container(P *p, bool is_map)
{
    FlLit *c = node(p, is_map ? FL_LIT_MAP : FL_LIT_LIST);
    u8 closer = is_map ? (u8)'}' : (u8)']';

    if (c == NULL)
        return NULL;
    if (++p->depth > (u32)SAG_FL_MAX_DEPTH) {
        fail(p, "nesting deeper than 32");
        return NULL;
    }
    (void)advance(p); /* the opener */
    for (;;) {
        const char *key = NULL;
        u64 keylen = 0U;
        FlLit *item;

        skip_ws(p);
        if (at_end(p)) {
            fail(p, "unterminated container");
            return NULL;
        }
        if (peek(p) == closer) {
            (void)advance(p);
            break;
        }
        if (is_map) {
            if (!parse_key(p, &key, &keylen))
                return NULL;
            skip_ws(p);
            if (at_end(p) || advance(p) != (u8)':') {
                fail(p, "expected ':' after a key");
                return NULL;
            }
            skip_ws(p);
        }
        item = parse_value(p);
        if (item == NULL)
            return NULL;
        push_item(p, c, key, keylen, item);
        skip_ws(p);
        /*
         * The trailing comma is REQUIRED after every element, so a
         * diff between two sessions is a line diff and not a line diff
         * plus comma churn.  A missing one before the closer is
         * accepted rather than fatal — the format is written by us and
         * read from disk, and being strict about punctuation on a
         * cache buys nothing.
         */
        if (!at_end(p) && peek(p) == (u8)',') {
            (void)advance(p);
            continue;
        }
        skip_ws(p);
        if (!at_end(p) && peek(p) == closer) {
            (void)advance(p);
            break;
        }
        fail(p, "expected ',' or a closer");
        return NULL;
    }
    p->depth--;
    return c;
}

static bool word_is(const P *p, u64 at, const char *w)
{
    u64 n = (u64)strlen(w);

    return at + n <= p->len && memcmp(p->src + at, w, (size_t)n) == 0;
}

static FlLit *parse_value(P *p)
{
    u8 c;

    skip_ws(p);
    if (at_end(p)) {
        fail(p, "expected a value");
        return NULL;
    }
    c = peek(p);
    if (c == (u8)'{')
        return parse_container(p, true);
    if (c == (u8)'[')
        return parse_container(p, false);
    if (c == (u8)'"')
        return parse_string(p);
    if (word_is(p, p->at, "true")) {
        FlLit *v = node(p, FL_LIT_BOOL);

        p->at += 4U;
        p->col += 4U;
        if (v != NULL)
            v->i = 1;
        return v;
    }
    if (word_is(p, p->at, "false")) {
        FlLit *v = node(p, FL_LIT_BOOL);

        p->at += 5U;
        p->col += 5U;
        if (v != NULL)
            v->i = 0;
        return v;
    }
    if (word_is(p, p->at, "nil")) {
        p->at += 3U;
        p->col += 3U;
        return node(p, FL_LIT_NIL);
    }
    if (c == (u8)'-' || (c >= (u8)'0' && c <= (u8)'9')) {
        FlLit *v;
        bool neg = false;
        /* Accumulated UNSIGNED so INT64_MIN is representable without
         * signed overflow, which is undefined and which a fuzzer will
         * find. */
        u64 mag = 0U;
        bool over = false;
        bool any = false;

        if (c == (u8)'-') {
            neg = true;
            (void)advance(p);
        }
        while (!at_end(p)) {
            u8 d = peek(p);

            if (d < (u8)'0' || d > (u8)'9')
                break;
            (void)advance(p);
            any = true;
            if (mag > (~(u64)0 - 9U) / 10U)
                over = true;
            else
                mag = mag * 10U + (u64)(d - (u8)'0');
        }
        if (!any) {
            fail(p, "expected digits");
            return NULL;
        }
        if (over || mag > (neg ? 9223372036854775808ULL
                               : 9223372036854775807ULL)) {
            fail(p, "integer out of range");
            return NULL;
        }
        v = node(p, FL_LIT_INT);
        if (v == NULL)
            return NULL;
        if (neg)
            v->i = mag == 9223372036854775808ULL ? (-9223372036854775807LL - 1)
                                                 : -(i64)mag;
        else
            v->i = (i64)mag;
        return v;
    }
    fail(p, "unexpected byte");
    return NULL;
}

FlLit *sag_fl_parse(Arena *a, const u8 *src, u64 len, FlParseErr *err)
{
    P p;
    FlLit *root;

    static const u8 nothing = 0U;

    if (err != NULL)
        (void)memset(err, 0, sizeof(*err));
    if (a == NULL)
        return NULL;
    /*
     * An EMPTY document is a document, and it must be rejected with a
     * position like any other.  A zero-length Bytebuf has a NULL data
     * pointer, so returning early here left `err` all zeroes — and a
     * failure reporting line 0 tells the §7 log line nothing and gives
     * the user nowhere to look.
     */
    if (src == NULL) {
        if (len != 0U)
            return NULL; /* a real caller bug: length without bytes */
        src = &nothing;
    }
    if (len > (u64)SAG_FL_MAX_BYTES) {
        if (err != NULL) {
            err->msg = "document exceeds 8 MiB";
            err->line = 1U;
            err->col = 1U;
        }
        return NULL;
    }
    (void)memset(&p, 0, sizeof(p));
    p.a = a;
    p.src = src;
    p.len = len;
    p.line = 1U;
    p.col = 1U;
    p.err = err;
    root = parse_value(&p);
    if (root == NULL || p.failed)
        return NULL;
    skip_ws(&p);
    if (!at_end(&p)) {
        fail(&p, "trailing bytes after the document");
        return NULL;
    }
    return root;
}

/* ---------------------------------------------------------------- */
/* Accessors                                                        */
/* ---------------------------------------------------------------- */

#ifndef SAG_STATE_ACCESSORS_EXTERNAL

const FlLit *sag_fl_get(const FlLit *map, const char *key)
{
    u32 i;
    u64 n;

    if (map == NULL || map->kind != FL_LIT_MAP || key == NULL)
        return NULL;
    n = (u64)strlen(key);
    /*
     * A linear scan of an insertion-ordered array, on purpose.  At
     * schema sizes it beats hashing outright, and a hash map's
     * iteration order would leak into re-emission and break
     * determinism.
     */
    for (i = 0U; i < map->len; i++) {
        if (map->keylens[i] == n &&
            memcmp(map->keys[i], key, (size_t)n) == 0)
            return map->items[i];
    }
    return NULL;
}

u32 sag_fl_len(const FlLit *list)
{
    if (list == NULL || (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP))
        return 0U;
    return list->len;
}

const FlLit *sag_fl_at(const FlLit *list, u32 i)
{
    if (list == NULL || (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP) ||
        i >= list->len)
        return NULL;
    return list->items[i];
}

i64 sag_fl_int_or(const FlLit *v, i64 dflt)
{
    return v != NULL && v->kind == FL_LIT_INT ? v->i : dflt;
}

bool sag_fl_bool_or(const FlLit *v, bool dflt)
{
    return v != NULL && v->kind == FL_LIT_BOOL ? v->i != 0 : dflt;
}

const char *sag_fl_str_or(const FlLit *v, const char *dflt, u64 *n)
{
    if (v != NULL && v->kind == FL_LIT_STR) {
        if (n != NULL)
            *n = v->slen;
        return v->s;
    }
    if (n != NULL)
        *n = dflt == NULL ? 0U : (u64)strlen(dflt);
    return dflt;
}
#endif /* SAG_STATE_ACCESSORS_EXTERNAL */
