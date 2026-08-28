#define _POSIX_C_SOURCE 200809L

#include "mod/ai/key.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "mod/ai/backend.h"
#include "util/base.h"

enum {
    YEW_AI_KEY_POLL_MS = 20,
    YEW_AI_KEY_CMD_TIMEOUT_MS = YEW_FILTER_TIMEOUT_MS
};

struct AiKeyCacheEntry {
    const AiBackend *backend;
    char *key;
    size_t key_cap;
};

static bool key_error(AiErr *err, const char *fmt, ...)
{
    va_list ap;

    if (err != NULL) {
        err->kind = YEW_AI_ERR_AUTH;
        err->retry_ms = -1;
        va_start(ap, fmt);
        (void)vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        va_end(ap);
    }
    return false;
}

static void key_error_clear(AiErr *err)
{
    if (err == NULL)
        return;
    (void)memset(err, 0, sizeof(*err));
    err->kind = YEW_AI_OK;
    err->retry_ms = -1;
}

static bool key_bytes_safe(const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        if (bytes[i] == 0U || bytes[i] == (u8)'\r' ||
            bytes[i] == (u8)'\n')
            return false;
    }
    return true;
}

static bool key_copy(const u8 *bytes, size_t len, const char *source,
                     char *out, size_t outsz, AiErr *err)
{
    if (len == 0U)
        return key_error(err, "API key from %s is empty", source);
    if (len > YEW_AI_KEY_MAX)
        return key_error(err, "API key from %s exceeds 8 KiB", source);
    if (!key_bytes_safe(bytes, len))
        return key_error(err, "API key from %s contains a line break or NUL",
                         source);
    if (outsz <= len)
        return key_error(err, "API key buffer is too small for %s", source);
    (void)memcpy(out, bytes, len);
    out[len] = '\0';
    return true;
}

static bool key_job_drive(Ed *ed, YewJob *job, bool *too_large)
{
    bool poll_failed = false;

    while (yew_job_pending(job)) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;
        int rc;

        yew_job_collect_fds(ed, pfd, &n);
        {
            i64 deadline = yew_job_deadline(ed, yew_now_ms());
            int wait_ms = YEW_AI_KEY_POLL_MS;

            if (deadline >= 0 && deadline < wait_ms)
                wait_ms = (int)deadline;
            rc = poll(pfd, (nfds_t)n, wait_ms);
        }
        if (rc < 0 && errno != EINTR) {
            poll_failed = true;
            if (job->state == YEW_JOB_RUNNING)
                (void)yew_job_signal(ed, job->id, SIGTERM);
        } else if (rc >= 0) {
            yew_job_pump(ed, pfd, n);
        }
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);

        /* A trailing CRLF is one newline and is allowed beyond the cap.
         * Stop a producer once it cannot possibly become an 8 KiB key. */
        if (!*too_large && job->collect.len > YEW_AI_KEY_MAX + 2U) {
            *too_large = true;
            if (job->state == YEW_JOB_RUNNING)
                (void)yew_job_signal(ed, job->id, SIGTERM);
        }
    }
    yew_job_settle(ed);
    return !poll_failed;
}

static bool key_from_env(const AiBackend *backend, char *out, size_t outsz,
                         AiErr *err)
{
    const char *value = getenv(backend->key_env);
    char source[160];
    size_t len;

    (void)snprintf(source, sizeof(source), "$%s", backend->key_env);
    if (value == NULL || value[0] == '\0')
        return key_error(err, "API key environment variable %s is not set",
                         source);
    len = strnlen(value, YEW_AI_KEY_MAX + 1U);
    return key_copy((const u8 *)value, len, source, out, outsz, err);
}

