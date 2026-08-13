#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ws/symwalk.h"

typedef struct SymWalkFix {
    char root[128];
    char *old_path;
} SymWalkFix;

static void sw_rm_rf(const char *path)
{
    char cmd[512];
    int rc;

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    rc = system(cmd);
    (void)rc;
}

static void sw_init(SymWalkFix *f)
{
    const char *path = getenv("PATH");

    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-symwalk-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    if (path != NULL) {
        f->old_path = yew_xmalloc(strlen(path) + 1U);
        (void)strcpy(f->old_path, path);
    }
}

static void sw_free(SymWalkFix *f)
{
    if (f->old_path != NULL)
        YEW_ASSERT_EQ_I64(setenv("PATH", f->old_path, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("PATH"), 0);
    free(f->old_path);
    sw_rm_rf(f->root);
}

static void sw_join(char *out, size_t cap, const SymWalkFix *f,
                    const char *rel)
{
    int n = snprintf(out, cap, "%s/%s", f->root, rel);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

static void sw_dir(const SymWalkFix *f, const char *rel)
{
    char path[512];

    sw_join(path, sizeof(path), f, rel);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void sw_file(const SymWalkFix *f, const char *rel, const char *text)
{
    char path[512];
    FILE *fp;

    sw_join(path, sizeof(path), f, rel);
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fputs(text, fp) < 0 ? -1 : 0, 0);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void sw_big_file(const SymWalkFix *f)
{
    char path[512];
    FILE *fp;

    sw_join(path, sizeof(path), f, "large.txt");
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fseek(fp, 5L * 1024L * 1024L, SEEK_SET), 0);
    YEW_ASSERT_EQ_I64(fputc('x', fp), 'x');
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void sw_long_line_file(const SymWalkFix *f)
{
    char path[512];
    static const char symbol[] = "pathological_symbol";
    FILE *fp;
    size_t i;

    sw_join(path, sizeof(path), f, "long.txt");
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(symbol, 1U, sizeof(symbol) - 1U, fp),
                      sizeof(symbol) - 1U);
    for (i = sizeof(symbol) - 1U;
         i <= (size_t)YEW_SYMWALK_MAX_LINE_BYTES; i++)
        YEW_ASSERT_EQ_I64(fputc('x', fp), 'x');
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void sw_symbol_file(const SymWalkFix *f)
{
    char path[512];
    FILE *fp;
    u32 i;

    sw_join(path, sizeof(path), f, "many.txt");
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    for (i = 0U; i < 10000U; i++)
        YEW_ASSERT(fprintf(fp, "symbol_%05u\n", i) > 0);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void sw_binary_file(const SymWalkFix *f)
{
    char path[512];
    static const u8 bytes[] = {'b', 'i', 'n', 0, 's', 'y', 'm'};
    FILE *fp;

    sw_join(path, sizeof(path), f, "binary.dat");
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, sizeof(bytes), fp), sizeof(bytes));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void sw_ed(Ed *ed, SymWalkFix *f)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, NULL, 0U, "symwalk-test"));
    ed->ws.dir = arena_strdup(&ed->arena, f->root);
}

static const SymEntry *sw_find(const Ed *ed, const char *name)
{
    size_t i;

    for (i = 0U; i < ed->ws.sym_ws.e.len; i++) {
        const SymEntry *entry = &ed->ws.sym_ws.e.data[i];
        const char *candidate = yew_intern_str(&ed->interner, entry->name);

        if (candidate != NULL && strcmp(candidate, name) == 0)
            return entry;
    }
    return NULL;
}

static u32 sw_file_entries(const Ed *ed, const char *suffix)
{
    size_t i;
    u32 n = 0U;

    for (i = 0U; i < ed->ws.sym_ws.e.len; i++) {
        const char *path = yew_intern_str(&ed->interner,
                                          ed->ws.sym_ws.e.data[i].file);
        size_t plen;
        size_t slen = strlen(suffix);

        if (path == NULL)
            continue;
        plen = strlen(path);
        if (plen >= slen && strcmp(path + plen - slen, suffix) == 0)
            n++;
    }
    return n;
}

static void sw_drive_job(Ed *ed)
{
    i64 deadline = yew_now_ms() + 30000;

    while (ed->ws.sym_walk.running && yew_now_ms() < deadline) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, n, 20);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_settle(ed);
        yew_symwalk_pump(ed, 0);
    }
    YEW_ASSERT(!ed->ws.sym_walk.running);
}

