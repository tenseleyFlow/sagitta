#define _POSIX_C_SOURCE 200809L

/* Sprint 51: 20,000 porcelain-v2 entries must parse inside one key budget. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"
#include "util/base.h"

enum {
    GIT_STATUS_ENTRIES = 20000,
    GIT_STATUS_SAMPLES = 9,
    GIT_STATUS_BUDGET_NS = 8000000,
    GIT_TYPING_SAMPLES = 5000,
    GIT_KEYPRESS_BUDGET_NS = 5000000,
    GIT_FAKE_MAX = 128,
    GIT_REAL_RUN_MS = 2500,
    GIT_REAL_SAMPLE_MAX = 8192
};

static volatile u64 git_status_sink;
static u64 arena_allocation_calls;
static u64 heap_allocation_calls;
static u64 outside_arena_calls;

#if defined(__linux__)
static bool allocation_probe_active;
static u32 arena_allocation_depth;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
void *__real_arena_alloc(Arena *arena, size_t size, size_t align);

void *__wrap_malloc(size_t size)
{
    if (allocation_probe_active) {
        heap_allocation_calls++;
        if (arena_allocation_depth == 0U)
            outside_arena_calls++;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (allocation_probe_active) {
        heap_allocation_calls++;
        if (arena_allocation_depth == 0U)
            outside_arena_calls++;
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (allocation_probe_active) {
        heap_allocation_calls++;
        if (arena_allocation_depth == 0U)
            outside_arena_calls++;
    }
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
    if (allocation_probe_active && arena_allocation_depth == 0U)
        outside_arena_calls++;
    __real_free(ptr);
}

void *__wrap_arena_alloc(Arena *arena, size_t size, size_t align)
{
    void *result;

    if (allocation_probe_active) {
        arena_allocation_calls++;
        arena_allocation_depth++;
    }
    result = __real_arena_alloc(arena, size, align);
    if (allocation_probe_active)
        arena_allocation_depth--;
    return result;
}

static void allocation_probe_begin(void)
{
    arena_allocation_depth = 0U;
    arena_allocation_calls = 0U;
    heap_allocation_calls = 0U;
    outside_arena_calls = 0U;
    allocation_probe_active = true;
}

static bool allocation_probe_end(void)
{
    allocation_probe_active = false;
    return arena_allocation_depth == 0U && arena_allocation_calls != 0U &&
           heap_allocation_calls != 0U && outside_arena_calls == 0U;
}
#else
static void allocation_probe_begin(void) { }
static bool allocation_probe_end(void) { return true; }
#endif

static void snapshot_begin(GitSnapshot *snap)
{
    (void)memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
}

static void snapshot_end(GitSnapshot *snap)
{
    arena_free_all(&snap->a);
}

static u64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "perf_git_status: clock_gettime: %s\n",
                      strerror(errno));
        return 0U;
    }
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static void sort_u64(u64 *values, size_t len)
{
    size_t i;

    for (i = 1U; i < len; i++) {
        u64 value = values[i];
        size_t at = i;

        while (at > 0U && values[at - 1U] > value) {
            values[at] = values[at - 1U];
            at--;
        }
        values[at] = value;
    }
}

static bool build_fixture(u8 **out, size_t *out_len)
{
    static const char prefix[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0";
    static const char row_fmt[] =
        "1 .M N... 100644 100644 100644 "
        "0123456789012345678901234567890123456789 "
        "abcdefabcdefabcdefabcdefabcdefabcdefabcd file-%05u.c";
    const size_t row_cap = sizeof(row_fmt) + 16U;
    size_t cap = sizeof(prefix) - 1U + row_cap * GIT_STATUS_ENTRIES;
    u8 *buf = malloc(cap);
    size_t at = 0U;
    u32 i;

    if (buf == NULL)
        return false;
    (void)memcpy(buf, prefix, sizeof(prefix) - 1U);
    at += sizeof(prefix) - 1U;
    for (i = 0U; i < GIT_STATUS_ENTRIES; i++) {
        int wrote = snprintf((char *)buf + at, cap - at, row_fmt, i);

        if (wrote <= 0 || (size_t)wrote + 1U > cap - at) {
            free(buf);
            return false;
        }
        at += (size_t)wrote + 1U;
    }
    *out = buf;
    *out_len = at;
    return true;
}

/* Keep a source-level guard as a second line of defence beside the Linux
 * link-time allocation probe above. */
