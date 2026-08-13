#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/symwalk.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "syn/defs.h"
#include "ui/message.h"
#include "util/log.h"
#include "ws/symidx.h"

#define YEW_SYMWALK_HEADROOM_US 750

static i64 symwalk_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

static bool path_join(char out[PATH_MAX], const char *root, const char *rel)
{
    int n;

    if (root == NULL || rel == NULL)
        return false;
    n = snprintf(out, PATH_MAX, "%s/%s", root, rel);
    return n > 0 && n < PATH_MAX;
}

static bool path_has_component(const char *path, const char *component)
{
    size_t want = strlen(component);
    const char *at = path;

    while (*at != '\0') {
        const char *end = strchr(at, '/');
        size_t len = end == NULL ? strlen(at) : (size_t)(end - at);

        if (len == want && memcmp(at, component, want) == 0)
            return true;
        if (end == NULL)
            break;
        at = end + 1;
    }
    return false;
}

static bool fallback_skips(const char *rel)
{
    static const char *const skip[] = {
        ".git", "build", "node_modules", "target", ".venv", "dist"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(skip); i++) {
        if (path_has_component(rel, skip[i]))
            return true;
    }
    return false;
}

static bool repo_marker(const char *root)
{
    char marker[PATH_MAX];
    struct stat st;

    return path_join(marker, root, ".git") && lstat(marker, &st) == 0;
}

