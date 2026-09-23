#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.26 §2: the fish oracle.  See compfish.h for the rules; this
 * file is, in order, the line fish sees, detection, the cache and the
 * request queue, the lookup, the parse, and the job.
 */

#include "ui/compfish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "ui/cmdline.h"
#include "ui/compgen.h"
#include "ui/comphelp.h"
#include "util/log.h"

/*
 * §2's measured facts, in the script's order:
 *   - `fish -N` does not load ~/.config/fish/completions, so the user's
 *     completion (and function, for the helpers their rules call)
 *     directories go back on the front of the search paths;
 *   - `complete -C` runs first, which AUTOLOADS the command's rules;
 *   - only then does `complete -c <cmd>` say whether fish knows the
 *     command at all.  Nothing is printed when it does not.
 */
const char YEW_COMPFISH_QUERY[] =
    "set -p fish_complete_path $__fish_config_dir/completions; "
    "set -p fish_function_path $__fish_config_dir/functions; "
    "set -l r (complete -C -- $argv[1]); "
    "if complete -c $argv[2] | string length -q; "
    "printf '%s\\n' $r; end";

enum {
    FISH_CACHE_MAX = 32U,
    FISH_QUEUE_MAX = 8U,
    /* §2: one in flight; it counts toward compgen's four. */
    FISH_INFLIGHT_MAX = 1U
};

/* ================================================================ */
/* The line fish sees                                                */
/* ================================================================ */

static bool fish_special(unsigned char c)
{
    return c == ' ' || c == '$' || c == '*' || c == '?' || c == '~' ||
           c == '#' || c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '[' || c == ']' || c == '<' || c == '>' || c == '&' ||
           c == '|' || c == ';' || c == '"' || c == '\'' || c == '\\';
}

void yew_compfish_escape(Bytebuf *out, const char *word)
{
    const unsigned char *p = (const unsigned char *)word;

    if (out == NULL || word == NULL)
        return;
    for (; *p != '\0'; p++) {
        if (*p == '\t') {
            bytebuf_append(out, "\\t", 2U);
        } else if (*p == '\n') {
            bytebuf_append(out, "\\n", 2U);
        } else if (*p < 0x20U || *p == 0x7FU) {
            bytebuf_printf(out, "\\x%02x", (unsigned)*p);
        } else {
            if (fish_special(*p))
                bytebuf_push_u8(out, (u8)'\\');
            bytebuf_push_u8(out, *p);
        }
    }
}

char *yew_compfish_query_stem(const char *stem)
{
    const char *eq;

    if (stem == NULL || stem[0] == '\0')
        return yew_xstrdup("");
    if (stem[0] == '-') {
        /* `--name=val`: fish completes the VALUE only with the flag in
         * front of it, and prints whole words (`--name=value`). */
        eq = strchr(stem, '=');
        if (eq != NULL && stem[1] == '-') {
            size_t n = (size_t)(eq - stem) + 1U;
            char *out = yew_xmalloc(n + 1U);

            (void)memcpy(out, stem, n);
            out[n] = '\0';
            return out;
        }
        return yew_xstrdup("-");
    }
    /* fish lists dotfiles only for a stem that starts with one. */
    if (stem[0] == '.')
        return yew_xstrdup(".");
    return yew_xstrdup("");
}