static bool parser_uses_heap(const char **offender)
{
    static const char *const banned[] = {
        "malloc(", "calloc(", "realloc(", "free(",
        "yew_xmalloc", "yew_xcalloc", "yew_xrealloc", "yew_sort_stable"
    };
    const char *path = getenv("YEW_PERF_GIT_STATUS_SRC");
    FILE *file = fopen(path != NULL ? path : "src/mod/git/porcelain.c", "r");
    char line[512];

    if (file == NULL) {
        *offender = "unreadable parser source";
        return true;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t i;

        for (i = 0U; i < YEW_ARRAY_LEN(banned); i++) {
            if (strstr(line, banned[i]) != NULL) {
                *offender = banned[i];
                (void)fclose(file);
                return true;
            }
        }
    }
    if (ferror(file)) {
        *offender = "parser source read error";
        (void)fclose(file);
        return true;
    }
    (void)fclose(file);
    return false;
}

static bool parse_once(const u8 *buf, size_t len, u64 *elapsed)
{
    GitSnapshot snap;
    GitParseErr err;
    u64 started;
    bool ok;

    (void)memset(&err, 0, sizeof(err));
    snapshot_begin(&snap);
    allocation_probe_begin();
    started = now_ns();
    ok = yew_git_parse_status(&snap, buf, (u64)len, &err);
    *elapsed = now_ns() - started;
    if (!allocation_probe_end()) {
        (void)fprintf(stderr,
                      "perf_git_status: runtime allocation escaped arena "
                      "(arena=%llu heap=%llu outside=%llu)\n",
                      (unsigned long long)arena_allocation_calls,
                      (unsigned long long)heap_allocation_calls,
                      (unsigned long long)outside_arena_calls);
        snapshot_end(&snap);
        return false;
    }
    if (!ok || snap.entries.len != GIT_STATUS_ENTRIES) {
        (void)fprintf(stderr,
                      "perf_git_status: parse failed at %llu: %s (%zu entries)\n",
                      (unsigned long long)err.off, err.message,
                      snap.entries.len);
        snapshot_end(&snap);
        return false;
    }
    git_status_sink ^= (u64)snap.entries.len;
    git_status_sink ^= (u8)snap.entries.data[0].path[0];
    git_status_sink ^= (u8)snap.entries.data[snap.entries.len - 1U].path[0];
    snapshot_end(&snap);
    return true;
}

typedef struct FakeGitEvent {
    u32 id;
    GitReqKind kind;
} FakeGitEvent;

typedef struct FakeGit {
    FakeGitEvent events[GIT_FAKE_MAX];
    size_t read_at;
    size_t len;
    u32 next_id;
    u32 status_spawns;
    u32 spawns[YEW_GREQ_VERB + 1U];
    u32 completions[YEW_GREQ_VERB + 1U];
} FakeGit;

static u32 fake_git_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                          const GitReq *req, void *opaque,
                          char *err, size_t errsz)
{
    FakeGit *fake = opaque;
    u32 id;

    (void)ed;
    (void)verb;
    (void)argv;
    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (fake->read_at != 0U) {
        size_t pending = fake->len - fake->read_at;

        (void)memmove(fake->events, fake->events + fake->read_at,
                      pending * sizeof(fake->events[0]));
        fake->read_at = 0U;
        fake->len = pending;
    }
    if (req == NULL || fake->len == YEW_ARRAY_LEN(fake->events))
        return 0U;
    id = ++fake->next_id;
    fake->events[fake->len++] = (FakeGitEvent){id, req->kind};
    fake->spawns[req->kind]++;
    if (req->kind == YEW_GREQ_STATUS)
        fake->status_spawns++;
    return id;
}