static bool key_from_cmd(Ed *ed, const AiBackend *backend, char *out,
                         size_t outsz, i64 timeout_ms, AiErr *err)
{
    YewJobSpec spec = {0};
    char spawn_err[192] = {0};
    const char *command = backend->key_cmd[0];
    char source[160];
    YewJob *job;
    u32 id;
    size_t len;
    bool too_large = false;
    bool driven;
    bool ok = false;

    (void)snprintf(source, sizeof(source), "key command '%s'", command);
    /* The job API predates this immutable backend description but neither
     * job.c nor execve mutates the argv vector or its strings. */
    spec.argv = (char **)backend->key_cmd;
    spec.sink = YEW_SINK_COLLECT;
    spec.display = command;
    spec.timeout_ms = timeout_ms;
    id = yew_job_spawn(ed, &spec, spawn_err, sizeof(spawn_err));
    if (id == 0U)
        return key_error(err, "cannot run %s: %s", source,
                         spawn_err[0] != '\0' ? spawn_err : "spawn failed");
    job = yew_job_find(ed, id);
    if (job == NULL)
        return key_error(err, "cannot run %s: job disappeared", source);
    job->synchronous = true;
    driven = key_job_drive(ed, job, &too_large);

    if (!driven) {
        (void)key_error(err, "cannot read %s output", source);
    } else if (too_large || job->collect.len > YEW_AI_KEY_MAX + 2U) {
        (void)key_error(err, "API key from %s exceeds 8 KiB", source);
    } else if (job->state == YEW_JOB_EXECFAIL) {
        (void)key_error(err, "cannot run %s: %s", source,
                        strerror(job->exec_errno));
    } else if (job->state == YEW_JOB_TIMEOUT) {
        (void)key_error(err, "%s timed out", source);
    } else if (job->state != YEW_JOB_EXITED || job->exit_code != 0) {
        if (job->state == YEW_JOB_EXITED)
            (void)key_error(err, "%s exited %d", source, job->exit_code);
        else
            (void)key_error(err, "%s was terminated", source);
    } else if (job->bytes_err != 0U) {
        /* The generic collect sink merges stderr into its buffer.  Refuse
         * rather than risk treating a diagnostic as credential bytes. */
        (void)key_error(err, "%s wrote to stderr", source);
    } else {
        len = job->collect.len;
        if (len != 0U && job->collect.data[len - 1U] == (u8)'\n') {
            len--;
            if (len != 0U && job->collect.data[len - 1U] == (u8)'\r')
                len--;
        }
        ok = key_copy(job->collect.data, len, source, out, outsz, err);
    }

    /* `bytebuf_free` cannot know that this particular allocation is a
     * secret, so wipe the full allocation before the generic release. */
    if (job->collect.data != NULL)
        yew_memzero(job->collect.data, job->collect.cap);
    if (job->hold.data != NULL)
        yew_memzero(job->hold.data, job->hold.cap);
    yew_memzero(spawn_err, sizeof(spawn_err));
    yew_job_release(ed, job);
    return ok;
}

static bool key_get_with_timeout(Ed *ed, const AiBackend *backend, char *out,
                                 size_t outsz, i64 timeout_ms, AiErr *err)
{
    if (out != NULL && outsz != 0U)
        yew_memzero(out, outsz);
    key_error_clear(err);
    if (ed == NULL || backend == NULL || out == NULL || outsz == 0U)
        return key_error(err, "cannot resolve API key: invalid argument");
    if (backend->key_env != NULL && backend->key_cmd != NULL)
        return key_error(err,
                         "backend '%s' has both key_env and key_cmd",
                         backend->name != NULL ? backend->name : "<unnamed>");
    if (backend->key_env != NULL) {
        if (backend->key_env[0] == '\0')
            return key_error(err, "API key environment variable name is empty");
        return key_from_env(backend, out, outsz, err);
    }
    if (backend->key_cmd != NULL) {
        if (backend->key_cmd[0] == NULL || backend->key_cmd[0][0] == '\0')
            return key_error(err, "API key command is empty");
        return key_from_cmd(ed, backend, out, outsz, timeout_ms, err);
    }
    return key_error(err, "backend '%s' has no API key source",
                     backend->name != NULL ? backend->name : "<unnamed>");
}

bool yew_ai_key_get(Ed *ed, const AiBackend *backend, char *out,
                    size_t outsz, AiErr *err)
{
    return key_get_with_timeout(ed, backend, out, outsz,
                                YEW_AI_KEY_CMD_TIMEOUT_MS, err);
}

static void key_cache_wipe_entries(AiKeyCache *cache)
{
    size_t i;

    if (cache == NULL)
        return;
    for (i = 0U; i < cache->len; i++) {
        AiKeyCacheEntry *entry = &cache->entries[i];

        if (entry->key != NULL) {
            yew_memzero(entry->key, entry->key_cap);
            yew_xfree(entry->key);
        }
    }
    if (cache->entries != NULL) {
        yew_memzero(cache->entries, cache->cap * sizeof(*cache->entries));
        yew_xfree(cache->entries);
    }
    cache->entries = NULL;
    cache->len = 0U;
    cache->cap = 0U;
}

