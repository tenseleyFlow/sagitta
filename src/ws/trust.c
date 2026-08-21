#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/trust.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/data.h"
#include "fl/gc.h"
#include "fl/vm.h"
#include "text/file.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/xdg.h"
#include "ws/workspace.h"

typedef struct TrustImpl {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlValue root;
} TrustImpl;

typedef struct TrustEntry {
    const char *state;
    u32 state_len;
    AiWsGrant ai;
    const char *hash;
    u32 hash_len;
    i64 dev;
    i64 ino;
    i64 at;
    bool has_state;
    bool identity;
    bool fingerprint;
} TrustEntry;

typedef struct TrustRow {
    FlValue key;
    FlValue value;
} TrustRow;

static bool trust_str(FlValue value, const char **s, u32 *n)
{
    const FlStr *str;

    if (value.t != (u8)FL_STR)
        return false;
    str = (const FlStr *)value.as.o;
    if (s != NULL)
        *s = str->b;
    if (n != NULL)
        *n = str->len;
    return true;
}

static bool trust_key_eq(FlValue key, const char *want)
{
    const FlStr *s;
    size_t n = strlen(want);

    if (key.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)key.as.o;
    return s->len == n && memcmp(s->b, want, n) == 0;
}

static bool trust_map_get(const FlMap *map, const char *name, FlValue *out)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (trust_key_eq(key, name)) {
            if (out != NULL)
                *out = value;
            return true;
        }
    }
    return false;
}

static void trust_map_set(TrustImpl *impl, FlMap *map, const char *key,
                          FlValue value)
{
    FlStr *name = fl_str_new(&impl->vm, key, (u32)strlen(key));

    (void)fl_map_set(&impl->vm, map, FL_OBJ_V(FL_STR, name), value);
}

static bool trust_map_del(TrustImpl *impl, FlMap *map, const char *key)
{
    FlStr *name = fl_str_new(&impl->vm, key, (u32)strlen(key));

    return fl_map_del(map, FL_OBJ_V(FL_STR, name));
}

static TrustImpl *trust_impl_new(void)
{
    TrustImpl *impl = yew_xcalloc(1U, sizeof(*impl));
    FlMap *root;
    FlMap *dirs;

    arena_init(&impl->arena);
    interner_init(&impl->in, &impl->arena);
    fl_diag_init(&impl->dc, &impl->arena);
    (void)fl_vm_init(&impl->vm, &impl->arena, &impl->in, &impl->dc);
    root = fl_map_new(&impl->vm);
    dirs = fl_map_new(&impl->vm);
    impl->root = FL_OBJ_V(FL_MAP, root);
    trust_map_set(impl, root, "schema", FL_INT_V(3));
    trust_map_set(impl, root, "dirs", FL_OBJ_V(FL_MAP, dirs));
    return impl;
}

static void trust_impl_free(TrustImpl *impl)
{
    if (impl == NULL)
        return;
    fl_vm_free(&impl->vm);
    interner_free(&impl->in);
    arena_free_all(&impl->arena);
    free(impl);
}

void yew_trust_db_init(YewTrustDb *db)
{
    if (db != NULL)
        db->impl = trust_impl_new();
}

void yew_trust_db_free(YewTrustDb *db)
{
    if (db == NULL)
        return;
    trust_impl_free((TrustImpl *)db->impl);
    db->impl = NULL;
}

static bool trust_read_file(const char *path, Bytebuf *out, bool *missing)
{
    u8 chunk[8192];
    int fd;

    *missing = false;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        *missing = errno == ENOENT;
        return false;
    }
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return false;
        }
        if (out->len > (size_t)FL_DATA_MAX_BYTES - (size_t)n) {
            (void)close(fd);
            errno = EFBIG;
            return false;
        }
        bytebuf_append(out, chunk, (size_t)n);
    }
    return close(fd) == 0;
}