static bool fake_git_complete_one(Ed *ed, FakeGit *fake)
{
    static const u8 version[] = "git version 2.45.0\n";
    static const u8 detect[] =
        "/tmp/yew-perf-git/.git\ntrue\nfalse\n/tmp/yew-perf-git\n";
    static const u8 upstream[] =
        "0123456789012345678901234567890123456789\n";
    static const u8 comment_char[] = "#\n";
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0"
        "# branch.upstream origin/trunk\0"
        "# branch.ab +0 -1\0"
        "1 .M N... 100644 100644 100644 "
        "0123456789012345678901234567890123456789 "
        "abcdefabcdefabcdefabcdefabcdefabcdefabcd fixture.c\0";
    const u8 *out = NULL;
    size_t out_len = 0U;
    FakeGitEvent event;

    if (fake->read_at == fake->len)
        return true;
    event = fake->events[fake->read_at++];
    fake->completions[event.kind]++;
    switch (event.kind) {
    case YEW_GREQ_VERSION:
        out = version;
        out_len = sizeof(version) - 1U;
        break;
    case YEW_GREQ_DETECT:
        out = detect;
        out_len = sizeof(detect) - 1U;
        break;
    case YEW_GREQ_STATUS:
        out = status;
        out_len = sizeof(status) - 1U;
        break;
    case YEW_GREQ_IGNORE:
        out = (const u8 *)"";
        out_len = 0U;
        break;
    case YEW_GREQ_UPSTREAM:
        out = upstream;
        out_len = sizeof(upstream) - 1U;
        break;
    case YEW_GREQ_INCOMING:
        out = (const u8 *)"";
        out_len = 0U;
        break;
    case YEW_GREQ_VERB:
        out = comment_char;
        out_len = sizeof(comment_char) - 1U;
        break;
    default:
        return false;
    }
    return yew_git_test_complete(ed, event.id, YEW_GIT_OK,
                                 out, (u64)out_len, NULL, 0U);
}

static bool measure_ttl_typing(u64 *p99_out, u32 *refreshes_out)
{
    Ed ed;
    FakeGit fake = {0};
    u64 *samples = malloc(GIT_TYPING_SAMPLES * sizeof(*samples));
    size_t i;
    bool ok = false;

    if (samples == NULL)
        return false;
    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed))
        goto done;
    yew_git_test_spawn_set(fake_git_spawn, &fake);
    for (i = 0U; i < GIT_TYPING_SAMPLES; i++) {
        Key key = {0};
        u64 started;

        yew_git_test_now_set(&ed, (i64)i);
        key.code = (i & 1U) == 0U ? YEW_KEY_RIGHT : YEW_KEY_LEFT;
        key.ev = YEW_KEY_PRESS;
        started = now_ns();
        (void)yew_git_snapshot(&ed);
        if (!fake_git_complete_one(&ed, &fake))
            goto done_spawn;
        yew_dispatch_key(&ed, key, (i64)i);
        samples[i] = now_ns() - started;
        git_status_sink ^= ed.dispatch_count;
    }
    while (fake.read_at != fake.len)
        if (!fake_git_complete_one(&ed, &fake))
            goto done_spawn;
    sort_u64(samples, GIT_TYPING_SAMPLES);
    *p99_out = samples[(GIT_TYPING_SAMPLES * 99U + 99U) / 100U - 1U];
    *refreshes_out = fake.status_spawns;
    ok = fake.status_spawns >= 9U && fake.status_spawns <= 11U &&
         yew_git_snapshot(&ed) != NULL &&
         yew_git_snapshot(&ed)->gen >= 9U;
    if (!ok) {
        const GitSnapshot *snap = yew_git_snapshot(&ed);

        (void)fprintf(stderr,
                      "perf_git_status: fake TTL cadence statuses=%u "
                      "generation=%u queued=%zu/%zu version=%u/%u "
                      "detect=%u/%u ignore=%u/%u upstream=%u/%u "
                      "incoming=%u/%u comment=%u/%u\n",
                      fake.status_spawns, snap == NULL ? 0U : snap->gen,
                      fake.read_at, fake.len,
                      fake.completions[YEW_GREQ_VERSION],
                      fake.spawns[YEW_GREQ_VERSION],
                      fake.completions[YEW_GREQ_DETECT],
                      fake.spawns[YEW_GREQ_DETECT],
                      fake.completions[YEW_GREQ_IGNORE],
                      fake.spawns[YEW_GREQ_IGNORE],
                      fake.completions[YEW_GREQ_UPSTREAM],
                      fake.spawns[YEW_GREQ_UPSTREAM],
                      fake.completions[YEW_GREQ_INCOMING],
                      fake.spawns[YEW_GREQ_INCOMING],
                      fake.completions[YEW_GREQ_VERB],
                      fake.spawns[YEW_GREQ_VERB]);
    }
