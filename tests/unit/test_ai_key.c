/* Sprint 48 §9: environment/argv key resolution and secret-buffer hygiene. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/ai/backend.h"
#include "mod/ai/key.h"

static void key_fixture(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
}

static bool bytes_are_zero(const void *bytes, size_t len)
{
    const u8 *p = bytes;
    size_t i;

    for (i = 0U; i < len; i++) {
        if (p[i] != 0U)
            return false;
    }
    return true;
}

static const char *key_false_path(void)
{
    return access("/bin/false", X_OK) == 0 ? "/bin/false" :
                                             "/usr/bin/false";
}

static void write_all(int fd, const u8 *bytes, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);

        YEW_ASSERT(n > 0);
        off += (size_t)n;
    }
}

static void key_file(char *path, const u8 *bytes, size_t len)
{
    int fd = mkstemp(path);

    YEW_ASSERT(fd >= 0);
    write_all(fd, bytes, len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static i64 key_counter_read(const char *path)
{
    char bytes[32] = {0};
    int fd = open(path, O_RDONLY);
    ssize_t got;

    YEW_ASSERT(fd >= 0);
    got = read(fd, bytes, sizeof(bytes) - 1U);
    YEW_ASSERT(got >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return got == 0 ? 0 : strtol(bytes, NULL, 10);
}

void test_ai_key_env_resolution(void)
{
    Ed ed;
    AiBackend backend = {0};
    AiErr err;
    char out[64];

    key_fixture(&ed);
    backend.name = "cloud";
    backend.key_env = "YEW_TEST_AI_KEY_S48";
    YEW_ASSERT_EQ_I64(setenv(backend.key_env, "environment-value", 1), 0);
    YEW_ASSERT(yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "environment-value");
    YEW_ASSERT_EQ_I64(err.kind, YEW_AI_OK);
    YEW_ASSERT_EQ_I64(err.retry_ms, -1);
    yew_memzero(out, sizeof(out));
    YEW_ASSERT_EQ_I64(unsetenv(backend.key_env), 0);
    yew_ed_free(&ed);
}

void test_ai_key_env_errors_wipe(void)
{
    Ed ed;
    AiBackend backend = {0};
    AiErr err;
    char out[32];
    char *oversize;
    char *both_argv[] = {(char *)key_false_path(), NULL};

    key_fixture(&ed);
    backend.name = "cloud";
    backend.key_env = "YEW_TEST_AI_KEY_MISSING_S48";
    YEW_ASSERT_EQ_I64(unsetenv(backend.key_env), 0);
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_I64(err.kind, YEW_AI_ERR_AUTH);
    YEW_ASSERT_EQ_STR(err.msg,
        "API key environment variable $YEW_TEST_AI_KEY_MISSING_S48 is not set");

    oversize = yew_xmalloc(YEW_AI_KEY_MAX + 2U);
    (void)memset(oversize, 'k', YEW_AI_KEY_MAX + 1U);
    oversize[YEW_AI_KEY_MAX + 1U] = '\0';
    YEW_ASSERT_EQ_I64(setenv(backend.key_env, oversize, 1), 0);
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_STR(err.msg,
        "API key from $YEW_TEST_AI_KEY_MISSING_S48 exceeds 8 KiB");
    YEW_ASSERT_EQ_I64(unsetenv(backend.key_env), 0);
    yew_memzero(oversize, YEW_AI_KEY_MAX + 2U);
    free(oversize);

    backend.key_cmd = both_argv;
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_STR(err.msg,
                     "backend 'cloud' has both key_env and key_cmd");
    yew_ed_free(&ed);
}

void test_ai_key_cmd_resolution(void)
{
    static const u8 contents[] = "command-value\r\n";
    static const char counter_script[] =
        "n=0; if test -s \"$1\"; then IFS= read -r n < \"$1\"; fi; "
        "n=$((n + 1)); printf '%s\\n' \"$n\" > \"$1\"; "
        "printf 'key-%s\\n' \"$n\"";
    Ed ed;
    AiBackend backend = {0};
    AiErr err;
    char path[] = "/tmp/yew-ai-key-XXXXXX";
    char *argv[] = {(char *)"/bin/cat", path, NULL};
    char counter_path[] = "/tmp/yew-ai-key-count-XXXXXX";
    char *counter_argv[] = {
        (char *)"/bin/sh", (char *)"-c", (char *)counter_script,
        (char *)"yew-key-counter", counter_path, NULL
    };
    AiKeyCache cache;
    char out[64];

    key_fixture(&ed);
    key_file(path, contents, sizeof(contents) - 1U);
    backend.name = "cloud";
    backend.key_cmd = argv;
    YEW_ASSERT(yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "command-value");
    YEW_ASSERT_EQ_I64(err.kind, YEW_AI_OK);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    yew_memzero(out, sizeof(out));
    YEW_ASSERT_EQ_I64(unlink(path), 0);

    key_file(counter_path, NULL, 0U);
    backend.key_cmd = counter_argv;
    yew_ai_key_cache_init(&cache);
    YEW_ASSERT(cache.enabled);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-1");
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-1");
    YEW_ASSERT_EQ_I64(key_counter_read(counter_path), 1);

    yew_ai_key_cache_reload(&cache);
    YEW_ASSERT(cache.enabled);
    YEW_ASSERT(cache.entries == NULL);
    YEW_ASSERT_EQ_U64(cache.len, 0U);
    YEW_ASSERT_EQ_U64(cache.cap, 0U);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-2");

    yew_ai_key_cache_enable(&cache, false);
    YEW_ASSERT(cache.entries == NULL);
    YEW_ASSERT_EQ_U64(cache.len, 0U);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-3");
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-4");
    YEW_ASSERT_EQ_I64(key_counter_read(counter_path), 4);

    yew_ai_key_cache_enable(&cache, true);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &cache, &backend,
                                    out, sizeof(out), &err));
    YEW_ASSERT_EQ_STR(out, "key-5");
    YEW_ASSERT(cache.entries != NULL);
    YEW_ASSERT_EQ_U64(cache.len, 1U);
    yew_ai_key_cache_drop(&cache);
    YEW_ASSERT(cache.entries == NULL);
    YEW_ASSERT_EQ_U64(cache.len, 0U);
    YEW_ASSERT_EQ_U64(cache.cap, 0U);
    YEW_ASSERT(!cache.enabled);
    YEW_ASSERT_EQ_I64(cache.command_timeout_ms, 0);
    yew_memzero(out, sizeof(out));
    YEW_ASSERT_EQ_I64(unlink(counter_path), 0);
    yew_ed_free(&ed);
}

void test_ai_key_cmd_errors_wipe(void)
{
    Ed ed;
    AiBackend backend = {0};
    AiErr err;
    char *false_argv[] = {(char *)key_false_path(), NULL};
    char *missing_argv[] = {
        (char *)"/nonexistent/yew-key-command-s48", NULL
    };
    char *sleep_argv[] = {(char *)"/bin/sleep", (char *)"5", NULL};
    AiKeyCache cache;
    i64 started;
    char out[64];

    key_fixture(&ed);
    backend.name = "cloud";
    backend.key_cmd = false_argv;
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_I64(err.kind, YEW_AI_ERR_AUTH);
    YEW_ASSERT_EQ_STR(err.msg,
                      strcmp(false_argv[0], "/bin/false") == 0 ?
                          "key command '/bin/false' exited 1" :
                          "key command '/usr/bin/false' exited 1");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    backend.key_cmd = missing_argv;
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT(strstr(err.msg, "key command '/nonexistent/yew-key-command-s48'")
               != NULL);
    YEW_ASSERT(strstr(err.msg, "No such file or directory") != NULL);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    yew_ai_key_cache_init(&cache);
    yew_ai_key_cache_set_timeout(&cache, 30);
    backend.key_cmd = sleep_argv;
    started = yew_now_ms();
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_cache_get(&ed, &cache, &backend,
                                     out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_STR(err.msg, "key command '/bin/sleep' timed out");
    YEW_ASSERT(yew_now_ms() - started < 1000);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    yew_ai_key_cache_drop(&cache);
    yew_ed_free(&ed);
}

void test_ai_key_cmd_cap_and_bytes(void)
{
    Ed ed;
    AiBackend backend = {0};
    AiErr err;
    char path[] = "/tmp/yew-ai-key-XXXXXX";
    char *argv[] = {(char *)"/bin/cat", path, NULL};
    u8 *contents = yew_xmalloc(YEW_AI_KEY_MAX + 1U);
    char out[64];

    key_fixture(&ed);
    (void)memset(contents, 'k', YEW_AI_KEY_MAX + 1U);
    key_file(path, contents, YEW_AI_KEY_MAX + 1U);
    backend.name = "cloud";
    backend.key_cmd = argv;
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_STR(err.msg,
        "API key from key command '/bin/cat' exceeds 8 KiB");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    YEW_ASSERT_EQ_I64(unlink(path), 0);

    path[0] = '/';
    (void)memcpy(path, "/tmp/yew-ai-key-XXXXXX", sizeof(path));
    contents[0] = (u8)'a';
    contents[1] = 0U;
    contents[2] = (u8)'b';
    key_file(path, contents, 3U);
    (void)memset(out, 'x', sizeof(out));
    YEW_ASSERT(!yew_ai_key_get(&ed, &backend, out, sizeof(out), &err));
    YEW_ASSERT(bytes_are_zero(out, sizeof(out)));
    YEW_ASSERT_EQ_STR(err.msg,
        "API key from key command '/bin/cat' contains a line break or NUL");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    yew_memzero(contents, YEW_AI_KEY_MAX + 1U);
    free(contents);
    yew_ed_free(&ed);
}
