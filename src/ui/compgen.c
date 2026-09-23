#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "ui/compgen.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "ui/cmdline.h"
#include "ui/comphelp.h"
#include "util/buf.h"
#include "util/log.h"

/* ---------------------------------------------------------------- */
/* Rows                                                               */
/* ---------------------------------------------------------------- */

static void push_row(Arena *a, Vec_CompItem *out, const char *text,
                     size_t text_len, const char *detail)
{
    CompItem item;

    (void)memset(&item, 0, sizeof(item));
    item.text = arena_strndup(a, text, text_len);
    item.match = item.text;
    item.detail = detail == NULL ? NULL : arena_strdup(a, detail);
    item.kind = (u8)YEW_COMP_GEN;
    Vec_CompItem_push(out, item);
}

static bool row_seen(const Vec_CompItem *out, size_t from, const char *text,
                     size_t len)
{
    size_t i;

    for (i = from; i < out->len; i++) {
        if (strlen(out->data[i].text) == len &&
            memcmp(out->data[i].text, text, len) == 0)
            return true;
    }
    return false;
}

static bool read_small_file(const char *path, Bytebuf *out)
{
    char chunk[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;

    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return false;
    }
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        /* A Makefile or known_hosts past 4 MiB is not one to complete
         * from on a keystroke. */
        if (out->len + (size_t)n > 4U * 1024U * 1024U)
            break;
        bytebuf_append(out, chunk, (size_t)n);
    }
    (void)close(fd);
    return true;
}

static char *path_join(const char *dir, const char *name)
{
    size_t nd = strlen(dir);
    size_t nn = strlen(name);
    bool slash = nd != 0U && dir[nd - 1U] != '/';
    char *out = yew_xmalloc(nd + nn + 2U);

    (void)memcpy(out, dir, nd);
    if (slash)
        out[nd++] = '/';
    (void)memcpy(out + nd, name, nn + 1U);
    return out;
}

/* ---------------------------------------------------------------- */
/* hosts                                                              */
/* ---------------------------------------------------------------- */