static bool trust_valid_root(FlValue root)
{
    FlValue schema;
    FlValue dirs;
    const FlMap *map;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    if (root.t != (u8)FL_MAP)
        return false;
    map = (const FlMap *)root.as.o;
    if (!trust_map_get(map, "schema", &schema) || schema.t != (u8)FL_INT ||
        schema.as.i < 1 || !trust_map_get(map, "dirs", &dirs) ||
        dirs.t != (u8)FL_MAP)
        return false;
    map = (const FlMap *)dirs.as.o;
    while (fl_map_iter(map, &cursor, &key, &value)) {
        if (key.t != (u8)FL_STR ||
            (value.t != (u8)FL_STR && value.t != (u8)FL_MAP))
            return false;
    }
    return true;
}

bool yew_trust_db_load_path(YewTrustDb *db, const char *path)
{
    TrustImpl *fresh;
    Bytebuf bytes;
    bool missing;

    if (db == NULL || path == NULL)
        return false;
    bytebuf_init(&bytes);
    if (!trust_read_file(path, &bytes, &missing)) {
        bytebuf_free(&bytes);
        if (!missing)
            return false;
        fresh = trust_impl_new();
    } else {
        char *source;

        fresh = trust_impl_new();
        source = arena_strndup(&fresh->arena, (const char *)bytes.data,
                               bytes.len);
        fresh->root = fl_data_read(&fresh->vm, source, bytes.len,
                                   &fresh->dc);
        bytebuf_free(&bytes);
        if (fl_diag_errors(&fresh->dc) != 0U ||
            !trust_valid_root(fresh->root)) {
            trust_impl_free(fresh);
            return false;
        }
    }
    trust_impl_free((TrustImpl *)db->impl);
    db->impl = fresh;
    return true;
}

static char *trust_xdg_path(bool ensure)
{
    char *dir = yew_xdg_state_dir();
    char *path;
    size_t n;

    if (dir == NULL)
        return NULL;
    if (ensure && !yew_mkdirs(dir, 0700U)) {
        free(dir);
        return NULL;
    }
    n = strlen(dir) + sizeof("/trust.fl");
    path = yew_xmalloc(n);
    (void)snprintf(path, n, "%s/trust.fl", dir);
    free(dir);
    return path;
}

bool yew_trust_db_load(YewTrustDb *db)
{
    char *path = trust_xdg_path(false);
    bool ok;

    if (path == NULL)
        return false;
    ok = yew_trust_db_load_path(db, path);
    free(path);
    return ok;
}

void yew_trust_probe_init(YewTrustProbe *probe)
{
    if (probe == NULL)
        return;
    (void)memset(probe, 0, sizeof(*probe));
    bytebuf_init(&probe->bytes);
}

void yew_trust_probe_free(YewTrustProbe *probe)
{
    if (probe == NULL)
        return;
    bytebuf_free(&probe->bytes);
    (void)memset(probe, 0, sizeof(*probe));
}

static bool trust_find_dir(TrustImpl *impl, const char *path, FlValue *value)
{
    FlValue dirs;
    const FlMap *map;
    u32 cursor = 0U;
    FlValue key;
    FlValue val;

    if (!trust_map_get((const FlMap *)impl->root.as.o, "dirs", &dirs))
        return false;
    map = (const FlMap *)dirs.as.o;
    while (fl_map_iter(map, &cursor, &key, &val)) {
        const FlStr *s = (const FlStr *)key.as.o;

        if (s->len == strlen(path) && memcmp(s->b, path, s->len) == 0) {
            if (value != NULL)
                *value = val;
            return true;
        }
    }
    return false;
}