static bool executable_in_path(const char *name, char out[PATH_MAX])
{
    const char *path = getenv("PATH");
    const char *at;

    if (path == NULL)
        return false;
    at = path;
    for (;;) {
        const char *end = strchr(at, ':');
        size_t len = end == NULL ? strlen(at) : (size_t)(end - at);
        const char *dir = len == 0U ? "." : at;
        size_t dlen = len == 0U ? 1U : len;
        size_t nlen = strlen(name);

        if (dlen + 1U + nlen + 1U <= PATH_MAX) {
            (void)memcpy(out, dir, dlen);
            out[dlen] = '/';
            (void)memcpy(out + dlen + 1U, name, nlen + 1U);
            if (access(out, X_OK) == 0)
                return true;
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
    return false;
}

static void fallback_message(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;

    if (sw->fallback_reported)
        return;
    sw->fallback_reported = true;
    yew_msg(ed, YEW_MSG_INFO,
            "git discovery unavailable; using workspace walk");
}

static void fallback_begin(Ed *ed, bool report)
{
    SymWalk *sw = &ed->ws.sym_walk;
    WalkOpts opts = {0};

    if (report)
        fallback_message(ed);
    if (!sw->files_init) {
        yew_filelist_init(&sw->files);
        sw->files_init = true;
    }
    opts.hidden = true;
    opts.max_entries = YEW_SYMWALK_MAX_FILES;
    sw->walk = yew_walk_begin(yew_ws_root(ed), &opts, &sw->files);
    if (sw->walk == NULL) {
        sw->running = false;
        sw->discovery_done = true;
    }
}

static void queue_path(Ed *ed, const char *rel, bool apply_fallback_skip)
{
    SymWalk *sw = &ed->ws.sym_walk;
    char joined[PATH_MAX];
    char *canonical;
    u32 id;

    if (sw->queue.len >= YEW_SYMWALK_MAX_FILES) {
        sw->capped = true;
        return;
    }
    if ((apply_fallback_skip && fallback_skips(rel)) ||
        !path_join(joined, yew_ws_root(ed), rel))
        return;
    canonical = realpath(joined, NULL);
    id = yew_intern_cstr(&ed->interner,
                         canonical == NULL ? joined : canonical);
    free(canonical);
    Vec_SymPath_push(&sw->queue, id);
}

static void queue_filelist(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;
    size_t i;

    for (i = 0U; i < sw->files.paths.len && !sw->capped; i++)
        queue_path(ed, sw->files.paths.data[i], true);
    if (sw->files.truncated)
        sw->capped = true;
    sw->files_total = sw->queue.len;
    sw->discovery_done = true;
}

static bool parse_git_paths(Ed *ed, const Bytebuf *bytes)
{
    size_t at = 0U;

    while (at < bytes->len) {
        const u8 *nul = memchr(bytes->data + at, 0, bytes->len - at);
        size_t len;
        char rel[PATH_MAX];

        if (nul == NULL)
            return false;
        len = (size_t)(nul - (bytes->data + at));
        if (len != 0U && len < sizeof(rel)) {
            (void)memcpy(rel, bytes->data + at, len);
            rel[len] = '\0';
            queue_path(ed, rel, false);
        }
        at += len + 1U;
        if (ed->ws.sym_walk.capped)
            break;
    }
    ed->ws.sym_walk.files_total = ed->ws.sym_walk.queue.len;
    ed->ws.sym_walk.discovery_done = true;
    return true;
}

static void git_settle(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;
    YewJob *job = yew_job_find(ed, sw->job);
    bool ok;

    if (job == NULL) {
        sw->job = 0U;
        fallback_begin(ed, true);
        return;
    }
    if (yew_job_pending(job))
        return;
    ok = job->state == YEW_JOB_EXITED && job->exit_code == 0 &&
         !job->collect_capped && parse_git_paths(ed, &job->collect);
    yew_job_release(ed, job);
    sw->job = 0U;
    if (!ok) {
        Vec_SymPath_free(&sw->queue);
        sw->next = 0U;
        sw->files_total = 0U;
        sw->discovery_done = false;
        fallback_begin(ed, true);
    }
}

static void retired_jobs_reap(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;
    size_t read_at;
    size_t write_at = 0U;

    for (read_at = 0U; read_at < sw->retired_jobs.len; read_at++) {
        YewJob *job = yew_job_find(ed, sw->retired_jobs.data[read_at]);

        if (job == NULL)
            continue;
        if (yew_job_pending(job)) {
            sw->retired_jobs.data[write_at++] = job->id;
            continue;
        }
        yew_job_release(ed, job);
    }
    sw->retired_jobs.len = write_at;
}

static bool open_buffer_owns(const Ed *ed, const char *path)
{
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        const Buffer *buf = ed->ws.bufs[i];
        const char *owned;

        if (buf == NULL)
            continue;
        owned = buf->meta.realpath != NULL ? buf->meta.realpath : buf->path;
        if (owned != NULL && strcmp(owned, path) == 0)
            return true;
    }
    return false;
}

static bool read_whole(const char *path, size_t len, u8 **out)
{
    int fd;
    size_t at = 0U;
    u8 *bytes = yew_xmalloc(len == 0U ? 1U : len);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(bytes);
        return false;
    }
    while (at < len) {
        ssize_t got = read(fd, bytes + at, len - at);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            free(bytes);
            return false;
        }
        if (got == 0)
            break;
        at += (size_t)got;
    }
    (void)close(fd);
    if (at != len) {
        free(bytes);
        return false;
    }
    *out = bytes;
    return true;
}

static void scan_file_end(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;
    Buffer *buf = sw->scan_buf;
    SynBuf *scratch;

    if (buf == NULL)
        return;
    scratch = sw->scratch_syn;
    if (scratch == NULL)
        YEW_BUG("symbol walk: active scan has no syntax scratch");
    *scratch = buf->syn;
    yew_syn_buf_init(&buf->syn);
    yew_textbuf_free(buf->tb);
    free(buf);
    sw->scan_buf = NULL;
    sw->scan_line = 0U;
    sw->scan_symbols = 0U;
}