char *yew_compfish_line(const YewShCtx *ctx)
{
    Bytebuf b;
    char *qstem;
    char *out;
    u32 i;

    if (ctx == NULL || ctx->argv == NULL || ctx->argc == 0U ||
        ctx->argv[0] == NULL || ctx->argv[0][0] == '\0' ||
        ctx->arg_index == 0U)
        return NULL;
    bytebuf_init(&b);
    for (i = 0U; i < ctx->arg_index && i < ctx->argc &&
                 ctx->argv[i] != NULL; i++) {
        if (i != 0U)
            bytebuf_push_u8(&b, (u8)' ');
        /* An empty word -- `''`, or an expansion whose value only the
         * shell knows -- stays a word, so fish counts positions as the
         * shell will. */
        if (ctx->argv[i][0] == '\0')
            bytebuf_append(&b, "''", 2U);
        else
            yew_compfish_escape(&b, ctx->argv[i]);
    }
    bytebuf_push_u8(&b, (u8)' ');
    qstem = yew_compfish_query_stem(ctx->stem);
    yew_compfish_escape(&b, qstem);
    yew_xfree(qstem);
    out = yew_xmalloc(b.len + 1U);
    (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

/* ================================================================ */
/* State                                                             */
/* ================================================================ */

typedef struct FishEntry {
    char key[YEW_COMPFISH_KEY_LEN];
    i64 stamp;
    bool gate;
    YewFishRow *rows;
    u32 n;
} FishEntry;

typedef struct FishReq {
    char key[YEW_COMPFISH_KEY_LEN];
    char *line;
    char *cmd;
    char *cwd;
    bool inflight;
    u32 job_id;
} FishReq;

typedef struct FishOwner {
    char key[YEW_COMPFISH_KEY_LEN];
    bool completed;
} FishOwner;

static struct {
    bool resolved;
    char *path;
    bool disabled;
    FishEntry cache[FISH_CACHE_MAX];
    u32 n_cache;
    FishReq q[FISH_QUEUE_MAX];
    u32 nq;
    u32 spawns;
} fish;

/* ================================================================ */
/* Detection                                                         */
/* ================================================================ */

static char *find_on_path(const char *name)
{
    const char *env = getenv("PATH");
    const char *p;

    if (env == NULL)
        return NULL;
    p = env;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon == NULL ? strlen(p) : (size_t)(colon - p);

        /* Relative elements are skipped, as the help layer does: `.` on
         * $PATH would run whatever `fish` the workspace holds. */
        if (n != 0U && p[0] == '/') {
            size_t nn = strlen(name);
            char *path = yew_xmalloc(n + nn + 2U);
            struct stat st;

            (void)memcpy(path, p, n);
            path[n] = '/';
            (void)memcpy(path + n + 1U, name, nn + 1U);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                access(path, X_OK) == 0)
                return path;
            yew_xfree(path);
        }
        if (colon == NULL)
            return NULL;
        p = colon + 1;
    }
}

const char *yew_compfish_path(void)
{
    if (!fish.resolved) {
        const char *seam = getenv("YEW_TEST_FISH");

        fish.resolved = true;
        yew_xfree(fish.path);
        fish.path = NULL;
        /* The suite's seam: a stub script, or empty for "no fish". */
        if (seam != NULL)
            fish.path = seam[0] == '\0' ? NULL : yew_xstrdup(seam);
        else
            fish.path = find_on_path("fish");
    }
    return fish.path;
}

static bool option_auto(Ed *ed)
{
    OptVal v;

    if (ed == NULL)
        return false;
    if (!yew_opt_get(ed, NULL, NULL, "shell.complete_fish", 19U, &v) ||
        (v.type != (u8)YEW_OPT_ENUM && v.type != (u8)YEW_OPT_STR))
        return true;
    return !(v.as.str.len == 3U && memcmp(v.as.str.s, "off", 3U) == 0);
}

bool yew_compfish_enabled(Ed *ed)
{
    return option_auto(ed) && !fish.disabled && yew_compfish_path() != NULL;
}

bool yew_compfish_test_disabled(void)
{
    return fish.disabled;
}

/* ================================================================ */
/* The cache and the queue                                           */
/* ================================================================ */

void yew_compfish_rows_free(YewFishRow *rows, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        yew_xfree((char *)rows[i].text);
        yew_xfree((char *)rows[i].desc);
    }
    yew_xfree(rows);
}

static void entry_clear(FishEntry *e)
{
    yew_compfish_rows_free(e->rows, e->n);
    (void)memset(e, 0, sizeof(*e));
}

static FishEntry *entry_find(const char *key)
{
    u32 i;

    for (i = 0U; i < fish.n_cache; i++) {
        if (strcmp(fish.cache[i].key, key) == 0)
            return &fish.cache[i];
    }
    return NULL;
}

/* Takes `rows`.  A full cache drops its oldest answer. */
static void entry_store(const char *key, bool gate, YewFishRow *rows, u32 n)
{
    FishEntry *e = entry_find(key);

    if (e == NULL) {
        if (fish.n_cache == FISH_CACHE_MAX) {
            u32 oldest = 0U;
            u32 i;

            for (i = 1U; i < fish.n_cache; i++) {
                if (fish.cache[i].stamp < fish.cache[oldest].stamp)
                    oldest = i;
            }
            entry_clear(&fish.cache[oldest]);
            for (i = oldest; i + 1U < fish.n_cache; i++)
                fish.cache[i] = fish.cache[i + 1U];
            fish.n_cache--;
            (void)memset(&fish.cache[fish.n_cache], 0,
                         sizeof(fish.cache[0]));
        }
        e = &fish.cache[fish.n_cache++];
        (void)memset(e, 0, sizeof(*e));
        (void)memcpy(e->key, key, YEW_COMPFISH_KEY_LEN);
    } else {
        yew_compfish_rows_free(e->rows, e->n);
    }
    e->stamp = yew_now_ms();
    e->gate = gate;
    e->rows = rows;
    e->n = n;
}