static FlMap *trust_dir_map(TrustImpl *impl, const char *path, bool create)
{
    FlValue value;
    FlValue dirs;
    FlMap *entry;
    const char *state;
    u32 state_len;
    FlStr *key;

    if (trust_find_dir(impl, path, &value)) {
        if (value.t == (u8)FL_MAP)
            return (FlMap *)value.as.o;
        if (!trust_str(value, &state, &state_len))
            return NULL;
        entry = fl_map_new(&impl->vm);
        trust_map_set(impl, entry, "state",
                      FL_OBJ_V(FL_STR, fl_str_new(&impl->vm, state,
                                                  state_len)));
    } else {
        if (!create)
            return NULL;
        entry = fl_map_new(&impl->vm);
    }
    if (!trust_map_get((const FlMap *)impl->root.as.o, "dirs", &dirs))
        return NULL;
    key = fl_str_new(&impl->vm, path, (u32)strlen(path));
    if (!fl_map_set(&impl->vm, (FlMap *)dirs.as.o,
                    FL_OBJ_V(FL_STR, key), FL_OBJ_V(FL_MAP, entry)))
        return NULL;
    return entry;
}

static AiWsGrant trust_ai_value(FlValue value)
{
    const char *s;
    u32 n;

    if (!trust_str(value, &s, &n))
        return YEW_AI_WS_UNSET;
    if (n == 5U && memcmp(s, "allow", 5U) == 0)
        return YEW_AI_WS_ALLOW;
    if (n == 4U && memcmp(s, "deny", 4U) == 0)
        return YEW_AI_WS_DENY;
    return YEW_AI_WS_UNSET;
}

static bool trust_entry(FlValue value, TrustEntry *entry)
{
    FlValue field;

    (void)memset(entry, 0, sizeof(*entry));
    if (trust_str(value, &entry->state, &entry->state_len)) {
        entry->has_state = true;
        return true;
    }
    if (value.t != (u8)FL_MAP)
        return false;
    if (trust_map_get((const FlMap *)value.as.o, "state", &field)) {
        if (!trust_str(field, &entry->state, &entry->state_len))
            return false;
        entry->has_state = true;
    }
    if (trust_map_get((const FlMap *)value.as.o, "ai", &field))
        entry->ai = trust_ai_value(field);
    if (trust_map_get((const FlMap *)value.as.o, "hash", &field))
        (void)trust_str(field, &entry->hash, &entry->hash_len);
    /* Hash and filesystem identity are independent: AI-only entries do not
     * have a config hash but still bind their grant to dev/ino. */
    if (trust_map_get((const FlMap *)value.as.o, "dev", &field) &&
        field.t == (u8)FL_INT) {
        entry->dev = field.as.i;
        if (trust_map_get((const FlMap *)value.as.o, "ino", &field) &&
            field.t == (u8)FL_INT) {
            entry->ino = field.as.i;
            entry->identity = true;
        }
    }
    entry->fingerprint = entry->hash != NULL && entry->identity;
    if (trust_map_get((const FlMap *)value.as.o, "at", &field) &&
        field.t == (u8)FL_INT)
        entry->at = field.as.i;
    return true;
}

static bool trust_state_is(const TrustEntry *entry, const char *want)
{
    size_t n = strlen(want);

    return entry->has_state && entry->state_len == n &&
           memcmp(entry->state, want, n) == 0;
}

static void trust_hash_hex(u64 hash, char out[17])
{
    (void)snprintf(out, 17U, "%016lx", (unsigned long)hash);
}

static bool trust_store(TrustImpl *impl, const YewTrustProbe *probe,
                        const char *state, time_t now, bool fingerprint)
{
    FlMap *entry = trust_dir_map(impl, probe->workspace, true);

    if (entry == NULL)
        return false;
    trust_map_set(impl, entry, "state",
                  FL_OBJ_V(FL_STR, fl_str_new(&impl->vm, state,
                                              (u32)strlen(state))));
    if (fingerprint) {
        char hash[17];

        trust_hash_hex(probe->hash, hash);
        trust_map_set(impl, entry, "hash",
                      FL_OBJ_V(FL_STR, fl_str_new(&impl->vm, hash, 16U)));
        trust_map_set(impl, entry, "dev", FL_INT_V((i64)probe->dev));
        trust_map_set(impl, entry, "ino", FL_INT_V((i64)probe->ino));
    } else {
        (void)trust_map_del(impl, entry, "hash");
    }
    trust_map_set(impl, entry, "at", FL_INT_V((i64)now));
    return true;
}

