/* Frozen-schema accessors.  The hand-written codec itself is test-only. */
#include "ws/state.h"

#include <string.h>

#include "util/log.h"
#include "util/sort.h"

const FlLit *yew_fl_get(const FlLit *map, const char *key)
{
    u32 i;
    u64 n;

    if (map == NULL || map->kind != FL_LIT_MAP || key == NULL)
        return NULL;
    n = (u64)strlen(key);
    for (i = 0U; i < map->len; i++) {
        if (map->keylens[i] == n &&
            memcmp(map->keys[i], key, (size_t)n) == 0)
            return map->items[i];
    }
    return NULL;
}

u32 yew_fl_len(const FlLit *list)
{
    if (list == NULL ||
        (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP))
        return 0U;
    return list->len;
}

const FlLit *yew_fl_at(const FlLit *list, u32 i)
{
    if (list == NULL ||
        (list->kind != FL_LIT_LIST && list->kind != FL_LIT_MAP) ||
        i >= list->len)
        return NULL;
    return list->items[i];
}

i64 yew_fl_int_or(const FlLit *v, i64 dflt)
{
    return v != NULL && v->kind == FL_LIT_INT ? v->i : dflt;
}

bool yew_fl_bool_or(const FlLit *v, bool dflt)
{
    return v != NULL && v->kind == FL_LIT_BOOL ? v->i != 0 : dflt;
}

const char *yew_fl_str_or(const FlLit *v, const char *dflt, u64 *n)
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

/* ---------------------------------------------------------------- */
/* YEW-F-079: sparse retained entity records                        */
/* ---------------------------------------------------------------- */

static const char *const group_keys[] = {
    "id", "label", "dir_path", "last_active_member"
};
static const char *const tab_keys[] = {
    "id", "path", "group", "group_ordinal", "deferred", "focus",
    "panes", "wins"
};
static const char *const pane_keys[] = {
    "win", "split", "ratio_permille", "a", "b"
};
static const char *const win_keys[] = {
    "cursors", "primary", "view", "jumps"
};
static const char *const cursor_keys[] = {"pos", "anchor", "goal"};
static const char *const view_keys[] = {"top", "top_sub", "left", "wrap"};
static const char *const ring_keys[] = {"cur", "entries"};
static const char *const ring_entry_keys[] = {
    "path", "line", "col", "stamp"
};
static const char *const file_keys[] = {"path", "marks", "changes", "undo"};
/* line/col/bias are frozen v1 spellings even though current state uses pos. */
static const char *const mark_keys[] = {"name", "pos", "line", "col", "bias"};
static const char *const undo_keys[] = {"file", "version"};

static const char *const *record_keys(WsRecordKind kind, u32 *n)
{
    const char *const *keys = NULL;

    *n = 0U;
    switch (kind) {
    case YEW_STATE_REC_GROUP:
        keys = group_keys;
        *n = YEW_ARRAY_LEN(group_keys);
        break;
    case YEW_STATE_REC_TAB:
        keys = tab_keys;
        *n = YEW_ARRAY_LEN(tab_keys);
        break;
    case YEW_STATE_REC_PANE:
        keys = pane_keys;
        *n = YEW_ARRAY_LEN(pane_keys);
        break;
    case YEW_STATE_REC_WIN:
        keys = win_keys;
        *n = YEW_ARRAY_LEN(win_keys);
        break;
    case YEW_STATE_REC_CURSOR:
        keys = cursor_keys;
        *n = YEW_ARRAY_LEN(cursor_keys);
        break;
    case YEW_STATE_REC_VIEW:
        keys = view_keys;
        *n = YEW_ARRAY_LEN(view_keys);
        break;
    case YEW_STATE_REC_JUMPS:
    case YEW_STATE_REC_CHANGES:
        keys = ring_keys;
        *n = YEW_ARRAY_LEN(ring_keys);
        break;
    case YEW_STATE_REC_JUMP_ENTRY:
    case YEW_STATE_REC_CHANGE_ENTRY:
        keys = ring_entry_keys;
        *n = YEW_ARRAY_LEN(ring_entry_keys);
        break;
    case YEW_STATE_REC_FILE:
        keys = file_keys;
        *n = YEW_ARRAY_LEN(file_keys);
        break;
    case YEW_STATE_REC_MARK:
        keys = mark_keys;
        *n = YEW_ARRAY_LEN(mark_keys);
        break;
    case YEW_STATE_REC_UNDO:
        keys = undo_keys;
        *n = YEW_ARRAY_LEN(undo_keys);
        break;
    case YEW_STATE_REC_COUNT:
    default:
        break;
    }
    return keys;
}