static bool host_pattern(const char *s, size_t n)
{
    return memchr(s, '*', n) != NULL || memchr(s, '?', n) != NULL ||
           memchr(s, '!', n) != NULL;
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static void hosts_from_config(const char *home, Arena *a, Vec_CompItem *out)
{
    char *path = path_join(home, ".ssh/config");
    Bytebuf buf;
    size_t at = 0U;

    bytebuf_init(&buf);
    (void)read_small_file(path, &buf);
    yew_xfree(path);
    while (at < buf.len) {
        const char *line = (const char *)buf.data + at;
        const char *nl = memchr(line, '\n', buf.len - at);
        size_t len = nl == NULL ? buf.len - at : (size_t)(nl - line);
        size_t i = 0U;

        at += len + 1U;
        while (i < len && is_blank(line[i]))
            i++;
        /* `Host` is case-insensitive and may be followed by `=`. */
        if (len - i < 5U || strncasecmp(line + i, "host", 4U) != 0 ||
            !(is_blank(line[i + 4U]) || line[i + 4U] == '='))
            continue;
        i += 4U;
        while (i < len && (is_blank(line[i]) || line[i] == '='))
            i++;
        while (i < len) {
            size_t start = i;

            if (line[i] == '#')
                break;
            while (i < len && !is_blank(line[i]))
                i++;
            if (!host_pattern(line + start, i - start) &&
                !row_seen(out, 0U, line + start, i - start))
                push_row(a, out, line + start, i - start, "ssh config");
            while (i < len && is_blank(line[i]))
                i++;
        }
    }
    bytebuf_free(&buf);
}

static void hosts_from_known(const char *home, Arena *a, Vec_CompItem *out)
{
    char *path = path_join(home, ".ssh/known_hosts");
    Bytebuf buf;
    size_t at = 0U;

    bytebuf_init(&buf);
    (void)read_small_file(path, &buf);
    yew_xfree(path);
    while (at < buf.len) {
        const char *line = (const char *)buf.data + at;
        const char *nl = memchr(line, '\n', buf.len - at);
        size_t len = nl == NULL ? buf.len - at : (size_t)(nl - line);
        size_t i = 0U;
        size_t end;

        at += len + 1U;
        while (i < len && is_blank(line[i]))
            i++;
        /* `@cert-authority` / `@revoked`: the names are the next field. */
        if (i < len && line[i] == '@') {
            while (i < len && !is_blank(line[i]))
                i++;
            while (i < len && is_blank(line[i]))
                i++;
        }
        /* Hashed entries (`|1|…`) name nothing we can show. */
        if (i >= len || line[i] == '#' || line[i] == '|')
            continue;
        end = i;
        while (end < len && !is_blank(line[end]))
            end++;
        while (i < end) {
            size_t start = i;
            size_t stop;

            while (i < end && line[i] != ',')
                i++;
            stop = i;
            /* `[host]:2222` names `host`; the port is ssh -p's. */
            if (stop > start && line[start] == '[') {
                const char *close = memchr(line + start, ']', stop - start);

                if (close != NULL) {
                    start++;
                    stop = (size_t)(close - line);
                }
            }
            if (stop > start && !host_pattern(line + start, stop - start) &&
                !row_seen(out, 0U, line + start, stop - start))
                push_row(a, out, line + start, stop - start, "known host");
            if (i < end)
                i++;
        }
    }
    bytebuf_free(&buf);
}

/* ---------------------------------------------------------------- */
/* signals, users                                                     */
/* ---------------------------------------------------------------- */

static const struct {
    const char *name;
    const char *action;
} signal_table[] = {
    {"HUP", "terminate"},       {"INT", "terminate"},
    {"QUIT", "core dump"},      {"KILL", "terminate, uncatchable"},
    {"TERM", "terminate"},      {"USR1", "terminate"},
    {"USR2", "terminate"},      {"STOP", "stop, uncatchable"},
    {"CONT", "continue"},       {"TSTP", "stop"},
    {"WINCH", "ignore"}
};

static void builtin_signals(Arena *a, Vec_CompItem *out)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(signal_table); i++)
        push_row(a, out, signal_table[i].name, strlen(signal_table[i].name),
                 signal_table[i].action);
    for (i = 0U; i < YEW_ARRAY_LEN(signal_table); i++) {
        char name[16];
        int n = snprintf(name, sizeof(name), "SIG%s", signal_table[i].name);

        push_row(a, out, name, (size_t)n, signal_table[i].action);
    }
}

static void builtin_users(Arena *a, Vec_CompItem *out)
{
    struct passwd *pw;

    setpwent();
    while ((pw = getpwent()) != NULL) {
        size_t n;

        if (pw->pw_name == NULL || pw->pw_name[0] == '\0')
            continue;
        n = strlen(pw->pw_name);
        if (row_seen(out, 0U, pw->pw_name, n))
            continue;
        push_row(a, out, pw->pw_name, n,
                 pw->pw_dir == NULL ? "" : pw->pw_dir);
    }
    endpwent();
}

/* ---------------------------------------------------------------- */
/* make_targets: PARSE the Makefile, never run make                   */
/* ---------------------------------------------------------------- */

static bool make_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
           c == '-';
}

static bool starts_word(const char *line, size_t len, const char *word)
{
    size_t n = strlen(word);

    return len >= n && memcmp(line, word, n) == 0 &&
           (len == n || is_blank(line[n]));
}

/*
 * One logical line: names of [A-Za-z0-9_./-], blanks between them, then
 * a `:` that is not `:=` or `::=` (both assignments).  `a b: dep` names
 * two targets.  Anything else on the line -- a `$(VAR)` target, a
 * pattern rule, a recipe -- is not offered: a hand-written scan, because
 * the regex engine has no lookahead and this needs none.
 */