static bool scan_file_begin(Ed *ed, const char *path)
{
    SymWalk *sw = &ed->ws.sym_walk;
    struct stat st;
    u8 *bytes;
    Buffer *buf;
    SynBuf *scratch;

    if (open_buffer_owns(ed, path) || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (u64)st.st_size > YEW_SYMWALK_MAX_FILE_BYTES)
        return false;
    if (!read_whole(path, (size_t)st.st_size, &bytes))
        return false;
    if (memchr(bytes, 0, (size_t)st.st_size < 8192U ?
                       (size_t)st.st_size : 8192U) != NULL) {
        free(bytes);
        return false;
    }
    buf = yew_xcalloc(1U, sizeof(*buf));
    buf->owner = ed;
    buf->path = (char *)path;
    buf->tb = yew_textbuf_from_bytes(bytes, (size_t)st.st_size);
    free(bytes);
    if (sw->scratch_syn == NULL) {
        sw->scratch_syn = yew_xcalloc(1U, sizeof(SynBuf));
        yew_syn_buf_init(sw->scratch_syn);
    }
    scratch = sw->scratch_syn;
    buf->syn = *scratch;
    (void)memset(scratch, 0, sizeof(*scratch));
    yew_ed_syn_bind(buf);
    sw->scan_buf = buf;
    sw->scan_line = 0U;
    sw->scan_symbols = 0U;
    sw->bytes_read += (u64)st.st_size;
    return true;
}

static void scan_file_chunk(Ed *ed)
{
    SymWalk *sw = &ed->ws.sym_walk;
    Buffer *buf = sw->scan_buf;
    u64 lines;
    u64 last;
    Span first_span;
    Span last_span;
    u32 remaining;

    if (buf == NULL)
        return;
    lines = yew_textbuf_line_count(buf->tb);
    last = sw->scan_line + YEW_SYMWALK_SCAN_LINES;
    if (last > lines)
        last = lines;
    first_span = yew_textbuf_line_span(buf->tb, LINENO(sw->scan_line));
    last_span = yew_textbuf_line_span(buf->tb, LINENO(last - 1U));
    remaining = YEW_SYMWALK_MAX_SYMS_PER_FILE - sw->scan_symbols;
    ed->ws.sym_ws.scan_limit = remaining;
    sw->scan_symbols += yew_symidx_scan_workspace(
        &ed->ws.sym_ws, buf, (Span){first_span.lo, last_span.hi});
    ed->ws.sym_ws.scan_limit = 0U;
    sw->scan_line = last;
    if (last == lines ||
        sw->scan_symbols >= YEW_SYMWALK_MAX_SYMS_PER_FILE)
        scan_file_end(ed);
}

void yew_symwalk_start(Ed *ed)
{
    SymWalk *sw;
    Vec_SymPath retired;
    void *scratch_syn;
    char git[PATH_MAX];
    char err[160];
    char *argv[7];
    YewJobSpec spec = {0};

    if (ed == NULL)
        return;
    yew_symwalk_stop(ed);
    sw = &ed->ws.sym_walk;
    retired = sw->retired_jobs;
    scratch_syn = sw->scratch_syn;
    (void)memset(sw, 0, sizeof(*sw));
    sw->retired_jobs = retired;
    sw->scratch_syn = scratch_syn;
    sw->running = true;
    yew_symidx_clear(&ed->ws.sym_ws);

    if (!repo_marker(yew_ws_root(ed))) {
        fallback_begin(ed, false);
        return;
    }
    if (!executable_in_path("git", git)) {
        fallback_begin(ed, true);
        return;
    }
    argv[0] = git;
    argv[1] = (char *)"ls-files";
    argv[2] = (char *)"-z";
    argv[3] = (char *)"--cached";
    argv[4] = (char *)"--others";
    argv[5] = (char *)"--exclude-standard";
    argv[6] = NULL;
    spec.argv = argv;
    spec.cwd = yew_ws_root(ed);
    spec.sink = YEW_SINK_COLLECT;
    spec.display = "symbol discovery";
    sw->job = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (sw->job == 0U)
        fallback_begin(ed, true);
}