static bool trust_resolve_workspace(const char *workspace,
                                    char resolved[PATH_MAX],
                                    struct stat *st)
{
    char *real;
    size_t n;

    if (workspace == NULL || resolved == NULL || st == NULL)
        return false;
    real = realpath(workspace, NULL);
    if (real == NULL)
        return false;
    n = strlen(real);
    if (n >= PATH_MAX || stat(real, st) != 0 || !S_ISDIR(st->st_mode)) {
        free(real);
        return false;
    }
    (void)memcpy(resolved, real, n + 1U);
    free(real);
    return true;
}

static void trust_clear_replaced(TrustImpl *impl, FlMap *entry)
{
    (void)trust_map_del(impl, entry, "state");
    (void)trust_map_del(impl, entry, "ai");
    (void)trust_map_del(impl, entry, "hash");
    (void)trust_map_del(impl, entry, "dev");
    (void)trust_map_del(impl, entry, "ino");
}

AiWsGrant yew_trust_ai_grant(YewTrustDb *db, const char *workspace)
{
    TrustImpl *impl;
    char resolved[PATH_MAX];
    struct stat st;
    FlValue value;
    TrustEntry entry;

    if (db == NULL || db->impl == NULL ||
        !trust_resolve_workspace(workspace, resolved, &st))
        return YEW_AI_WS_UNSET;
    impl = (TrustImpl *)db->impl;
    if (!trust_find_dir(impl, resolved, &value) ||
        !trust_entry(value, &entry))
        return YEW_AI_WS_UNSET;
    if (entry.identity &&
        (entry.dev != (i64)st.st_dev || entry.ino != (i64)st.st_ino)) {
        FlMap *map = trust_dir_map(impl, resolved, false);

        if (map != NULL)
            trust_clear_replaced(impl, map);
        return YEW_AI_WS_UNSET;
    }
    if (entry.ai != YEW_AI_WS_UNSET && !entry.identity) {
        FlMap *map = trust_dir_map(impl, resolved, false);

        if (map != NULL) {
            trust_map_set(impl, map, "dev", FL_INT_V((i64)st.st_dev));
            trust_map_set(impl, map, "ino", FL_INT_V((i64)st.st_ino));
        }
    }
    return entry.ai;
}

bool yew_trust_ai_set(YewTrustDb *db, const char *workspace,
                      AiWsGrant grant, time_t now)
{
    TrustImpl *impl;
    char resolved[PATH_MAX];
    struct stat st;
    FlMap *entry;
    const char *name;

    if (db == NULL || db->impl == NULL ||
        (grant != YEW_AI_WS_UNSET && grant != YEW_AI_WS_ALLOW &&
         grant != YEW_AI_WS_DENY) ||
        !trust_resolve_workspace(workspace, resolved, &st))
        return false;
    impl = (TrustImpl *)db->impl;
    entry = trust_dir_map(impl, resolved, grant != YEW_AI_WS_UNSET);
    if (entry == NULL)
        return grant == YEW_AI_WS_UNSET;
    if (grant == YEW_AI_WS_UNSET) {
        (void)trust_map_del(impl, entry, "ai");
        return true;
    }
    name = grant == YEW_AI_WS_ALLOW ? "allow" : "deny";
    trust_map_set(impl, entry, "ai",
                  FL_OBJ_V(FL_STR, fl_str_new(&impl->vm, name,
                                              (u32)strlen(name))));
    trust_map_set(impl, entry, "dev", FL_INT_V((i64)st.st_dev));
    trust_map_set(impl, entry, "ino", FL_INT_V((i64)st.st_ino));
    trust_map_set(impl, entry, "at", FL_INT_V((i64)now));
    return true;
}

bool yew_trust_ai_forget(YewTrustDb *db, const char *workspace)
{
    return yew_trust_ai_set(db, workspace, YEW_AI_WS_UNSET, 0);
}

