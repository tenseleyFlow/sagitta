/* Sprint 48 §7-§8: curl configuration, probing, and fake transport. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "mod/ai/backend_curl.h"

#ifndef YEW_TEST_FAKECURL
#define YEW_TEST_FAKECURL "build/tests/helpers/fakecurl"
#endif

typedef struct CurlStreamWitness {
    Bytebuf out;
    Bytebuf err;
    u32 destroys;
} CurlStreamWitness;

static bool curl_test_out(void *owner, const u8 *bytes, u64 len)
{
    CurlStreamWitness *w = owner;

    bytebuf_append(&w->out, bytes, (size_t)len);
    return true;
}

static bool curl_test_finish(void *owner)
{
    (void)owner;
    return true;
}

static bool curl_test_err(void *owner, const u8 *bytes, u64 len)
{
    CurlStreamWitness *w = owner;

    bytebuf_append(&w->err, bytes, (size_t)len);
    return true;
}

static void curl_test_destroy(void *owner)
{
    CurlStreamWitness *w = owner;

    w->destroys++;
}

static const YewJobStreamOps curl_test_ops = {
    curl_test_out,
    curl_test_finish,
    curl_test_err,
    curl_test_finish,
    NULL,
    NULL,
    curl_test_destroy
};

static bool curl_drive(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    for (;;) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;
        YewJob *job = yew_job_find(ed, id);

        if (job == NULL)
            return false;
        if (job->drained)
            return true;
        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

static char *curl_save_env(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = malloc(strlen(value) + 1U);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void curl_restore_env(const char *name, char *saved)
{
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv(name, saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    }
}

static void curl_fixture_path(char *dir, size_t dirsz, char *counter,
                              size_t countersz, char *capture,
                              size_t capturesz)
{
    char link[512];

    (void)snprintf(dir, dirsz, "/tmp/yew-fakecurl-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(link, sizeof(link), "%s/curl", dir);
    YEW_ASSERT_EQ_I64(symlink(YEW_TEST_FAKECURL, link), 0);
    (void)snprintf(counter, countersz, "%s/probes", dir);
    (void)snprintf(capture, capturesz, "%s/config", dir);
}

static u64 curl_file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fseek(fp, 0L, SEEK_END), 0);
    n = ftell(fp);
    YEW_ASSERT(n >= 0L);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    return (u64)n;
}

static void curl_fixture_remove(const char *dir, const char *counter,
                                const char *capture)
{
    char link[512];

    (void)snprintf(link, sizeof(link), "%s/curl", dir);
    (void)unlink(capture);
    (void)unlink(counter);
    (void)unlink(link);
    (void)rmdir(dir);
}

void test_ai_curl_argv_is_fixed_and_secret_free(void)
{
    char *const *argv = yew_ai_curl_argv();
    u32 i;

    YEW_ASSERT_EQ_STR(argv[0], "curl");
    YEW_ASSERT_EQ_STR(argv[1], "-sS");
    YEW_ASSERT_EQ_STR(argv[2], "--no-buffer");
    YEW_ASSERT_EQ_STR(argv[3], "--config");
    YEW_ASSERT_EQ_STR(argv[4], "-");
    YEW_ASSERT_NULL(argv[5]);
    for (i = 0U; argv[i] != NULL; i++) {
        YEW_ASSERT(strstr(argv[i], "sk-test") == NULL);
        YEW_ASSERT(strstr(argv[i], "example.invalid") == NULL);
    }
}

void test_ai_curl_config_golden_and_escaping(void)
{
    static const HttpHdr hdrs[] = {
        {"content-type", "application/json"}
    };
    static const u8 key[] = "sk-test-\\\"quoted";
    static const AiCurlSecret secret = {
        YEW_CURL_AUTH_X_API_KEY, key, sizeof(key) - 1U
    };
    static const u8 body[] = "{\"model\":\"say \\\"hi\\\"\\\\now\"}";
    static const char expected[] =
        "url = \"https://example.invalid/v1/messages\"\n"
        "request = \"POST\"\n"
        "header = \"content-type: application/json\"\n"
        "header = \"x-api-key: sk-test-\\\\\\\"quoted\"\n"
        "data-binary = \"{\\\"model\\\":\\\"say \\\\\\\"hi\\\\\\\""
        "\\\\\\\\now\\\"}\"\n"
        "connect-timeout = 2\n"
        "max-time = 120\n"
        "write-out = \"%{stderr}yew-http-status: %{http_code}\\n\"\n";
    AiCurlRequest req = {
        "https://example.invalid/v1/messages",
        "POST",
        hdrs,
        YEW_ARRAY_LEN(hdrs),
        body,
        sizeof(body) - 1U
    };
    Bytebuf one;
    Bytebuf two;
    char err[128] = {0};

    bytebuf_init(&one);
    bytebuf_init(&two);
    bytebuf_reserve(&one, sizeof(expected) - 1U);
    {
        u8 *stable = one.data;

        YEW_ASSERT(yew_ai_curl_config(&one, &req, &secret,
                                      err, sizeof(err)));
        YEW_ASSERT(one.data == stable);
    }
    YEW_ASSERT_EQ_STR(err, "");
    YEW_ASSERT_EQ_U64((u64)one.len, sizeof(expected) - 1U);
    YEW_ASSERT_EQ_MEM(one.data, expected, sizeof(expected) - 1U);

    /* Forced-growth regression: the builder must grow before copying the
     * secret.  A second same-sized build then pins allocator stability. */
    YEW_ASSERT_NULL(two.data);
    YEW_ASSERT(yew_ai_curl_config(&two, &req, &secret, err, sizeof(err)));
    YEW_ASSERT_EQ_U64((u64)two.len, (u64)one.len);
    YEW_ASSERT_EQ_MEM(two.data, one.data, one.len);
    {
        u8 *stable = two.data;

        yew_memzero(two.data, two.cap);
        two.len = 0U;
        YEW_ASSERT(yew_ai_curl_config(&two, &req, &secret,
                                      err, sizeof(err)));
        YEW_ASSERT(two.data == stable);
        YEW_ASSERT_EQ_U64((u64)two.len, (u64)one.len);
        YEW_ASSERT_EQ_MEM(two.data, one.data, one.len);
    }
    yew_memzero(one.data, one.len);
    yew_memzero(two.data, two.len);
    bytebuf_free(&one);
    bytebuf_free(&two);
}