static void make_line(const char *line, size_t len, Arena *a,
                      Vec_CompItem *out)
{
    size_t starts[64];
    size_t lens[64];
    u32 n = 0U;
    size_t i = 0U;
    u32 k;

    if (len == 0U || !make_name_char(line[0]))
        return;
    for (;;) {
        size_t start = i;

        while (i < len && make_name_char(line[i]))
            i++;
        if (i == start)
            return;
        if (n < YEW_ARRAY_LEN(starts)) {
            starts[n] = start;
            lens[n] = i - start;
            n++;
        }
        while (i < len && is_blank(line[i]))
            i++;
        if (i >= len)
            return;
        if (line[i] == ':')
            break;
        if (!make_name_char(line[i]))
            return;
    }
    /* i is at ':'.  `:=` and `::=` (and `:::=`) are assignments. */
    {
        size_t j = i;

        while (j < len && line[j] == ':')
            j++;
        if (j < len && line[j] == '=')
            return;
    }
    for (k = 0U; k < n; k++) {
        const char *name = line + starts[k];

        if (name[0] == '.' || memchr(name, '%', lens[k]) != NULL)
            continue;
        if (!row_seen(out, 0U, name, lens[k]))
            push_row(a, out, name, lens[k], "make target");
    }
}

static void make_targets_from(const char *path, Arena *a, Vec_CompItem *out)
{
    Bytebuf buf;
    size_t at = 0U;
    bool continued = false;
    bool in_define = false;

    bytebuf_init(&buf);
    (void)read_small_file(path, &buf);
    while (at < buf.len) {
        const char *line = (const char *)buf.data + at;
        const char *nl = memchr(line, '\n', buf.len - at);
        size_t len = nl == NULL ? buf.len - at : (size_t)(nl - line);
        bool cont_next = len != 0U && line[len - 1U] == '\\';

        at += len + 1U;
        if (len != 0U && line[len - 1U] == '\r')
            len--;
        if (in_define) {
            size_t i = 0U;

            while (i < len && is_blank(line[i]))
                i++;
            if (starts_word(line + i, len - i, "endef"))
                in_define = false;
            continued = false;
            continue;
        }
        if (!continued) {
            if (starts_word(line, len, "define") ||
                (starts_word(line, len, "override") &&
                 len > 9U && starts_word(line + 9U, len - 9U, "define")))
                in_define = true;
            else
                make_line(line, len, a, out);
        }
        continued = cont_next;
    }
    bytebuf_free(&buf);
}

static void builtin_make_targets(const char *cwd, const char *makefile,
                                 Arena *a, Vec_CompItem *out)
{
    static const char *const names[] = {"GNUmakefile", "makefile",
                                        "Makefile"};
    size_t i;

    if (makefile != NULL) {
        char *path = makefile[0] == '/' ? yew_xstrdup(makefile)
                                        : path_join(cwd, makefile);

        make_targets_from(path, a, out);
        yew_xfree(path);
        return;
    }
    /* make's own order: the first that exists is the one it reads. */
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char *path = path_join(cwd, names[i]);
        struct stat st;
        bool found = stat(path, &st) == 0 && S_ISREG(st.st_mode);

        if (found)
            make_targets_from(path, a, out);
        yew_xfree(path);
        if (found)
            return;
    }
}