bool yew_state_record_key_known(WsRecordKind kind, const char *key,
                                u64 key_len)
{
    const char *const *keys;
    u32 n;
    u32 i;

    if (key == NULL)
        return false;
    keys = record_keys(kind, &n);
    for (i = 0U; i < n; i++) {
        size_t len = strlen(keys[i]);

        if ((u64)len == key_len && memcmp(keys[i], key, len) == 0)
            return true;
    }
    return false;
}

static bool record_has_unknown(WsRecordKind kind, const FlLit *lit)
{
    u32 i;

    if (lit == NULL || lit->kind != FL_LIT_MAP)
        return false;
    for (i = 0U; i < lit->len; i++)
        if (!yew_state_record_key_known(kind, lit->keys[i],
                                        lit->keylens[i]))
            return true;
    return false;
}

void yew_state_records_reset(WsState *s)
{
    if (s == NULL)
        return;
    s->records_len = 0U;
    s->next_record_token = 1U;
}

void yew_state_records_free(WsState *s)
{
    if (s == NULL)
        return;
    yew_xfree(s->records);
    s->records = NULL;
    s->records_len = 0U;
    s->records_cap = 0U;
    s->next_record_token = 0U;
}

void yew_state_record_retain(WsState *s, WsRecordKind kind, u32 owner,
                             u64 entity, const FlLit *lit)
{
    WsRetainedRecord *r;

    if (s == NULL || !record_has_unknown(kind, lit))
        return;
    if (s->records_len == s->records_cap) {
        u32 cap = s->records_cap == 0U ? 16U : s->records_cap * 2U;

        s->records = yew_xreallocarray(s->records, cap,
                                        sizeof(*s->records));
        s->records_cap = cap;
    }
    r = &s->records[s->records_len++];
    r->lit = lit;
    r->entity = entity;
    r->owner = owner;
    r->kind = (u8)kind;
}

u32 yew_state_record_retain_token(WsState *s, WsRecordKind kind,
                                  u32 owner, const FlLit *lit)
{
    u32 token;

    if (s == NULL || !record_has_unknown(kind, lit))
        return 0U;
    if (s->next_record_token == 0U ||
        s->next_record_token == UINT32_MAX)
        YEW_BUG("workspace state: retained-record token overflow");
    token = s->next_record_token++;
    yew_state_record_retain(s, kind, owner, token, lit);
    return token;
}

static int record_cmp(const void *a, const void *b, void *ctx)
{
    const WsRetainedRecord *x = a;
    const WsRetainedRecord *y = b;

    (void)ctx;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    if (x->owner != y->owner)
        return x->owner < y->owner ? -1 : 1;
    if (x->entity != y->entity)
        return x->entity < y->entity ? -1 : 1;
    return 0;
}

void yew_state_records_finish(WsState *s)
{
    u32 read;
    u32 write = 0U;

    if (s == NULL || s->records_len == 0U)
        return;
    yew_sort_stable(s->records, s->records_len, sizeof(*s->records),
                    record_cmp, NULL);
    for (read = 0U; read < s->records_len;) {
        u32 last = read;

        while (last + 1U < s->records_len &&
               record_cmp(&s->records[last], &s->records[last + 1U],
                          NULL) == 0)
            last++;
        /* Applying repeated records is last-wins; retention must agree. */
        s->records[write++] = s->records[last];
        read = last + 1U;
    }
    s->records_len = write;
}

const FlLit *yew_state_record_get(const WsState *s, WsRecordKind kind,
                                  u32 owner, u64 entity)
{
    u32 lo = 0U;
    u32 hi;
    WsRetainedRecord key;

    if (s == NULL)
        return NULL;
    hi = s->records_len;
    (void)memset(&key, 0, sizeof(key));
    key.kind = (u8)kind;
    key.owner = owner;
    key.entity = entity;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        int cmp = record_cmp(&s->records[mid], &key, NULL);

        if (cmp < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo < s->records_len &&
        record_cmp(&s->records[lo], &key, NULL) == 0)
        return s->records[lo].lit;
    return NULL;
}