YewTrustDecision yew_trust_check(YewTrustDb *db, const char *workspace,
                                 bool has_tty, bool pregrant,
                                 YewTrustProbe *probe)
{
    TrustImpl *impl;
    char *resolved;
    struct stat st;
    bool missing;
    FlValue value;
    TrustEntry entry;
    char hash[17];
    YewTrustDecision prompt = YEW_TRUST_PROMPT_NEW;

    if (db == NULL || db->impl == NULL || workspace == NULL ||
        probe == NULL)
        return YEW_TRUST_ERROR;
    impl = (TrustImpl *)db->impl;
    probe->bytes.len = 0U;
    probe->has_config = false;
    resolved = realpath(workspace, NULL);
    if (resolved == NULL)
        return YEW_TRUST_ERROR;
    if (strlen(resolved) >= sizeof(probe->workspace)) {
        free(resolved);
        return YEW_TRUST_ERROR;
    }
    (void)snprintf(probe->workspace, sizeof(probe->workspace), "%s",
                   resolved);
    free(resolved);
    if (stat(probe->workspace, &st) != 0 || !S_ISDIR(st.st_mode))
        return YEW_TRUST_ERROR;
    probe->dev = st.st_dev;
    probe->ino = st.st_ino;
    if (snprintf(probe->config_path, sizeof(probe->config_path),
                 "%s/.yew.fl", probe->workspace) >=
        (int)sizeof(probe->config_path))
        return YEW_TRUST_ERROR;
    if (!trust_read_file(probe->config_path, &probe->bytes, &missing))
        return missing ? YEW_TRUST_NO_CONFIG : YEW_TRUST_ERROR;
    probe->has_config = true;
    probe->hash = yew_fnv1a64(probe->bytes.data, probe->bytes.len);
    if (pregrant)
        return YEW_TRUST_GRANTED;
    if (trust_find_dir(impl, probe->workspace, &value) &&
        trust_entry(value, &entry)) {
        if (entry.identity &&
            (entry.dev != (i64)probe->dev ||
             entry.ino != (i64)probe->ino)) {
            FlMap *map = trust_dir_map(impl, probe->workspace, false);

            if (map != NULL)
                trust_clear_replaced(impl, map);
            prompt = YEW_TRUST_PROMPT_REPLACED;
        } else if (trust_state_is(&entry, "denied")) {
            return YEW_TRUST_DENIED;
        } else if (trust_state_is(&entry, "trusted")) {
            if (!entry.fingerprint) {
                (void)trust_store(impl, probe, "trusted", time(NULL), true);
                return YEW_TRUST_GRANTED;
            }
            trust_hash_hex(probe->hash, hash);
            if (entry.hash_len != 16U ||
                memcmp(entry.hash, hash, 16U) != 0)
                prompt = YEW_TRUST_PROMPT_CHANGED;
            else
                return YEW_TRUST_GRANTED;
        }
    }
    return has_tty ? prompt : YEW_TRUST_SKIP_NO_TTY;
}

bool yew_trust_answer(YewTrustDb *db, const YewTrustProbe *probe,
                      YewTrustAnswer answer, time_t now)
{
    if (db == NULL || db->impl == NULL || probe == NULL ||
        !probe->has_config)
        return false;
    switch (answer) {
    case YEW_TRUST_ALWAYS:
        return trust_store((TrustImpl *)db->impl, probe, "trusted", now,
                           true);
    case YEW_TRUST_NEVER:
        return trust_store((TrustImpl *)db->impl, probe, "denied", now,
                           false);
    case YEW_TRUST_ONCE:
    case YEW_TRUST_VIEW:
    case YEW_TRUST_SKIP:
        return true;
    default:
        return false;
    }
}

const char *yew_trust_decision_reason(YewTrustDecision decision)
{
    switch (decision) {
    case YEW_TRUST_PROMPT_CHANGED:
        return "the config changed since you trusted it";
    case YEW_TRUST_PROMPT_REPLACED:
        return "the workspace is a different filesystem object";
    case YEW_TRUST_PROMPT_NEW:
        return "this directory has not been trusted";
    case YEW_TRUST_SKIP_NO_TTY:
        return "workspace config skipped because no terminal is available";
    default:
        return "";
    }
}