bool yew_compgen_builtin(const char *name, const char *cwd,
                         const char *makefile, Arena *a, Vec_CompItem *out)
{
    if (name == NULL || a == NULL || out == NULL)
        return false;
    if (strcmp(name, "hosts") == 0) {
        /* $HOME at CALL time: a test points it at a fixture and must
         * never see the developer's real ~/.ssh. */
        const char *home = getenv("HOME");

        if (home != NULL && home[0] != '\0') {
            hosts_from_config(home, a, out);
            hosts_from_known(home, a, out);
        }
        return true;
    }
    if (strcmp(name, "signals") == 0) {
        builtin_signals(a, out);
        return true;
    }
    if (strcmp(name, "users") == 0) {
        builtin_users(a, out);
        return true;
    }
    if (strcmp(name, "make_targets") == 0) {
        builtin_make_targets(cwd == NULL ? "." : cwd, makefile, a, out);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Subprocess generators: the cache and the flights                   */
/* ---------------------------------------------------------------- */

typedef struct GenRow {
    char *text;
    char *desc;
} GenRow;

typedef struct GenEntry {
    char *key;
    i64 stamp;
    i64 cache_ms;
    GenRow *rows;
    u32 n;
} GenEntry;

typedef struct GenFlight {
    char *key;
    u32 job_id;
} GenFlight;

typedef struct GenOwner {
    char *key;
    char *name;
    i64 cache_ms;
    bool ps_columns;
    bool completed;
} GenOwner;

static struct {
    GenEntry *v;
    u32 n;
    u32 cap;
    GenFlight flights[YEW_COMPGEN_MAX_INFLIGHT];
    u32 n_flights;
    i64 timeout_ms;
    u32 spawns;
} gen;

void yew_compgen_test_set_timeout_ms(i64 ms)
{
    gen.timeout_ms = ms;
}

u32 yew_compgen_test_spawns(void)
{
    return gen.spawns;
}

u32 yew_compgen_inflight(void)
{
    return gen.n_flights;
}

static void entry_free(GenEntry *e)
{
    u32 i;

    for (i = 0U; i < e->n; i++) {
        yew_xfree(e->rows[i].text);
        yew_xfree(e->rows[i].desc);
    }
    yew_xfree(e->rows);
    yew_xfree(e->key);
}

void yew_compgen_cache_clear(void)
{
    u32 i;

    for (i = 0U; i < gen.n; i++)
        entry_free(&gen.v[i]);
    yew_xfree(gen.v);
    gen.v = NULL;
    gen.n = 0U;
    gen.cap = 0U;
    gen.spawns = 0U;
}

static GenEntry *entry_find(const char *key)
{
    u32 i;

    for (i = 0U; i < gen.n; i++) {
        if (strcmp(gen.v[i].key, key) == 0)
            return &gen.v[i];
    }
    return NULL;
}

static int flight_find(const char *key)
{
    u32 i;

    for (i = 0U; i < gen.n_flights; i++) {
        if (strcmp(gen.flights[i].key, key) == 0)
            return (int)i;
    }
    return -1;
}

static void flight_drop(const char *key)
{
    int at = flight_find(key);
    u32 i;

    if (at < 0)
        return;
    yew_xfree(gen.flights[at].key);
    for (i = (u32)at; i + 1U < gen.n_flights; i++)
        gen.flights[i] = gen.flights[i + 1U];
    gen.n_flights--;
}

bool yew_compgen_awaiting(const char *key_string)
{
    return key_string != NULL && flight_find(key_string) >= 0 &&
           entry_find(key_string) == NULL;
}

/* Replace (or create) the cached answer for `key`; takes `rows`. */
static void entry_store(const char *key, i64 cache_ms, GenRow *rows, u32 n)
{
    GenEntry *e = entry_find(key);

    if (e == NULL) {
        if (gen.n == gen.cap) {
            gen.cap = gen.cap == 0U ? 8U : gen.cap * 2U;
            gen.v = yew_xrealloc(gen.v, gen.cap * sizeof(*gen.v));
        }
        e = &gen.v[gen.n++];
        (void)memset(e, 0, sizeof(*e));
        e->key = yew_xstrdup(key);
    } else {
        char *k = e->key;

        e->key = NULL;
        entry_free(e);
        e->key = k;
    }
    e->stamp = yew_now_ms();
    e->cache_ms = cache_ms;
    e->rows = rows;
    e->n = n;
}

/* A field for the pager: control bytes drawn as `·`, trailing blanks
 * trimmed.  Heap-owned. */
static char *clean_field(const char *s, size_t n)
{
    Bytebuf b;
    char *out;
    size_t i;

    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' ||
                      s[n - 1U] == '\r'))
        n--;
    bytebuf_init(&b);
    for (i = 0U; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20U || c == 0x7FU)
            bytebuf_append(&b, "\xC2\xB7", 2U);
        else
            bytebuf_push_u8(&b, c);
    }
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