void test_symwalk_fallback_caps_skips_and_repeat_interning(void)
{
    SymWalkFix f;
    Ed ed;
    char name[32];
    char path[512];
    size_t first_interns;
    u32 i;

    sw_init(&f);
    sw_dir(&f, ".git");
    sw_dir(&f, "node_modules");
    sw_file(&f, "node_modules/noise.txt", "dependency_noise\n");
    sw_dir(&f, "ignored");
    sw_file(&f, ".gitignore", "ignored/\n");
    sw_file(&f, "ignored/kept.txt", "fallback_kept\n");
    for (i = 0U; i < 200U; i++) {
        (void)snprintf(name, sizeof(name), "file%03u.txt", i);
        sw_file(&f, name, "shared_symbol\n");
    }
    sw_big_file(&f);
    sw_long_line_file(&f);
    sw_binary_file(&f);
    sw_symbol_file(&f);
    sw_ed(&ed, &f);
    YEW_ASSERT_EQ_I64(setenv("PATH", "/definitely/no/git", 1), 0);

    yew_symwalk_start(&ed);
    YEW_ASSERT(ed.ws.sym_walk.running);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.job, 0U);
    yew_symwalk_pump(&ed, 0);
    YEW_ASSERT(!ed.ws.sym_walk.running);
    YEW_ASSERT(ed.ws.sym_walk.fallback_reported);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "workspace walk"));
    YEW_ASSERT_NOT_NULL(sw_find(&ed, "shared_symbol"));
    YEW_ASSERT_NOT_NULL(sw_find(&ed, "fallback_kept"));
    YEW_ASSERT_NULL(sw_find(&ed, "dependency_noise"));
    YEW_ASSERT_NULL(sw_find(&ed, "pathological_symbol"));
    YEW_ASSERT_EQ_U64(sw_file_entries(&ed, "/many.txt"), 4000U);
    YEW_ASSERT_NULL(sw_find(&ed, "symbol_04000"));
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.files_total, 206U);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.files_done, 206U);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.long_files_skipped, 1U);
    YEW_ASSERT(ed.ws.sym_walk.bytes_read < 5U * 1024U * 1024U);

    first_interns = yew_intern_count(&ed.interner);
    for (i = 0U; i < 9U; i++) {
        yew_symwalk_start(&ed);
        yew_symwalk_pump(&ed, 0);
        YEW_ASSERT_EQ_U64(yew_intern_count(&ed.interner), first_interns);
    }
    sw_join(path, sizeof(path), &f, "many.txt");
    YEW_ASSERT(access(path, F_OK) == 0);
    yew_ed_free(&ed);
    sw_free(&f);
}

void test_symwalk_stop_retires_exited_undrained_job(void)
{
    SymWalkFix f;
    Ed ed;
    YewJob *job;
    u32 job_id;
    i64 deadline;

    sw_init(&f);
    if (f.old_path == NULL) {
        sw_free(&f);
        return;
    }
    sw_dir(&f, ".git");
    sw_file(&f, "tracked.txt", "retired_job_symbol\n");
    sw_ed(&ed, &f);
    yew_symwalk_start(&ed);
    job_id = ed.ws.sym_walk.job;
    job = yew_job_find(&ed, job_id);
    YEW_ASSERT_NOT_NULL(job);
    YEW_ASSERT(yew_job_pending(job));

    /* Reproduce the narrow state in which waitpid observed exit before
     * stdout/stderr reached EOF.  Stop must retain ownership until both
     * pipes drain instead of forgetting the job. */
    job->state = YEW_JOB_EXITED;
    job->exit_code = 0;
    yew_symwalk_stop(&ed);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.retired_jobs.len, 1U);
    YEW_ASSERT_NOT_NULL(yew_job_find(&ed, job_id));

    deadline = yew_now_ms() + 30000;
    while (ed.ws.sym_walk.retired_jobs.len != 0U &&
           yew_now_ms() < deadline) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(&ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, n, 20);
        yew_job_pump(&ed, pfd, n);
        yew_job_reap(&ed);
        yew_job_settle(&ed);
        yew_symwalk_pump(&ed, 0);
    }
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.retired_jobs.len, 0U);
    YEW_ASSERT_NULL(yew_job_find(&ed, job_id));
    yew_ed_free(&ed);
    sw_free(&f);
}

void test_symwalk_git_discovery_honors_ignore_when_available(void)
{
    SymWalkFix f;
    Ed ed;
    char cmd[768];
    int rc;

    sw_init(&f);
    if (f.old_path == NULL) {
        sw_free(&f);
        return;
    }
    (void)snprintf(cmd, sizeof(cmd),
                   "git -C '%s' init -q && "
                   "printf 'ignored/\\n' > '%s/.gitignore'", f.root,
                   f.root);
    rc = system(cmd);
    if (rc != 0) {
        sw_free(&f);
        return;
    }
    sw_dir(&f, "ignored");
    sw_file(&f, "ignored/noise.txt", "ignored_symbol\n");
    sw_file(&f, "visible.txt", "visible_symbol\n");
    sw_ed(&ed, &f);

    yew_symwalk_start(&ed);
    YEW_ASSERT(ed.ws.sym_walk.job != 0U);
    sw_drive_job(&ed);
    YEW_ASSERT_NOT_NULL(sw_find(&ed, "visible_symbol"));
    YEW_ASSERT_NULL(sw_find(&ed, "ignored_symbol"));
    YEW_ASSERT(!ed.ws.sym_walk.fallback_reported);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.files_total, 2U);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.files_done, 2U);

    yew_ed_free(&ed);
    sw_free(&f);
}

void test_symwalk_skips_an_open_buffer_and_stops_cleanly(void)
{
    SymWalkFix f;
    Ed ed;
    char open_path[512];

    sw_init(&f);
    sw_file(&f, "open.txt", "open_buffer_symbol\n");
    sw_file(&f, "closed.txt", "closed_file_symbol\n");
    sw_ed(&ed, &f);
    sw_join(open_path, sizeof(open_path), &f, "open.txt");
    ed.buffer.path = arena_strdup(&ed.arena, open_path);

    yew_symwalk_start(&ed);
    yew_symwalk_pump(&ed, 1);
    while (ed.ws.sym_walk.running)
        yew_symwalk_pump(&ed, 1);
    YEW_ASSERT_NULL(sw_find(&ed, "open_buffer_symbol"));
    YEW_ASSERT_NOT_NULL(sw_find(&ed, "closed_file_symbol"));
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.files_done, 2U);
    yew_symwalk_stop(&ed);
    YEW_ASSERT(!ed.ws.sym_walk.running);
    YEW_ASSERT_EQ_U64(ed.ws.sym_walk.queue.len, 0U);

    yew_ed_free(&ed);
    sw_free(&f);
}