void yew_symwalk_pump(Ed *ed, i64 budget_us)
{
    SymWalk *sw;
    i64 started;

    if (ed == NULL)
        return;
    sw = &ed->ws.sym_walk;
    retired_jobs_reap(ed);
    if (!sw->running)
        return;
    started = symwalk_now_us();
    if (sw->job != 0U) {
        git_settle(ed);
        if (sw->job != 0U || !sw->running)
            return;
    }
    if (sw->walk != NULL) {
        i64 remaining = budget_us;

        if (budget_us > 0)
            remaining -= symwalk_now_us() - started;
        /* Both the directory walker and a line chunk check their clocks
         * cooperatively.  Leave one worst-case chunk of headroom so their
         * final unit of work cannot consume the event-loop deadline. */
        if (remaining > YEW_SYMWALK_HEADROOM_US)
            remaining -= YEW_SYMWALK_HEADROOM_US;
        if (remaining < 1 && budget_us > 0)
            return;
        if (yew_walk_step(sw->walk, budget_us == 0 ? 0 : remaining))
            return;
        yew_walk_end(sw->walk);
        sw->walk = NULL;
        queue_filelist(ed);
        if (budget_us > 0 &&
            symwalk_now_us() - started >=
                (budget_us > YEW_SYMWALK_HEADROOM_US ?
                    budget_us - YEW_SYMWALK_HEADROOM_US : 0))
            return;
    }
    while (sw->discovery_done &&
           (sw->scan_buf != NULL || sw->next < sw->queue.len) &&
           !ed->ws.sym_ws.capped) {
        if (sw->scan_buf == NULL) {
            const char *path = yew_intern_str(
                &ed->interner, sw->queue.data[sw->next++]);

            if (path == NULL || !scan_file_begin(ed, path)) {
                sw->files_done++;
                continue;
            }
        }
        scan_file_chunk(ed);
        if (sw->scan_buf == NULL)
            sw->files_done++;
        if (budget_us > 0 &&
            symwalk_now_us() - started >=
                (budget_us > YEW_SYMWALK_HEADROOM_US ?
                    budget_us - YEW_SYMWALK_HEADROOM_US : 0))
            break;
    }
    if (ed->ws.sym_ws.capped) {
        scan_file_end(ed);
        sw->capped = true;
        sw->running = false;
    } else if (sw->scan_buf == NULL && sw->next >= sw->queue.len) {
        sw->running = false;
    }
}

void yew_symwalk_stop(Ed *ed)
{
    SymWalk *sw;
    YewJob *job;

    if (ed == NULL)
        return;
    sw = &ed->ws.sym_walk;
    job = yew_job_find(ed, sw->job);
    if (job != NULL) {
        if (yew_job_pending(job)) {
            if (yew_job_signal(ed, job->id, SIGTERM))
                Vec_SymPath_push(&sw->retired_jobs, job->id);
        } else {
            yew_job_release(ed, job);
        }
    }
    if (sw->walk != NULL)
        yew_walk_end(sw->walk);
    scan_file_end(ed);
    if (sw->files_init)
        yew_filelist_free(&sw->files);
    Vec_SymPath_free(&sw->queue);
    sw->job = 0U;
    sw->walk = NULL;
    sw->next = 0U;
    sw->files_done = 0U;
    sw->files_total = 0U;
    sw->bytes_read = 0U;
    sw->running = false;
    sw->capped = false;
    sw->files_init = false;
    sw->discovery_done = false;
    sw->fallback_reported = false;
}

void yew_symwalk_dispose(Ed *ed)
{
    if (ed == NULL)
        return;
    yew_symwalk_stop(ed);
    Vec_SymPath_free(&ed->ws.sym_walk.retired_jobs);
    if (ed->ws.sym_walk.scratch_syn != NULL) {
        yew_syn_detach(ed->ws.sym_walk.scratch_syn);
        free(ed->ws.sym_walk.scratch_syn);
        ed->ws.sym_walk.scratch_syn = NULL;
    }
}