static void req_drop_at(u32 at)
{
    FishReq *r = &fish.q[at];
    u32 i;

    yew_xfree(r->line);
    yew_xfree(r->cmd);
    yew_xfree(r->cwd);
    for (i = at; i + 1U < fish.nq; i++)
        fish.q[i] = fish.q[i + 1U];
    fish.nq--;
    (void)memset(&fish.q[fish.nq], 0, sizeof(fish.q[0]));
}

static int req_find(const char *key)
{
    u32 i;

    for (i = 0U; i < fish.nq; i++) {
        if (strcmp(fish.q[i].key, key) == 0)
            return (int)i;
    }
    return -1;
}

static void queue_request(const char *key, const char *line,
                          const char *cmd, const char *cwd)
{
    FishReq *r;
    u32 i;

    if (req_find(key) >= 0)
        return;
    if (fish.nq == FISH_QUEUE_MAX) {
        /* The oldest unspawned request is the least likely wanted. */
        for (i = 0U; i < fish.nq; i++) {
            if (!fish.q[i].inflight) {
                req_drop_at(i);
                break;
            }
        }
        if (fish.nq == FISH_QUEUE_MAX)
            return;
    }
    r = &fish.q[fish.nq++];
    (void)memset(r, 0, sizeof(*r));
    (void)memcpy(r->key, key, YEW_COMPFISH_KEY_LEN);
    r->line = yew_xstrdup(line);
    r->cmd = yew_xstrdup(cmd);
    r->cwd = yew_xstrdup(cwd);
}

u32 yew_compfish_inflight(void)
{
    u32 i;
    u32 n = 0U;

    for (i = 0U; i < fish.nq; i++) {
        if (fish.q[i].inflight)
            n++;
    }
    return n;
}

u32 yew_compfish_queued(void)
{
    return fish.nq - yew_compfish_inflight();
}

bool yew_compfish_awaiting(const char *key)
{
    return key != NULL && strncmp(key, "fish:", 5U) == 0 &&
           req_find(key) >= 0 && entry_find(key) == NULL;
}

void yew_compfish_prompt_closed(void)
{
    u32 i = 0U;

    /* Unspawned requests belonged to the prompt that asked; an answer
     * already on its way still lands in the cache. */
    while (i < fish.nq) {
        if (fish.q[i].inflight)
            i++;
        else
            req_drop_at(i);
    }
}

void yew_compfish_reset(void)
{
    u32 i;

    for (i = 0U; i < fish.n_cache; i++)
        entry_clear(&fish.cache[i]);
    fish.n_cache = 0U;
    while (fish.nq != 0U)
        req_drop_at(fish.nq - 1U);
    yew_xfree(fish.path);
    fish.path = NULL;
    fish.resolved = false;
    fish.disabled = false;
    fish.spawns = 0U;
}

u32 yew_compfish_test_spawns(void)
{
    return fish.spawns;
}

void yew_compfish_test_age(i64 ms)
{
    u32 i;

    for (i = 0U; i < fish.n_cache; i++)
        fish.cache[i].stamp -= ms;
}

/* ================================================================ */
/* The lookup                                                        */
/* ================================================================ */

static u64 fnv_str(u64 h, const char *s)
{
    const u8 *p = (const u8 *)s;

    for (;;) {
        h ^= (u64)*p;
        h *= UINT64_C(0x100000001b3);
        if (*p == '\0')
            return h;
        p++;
    }
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL || slash[1] == '\0' ? path : slash + 1;
}