/* Output lines -> rows: `candidate` or `candidate<TAB>description`,
 * blank lines skipped, at most YEW_COMPGEN_MAX_LINES, order kept. */
static GenRow *parse_output(const u8 *data, size_t len, bool ps, u32 *count)
{
    GenRow *rows = NULL;
    u32 n = 0U;
    u32 cap = 0U;
    size_t at = 0U;

    while (at < len && n < YEW_COMPGEN_MAX_LINES) {
        const char *line = (const char *)data + at;
        const char *nl = memchr(line, '\n', len - at);
        size_t ll = nl == NULL ? len - at : (size_t)(nl - line);
        const char *text = line;
        size_t tn;
        const char *desc = NULL;
        size_t dn = 0U;

        at += ll + 1U;
        if (ps) {
            size_t i = 0U;

            while (i < ll && is_blank(line[i]))
                i++;
            text = line + i;
            while (i < ll && !is_blank(line[i]))
                i++;
            tn = (size_t)(line + i - text);
            while (i < ll && is_blank(line[i]))
                i++;
            if (i < ll) {
                /* The command's basename, which is what a user knows it
                 * by. */
                const char *cmd = line + i;
                size_t cn = ll - i;
                const char *slash = NULL;
                size_t k;

                for (k = 0U; k < cn; k++) {
                    if (cmd[k] == '/')
                        slash = cmd + k;
                }
                if (slash != NULL && slash + 1 < cmd + cn) {
                    cn -= (size_t)(slash + 1 - cmd);
                    cmd = slash + 1;
                }
                desc = cmd;
                dn = cn;
            }
        } else {
            const char *tab = memchr(line, '\t', ll);

            tn = tab == NULL ? ll : (size_t)(tab - line);
            if (tab != NULL) {
                desc = tab + 1;
                dn = ll - tn - 1U;
            }
        }
        while (tn > 0U && is_blank(text[tn - 1U]))
            tn--;
        if (tn == 0U)
            continue;
        if (n == cap) {
            cap = cap == 0U ? 64U : cap * 2U;
            rows = yew_xrealloc(rows, cap * sizeof(*rows));
        }
        rows[n].text = clean_field(text, tn);
        rows[n].desc = desc == NULL ? NULL : clean_field(desc, dn);
        if (rows[n].desc != NULL && rows[n].desc[0] == '\0') {
            yew_xfree(rows[n].desc);
            rows[n].desc = NULL;
        }
        n++;
    }
    *count = n;
    return rows;
}

static void gen_complete(void *owner_ptr, Ed *ed, const YewJob *job)
{
    GenOwner *owner = owner_ptr;
    GenRow *rows = NULL;
    u32 n = 0U;
    bool ok = job->state == YEW_JOB_EXITED && job->exit_code == 0;

    owner->completed = true;
    if (ok || job->collect_capped)
        rows = parse_output(job->collect.data, job->collect.len,
                            owner->ps_columns, &n);
    if (!ok && !job->collect_capped)
        yew_log(YEW_LOG_INFO, "completion generator %s: %s (exit %d)",
                owner->name, yew_job_state_name(job->state),
                job->exit_code);
    entry_store(owner->key, owner->cache_ms, rows, n);
    flight_drop(owner->key);
    /* Refilter an open menu that asked for this key -- and nothing else.
     * The prompt's TEXT is never touched here. */
    yew_cmdline_compgen_arrived(ed, owner->key);
}

static void gen_destroy(void *owner_ptr)
{
    GenOwner *owner = owner_ptr;

    if (!owner->completed) {
        /* Evicted for a user's job, or torn down with the editor: no
         * answer arrived, so none is cached. */
        flight_drop(owner->key);
    }
    yew_xfree(owner->key);
    yew_xfree(owner->name);
    yew_xfree(owner);
}

static const YewJobCallbackOps gen_ops = {gen_complete, gen_destroy};

