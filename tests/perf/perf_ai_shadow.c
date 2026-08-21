#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend_curl.h"
#include "mod/ai/context.h"
#include "mod/ai/prompt.h"
#include "mod/lsp/json.h"
#include "text/edit.h"
#include "unicode/utf8.h"
#include "util/sort.h"

#ifndef YEW_TEST_MOCKAI
#define YEW_TEST_MOCKAI "build/tests/helpers/mockai"
#endif
#ifndef YEW_TEST_MOCKCURL
#define YEW_TEST_MOCKCURL "build/tests/helpers/mockcurl"
#endif

enum {
    AI_SHADOW_RUNS = 100,
    AI_SHADOW_CONTEXT_RUNS = 101,
    AI_SHADOW_PROMPT_RUNS = 1001,
    AI_SHADOW_TIMEOUT_MS = 3000,
    AI_SHADOW_FIRST_P95_MS = 150,
    AI_SHADOW_CONTEXT_P99_NS = 200000,
    AI_SHADOW_PROMPT_P99_NS = 1000000,
    AI_SHADOW_SETUP_MAX_MS = 6,
    AI_SHADOW_TRANSPORT_FIRST_MAX_MS = 115,
    AI_SHADOW_DELIVER_MAX_MS = 5,
    AI_SHADOW_KEY_SAMPLES = 480,
    AI_SHADOW_KEY_P99_NS = 5000000,
    AI_SHADOW_BIG_BYTES = 100 * 1024 * 1024
};

typedef struct LiveSample {
    u64 first_ms;
    u64 context_ms;
    u64 prompt_ms;
    u64 setup_ms;
    u64 transport_first_ms;
    u64 deliver_ms;
} LiveSample;

static volatile u64 ai_shadow_sink;

static void fail(const char *message)
{
    (void)fprintf(stderr, "perf_ai_shadow: %s\n", message);
    exit(2);
}

static u64 now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime failed");
    return (u64)now.tv_sec * UINT64_C(1000000000) + (u64)now.tv_nsec;
}

static int cmp_u64(const void *left, const void *right, void *ctx)
{
    const u64 a = *(const u64 *)left;
    const u64 b = *(const u64 *)right;

    (void)ctx;
    return a < b ? -1 : a > b ? 1 : 0;
}

static u64 percentile(u64 *samples, u32 count, u32 pct)
{
    u32 index;

    yew_sort_stable(samples, count, sizeof(*samples), cmp_u64, NULL);
    index = (u32)(((u64)count * pct + 99U) / 100U);
    if (index == 0U)
        index = 1U;
    if (index > count)
        index = count;
    return samples[index - 1U];
}

