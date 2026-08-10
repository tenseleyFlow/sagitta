/* Sprint 36: adapt the real Fletch pure-literal value tree to v1's mapper. */
#include "ws/state.h"

#include <string.h>

#include "fl/data.h"
#include "fl/diag.h"
#include "fl/vm.h"
#include "util/intern.h"

typedef struct StateDiag {
    Arena *arena;
    FlParseErr *err;
    const u8 *src;
    u64 len;
    bool seen;
} StateDiag;

static void state_diag_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                            const char *msg, const char *rendered)
{
    StateDiag *sd = ctx;

    (void)rendered;
    if (level != FL_DIAG_ERROR || sd->seen)
        return;
    sd->seen = true;
    if (sd->err != NULL) {
        u32 line = sp.line;
        u32 col = sp.col;

        if (strstr(msg, "integer literal does not fit") != NULL) {
            u64 at = 0U;
            u32 line = 1U;
            u32 here = 1U;

            while (at < sd->len &&
                   (line < sp.line || here < sp.col)) {
                if (sd->src[at++] == (u8)'\n') {
                    line++;
                    here = 1U;
                } else {
                    here++;
                }
            }
            if (at < sd->len && sd->src[at] == (u8)'-') {
                at++;
                col++;
            }
            while (at < sd->len && sd->src[at] >= (u8)'0' &&
                   sd->src[at] <= (u8)'9') {
                at++;
                col++;
            }
        } else if (strstr(msg, "unknown escape in string literal") != NULL) {
            col += 2U;
        } else if (strstr(msg, "expected ':', found 'identifier'") != NULL) {
            col += 1U;
        } else if (strstr(msg, "unterminated string literal") != NULL ||
                   strstr(msg, "newline in string literal") != NULL) {
            u64 at;

            line = 1U;
            col = 1U;
            for (at = 0U; at < sd->len; at++) {
                if (sd->src[at] == (u8)'\n') {
                    line++;
                    col = 1U;
                } else {
                    col++;
                }
            }
        }
        sd->err->line = line;
        sd->err->col = col;
        sd->err->off = 0U;
        sd->err->msg = arena_strdup(sd->arena, msg);
    }
}

static FlLit *state_lit_new(Arena *a, WsFlLitKind kind)
{
    FlLit *lit = arena_alloc(a, sizeof(*lit), sizeof(void *));

    (void)memset(lit, 0, sizeof(*lit));
    lit->kind = kind;
    return lit;
}

static void state_lit_push(Arena *a, FlLit *parent, const char *key,
                           u64 keylen, FlLit *child)
{
    if (parent->len == parent->cap) {
        u32 cap = parent->cap == 0U ? 4U : parent->cap * 2U;
        FlLit **items = arena_alloc(a, (size_t)cap * sizeof(*items),
                                    sizeof(void *));

        if (parent->items != NULL)
            (void)memcpy(items, parent->items,
                         (size_t)parent->len * sizeof(*items));
        parent->items = items;
        if (parent->kind == FL_LIT_MAP) {
            const char **keys = arena_alloc(a,
                                            (size_t)cap * sizeof(*keys),
                                            sizeof(void *));
            u64 *lens = arena_alloc(a, (size_t)cap * sizeof(*lens),
                                    sizeof(u64));

            if (parent->keys != NULL) {
                (void)memcpy(keys, parent->keys,
                             (size_t)parent->len * sizeof(*keys));
                (void)memcpy(lens, parent->keylens,
                             (size_t)parent->len * sizeof(*lens));
            }
            parent->keys = keys;
            parent->keylens = lens;
        }
        parent->cap = cap;
    }
    parent->items[parent->len] = child;
    if (parent->kind == FL_LIT_MAP) {
        parent->keys[parent->len] = key;
        parent->keylens[parent->len] = keylen;
    }
    parent->len++;
}

static FlLit *state_value_to_lit(Arena *a, FlValue value)
{
    FlLit *lit;
    u32 i;

    switch ((FlType)value.t) {
    case FL_NIL:
        return state_lit_new(a, FL_LIT_NIL);
    case FL_BOOL:
        lit = state_lit_new(a, FL_LIT_BOOL);
        lit->i = value.as.b ? 1 : 0;
        return lit;
    case FL_INT:
        lit = state_lit_new(a, FL_LIT_INT);
        lit->i = value.as.i;
        return lit;
    case FL_STR: {
        const FlStr *s = (const FlStr *)value.as.o;

        lit = state_lit_new(a, FL_LIT_STR);
        lit->s = arena_strndup(a, s->b, s->len);
        lit->slen = s->len;
        return lit;
    }
    case FL_LIST: {
        const FlList *list = (const FlList *)value.as.o;

        lit = state_lit_new(a, FL_LIT_LIST);
        for (i = 0U; i < list->n; i++) {
            FlLit *child = state_value_to_lit(a, list->v[i]);

            if (child == NULL)
                return NULL;
            state_lit_push(a, lit, NULL, 0U, child);
        }
        return lit;
    }
    case FL_MAP: {
        const FlMap *map = (const FlMap *)value.as.o;
        u32 cursor = 0U;
        FlValue key;
        FlValue val;

        lit = state_lit_new(a, FL_LIT_MAP);
        while (fl_map_iter(map, &cursor, &key, &val)) {
            const FlStr *s;
            const char *copy;

            if (key.t != (u8)FL_STR)
                return NULL;
            s = (const FlStr *)key.as.o;
            copy = arena_strndup(a, s->b, s->len);
            state_lit_push(a, lit, copy, s->len,
                           state_value_to_lit(a, val));
            if (lit->items[lit->len - 1U] == NULL)
                return NULL;
        }
        return lit;
    }
    case FL_FLOAT:
    default:
        /* The frozen workspace schema has no floats or runtime values. */
        return NULL;
    }
}

FlLit *yew_fl_parse_fletch(Arena *a, const u8 *src, u64 len,
                           FlParseErr *err)
{
    Interner in;
    DiagCtx dc;
    FlVm vm;
    StateDiag sink;
    FlValue value;
    FlLit *result = NULL;
    static const u8 nothing = 0U;

    if (err != NULL)
        (void)memset(err, 0, sizeof(*err));
    if (a == NULL || len > (u64)SIZE_MAX)
        return NULL;
    if (src == NULL) {
        if (len != 0U)
            return NULL;
        src = &nothing;
    }
    interner_init(&in, a);
    fl_diag_init(&dc, a);
    sink.arena = a;
    sink.err = err;
    sink.src = src;
    sink.len = len;
    sink.seen = false;
    fl_diag_set_sink(&dc, state_diag_sink, &sink);
    (void)fl_vm_init(&vm, a, &in, &dc);
    value = fl_data_read(&vm, (const char *)src, (size_t)len, &dc);
    if (fl_diag_errors(&dc) == 0U)
        result = state_value_to_lit(a, value);
    if (result == NULL && fl_diag_errors(&dc) == 0U && err != NULL) {
        err->line = 1U;
        err->col = 1U;
        err->msg = "workspace state contains a non-schema literal";
    }
    fl_vm_free(&vm);
    interner_free(&in);
    return result;
}