static int trust_row_cmp(const void *ap, const void *bp, void *ctx)
{
    const TrustRow *a = ap;
    const TrustRow *b = bp;
    const FlStr *as = (const FlStr *)a->key.as.o;
    const FlStr *bs = (const FlStr *)b->key.as.o;
    u32 n = as->len < bs->len ? as->len : bs->len;
    int c = memcmp(as->b, bs->b, n);

    (void)ctx;
    if (c != 0)
        return c;
    return as->len < bs->len ? -1 : as->len > bs->len ? 1 : 0;
}

static bool trust_prune(FlValue key, FlValue value, time_t now,
                        u32 prune_days)
{
    TrustEntry entry;
    const FlStr *path = (const FlStr *)key.as.o;
    char copy[PATH_MAX];
    i64 threshold;

    if (path->len >= sizeof(copy) || !trust_entry(value, &entry) ||
        entry.at <= 0)
        return false;
    (void)memcpy(copy, path->b, path->len);
    copy[path->len] = '\0';
    if (stat(copy, &(struct stat){0}) == 0)
        return false;
    threshold = (i64)prune_days * 86400;
    return entry.at < (i64)now - threshold;
}

static bool trust_rebuild_sorted(TrustImpl *impl, time_t now, u32 prune_days)
{
    FlMap *root = (FlMap *)impl->root.as.o;
    FlValue old_dirs;
    TrustRow *rows;
    u32 n = 0U;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    u32 i;
    FlMap *dirs;

    if (!trust_map_get(root, "dirs", &old_dirs))
        return false;
    rows = yew_xcalloc(fl_map_count((const FlMap *)old_dirs.as.o),
                       sizeof(*rows));
    while (fl_map_iter((const FlMap *)old_dirs.as.o, &cursor, &key, &value)) {
        if (!trust_prune(key, value, now, prune_days)) {
            rows[n].key = key;
            rows[n].value = value;
            n++;
        }
    }
    yew_sort_stable(rows, n, sizeof(*rows), trust_row_cmp, NULL);
    dirs = fl_map_new(&impl->vm);
    for (i = 0U; i < n; i++)
        (void)fl_map_set(&impl->vm, dirs, rows[i].key, rows[i].value);
    free(rows);
    trust_map_set(impl, root, "dirs", FL_OBJ_V(FL_MAP, dirs));
    return true;
}

static bool trust_schema_current(TrustImpl *impl)
{
    FlMap *root = (FlMap *)impl->root.as.o;
    FlValue schema;

    if (!trust_map_get(root, "schema", &schema) ||
        schema.t != (u8)FL_INT || schema.as.i < 1)
        return false;
    if (schema.as.i < 3)
        trust_map_set(impl, root, "schema", FL_INT_V(3));
    return true;
}

bool yew_trust_db_write_path(YewTrustDb *db, const char *path, time_t now,
                             u32 prune_days)
{
    static const char header[] =
        "# yew trust database — hand-editable; delete a line to be asked again.\n";
    TrustImpl *impl;
    Bytebuf out;
    bool ok;

    if (db == NULL || db->impl == NULL || path == NULL)
        return false;
    impl = (TrustImpl *)db->impl;
    if (!trust_schema_current(impl) ||
        !trust_rebuild_sorted(impl, now, prune_days))
        return false;
    bytebuf_init(&out);
    bytebuf_append(&out, (const u8 *)header, sizeof(header) - 1U);
    fl_data_write(&out, impl->root, 0U);
    ok = yew_file_write_atomic(path, out.data, out.len, 0600) == YEW_SAVE_OK;
    bytebuf_free(&out);
    return ok;
}

bool yew_trust_db_write(YewTrustDb *db, time_t now, u32 prune_days)
{
    char *path = trust_xdg_path(true);
    bool ok;

    if (path == NULL)
        return false;
    ok = yew_trust_db_write_path(db, path, now, prune_days);
    free(path);
    return ok;
}