YewFishState yew_compfish_lookup(Ed *ed, const YewShCtx *ctx,
                                 YewFishLookup *out)
{
    const char *path;
    const char *cwd;
    FishEntry *e;
    char *line;
    u64 h;

    if (out == NULL)
        return YEW_FISH_OFF;
    (void)memset(out, 0, sizeof(*out));
    if (ed == NULL || ctx == NULL || ctx->pos != YEW_SH_POS_ARGUMENT ||
        !yew_compfish_enabled(ed))
        return YEW_FISH_OFF;
    line = yew_compfish_line(ctx);
    if (line == NULL)
        return YEW_FISH_OFF;
    path = yew_compfish_path();
    /* The directory a `:!` command runs in: fish lists ITS files. */
    cwd = yew_ws_root(ed);
    if (cwd == NULL)
        cwd = "";
    /* §2: the key is (fish, cwd, the rebuilt words, the query stem) --
     * the last two are exactly the line. */
    h = UINT64_C(0xcbf29ce484222325);
    h = fnv_str(h, path);
    h = fnv_str(h, cwd);
    h = fnv_str(h, line);
    (void)snprintf(out->key, sizeof(out->key), "fish:%016llx",
                   (unsigned long long)h);
    e = entry_find(out->key);
    if (e == NULL || yew_now_ms() - e->stamp >= YEW_COMPFISH_FRESH_MS) {
        /* Missing: ask.  Stale: serve what we have AND refresh. */
        queue_request(out->key, line, base_of(ctx->argv[0]), cwd);
    }
    yew_xfree(line);
    if (e == NULL) {
        out->state = YEW_FISH_PENDING;
        return out->state;
    }
    out->state = e->gate ? YEW_FISH_ROWS : YEW_FISH_CLOSED;
    out->rows = e->rows;
    out->n = e->n;
    return out->state;
}

/* ================================================================ */
/* The parse                                                         */
/* ================================================================ */

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

YewFishRow *yew_compfish_parse(const char *data, size_t len, u32 *count)
{
    YewFishRow *rows = NULL;
    u32 n = 0U;
    u32 cap = 0U;
    size_t at = 0U;

    if (count != NULL)
        *count = 0U;
    if (data == NULL)
        return NULL;
    while (at < len && n < YEW_COMPFISH_MAX_LINES) {
        const char *line = data + at;
        const char *nl = memchr(line, '\n', len - at);
        size_t ll = nl == NULL ? len - at : (size_t)(nl - line);
        const char *tab = memchr(line, '\t', ll);
        size_t tn = tab == NULL ? ll : (size_t)(tab - line);
        bool is_dir = false;
        char *text;
        u32 k;

        at += ll + 1U;
        /* Candidates are printed RAW (`a b.txt`, `d$x`): nothing is
         * trimmed from the text. */
        if (tn > 1U && line[tn - 1U] == '/') {
            is_dir = true;
            tn--;
        }
        if (tn == 0U)
            continue;
        text = yew_xmalloc(tn + 1U);
        for (k = 0U; k < tn; k++) {
            unsigned char c = (unsigned char)line[k];

            /* A control byte in a file name cannot be shown; fish's
             * other rows are still worth having.  Drop just this one. */
            if (c < 0x20U || c == 0x7FU)
                break;
            text[k] = (char)c;
        }
        text[k] = '\0';
        if (k != tn) {
            yew_xfree(text);
            continue;
        }
        for (k = 0U; k < n; k++) {
            if (strcmp(rows[k].text, text) == 0)
                break;
        }
        if (k != n) {
            yew_xfree(text);
            continue;
        }
        if (n == cap) {
            cap = cap == 0U ? 64U : cap * 2U;
            rows = yew_xrealloc(rows, cap * sizeof(*rows));
        }
        rows[n].text = text;
        rows[n].desc = NULL;
        rows[n].is_dir = is_dir;
        if (tab != NULL) {
            char *d = clean_field(tab + 1,
                                  ll - (size_t)(tab - line) - 1U);

            if (d[0] == '\0')
                yew_xfree(d);
            else
                rows[n].desc = d;
        }
        n++;
    }
    if (count != NULL)
        *count = n;
    return rows;
}

/* ================================================================ */
/* The job                                                           */
/* ================================================================ */

static void disable_once(const char *why)
{
    if (fish.disabled)
        return;
    fish.disabled = true;
    /* The idle path and a job's arrival are not lookups: the log is
     * theirs to write.  One line, then silence for the session. */
    yew_log(YEW_LOG_INFO, "completion: fish oracle disabled: %s", why);
}