done_spawn:
    yew_git_test_spawn_set(NULL, NULL);
done:
    yew_ed_free(&ed);
    free(samples);
    return ok;
}

static bool run_child(char *const *argv)
{
    pid_t child = fork();
    pid_t waited;
    int status;

    if (child < 0)
        return false;
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char *real_fixture_create(char **parent_out)
{
    const char *base = getenv("TMPDIR");
    static const char suffix[] = "/yew-perf-git-XXXXXX";
    size_t base_len;
    bool slash;
    char *parent;
    char *repo;
    char *argv[4];

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    base_len = strlen(base);
    slash = base_len != 0U && base[base_len - 1U] == '/';
    if (base_len > SIZE_MAX - sizeof(suffix))
        return NULL;
    parent = malloc(base_len + sizeof(suffix) - (slash ? 1U : 0U));
    if (parent == NULL)
        return NULL;
    (void)memcpy(parent, base, base_len);
    (void)memcpy(parent + base_len, suffix + (slash ? 1U : 0U),
                 sizeof(suffix) - (slash ? 1U : 0U));
    if (mkdtemp(parent) == NULL) {
        free(parent);
        return NULL;
    }
    base_len = strlen(parent);
    if (base_len > SIZE_MAX - sizeof("/repo"))
        goto fail;
    repo = malloc(base_len + sizeof("/repo"));
    if (repo == NULL)
        goto fail;
    (void)snprintf(repo, base_len + sizeof("/repo"), "%s/repo", parent);
    argv[0] = (char *)"tests/fixtures/git/mkrepo.sh";
    argv[1] = repo;
    argv[2] = NULL;
    argv[3] = NULL;
    if (!run_child(argv)) {
        free(repo);
        goto fail;
    }
    *parent_out = parent;
    return repo;
fail:
    {
        char *remove_argv[] = {(char *)"rm", (char *)"-rf",
                               (char *)"--", parent, NULL};
        (void)run_child(remove_argv);
    }
    free(parent);
    return NULL;
}

static void real_fixture_drop(char *parent)
{
    char *argv[] = {(char *)"rm", (char *)"-rf", (char *)"--",
                    parent, NULL};

    if (parent != NULL)
        (void)run_child(argv);
    free(parent);
}

static bool jobs_have_events(const struct pollfd *pfd, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++)
        if (pfd[i].revents != 0)
            return true;
    return false;
}