void yew_ai_key_cache_init(AiKeyCache *cache)
{
    if (cache == NULL)
        return;
    (void)memset(cache, 0, sizeof(*cache));
    cache->enabled = true;
    cache->command_timeout_ms = YEW_AI_KEY_CMD_TIMEOUT_MS;
}

void yew_ai_key_cache_reload(AiKeyCache *cache)
{
    key_cache_wipe_entries(cache);
}

void yew_ai_key_cache_drop(AiKeyCache *cache)
{
    if (cache == NULL)
        return;
    key_cache_wipe_entries(cache);
    yew_memzero(cache, sizeof(*cache));
}

void yew_ai_key_cache_enable(AiKeyCache *cache, bool enabled)
{
    if (cache == NULL)
        return;
    if (!enabled)
        key_cache_wipe_entries(cache);
    cache->enabled = enabled;
}

void yew_ai_key_cache_set_timeout(AiKeyCache *cache, i64 timeout_ms)
{
    if (cache == NULL)
        return;
    cache->command_timeout_ms = timeout_ms > 0
        ? timeout_ms : YEW_AI_KEY_CMD_TIMEOUT_MS;
}

static AiKeyCacheEntry *key_cache_find(AiKeyCache *cache,
                                       const AiBackend *backend)
{
    size_t i;

    for (i = 0U; i < cache->len; i++) {
        if (cache->entries[i].backend == backend)
            return &cache->entries[i];
    }
    return NULL;
}

static i64 key_cache_timeout(const AiKeyCache *cache)
{
    return cache->command_timeout_ms > 0
        ? cache->command_timeout_ms : YEW_AI_KEY_CMD_TIMEOUT_MS;
}

static AiKeyCacheEntry *key_cache_append(AiKeyCache *cache,
                                         const AiBackend *backend,
                                         const char *key, size_t len)
{
    AiKeyCacheEntry *entry;

    if (cache->len == cache->cap) {
        size_t newcap = cache->cap == 0U ? 4U : cache->cap * 2U;
        AiKeyCacheEntry *entries = yew_xcalloc(newcap, sizeof(*entries));

        if (cache->len != 0U)
            (void)memcpy(entries, cache->entries,
                         cache->len * sizeof(*entries));
        if (cache->entries != NULL) {
            yew_memzero(cache->entries,
                        cache->cap * sizeof(*cache->entries));
            yew_xfree(cache->entries);
        }
        cache->entries = entries;
        cache->cap = newcap;
    }
    entry = &cache->entries[cache->len++];
    entry->backend = backend;
    entry->key_cap = len + 1U;
    entry->key = yew_xmalloc(entry->key_cap);
    (void)memcpy(entry->key, key, entry->key_cap);
    return entry;
}

bool yew_ai_key_cache_get(Ed *ed, AiKeyCache *cache,
                          const AiBackend *backend, char *out, size_t outsz,
                          AiErr *err)
{
    AiKeyCacheEntry *entry;
    char key[YEW_AI_KEY_MAX + 1U];
    char source[160];
    size_t len;
    bool ok;

    if (cache == NULL)
        return key_get_with_timeout(ed, backend, out, outsz,
                                    YEW_AI_KEY_CMD_TIMEOUT_MS, err);
    if (!cache->enabled)
        return key_get_with_timeout(ed, backend, out, outsz,
                                    key_cache_timeout(cache), err);
    if (out != NULL && outsz != 0U)
        yew_memzero(out, outsz);
    key_error_clear(err);
    if (ed == NULL || backend == NULL || out == NULL || outsz == 0U)
        return key_error(err, "cannot resolve API key: invalid argument");
    entry = key_cache_find(cache, backend);
    if (entry != NULL) {
        (void)snprintf(source, sizeof(source), "cached backend '%s'",
                       backend->name != NULL ? backend->name : "<unnamed>");
        return key_copy((const u8 *)entry->key, entry->key_cap - 1U,
                        source, out, outsz, err);
    }

    ok = key_get_with_timeout(ed, backend, key, sizeof(key),
                              key_cache_timeout(cache), err);
    if (ok) {
        len = strlen(key);
        entry = key_cache_append(cache, backend, key, len);
        (void)snprintf(source, sizeof(source), "cached backend '%s'",
                       backend->name != NULL ? backend->name : "<unnamed>");
        ok = key_copy((const u8 *)entry->key, len, source,
                      out, outsz, err);
    }
    yew_memzero(key, sizeof(key));
    return ok;
}