static void fish_complete(void *owner_ptr, Ed *ed, const YewJob *job)
{
    FishOwner *o = owner_ptr;
    YewFishRow *rows;
    u32 n = 0U;
    int at;

    o->completed = true;
    at = req_find(o->key);
    if (at < 0 || !fish.q[at].inflight)
        return; /* reset underneath it */
    if (job->state == YEW_JOB_EXECFAIL ||
        (job->state == YEW_JOB_EXITED && job->exit_code != 0 &&
         job->collect.len == 0U)) {
        char why[96];

        (void)snprintf(why, sizeof(why), "%s (exit %d)",
                       yew_job_state_name(job->state), job->exit_code);
        disable_once(why);
        /* Nothing more will spawn: this request and every unspawned
         * one go (at most one is ever in flight -- this one). */
        while (fish.nq != 0U)
            req_drop_at(fish.nq - 1U);
        /* The menu that waited now falls through to the help rung. */
        yew_cmdline_compgen_arrived(ed, o->key);
        return;
    }
    rows = NULL;
    if (job->state == YEW_JOB_EXITED || job->collect_capped)
        rows = yew_compfish_parse((const char *)job->collect.data,
                                  job->collect.len, &n);
    /* The script prints only when fish has rules for the command, so an
     * empty answer (or a timeout) is the gate closed. */
    entry_store(o->key, n != 0U, rows, n);
    req_drop_at((u32)at);
    /* 57.24 §5.4: refilter an open menu that asked for this key; the
     * line itself is never touched. */
    yew_cmdline_compgen_arrived(ed, o->key);
}

static void fish_destroy(void *owner_ptr)
{
    FishOwner *o = owner_ptr;

    if (!o->completed) {
        /* Evicted for the user's own job, or torn down: no answer. */
        int at = req_find(o->key);

        if (at >= 0 && fish.q[at].inflight)
            req_drop_at((u32)at);
    }
    yew_xfree(o);
}

static const YewJobCallbackOps fish_ops = {fish_complete, fish_destroy};

bool yew_compfish_idle_ready(void)
{
    u32 flying = yew_compfish_inflight();
    u32 i;

    if (fish.disabled || flying >= FISH_INFLIGHT_MAX ||
        yew_compgen_inflight() + yew_comphelp_inflight() + flying >=
            YEW_COMPGEN_MAX_INFLIGHT)
        return false;
    for (i = 0U; i < fish.nq; i++) {
        if (!fish.q[i].inflight)
            return true;
    }
    return false;
}

u32 yew_compfish_idle(Ed *ed)
{
    static const char *const env_set[] = {"NO_COLOR=1", "TERM=dumb",
                                          NULL};
    char *argv[9];
    FishReq *r = NULL;
    YewJobSpec spec;
    FishOwner *owner;
    const char *path;
    char err[256];
    u32 i;
    u32 id;

    if (ed == NULL || !yew_compfish_idle_ready())
        return 0U;
    for (i = 0U; i < fish.nq; i++) {
        if (!fish.q[i].inflight) {
            r = &fish.q[i];
            break;
        }
    }
    if (r == NULL)
        return 0U;
    path = yew_compfish_path();
    if (!option_auto(ed) || path == NULL) {
        req_drop_at(i);
        return 0U;
    }
    /* The user's line is $argv[1] and nothing else: it is never part of
     * the script fish evaluates. */
    argv[0] = (char *)path;
    argv[1] = "-N";
    argv[2] = "--private";
    argv[3] = "-c";
    argv[4] = (char *)YEW_COMPFISH_QUERY;
    argv[5] = "--";
    argv[6] = r->line;
    argv[7] = r->cmd;
    argv[8] = NULL;
    owner = yew_xcalloc(1U, sizeof(*owner));
    (void)memcpy(owner->key, r->key, YEW_COMPFISH_KEY_LEN);
    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = argv;
    spec.cwd = r->cwd[0] == '\0' ? NULL : r->cwd;
    spec.sink = YEW_SINK_CALLBACK;
    spec.internal = true;
    spec.evictable = true;
    spec.timeout_ms = YEW_COMPFISH_TIMEOUT_MS;
    spec.collect_max = YEW_COMPFISH_COLLECT_MAX;
    spec.env_set = env_set;
    spec.display = "fish";
    spec.callback_owner = owner;
    spec.callback_ops = &fish_ops;
    fish.spawns++;
    id = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        char key[YEW_COMPFISH_KEY_LEN];

        (void)memcpy(key, r->key, sizeof(key));
        yew_xfree(owner);
        disable_once(err);
        while (fish.nq != 0U)
            req_drop_at(fish.nq - 1U);
        /* The menu that waited falls through to the help rung now. */
        yew_cmdline_compgen_arrived(ed, key);
        return 0U;
    }
    r->inflight = true;
    r->job_id = id;
    return 1U;
}