static bool measure_real_ttl_typing(u64 *p99_out, u64 *boundary_max_out,
                                    u32 *refreshes_out, size_t *samples_out,
                                    size_t *boundaries_out)
{
    char *parent = NULL;
    char *repo = real_fixture_create(&parent);
    char *saved_cwd = NULL;
    u64 *samples = NULL;
    u64 *boundaries = NULL;
    size_t sample_len = 0U;
    size_t boundary_len = 0U;
    Ed ed;
    bool ed_ready = false;
    bool ok = false;
    i64 started_ms;
    i64 deadline_ms;
    u32 last_gen = 0U;

    if (repo == NULL)
        return false;
    {
        size_t n = strlen(repo) + sizeof("/.git");
        char *git_dir = malloc(n);

        if (git_dir == NULL)
            goto done;
        (void)snprintf(git_dir, n, "%s/.git", repo);
        if (stat(git_dir, &(struct stat){0}) != 0) {
            free(git_dir);
            (void)fputs("git.status real fixture skipped (git unavailable)\n",
                        stdout);
            ok = true;
            goto done;
        }
        free(git_dir);
    }
    samples = malloc(GIT_REAL_SAMPLE_MAX * sizeof(*samples));
    boundaries = malloc(GIT_REAL_SAMPLE_MAX * sizeof(*boundaries));
    saved_cwd = getcwd(NULL, 0U);
    if (samples == NULL || boundaries == NULL || saved_cwd == NULL ||
        chdir(repo) != 0)
        goto done;
    yew_ed_init(&ed);
    ed_ready = true;
    if (!yew_ed_open_scratch(&ed))
        goto done;
    started_ms = yew_now_ms();
    deadline_ms = started_ms + GIT_REAL_RUN_MS;
    while (yew_now_ms() < deadline_ms) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        const GitSnapshot *after_snap;
        u32 nfds = 0U;
        u32 before_jobs;
        u64 started;
        u64 elapsed;
        bool boundary;
        Key key = {0};
        i64 now = yew_now_ms();

        if (sample_len == GIT_REAL_SAMPLE_MAX)
            goto done;
        ed.now_ms = now;
        yew_job_collect_fds(&ed, pfd, &nfds);
        if (poll(pfd, (nfds_t)nfds, 1) < 0 && errno != EINTR)
            goto done;
        before_jobs = ed.jobs.len;
        key.code = (sample_len & 1U) == 0U ? YEW_KEY_RIGHT : YEW_KEY_LEFT;
        key.ev = YEW_KEY_PRESS;
        started = now_ns();
        (void)yew_git_snapshot(&ed);
        yew_job_pump(&ed, pfd, nfds);
        yew_job_reap(&ed);
        yew_job_tick(&ed, now);
        yew_job_settle(&ed);
        yew_dispatch_key(&ed, key, now);
        elapsed = now_ns() - started;
        after_snap = yew_git_snapshot(&ed);
        boundary = jobs_have_events(pfd, nfds) ||
                   before_jobs != ed.jobs.len ||
                   (after_snap != NULL && last_gen != after_snap->gen);
        if (after_snap != NULL)
            last_gen = after_snap->gen;
        samples[sample_len++] = elapsed;
        if (boundary)
            boundaries[boundary_len++] = elapsed;
        git_status_sink ^= ed.dispatch_count;
    }
    {
        i64 drain_deadline = yew_now_ms() + 10000;

        while (ed.jobs.len != 0U && yew_now_ms() < drain_deadline) {
            struct pollfd pfd[YEW_JOB_MAX * 4U];
            u32 nfds = 0U;

            yew_job_collect_fds(&ed, pfd, &nfds);
            if (poll(pfd, (nfds_t)nfds, 5) < 0 && errno != EINTR)
                goto done;
            yew_job_pump(&ed, pfd, nfds);
            yew_job_reap(&ed);
            yew_job_tick(&ed, yew_now_ms());
            yew_job_settle(&ed);
        }
    }
    if (ed.jobs.len != 0U || sample_len == 0U || boundary_len == 0U)
        goto done;
    sort_u64(samples, sample_len);
    sort_u64(boundaries, boundary_len);
    *p99_out = samples[(sample_len * 99U + 99U) / 100U - 1U];
    *boundary_max_out = boundaries[boundary_len - 1U];
    *refreshes_out = yew_git_snapshot(&ed)->gen;
    *samples_out = sample_len;
    *boundaries_out = boundary_len;
    ok = *refreshes_out >= 4U;