char *yew_compgen_key_string(const YewCompGenKey *key)
{
    Bytebuf b;
    char *out;
    u32 i;

    bytebuf_init(&b);
    bytebuf_printf(&b, "%s", key->name == NULL ? "" : key->name);
    for (i = 0U; key->argv != NULL && key->argv[i] != NULL; i++) {
        bytebuf_push_u8(&b, 0x1FU);
        bytebuf_append(&b, key->argv[i], strlen(key->argv[i]));
    }
    bytebuf_push_u8(&b, 0x1EU);
    if (key->cwd != NULL)
        bytebuf_append(&b, key->cwd, strlen(key->cwd));
    out = yew_xmalloc(b.len + 1U);
    (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

static void spawn_for(Ed *ed, const YewCompGenKey *key, const char *ks)
{
    static const char *const env_set[] = {"NO_COLOR=1", "PAGER=cat",
                                          "GIT_PAGER=cat", "TERM=dumb",
                                          NULL};
    YewJobSpec spec;
    GenOwner *owner;
    char err[256];
    u32 id;

    /* §5.2: one per key, four in all.  A fifth distinct key waits for a
     * slot -- YEW_JOB_MAX fails rather than queues, and the user's own
     * `:!` command matters more than a branch list. */
    if (gen.n_flights + yew_comphelp_inflight() >=
        YEW_COMPGEN_MAX_INFLIGHT)
        return; /* Sprint 57.25: a help job holds one of the four */
    owner = yew_xcalloc(1U, sizeof(*owner));
    owner->key = yew_xstrdup(ks);
    owner->name = yew_xstrdup(key->name == NULL ? "?" : key->name);
    owner->cache_ms = key->cache_ms;
    owner->ps_columns = key->ps_columns;
    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = (char **)key->argv;
    spec.cwd = key->cwd;
    spec.sink = YEW_SINK_CALLBACK;
    spec.internal = true;
    spec.evictable = true;
    spec.timeout_ms = gen.timeout_ms > 0 ? gen.timeout_ms
                                         : (i64)YEW_COMPGEN_TIMEOUT_MS;
    spec.collect_max = YEW_COMPGEN_COLLECT_MAX;
    spec.env_set = env_set;
    spec.display = owner->name;
    spec.callback_owner = owner;
    spec.callback_ops = &gen_ops;
    gen.spawns++;
    id = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        /* §5.6: cache the empty answer so the next keystroke does not
         * try again, and say so in the log -- not the footer. */
        yew_log(YEW_LOG_INFO, "completion generator %s: %s", owner->name,
                err);
        entry_store(ks, key->cache_ms, NULL, 0U);
        yew_xfree(owner->key);
        yew_xfree(owner->name);
        yew_xfree(owner);
        return;
    }
    gen.flights[gen.n_flights].key = yew_xstrdup(ks);
    gen.flights[gen.n_flights].job_id = id;
    gen.n_flights++;
}

bool yew_compgen_rows(Ed *ed, const YewCompGenKey *key, i64 now_ms,
                      Arena *a, Vec_CompItem *out, bool *pending)
{
    char *ks;
    GenEntry *e;
    u32 i;

    if (pending != NULL)
        *pending = false;
    if (ed == NULL || key == NULL || key->argv == NULL ||
        key->argv[0] == NULL || a == NULL || out == NULL)
        return false;
    ks = yew_compgen_key_string(key);
    e = entry_find(ks);
    if (e == NULL || now_ms - e->stamp >= e->cache_ms) {
        /* Missing: ask.  Stale: serve what we have AND refresh. */
        if (flight_find(ks) < 0)
            spawn_for(ed, key, ks);
        e = entry_find(ks); /* a spawn failure caches an empty answer */
    }
    if (e != NULL) {
        for (i = 0U; i < e->n; i++)
            push_row(a, out, e->rows[i].text, strlen(e->rows[i].text),
                     e->rows[i].desc);
    }
    if (pending != NULL)
        *pending = flight_find(ks) >= 0;
    yew_xfree(ks);
    return true;
}