static bool set_bool(Ed *ed, const char *name, bool value)
{
    OptVal option = {YEW_OPT_BOOL, {.b = value}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool set_int(Ed *ed, const char *name, i64 value)
{
    OptVal option = {YEW_OPT_INT, {.i = value}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool set_str(Ed *ed, const char *name, const char *value)
{
    OptVal option = {YEW_OPT_STR, {.str = {value, (u32)strlen(value)}}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static pid_t server_start(const char *script, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;
    bool parsed;

    if (pipe(output) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(YEW_TEST_MOCKAI, YEW_TEST_MOCKAI, "--port", "0",
              "--script", script, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    if (pid < 0) {
        (void)close(output[0]);
        return -1;
    }
    stream = fdopen(output[0], "r");
    if (stream == NULL) {
        (void)close(output[0]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    parsed = fscanf(stream, "port %u", &value) == 1 &&
             value != 0U && value <= 65535U;
    if (fclose(stream) != 0)
        parsed = false;
    if (!parsed) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    *port = (u16)value;
    return pid;
}

static void server_stop(pid_t pid)
{
    int status;

    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static bool pump_once(Ed *ed, int timeout_ms)
{
    struct pollfd fds[YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
    u32 nfds = 0U;
    int result;

    yew_job_collect_fds(ed, fds, &nfds);
    yew_ai_collect_fds(ed, fds, &nfds);
    result = poll(fds, (nfds_t)nfds, timeout_ms);
    if (result < 0 && errno != EINTR)
        return false;
    ed->now_ms = yew_now_ms();
    yew_job_pump(ed, fds, nfds);
    yew_job_reap(ed);
    yew_job_tick(ed, ed->now_ms);
    yew_job_settle(ed);
    yew_ai_pump(ed, fds, nfds);
    return true;
}

static bool curl_probe(Ed *ed)
{
    char error[192] = {0};
    i64 started = yew_now_ms();

    if (yew_ai_curl_probe(ed, &ed->ai->curl, error, sizeof(error)) ||
        error[0] != '\0')
        return false;
    while ((ed->ai->curl.running || ed->ai->curl.job_id != 0U) &&
           yew_now_ms() - started < AI_SHADOW_TIMEOUT_MS)
        if (!pump_once(ed, 10))
            return false;
    return yew_ai_curl_probe(ed, &ed->ai->curl, error, sizeof(error)) &&
           error[0] == '\0';
}

static bool drive_live(Ed *ed, LiveSample *sample)
{
    i64 started = yew_now_ms();
    bool saw_first = false;

    while (yew_now_ms() - started < AI_SHADOW_TIMEOUT_MS) {
        i64 deadline = yew_ai_deadline(ed, yew_now_ms());
        int timeout = deadline < 0 || deadline > 10 ? 10 : (int)deadline;

        if (!pump_once(ed, timeout))
            return false;
        if (!saw_first && ed->win->shadow.live) {
            const AiCall *call = &ed->ai->call;
            i64 now = yew_now_ms();

            if (call->t_armed < 0 || call->t_context < call->t_armed ||
                call->t_prompt < call->t_context ||
                call->t_sent < call->t_prompt ||
                call->t_first_token < call->t_sent)
                return false;
            sample->first_ms = (u64)(now - call->t_armed);
            sample->context_ms = (u64)(call->t_context - call->t_armed);
            sample->prompt_ms = (u64)(call->t_prompt - call->t_context);
            sample->setup_ms = (u64)(call->t_sent - call->t_prompt);
            sample->transport_first_ms =
                (u64)(call->t_first_token - call->t_sent);
            sample->deliver_ms = (u64)(now - call->t_first_token);
            saw_first = true;
        }
        if (saw_first && !ed->ai->call.active)
            return true;
    }
    return false;
}

static bool start_provider(Ed *ed, u16 port, bool curl_transport)
{
    static const u8 input[] = "int main(void) {\n    ";
    char source[512];
    int n;

    yew_ed_init(ed);
    if (!yew_ed_open_memory(ed, input, sizeof(input) - 1U, "ai-perf")) {
        (void)fprintf(stderr, "perf_ai_shadow: opening live buffer failed\n");
        return false;
    }
    ed->win->cs.curs.data[0].pos = BYTEOFF(sizeof(input) - 1U);
    ed->win->cs.curs.data[0].anchor = BYTEOFF(sizeof(input) - 1U);
    if (!set_bool(ed, "ai.enable", true) ||
        !set_int(ed, "ai.frame_ms", 0) ||
        !set_str(ed, "ai.backend", "local") ||
        !set_str(ed, "shadow.providers", "ai")) {
        (void)fprintf(stderr, "perf_ai_shadow: setting live options failed\n");
        return false;
    }
    n = snprintf(source, sizeof(source),
                 "import ai\n"
                 "ai.backend(\"local\", {kind: \"openai\", "
                 "transport: \"%s\", url: \"%s://127.0.0.1:%u/v1\", "
                 "model: \"qwen-coder\", "
                 "key_env: \"YEW_TEST_AI_PERF_KEY\"})\n",
                 curl_transport ? "curl" : "http",
                 curl_transport ? "https" : "http", (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(source) ||
        yew_fl_eval(ed, source, (u32)n) != YEW_CMD_OK) {
        (void)fprintf(stderr, "perf_ai_shadow: defining live backend failed: %s\n",
                      ed->msg.full == NULL ? ed->msg.text : ed->msg.full);
        return false;
    }
    if (curl_transport && !curl_probe(ed)) {
        (void)fprintf(stderr, "perf_ai_shadow: curl probe failed\n");
        return false;
    }
    ed->now_ms = yew_now_ms();
    yew_shadow_arm(ed, ed->win);
    if (ed->win->shadow.timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, ed->win->shadow.timer);
        ed->win->shadow.timer = YEW_TIMER_NONE;
    }
    ed->win->shadow.armed_at_ms = ed->now_ms - 350;
    yew_shadow_fire(ed, ed->win);
    if (!ed->ai->call.active)
        (void)fprintf(stderr,
                      "perf_ai_shadow: provider declined: pending=%u msg=%s\n",
                      ed->win->shadow.pending_mask,
                      ed->msg.full == NULL ? ed->msg.text : ed->msg.full);
    return ed->ai->call.active;
}

static bool live_sample(bool curl_transport, LiveSample *sample)
{
    const char *script = "tests/fixtures/ai/openai-perf.script";
    Ed ed;
    pid_t server = 0;
    u16 port = 443U;
    bool initialized = false;
    bool ok = false;

    if (curl_transport) {
        if (setenv("YEW_AI_MOCK_SCRIPT", script, 1) != 0)
            return false;
    } else {
        server = server_start(script, &port);
        if (server <= 0)
            return false;
    }
    if (!start_provider(&ed, port, curl_transport)) {
        initialized = true;
        goto done;
    }
    initialized = true;
    ok = drive_live(&ed, sample) && ed.win->shadow.live &&
         ed.win->shadow.sug.len == sizeof("int answer = 42;") - 1U &&
         memcmp(ed.win->shadow.sug.text, "int answer = 42;",
                sizeof("int answer = 42;") - 1U) == 0;
    if (!ok)
        (void)fprintf(stderr,
                      "perf_ai_shadow: %s live sample failed: active=%u live=%u len=%u msg=%s\n",
                      curl_transport ? "curl" : "http",
                      ed.ai->call.active ? 1U : 0U,
                      ed.win->shadow.live ? 1U : 0U,
                      ed.win->shadow.sug.len,
                      ed.msg.full == NULL ? ed.msg.text : ed.msg.full);

done:
    if (initialized)
        yew_ed_free(&ed);
    server_stop(server);
    return ok;
}

static bool measure_live_transport(bool curl_transport, u64 *p95_out)
{
    u64 first[AI_SHADOW_RUNS];
    u64 maximum = 0U;
    u32 i;

    for (i = 0U; i < AI_SHADOW_RUNS; i++) {
        LiveSample sample = {0U, 0U, 0U, 0U, 0U, 0U};

        if (!live_sample(curl_transport, &sample))
            return false;
        if (sample.context_ms > 3U || sample.prompt_ms > 1U ||
            sample.setup_ms > AI_SHADOW_SETUP_MAX_MS ||
            sample.transport_first_ms > AI_SHADOW_TRANSPORT_FIRST_MAX_MS ||
            sample.deliver_ms > AI_SHADOW_DELIVER_MAX_MS) {
            (void)fprintf(stderr,
                          "perf_ai_shadow: %s stage regression run=%u context=%llu prompt=%llu setup=%llu transport=%llu deliver=%llu first=%llu\n",
                          curl_transport ? "curl" : "http", i,
                          (unsigned long long)sample.context_ms,
                          (unsigned long long)sample.prompt_ms,
                          (unsigned long long)sample.setup_ms,
                          (unsigned long long)sample.transport_first_ms,
                          (unsigned long long)sample.deliver_ms,
                          (unsigned long long)sample.first_ms);
            return false;
        }
        first[i] = sample.first_ms;
        if (sample.first_ms > maximum)
            maximum = sample.first_ms;
    }
    *p95_out = percentile(first, AI_SHADOW_RUNS, 95U);
    (void)printf("ai.shadow.%s first_p50_ms=%llu first_p95_ms=%llu "
                 "first_max_ms=%llu budget_ms=%u%s\n",
                 curl_transport ? "curl" : "http",
                 (unsigned long long)first[AI_SHADOW_RUNS / 2U],
                 (unsigned long long)*p95_out,
                 (unsigned long long)maximum, AI_SHADOW_FIRST_P95_MS,
                 *p95_out <= AI_SHADOW_FIRST_P95_MS ? " ok" :
                                                       " REGRESSION");
    return *p95_out <= AI_SHADOW_FIRST_P95_MS;
}

static bool measure_context(u64 *p99_out)
{
    u8 *bytes = malloc(AI_SHADOW_BIG_BYTES);
    u64 samples[AI_SHADOW_CONTEXT_RUNS];
    Ed ed;
    ShadowReq request = {0};
    u32 i;
    bool initialized = false;
    bool ok = false;

    if (bytes == NULL)
        return false;
    (void)memset(bytes, 'x', AI_SHADOW_BIG_BYTES);
    for (i = 79U; i < AI_SHADOW_BIG_BYTES; i += 80U)
        bytes[i] = (u8)'\n';
    yew_ed_init(&ed);
    initialized = true;
    if (!yew_ed_open_memory(&ed, bytes, AI_SHADOW_BIG_BYTES,
                            "context-100m.c"))
        goto done;
    request.buf_id = ed.win->buf->id;
    request.buf_gen = ed.win->buf->tb->gen;
    request.pos = BYTEOFF(AI_SHADOW_BIG_BYTES);
    request.line = yew_textbuf_line_span(
        ed.win->buf->tb,
        yew_textbuf_line_of(ed.win->buf->tb, request.pos));
    request.seq = 1U;
    request.prov = (u8)YEW_SHADOW_AI;
    for (i = 0U; i < AI_SHADOW_CONTEXT_RUNS; i++) {
        Arena arena;
        AiCtx context;
        AiErr error;
        u64 started;

        arena_init(&arena);
        started = now_ns();
        if (!yew_ai_context_build(&ed, ed.win, &request, &arena,
                                  &context, &error)) {
            arena_free_all(&arena);
            goto done;
        }
        samples[i] = now_ns() - started;
        if (context.plen + context.slen > 4096U ||
            yew_utf8_validate(context.prefix, context.plen) != context.plen ||
            yew_utf8_validate(context.suffix, context.slen) != context.slen) {
            arena_free_all(&arena);
            goto done;
        }
        ai_shadow_sink ^= context.plen + context.slen;
        arena_free_all(&arena);
    }
    *p99_out = percentile(samples, AI_SHADOW_CONTEXT_RUNS, 99U);
    ok = *p99_out <= AI_SHADOW_CONTEXT_P99_NS;

done:
    if (initialized)
        yew_ed_free(&ed);
    free(bytes);
    (void)printf("ai.shadow.context_100m p99_ns=%llu budget_ns=%u%s\n",
                 (unsigned long long)*p99_out,
                 AI_SHADOW_CONTEXT_P99_NS, ok ? " ok" : " REGRESSION");
    return ok;
}

static bool measure_prompt(u64 *p99_out)
{
    static const u8 prefix[] = "int main(void) {\n    ";
    static const u8 suffix[] = "\n}\n";
    AiBackend backend = {0};
    AiCtx context = {0};
    u64 samples[AI_SHADOW_PROMPT_RUNS];
    u32 i;

    backend.kind = (u8)YEW_AI_OPENAI;
    backend.model = "qwen-coder";
    backend.max_tokens = 256;
    backend.temperature = 0.01;
    backend.stream = true;
    context.prefix = prefix;
    context.plen = sizeof(prefix) - 1U;
    context.suffix = suffix;
    context.slen = sizeof(suffix) - 1U;
    context.path = "src/main.c";
    context.lang = "c";
    context.line_1based = 2U;
    for (i = 0U; i < AI_SHADOW_PROMPT_RUNS; i++) {
        Bytebuf body;
        JsonW writer;
        u64 started;

        bytebuf_init(&body);
        yew_jsonw_init(&writer, &body);
        started = now_ns();
        yew_ai_prompt_build(&writer, &backend, &context);
        samples[i] = now_ns() - started;
        if (body.len == 0U || writer.depth != 0U ||
            writer.key_pending || !writer.root_written) {
            bytebuf_free(&body);
            return false;
        }
        ai_shadow_sink ^= body.data[i % body.len];
        bytebuf_free(&body);
    }
    *p99_out = percentile(samples, AI_SHADOW_PROMPT_RUNS, 99U);
    (void)printf("ai.shadow.prompt p99_ns=%llu budget_ns=%u%s\n",
                 (unsigned long long)*p99_out, AI_SHADOW_PROMPT_P99_NS,
                 *p99_out <= AI_SHADOW_PROMPT_P99_NS ? " ok" :
                                                       " REGRESSION");
    return *p99_out <= AI_SHADOW_PROMPT_P99_NS;
}

static void synthetic_call_begin(Ed *ed)
{
    AiCall *call = &ed->ai->call;

    (void)memset(call, 0, sizeof(*call));
    call->ed = ed;
    call->backend.name = "perf";
    call->active = true;
    call->live = true;
    call->seq = ed->win->shadow.seq_next[YEW_SHADOW_AI]++;
    call->buf_id = ed->win->buf->id;
    call->buf_gen = ed->win->buf->tb->gen;
    call->pos = BYTEOFF(0U);
    call->t_context = -1;
    call->t_prompt = -1;
    call->t_sent = -1;
    call->t_first_token = -1;
    call->t_done = -1;
    call->retry_after_ms = -1;
    arena_init(&call->arena);
    bytebuf_init(&call->raw);
    bytebuf_init(&call->text);
    bytebuf_init(&call->body);
    bytebuf_init(&call->response);
    bytebuf_init(&call->curl_config);
    bytebuf_init(&call->curl_err);
    yew_ai_adapter_state_init(&call->adapter);
}

static bool measure_keypress(u64 *p99_out)
{
    struct timespec delay = {0, 8333333L};
    u64 samples[AI_SHADOW_KEY_SAMPLES];
    u64 deliveries = 0U;
    Ed ed;
    u32 i;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, NULL, 0U, "ai-key-perf")) {
        yew_ed_free(&ed);
        return false;
    }
    synthetic_call_begin(&ed);
    for (i = 0U; i < 64U; i++)
        bytebuf_push_u8(&ed.ai->call.raw, (u8)'x');
    ed.ai->call.dirty = true;
    yew_ai_shadow_pump(&ed);
    for (i = 0U; i < AI_SHADOW_KEY_SAMPLES; i++) {
        AiCall *call = &ed.ai->call;
        Cursor *cursor = &ed.win->cs.curs.data[0];
        EditCtx edit;
        u8 token = (u8)'x';
        u64 started;

        bytebuf_push_u8(&call->raw, token);
        call->dirty = true;
        yew_ai_shadow_pump(&ed);
        if (!ed.win->shadow.live ||
            yew_shadow_revalidate(ed.win->buf->tb, &ed.win->shadow.sug,
                                  cursor->pos) < 0) {
            (void)fprintf(stderr,
                          "perf_ai_shadow: key stream invalid at %u active=%u live=%u seq=%u min=%u consumed=%u len=%u cursor=%llu\n",
                          i, call->active ? 1U : 0U,
                          ed.win->shadow.live ? 1U : 0U, call->seq,
                          ed.win->shadow.seq_min[YEW_SHADOW_AI],
                          ed.win->shadow.sug.consumed,
                          ed.win->shadow.sug.len,
                          (unsigned long long)cursor->pos.v);
            goto done;
        }
        edit = yew_ed_edit_ctx(&ed);
        started = now_ns();
        if (!yew_edit_insert(&edit, cursor->pos, &token, 1U)) {
            (void)fprintf(stderr,
                          "perf_ai_shadow: key insert failed at %u\n", i);
            goto done;
        }
        yew_ed_finish_edit(&ed, &edit);
        cursor->anchor = cursor->pos;
        samples[i] = now_ns() - started;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        delay.tv_sec = 0;
        delay.tv_nsec = 8333333L;
    }
    *p99_out = percentile(samples, AI_SHADOW_KEY_SAMPLES, 99U);
    ok = *p99_out <= AI_SHADOW_KEY_P99_NS && ed.ai->call.active &&
         ed.shadow_stats.delivered <= 4000U / 33U + 1U;

done:
    deliveries = ed.shadow_stats.delivered;
    if (ed.ai->call.active)
        yew_ai_call_abort(&ed, &ed.ai->call, YEW_AI_ERR_CANCELLED);
    yew_ed_free(&ed);
    (void)printf("ai.shadow.keypress_120tps p99_ns=%llu budget_ns=%u "
                 "deliveries=%llu max_deliveries=%u%s\n",
                 (unsigned long long)*p99_out, AI_SHADOW_KEY_P99_NS,
                 (unsigned long long)deliveries,
                 4000U / 33U + 1U,
                 ok ? " ok" : " REGRESSION");
    return ok;
}

int main(void)
{
    char state_root[] = "/tmp/yew-ai-perf-state-XXXXXX";
    char state_dir[sizeof(state_root) + sizeof("/yew")];
    char state_file[sizeof(state_dir) + sizeof("/ai_stats.fl")];
    char curl_root[] = "/tmp/yew-ai-perf-curl-XXXXXX";
    char curl_link[sizeof(curl_root) + sizeof("/curl")];
    u64 context_p99 = 0U;
    u64 prompt_p99 = 0U;
    u64 http_p95 = 0U;
    u64 curl_p95 = 0U;
    u64 key_p99 = 0U;
    bool ok;

    if (mkdtemp(state_root) == NULL || mkdtemp(curl_root) == NULL ||
        snprintf(curl_link, sizeof(curl_link), "%s/curl", curl_root) <= 0 ||
        symlink(YEW_TEST_MOCKCURL, curl_link) != 0 ||
        setenv("XDG_STATE_HOME", state_root, 1) != 0 ||
        setenv("PATH", curl_root, 1) != 0 ||
        setenv("YEW_AI_MOCK", "1", 1) != 0 ||
        setenv("YEW_TEST_AI_PERF_KEY", "local-test-key", 1) != 0)
        fail("cannot prepare hermetic mock environment");
    yew_ai_shadow_init(NULL);
    ok = measure_context(&context_p99) &&
         measure_prompt(&prompt_p99) &&
         measure_live_transport(false, &http_p95) &&
         measure_live_transport(true, &curl_p95) &&
         measure_keypress(&key_p99);
    (void)unlink(curl_link);
    (void)rmdir(curl_root);
    (void)snprintf(state_dir, sizeof(state_dir), "%s/yew", state_root);
    (void)snprintf(state_file, sizeof(state_file), "%s/ai_stats.fl",
                   state_dir);
    (void)unlink(state_file);
    (void)rmdir(state_dir);
    (void)rmdir(state_root);
    if (!ok)
        return 1;
    (void)printf("ai.shadow.gate context=%llu prompt=%llu http=%llu "
                 "curl=%llu key=%llu sink=%llu ok\n",
                 (unsigned long long)context_p99,
                 (unsigned long long)prompt_p99,
                 (unsigned long long)http_p95,
                 (unsigned long long)curl_p95,
                 (unsigned long long)key_p99,
                 (unsigned long long)ai_shadow_sink);
    return 0;
}