done:
    if (ed_ready)
        yew_ed_free(&ed);
    if (saved_cwd != NULL) {
        if (chdir(saved_cwd) != 0)
            ok = false;
        free(saved_cwd);
    }
    free(boundaries);
    free(samples);
    free(repo);
    real_fixture_drop(parent);
    return ok;
}

int main(void)
{
    u8 *fixture = NULL;
    size_t fixture_len = 0U;
    u64 samples[GIT_STATUS_SAMPLES];
    u64 warmup;
    u64 median;
    u64 keypress_p99 = 0U;
    u64 real_keypress_p99 = 0U;
    u64 real_boundary_max = 0U;
    u32 refreshes = 0U;
    u32 real_refreshes = 0U;
    size_t real_samples = 0U;
    size_t real_boundaries = 0U;
    const char *offender = NULL;
    size_t i;
    int status = 0;

    if (!build_fixture(&fixture, &fixture_len)) {
        (void)fputs("perf_git_status: could not build fixture\n", stderr);
        return 2;
    }
    if (!parse_once(fixture, fixture_len, &warmup)) {
        free(fixture);
        return 1;
    }
    for (i = 0U; i < GIT_STATUS_SAMPLES; i++) {
        if (!parse_once(fixture, fixture_len, &samples[i])) {
            free(fixture);
            return 1;
        }
    }
    sort_u64(samples, YEW_ARRAY_LEN(samples));
    median = samples[YEW_ARRAY_LEN(samples) / 2U];
    (void)printf("git.status parse 20000  %.3f ms (limit 8.000 ms)\n",
                 (double)median / 1000000.0);
#if defined(__linux__)
    (void)printf("git.status allocations arena=%llu heap=%llu outside=%llu\n",
                 (unsigned long long)arena_allocation_calls,
                 (unsigned long long)heap_allocation_calls,
                 (unsigned long long)outside_arena_calls);
#endif
    if (median > GIT_STATUS_BUDGET_NS) {
        (void)fprintf(stderr,
                      "perf_git_status: median %.3f ms exceeds 8.000 ms\n",
                      (double)median / 1000000.0);
        status = 1;
    }
    if (parser_uses_heap(&offender)) {
        (void)fprintf(stderr,
                      "perf_git_status: parser allocates outside arena: %s\n",
                      offender);
        status = 1;
    }
    if (!measure_ttl_typing(&keypress_p99, &refreshes)) {
        (void)fputs("perf_git_status: TTL typing fixture failed\n", stderr);
        status = 1;
    } else {
        (void)printf("git.status ttl500 keypress_p99 %.3f ms "
                     "(limit 5.000 ms, %u refreshes)\n",
                     (double)keypress_p99 / 1000000.0, refreshes);
        if (keypress_p99 > GIT_KEYPRESS_BUDGET_NS) {
            (void)fprintf(stderr,
                          "perf_git_status: keypress p99 %.3f ms exceeds "
                          "5.000 ms\n",
                          (double)keypress_p99 / 1000000.0);
            status = 1;
        }
    }
    if (!measure_real_ttl_typing(&real_keypress_p99, &real_boundary_max,
                                 &real_refreshes, &real_samples,
                                 &real_boundaries)) {
        (void)fputs("perf_git_status: real TTL typing fixture failed\n",
                    stderr);
        status = 1;
    } else if (real_samples != 0U) {
        (void)printf("git.status real ttl500 keypress_p99 %.3f ms "
                     "boundary_max %.3f ms (limit 5.000 ms, "
                     "%u refreshes, %zu/%zu boundaries)\n",
                     (double)real_keypress_p99 / 1000000.0,
                     (double)real_boundary_max / 1000000.0,
                     real_refreshes, real_boundaries, real_samples);
        if (real_keypress_p99 > GIT_KEYPRESS_BUDGET_NS ||
            real_boundary_max > GIT_KEYPRESS_BUDGET_NS) {
            (void)fputs("perf_git_status: real TTL typing exceeds "
                        "5.000 ms\n", stderr);
            status = 1;
        }
    }
    free(fixture);
    return status;
}