void test_ai_curl_config_rejects_controls_transactionally(void)
{
    static const HttpHdr bad_hdr[] = {{"X-Api-Key", "misplaced-secret"}};
    static const u8 bad_secret_bytes[] = "secret\nnext";
    static const AiCurlSecret bad_secret = {
        YEW_CURL_AUTH_X_API_KEY,
        bad_secret_bytes,
        sizeof(bad_secret_bytes) - 1U
    };
    static const u8 bad_body[] = {'{', 0U, '}'};
    AiCurlRequest req = {
        "https://example.invalid/",
        "POST",
        bad_hdr,
        YEW_ARRAY_LEN(bad_hdr),
        (const u8 *)"{}",
        2U
    };
    Bytebuf out;
    char err[128] = {0};

    bytebuf_init(&out);
    bytebuf_append(&out, "keep", 4U);
    YEW_ASSERT(!yew_ai_curl_config(&out, &req, NULL, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err,
                      "authorization headers must use the secret argument");
    YEW_ASSERT_EQ_U64((u64)out.len, 4U);
    YEW_ASSERT_EQ_MEM(out.data, "keep", 4U);

    req.hdrs = NULL;
    req.nhdr = 0U;
    YEW_ASSERT(!yew_ai_curl_config(&out, &req, &bad_secret,
                                   err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, "curl config value contains a control byte");
    YEW_ASSERT_EQ_U64((u64)out.len, 4U);

    req.body = bad_body;
    req.blen = sizeof(bad_body);
    YEW_ASSERT(!yew_ai_curl_config(&out, &req, NULL, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, "curl config value contains a control byte");
    YEW_ASSERT_EQ_U64((u64)out.len, 4U);
    bytebuf_free(&out);
}

void test_ai_curl_exit_code_mapping(void)
{
    struct Case {
        int code;
        int sig;
        AiErrKind want;
    } cases[] = {
        {0, 0, YEW_AI_OK},
        {2, 0, YEW_AI_ERR_PROTOCOL},
        {6, 0, YEW_AI_ERR_UNREACHABLE},
        {7, 0, YEW_AI_ERR_UNREACHABLE},
        {22, 0, YEW_AI_ERR_PROTOCOL},
        {28, 0, YEW_AI_ERR_TIMEOUT},
        {35, 0, YEW_AI_ERR_TLS},
        {60, 0, YEW_AI_ERR_TLS},
        {130, 0, YEW_AI_ERR_CANCELLED},
        {99, 0, YEW_AI_ERR_PROTOCOL},
        {0, SIGTERM, YEW_AI_ERR_CANCELLED},
        {0, SIGKILL, YEW_AI_ERR_PROTOCOL}
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++)
        YEW_ASSERT(yew_ai_curl_exit_class(cases[i].code, cases[i].sig) ==
                   cases[i].want);
}

void test_ai_curl_probe_messages_and_cache(void)
{
    static const char too_old[] =
        "curl 7.61 is too old for the AI transport (need >= 7.63 for "
        "--write-out %{stderr}). Upgrade curl, or use a local http:// "
        "backend.";
    static const char absent[] =
        "cloud AI backends need curl, which is not in $PATH. Install curl, "
        "or run a local model: :ai backend local (see :help ai-local).";
    char dir[128];
    char counter[192];
    char capture[192];
    char link[192];
    char err[256] = {0};
    char *old_path = curl_save_env("PATH");
    char *old_version = curl_save_env("YEW_FAKECURL_VERSION");
    char *old_counter = curl_save_env("YEW_FAKECURL_COUNTER");
    char *old_exit = curl_save_env("YEW_FAKECURL_EXIT");
    Ed ed;
    AiCurlProbe probe;
    u32 id;
    u32 i;

    curl_fixture_path(dir, sizeof(dir), counter, sizeof(counter), capture,
                      sizeof(capture));
    YEW_ASSERT_EQ_I64(setenv("PATH", dir, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_COUNTER", counter, 1), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_FAKECURL_EXIT"), 0);

    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                             "curl 8.5.0 fakecurl", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_ai_curl_probe_init(&probe);
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, "");
    id = probe.job_id;
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(curl_drive(&ed, id));
    YEW_ASSERT(yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(yew_ai_curl_probe_version(&probe), "curl 8.5.0");
    for (i = 0U; i < 50U; i++)
        YEW_ASSERT(yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT_EQ_U64(probe.probes, 1U);
    YEW_ASSERT_EQ_U64(curl_file_size(counter), 1U);
    yew_ai_curl_probe_free(&probe);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                             "curl 7.61.0 fakecurl", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_ai_curl_probe_init(&probe);
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    id = probe.job_id;
    YEW_ASSERT(curl_drive(&ed, id));
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, too_old);
    YEW_ASSERT(probe.state == YEW_CURL_TOO_OLD);
    YEW_ASSERT_EQ_U64(probe.probes, 1U);
    YEW_ASSERT_EQ_U64(curl_file_size(counter), 2U);
    yew_ai_curl_probe_free(&probe);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                             "curl 7.63.0 fakecurl", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_ai_curl_probe_init(&probe);
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    id = probe.job_id;
    YEW_ASSERT(curl_drive(&ed, id));
    YEW_ASSERT(yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT(probe.state == YEW_CURL_OK);
    YEW_ASSERT_EQ_STR(yew_ai_curl_probe_version(&probe), "curl 7.63.0");
    YEW_ASSERT_EQ_U64(probe.probes, 1U);
    YEW_ASSERT_EQ_U64(curl_file_size(counter), 3U);
    yew_ai_curl_probe_free(&probe);
    yew_ed_free(&ed);

    (void)snprintf(link, sizeof(link), "%s/curl", dir);
    YEW_ASSERT_EQ_I64(unlink(link), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_ai_curl_probe_init(&probe);
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    id = probe.job_id;
    YEW_ASSERT(curl_drive(&ed, id));
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT_EQ_STR(err, absent);
    YEW_ASSERT(probe.state == YEW_CURL_ABSENT);
    YEW_ASSERT_EQ_U64(probe.probes, 1U);
    yew_ai_curl_probe_free(&probe);
    yew_ed_free(&ed);

    curl_restore_env("YEW_FAKECURL_EXIT", old_exit);
    curl_restore_env("YEW_FAKECURL_COUNTER", old_counter);
    curl_restore_env("YEW_FAKECURL_VERSION", old_version);
    curl_restore_env("PATH", old_path);
    curl_fixture_remove(dir, counter, capture);
}

void test_ai_curl_fake_transport_routes_and_hides_secret(void)
{
    static const char secret[] = "sk-fakecurl-never-in-argv";
    static const HttpHdr hdrs[] = {
        {"content-type", "application/json"}
    };
    static const AiCurlSecret auth = {
        YEW_CURL_AUTH_X_API_KEY,
        (const u8 *)secret,
        sizeof(secret) - 1U
    };
    static const u8 body[] = "{\"stream\":true}";
    AiCurlRequest req = {
        "https://example.invalid/v1/messages", "POST", hdrs,
        YEW_ARRAY_LEN(hdrs), body, sizeof(body) - 1U
    };
    char dir[128];
    char counter[192];
    char capture[192];
    char err[256] = {0};
    char captured[4096];
    char *old_path = curl_save_env("PATH");
    char *old_version = curl_save_env("YEW_FAKECURL_VERSION");
    char *old_counter = curl_save_env("YEW_FAKECURL_COUNTER");
    char *old_capture = curl_save_env("YEW_FAKECURL_CAPTURE");
    char *old_secret = curl_save_env("YEW_FAKECURL_SECRET");
    char *old_stdout = curl_save_env("YEW_FAKECURL_STDOUT");
    char *old_status = curl_save_env("YEW_FAKECURL_STATUS");
    char *old_exit = curl_save_env("YEW_FAKECURL_EXIT");
    Ed ed;
    AiCurlProbe probe;
    CurlStreamWitness witness = {0};
    YewJobSpec spec = {0};
    Bytebuf config;
    YewJob *job;
    FILE *fp;
    size_t got;
    u32 id;
    u32 i;

    curl_fixture_path(dir, sizeof(dir), counter, sizeof(counter), capture,
                      sizeof(capture));
    YEW_ASSERT_EQ_I64(setenv("PATH", dir, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                             "curl 8.5.0 fakecurl", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_COUNTER", counter, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_CAPTURE", capture, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_SECRET", secret, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_STDOUT", "data: token\n\n", 1),
                      0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_STATUS", "201", 1), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_FAKECURL_EXIT"), 0);

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_ai_curl_probe_init(&probe);
    YEW_ASSERT(!yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    YEW_ASSERT(curl_drive(&ed, probe.job_id));
    YEW_ASSERT(yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));
    for (i = 0U; i < 10U; i++)
        YEW_ASSERT(yew_ai_curl_probe(&ed, &probe, err, sizeof(err)));

    bytebuf_init(&witness.out);
    bytebuf_init(&witness.err);
    bytebuf_init(&config);
    YEW_ASSERT(yew_ai_curl_config(&config, &req, &auth,
                                  err, sizeof(err)));
    spec.argv = (char **)yew_ai_curl_argv();
    spec.sink = YEW_SINK_STREAM;
    spec.in_bytes = config.data;
    spec.in_len = (u64)config.len;
    spec.stream_owner = &witness;
    spec.stream_ops = &curl_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(curl_drive(&ed, id));
    job = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT(job->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(job->exit_code, 0);
    YEW_ASSERT(yew_ai_curl_exit_class(job->exit_code, job->termsig) ==
               YEW_AI_OK);
    YEW_ASSERT_EQ_MEM(witness.out.data, "data: token\n\n", 13U);
    YEW_ASSERT_EQ_MEM(witness.err.data, "yew-http-status: 201\n", 21U);
    YEW_ASSERT_EQ_MEM(job->stream_err.data, witness.err.data, witness.err.len);
    YEW_ASSERT_EQ_U64(witness.destroys, 1U);
    for (i = 0U; i < config.len; i++)
        YEW_ASSERT_EQ_U64(config.data[i], 0U);
    YEW_ASSERT_EQ_U64(probe.probes, 1U);
    YEW_ASSERT_EQ_U64(curl_file_size(counter), 1U);

    fp = fopen(capture, "rb");
    YEW_ASSERT_NOT_NULL(fp);
    got = fread(captured, 1U, sizeof(captured) - 1U, fp);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    captured[got] = '\0';
    YEW_ASSERT(strstr(captured, secret) != NULL);
    YEW_ASSERT(strstr(captured, "url = \"https://example.invalid") != NULL);
    YEW_ASSERT(strstr(captured, "write-out = \"%{stderr}") != NULL);

    /* Drive a real nonzero fake-curl exit through the job layer too; the
     * complete mapping table above then pins every remaining code. */
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_EXIT", "28", 1), 0);
    witness.out.len = 0U;
    witness.err.len = 0U;
    config.len = 0U;
    YEW_ASSERT(yew_ai_curl_config(&config, &req, &auth,
                                  err, sizeof(err)));
    spec.in_bytes = config.data;
    spec.in_len = (u64)config.len;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(curl_drive(&ed, id));
    job = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT_EQ_I64(job->exit_code, 28);
    YEW_ASSERT(yew_ai_curl_exit_class(job->exit_code, job->termsig) ==
               YEW_AI_ERR_TIMEOUT);
    YEW_ASSERT_EQ_U64(witness.destroys, 2U);

    bytebuf_free(&witness.out);
    bytebuf_free(&witness.err);
    bytebuf_free(&config);
    yew_ai_curl_probe_free(&probe);
    yew_ed_free(&ed);

    curl_restore_env("YEW_FAKECURL_EXIT", old_exit);
    curl_restore_env("YEW_FAKECURL_STATUS", old_status);
    curl_restore_env("YEW_FAKECURL_STDOUT", old_stdout);
    curl_restore_env("YEW_FAKECURL_SECRET", old_secret);
    curl_restore_env("YEW_FAKECURL_CAPTURE", old_capture);
    curl_restore_env("YEW_FAKECURL_COUNTER", old_counter);
    curl_restore_env("YEW_FAKECURL_VERSION", old_version);
    curl_restore_env("PATH", old_path);
    curl_fixture_remove(dir, counter, capture);
}
