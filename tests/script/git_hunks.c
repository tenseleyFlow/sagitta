#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "mod/git/editor.h"
#include "mod/git/git.h"
#include "mod/git/gutter.h"
#include "text/edit.h"
#include "text/undo.h"

typedef struct Bytes {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

static unsigned assertions;
static unsigned failures;
static unsigned patches_checked;

static const GitEntry *find_git_entry(const GitSnapshot *snapshot,
                                      const char *path);

static bool isolate_git_environment(void)
{
    static const char *const inherited[] = {
        "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_NAMESPACE", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS",
        "GIT_CONFIG_COUNT", "GIT_DEFAULT_HASH", "GIT_DEFAULT_REF_FORMAT",
        "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_DATE",
        "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", "GIT_COMMITTER_DATE",
        "GIT_TEMPLATE_DIR"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(inherited); i++)
        if (unsetenv(inherited[i]) != 0)
            return false;
    return setenv("GIT_CONFIG_NOSYSTEM", "1", 1) == 0 &&
           setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1) == 0 &&
           setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1) == 0;
}

#define CHECK(expr) do {                                                   \
    assertions++;                                                          \
    if (!(expr)) {                                                         \
        (void)fprintf(stderr, "git_hunks:%d: check failed: %s\n",         \
                      __LINE__, #expr);                                    \
        failures++;                                                        \
    }                                                                      \
} while (0)

static bool bytes_append(Bytes *out, const u8 *data, size_t len)
{
    size_t cap;
    u8 *grown;

    if (len == 0U)
        return true;
    if (out->len > SIZE_MAX - len - 1U)
        return false;
    if (out->len + len + 1U > out->cap) {
        cap = out->cap == 0U ? 256U : out->cap;
        while (cap < out->len + len + 1U) {
            if (cap > SIZE_MAX / 2U)
                return false;
            cap *= 2U;
        }
        grown = realloc(out->data, cap);
        if (grown == NULL)
            return false;
        out->data = grown;
        out->cap = cap;
    }
    (void)memcpy(out->data + out->len, data, len);
    out->len += len;
    out->data[out->len] = 0U;
    return true;
}

static void bytes_drop(Bytes *out)
{
    free(out->data);
    (void)memset(out, 0, sizeof(*out));
}

static bool bytes_contain(const u8 *haystack, size_t haystack_len,
                          const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0U)
        return true;
    if (needle_len > haystack_len)
        return false;
    for (i = 0U; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static bool read_all(int fd, Bytes *out)
{
    u8 buf[4096];

    for (;;) {
        ssize_t got = read(fd, buf, sizeof(buf));

        if (got == 0)
            return true;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (!bytes_append(out, buf, (size_t)got))
            return false;
    }
}

static bool write_all(int fd, const u8 *data, size_t len)
{
    while (len != 0U) {
        ssize_t wrote = write(fd, data, len);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        data += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return true;
}

static bool run_process(const char *repo, char *const *argv,
                        const u8 *input, size_t input_len, Bytes *out)
{
    int inpipe[2];
    int outpipe[2];
    pid_t pid;
    int status;
    bool ok;

    if (pipe(inpipe) != 0 || pipe(outpipe) != 0)
        return false;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        (void)close(inpipe[1]);
        (void)close(outpipe[0]);
        if (chdir(repo) != 0 || dup2(inpipe[0], STDIN_FILENO) < 0 ||
            dup2(outpipe[1], STDOUT_FILENO) < 0 ||
            dup2(outpipe[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(inpipe[0]);
        (void)close(outpipe[1]);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    (void)close(inpipe[0]);
    (void)close(outpipe[1]);
    ok = write_all(inpipe[1], input, input_len);
    ok = close(inpipe[1]) == 0 && ok;
    if (out != NULL)
        ok = read_all(outpipe[0], out) && ok;
    else {
        Bytes ignored = {0};

        ok = read_all(outpipe[0], &ignored) && ok;
        bytes_drop(&ignored);
    }
    ok = close(outpipe[0]) == 0 && ok;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_git(const char *repo, char *const *tail,
                    const u8 *input, size_t input_len, Bytes *out)
{
    char *argv[24];
    size_t n = 0U;

    argv[n++] = (char *)"git";
    while (*tail != NULL && n + 1U < YEW_ARRAY_LEN(argv))
        argv[n++] = *tail++;
    if (*tail != NULL)
        return false;
    argv[n] = NULL;
    return run_process(repo, argv, input, input_len, out);
}

static bool make_file(const char *repo, const char *path,
                      const u8 *bytes, size_t len)
{
    size_t need = strlen(repo) + strlen(path) + 2U;
    char *full = malloc(need);
    int fd;
    bool ok;

    if (full == NULL)
        return false;
    (void)snprintf(full, need, "%s/%s", repo, path);
    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    free(full);
    if (fd < 0)
        return false;
    ok = write_all(fd, bytes, len);
    return close(fd) == 0 && ok;
}

static bool read_file(const char *repo, const char *path, Bytes *out)
{
    size_t need = strlen(repo) + strlen(path) + 2U;
    char *full = malloc(need);
    int fd;
    bool ok;

    if (full == NULL)
        return false;
    (void)snprintf(full, need, "%s/%s", repo, path);
    fd = open(full, O_RDONLY);
    free(full);
    if (fd < 0)
        return false;
    ok = read_all(fd, out);
    return close(fd) == 0 && ok;
}

static void pump_editor_jobs(Ed *ed)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 n = 0U;

    yew_job_collect_fds(ed, pfd, &n);
    if (n != 0U)
        (void)poll(pfd, (nfds_t)n, 20);
    else
        (void)poll(NULL, 0U, 5);
    yew_job_pump(ed, pfd, n);
    yew_job_reap(ed);
    yew_job_tick(ed, yew_now_ms());
    yew_job_settle(ed);
}

static bool run_editor_jobs_idle(Ed *ed)
{
    i64 start = yew_now_ms();

    while (ed->jobs.len != 0U) {
        pump_editor_jobs(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
    return true;
}

static bool repo_init(const char *repo)
{
    char *init[] = {(char *)"init", (char *)"-q", NULL};
    char *name[] = {(char *)"config", (char *)"user.name",
                    (char *)"Yew Test", NULL};
    char *mail[] = {(char *)"config", (char *)"user.email",
                    (char *)"yew@test.invalid", NULL};

    return mkdir(repo, 0700) == 0 &&
           run_git(repo, init, NULL, 0U, NULL) &&
           run_git(repo, name, NULL, 0U, NULL) &&
           run_git(repo, mail, NULL, 0U, NULL);
}

static bool build_patch(const char *path, const u8 *base, size_t base_len,
                        const u8 *buf, size_t buf_len, Bytebuf *patch,
                        GitHunk *hunk_out)
{
    Arena arena;
    u64 *base_hashes = NULL;
    u64 *buf_hashes = NULL;
    u32 base_n = 0U;
    u32 buf_n = 0U;
    bool base_missing = false;
    bool buf_missing = false;
    GitHunkVec hunks = {0};
    bool ok;

    arena_init(&arena);
    ok = yew_git_hash_lines(base, base_len, &arena, &base_hashes, &base_n,
                            &base_missing) &&
         yew_git_hash_lines(buf, buf_len, &arena, &buf_hashes, &buf_n,
                            &buf_missing) &&
         yew_diff_lines(&arena, base_hashes, base_n, buf_hashes, buf_n,
                        YEW_DIFF_MAX_D, &hunks) && hunks.len == 1U;
    if (ok) {
        *hunk_out = hunks.data[0];
        ok = yew_git_hunk_patch(patch, path, base, base_len, buf, buf_len,
                                hunk_out);
    }
    GitHunkVec_free(&hunks);
    arena_free_all(&arena);
    return ok;
}

static void check_stage_case(const char *repo, const char *path,
                             const u8 *base, size_t base_len,
                             const u8 *buf, size_t buf_len,
                             const u8 *worktree, size_t worktree_len,
                             const char *must_contain,
                             const char *must_omit)
{
    char *add[] = {(char *)"add", (char *)"--", (char *)path, NULL};
    char *check[] = {(char *)"apply", (char *)"--check",
                     (char *)"--cached", (char *)"-", NULL};
    char *apply[] = {(char *)"apply", (char *)"--cached",
                     (char *)"-", NULL};
    char object[512];
    char *show[] = {(char *)"show", object, NULL};
    Bytebuf patch;
    GitHunk hunk;
    Bytes indexed = {0};
    Bytes disk = {0};
    bool accepted;

    CHECK(make_file(repo, path, base, base_len));
    CHECK(run_git(repo, add, NULL, 0U, NULL));
    if (worktree != NULL)
        CHECK(make_file(repo, path, worktree, worktree_len));
    if (!build_patch(path, base, base_len, buf, buf_len, &patch, &hunk)) {
        (void)fprintf(stderr, "git_hunks: patch build failed for %s\n", path);
        CHECK(false);
        return;
    }
    if (must_contain != NULL)
        CHECK(bytes_contain(patch.data, patch.len, must_contain));
    if (must_omit != NULL)
        CHECK(!bytes_contain(patch.data, patch.len, must_omit));
    accepted = run_git(repo, check, patch.data, patch.len, NULL);
    CHECK(accepted);
    if (accepted)
        patches_checked++;
    CHECK(run_git(repo, apply, patch.data, patch.len, NULL));
    {
        int wrote = snprintf(object, sizeof(object), ":%s", path);

        CHECK(wrote > 0 && (size_t)wrote < sizeof(object));
        if (wrote <= 0 || (size_t)wrote >= sizeof(object)) {
            bytebuf_free(&patch);
            return;
        }
    }
    CHECK(run_git(repo, show, NULL, 0U, &indexed));
    CHECK(indexed.len == buf_len &&
          (buf_len == 0U || memcmp(indexed.data, buf, buf_len) == 0));
    CHECK(read_file(repo, path, &disk));
    if (worktree != NULL)
        CHECK(disk.len == worktree_len &&
              (worktree_len == 0U ||
               memcmp(disk.data, worktree, worktree_len) == 0));
    else
        CHECK(disk.len == base_len &&
              (base_len == 0U || memcmp(disk.data, base, base_len) == 0));
    bytes_drop(&disk);
    bytes_drop(&indexed);
    bytebuf_free(&patch);
}

static void check_edit_fixture_matrix(const char *repo)
{
    static const u8 base[] = "a\nb\nc\nd\ne\nf\n";
    static const u8 add_eof[] = "a\nb\nc\nd\ne\nf\ntail\n";
    static const u8 del_bof[] = "b\nc\nd\ne\nf\n";
    static const u8 mod_bof[] = "A\nb\nc\nd\ne\nf\n";
    static const u8 mod_eof[] = "a\nb\nc\nd\ne\nF\n";
    static const u8 insert_middle[] = "a\nb\nc\ninside\nd\ne\nf\n";
    static const u8 delete_middle[] = "a\nb\nd\ne\nf\n";
    static const u8 multiline[] = "a\nB\nC\nD\ne\nf\n";
    static const u8 add_two_eof[] = "a\nb\nc\nd\ne\nf\ntail-1\ntail-2\n";
    static const u8 one_line[] = "only\n";

    check_stage_case(repo, "add-eof.txt", base, sizeof(base) - 1U,
                     add_eof, sizeof(add_eof) - 1U, NULL, 0U, NULL, NULL);
    check_stage_case(repo, "delete-bof.txt", base, sizeof(base) - 1U,
                     del_bof, sizeof(del_bof) - 1U, NULL, 0U, NULL, NULL);
    check_stage_case(repo, "modify-bof.txt", base, sizeof(base) - 1U,
                     mod_bof, sizeof(mod_bof) - 1U, NULL, 0U, NULL, NULL);
    check_stage_case(repo, "modify-eof.txt", base, sizeof(base) - 1U,
                     mod_eof, sizeof(mod_eof) - 1U, NULL, 0U, NULL, NULL);
    check_stage_case(repo, "insert-middle.txt", base, sizeof(base) - 1U,
                     insert_middle, sizeof(insert_middle) - 1U,
                     NULL, 0U, NULL, NULL);
    check_stage_case(repo, "delete-middle.txt", base, sizeof(base) - 1U,
                     delete_middle, sizeof(delete_middle) - 1U,
                     NULL, 0U, NULL, NULL);
    check_stage_case(repo, "modify-many.txt", base, sizeof(base) - 1U,
                     multiline, sizeof(multiline) - 1U,
                     NULL, 0U, NULL, NULL);
    check_stage_case(repo, "add-two-eof.txt", base, sizeof(base) - 1U,
                     add_two_eof, sizeof(add_two_eof) - 1U,
                     NULL, 0U, NULL, NULL);
    check_stage_case(repo, "empty-to-line.txt", NULL, 0U,
                     one_line, sizeof(one_line) - 1U, NULL, 0U, NULL, NULL);
    check_stage_case(repo, "line-to-empty.txt", one_line,
                     sizeof(one_line) - 1U, NULL, 0U, NULL, 0U, NULL, NULL);
}

static void check_newline_matrix(const char *repo)
{
    static const u8 base_nl[] = "old\n";
    static const u8 base_no_nl[] = "old";
    static const u8 buf_nl[] = "new\n";
    static const u8 buf_no_nl[] = "new";

    check_stage_case(repo, "nl-yes-yes.txt", base_nl,
                     sizeof(base_nl) - 1U, buf_nl, sizeof(buf_nl) - 1U,
                     NULL, 0U, NULL, NULL);
    check_stage_case(repo, "nl-yes-no.txt", base_nl,
                     sizeof(base_nl) - 1U, buf_no_nl,
                     sizeof(buf_no_nl) - 1U, NULL, 0U,
                     "\\ No newline at end of file", NULL);
    check_stage_case(repo, "nl-no-yes.txt", base_no_nl,
                     sizeof(base_no_nl) - 1U, buf_nl,
                     sizeof(buf_nl) - 1U, NULL, 0U,
                     "\\ No newline at end of file", NULL);
    check_stage_case(repo, "nl-no-no.txt", base_no_nl,
                     sizeof(base_no_nl) - 1U, buf_no_nl,
                     sizeof(buf_no_nl) - 1U, NULL, 0U,
                     "\\ No newline at end of file", NULL);
}

static void check_bounds_and_context(const char *repo)
{
    static const u8 base[] =
        "zero\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n";
    static const u8 middle[] =
        "zero\none\ntwo\nthree\nCHANGED\nfive\nsix\nseven\neight\nnine\n";
    static const u8 add_bof[] =
        "added\nzero\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n";
    static const u8 del_eof[] =
        "zero\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n";

    check_stage_case(repo, "middle.txt", base, sizeof(base) - 1U,
                     middle, sizeof(middle) - 1U, NULL, 0U,
                     " three\n-four\n+CHANGED\n five\n six\n seven\n",
                     " zero\n");
    check_stage_case(repo, "add-bof.txt", base, sizeof(base) - 1U,
                     add_bof, sizeof(add_bof) - 1U, NULL, 0U,
                     "@@ -1,3 +1,4 @@", " four\n");
    check_stage_case(repo, "del-eof.txt", base, sizeof(base) - 1U,
                     del_eof, sizeof(del_eof) - 1U, NULL, 0U,
                     "@@ -7,4 +7,3 @@", " five\n");
}

static void check_crlf_and_dirty_worktree(const char *repo)
{
    static const u8 crlf_base[] = "one\r\ntwo\r\nthree\r\n";
    static const u8 crlf_buf[] = "one\r\nTWO\r\nthree\r\n";
    static const u8 dirty_base[] = "index base\nkeep\n";
    static const u8 dirty_buf[] = "buffer edit\nkeep\n";
    static const u8 dirty_disk[] = "worktree-only edit\nkeep\n";

    check_stage_case(repo, "crlf.txt", crlf_base, sizeof(crlf_base) - 1U,
                     crlf_buf, sizeof(crlf_buf) - 1U, NULL, 0U,
                     "-two\r\n+TWO\r\n", NULL);
    check_stage_case(repo, "dirty.txt", dirty_base,
                     sizeof(dirty_base) - 1U, dirty_buf,
                     sizeof(dirty_buf) - 1U, dirty_disk,
                     sizeof(dirty_disk) - 1U, "-index base\n+buffer edit\n",
                     "worktree-only");
}

static void check_three_way_index_base(const char *repo)
{
    static const u8 indexed[] = "index-only\nkeep\n";
    static const u8 disk[] = "disk-only\nkeep\n";
    static const u8 live[] = "buffer-only\nindex-only\nkeep\n";
    char *add[] = {(char *)"add", (char *)"--",
                   (char *)"three-way.txt", NULL};
    char full[4096];
    const GitSnapshot *snapshot;
    const GitEntry *entry;
    const HunkList *hunks = NULL;
    Bytes actual_disk = {0};
    i64 start;
    Ed ed;

    CHECK(make_file(repo, "three-way.txt", indexed, sizeof(indexed) - 1U));
    CHECK(run_git(repo, add, NULL, 0U, NULL));
    CHECK(make_file(repo, "three-way.txt", disk, sizeof(disk) - 1U));
    CHECK(snprintf(full, sizeof(full), "%s/three-way.txt", repo) > 0);

    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    CHECK(yew_ed_open_memory(&ed, live, sizeof(live) - 1U,
                             "three-way.txt"));
    ed.buffer.path = arena_strdup(&ed.arena, full);
    CHECK(yew_git_refresh(&ed, true));
    CHECK(run_editor_jobs_idle(&ed));
    snapshot = yew_git_snapshot_cached(&ed);
    entry = find_git_entry(snapshot, "three-way.txt");
    CHECK(entry != NULL && entry->index_oid[0] != '\0');

    ed.now_ms = 0;
    yew_git_editor_prepare(&ed, ed.win);
    start = yew_now_ms();
    while (yew_now_ms() - start <= 10000) {
        pump_editor_jobs(&ed);
        yew_git_editor_tick(&ed, 5000);
        hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
        if (hunks != NULL && hunks->h.len != 0U)
            break;
    }
    CHECK(hunks != NULL && !hunks->base_is_head);
    CHECK(hunks != NULL && hunks->h.len == 1U);
    if (hunks != NULL && hunks->h.len == 1U) {
        CHECK(hunks->h.data[0].kind == YEW_HUNK_ADD);
        CHECK(hunks->h.data[0].base_lo.v == 0U);
        CHECK(hunks->h.data[0].base_n.v == 0U);
        CHECK(hunks->h.data[0].buf_lo.v == 0U);
        CHECK(hunks->h.data[0].buf_n.v == 1U);
    }
    CHECK(read_file(repo, "three-way.txt", &actual_disk));
    CHECK(actual_disk.len == sizeof(disk) - 1U &&
          memcmp(actual_disk.data, disk, sizeof(disk) - 1U) == 0);
    bytes_drop(&actual_disk);
    yew_ed_free(&ed);
}

static void check_path_rules(const char *repo)
{
    static const u8 base[] = "before\n";
    static const u8 buf[] = "after\n";
    Bytebuf patch;
    GitHunk hunk = {LINENO(0U), LINENO(1U), LINENO(0U), LINENO(1U),
                    YEW_HUNK_MOD};

    check_stage_case(repo, "space name.txt", base, sizeof(base) - 1U,
                     buf, sizeof(buf) - 1U, NULL, 0U,
                     "diff --git a/space name.txt b/space name.txt", NULL);
    CHECK(!yew_git_hunk_patch(&patch, "line\nbreak.txt", base,
                              sizeof(base) - 1U, buf, sizeof(buf) - 1U,
                              &hunk));
}

static void check_stage_command_selection(const char *repo)
{
    static const u8 base[] =
        "zero\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n";
    static const u8 live[] =
        "zero\nONE\ntwo\nthree\nfour\nFIVE\nsix\nseven\neight\nNINE\nten\n";
    char full[4096];
    char object[] = ":command-selection.txt";
    char *add[] = {(char *)"add", (char *)"--",
                   (char *)"command-selection.txt", NULL};
    char *show[] = {(char *)"show", object, NULL};
    Bytes indexed = {0};
    Bytes disk = {0};
    CmdCtx cx = {0};
    CmdId stage;
    EditCtx edit;
    const HunkList *hunks = NULL;
    ByteOff first;
    ByteOff last;
    char *editor_path;
    Ed ed;
    u32 ticks = 0U;

    CHECK(make_file(repo, "command-selection.txt", base, sizeof(base) - 1U));
    CHECK(run_git(repo, add, NULL, 0U, NULL));
    CHECK(snprintf(full, sizeof(full), "%s/command-selection.txt", repo) > 0);
    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    CHECK(yew_ed_open_memory(&ed, live, sizeof(live) - 1U,
                             "command-selection.txt"));
    ed.buffer.path = arena_strdup(&ed.arena, full);
    CHECK(yew_git_refresh(&ed, true));
    CHECK(run_editor_jobs_idle(&ed));
    CHECK(yew_git_editor_test_base(&ed, &ed.buffer,
                                    base, sizeof(base) - 1U,
                                    "command-index", false, 0));
    do {
        yew_git_editor_tick(&ed, 5000);
        hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
        ticks++;
    } while ((hunks == NULL || hunks->h.len != 3U) && ticks < 20000U);
    CHECK(hunks != NULL && hunks->h.len == 3U);

    editor_path = ed.buffer.path;
    ed.buffer.path = NULL;
    edit = yew_ed_edit_ctx(&ed);
    yew_undo_begin(&edit, YEW_TXN_EXTERNAL);
    CHECK(yew_edit_insert(&edit, BYTEOFF(yew_textbuf_len(ed.buffer.tb)),
                          (const u8 *)"x", 1U));
    CHECK(yew_edit_delete(&edit,
                          (Span){yew_textbuf_len(ed.buffer.tb) - 1U,
                                 yew_textbuf_len(ed.buffer.tb)}));
    yew_undo_end(&edit);
    yew_ed_finish_edit(&ed, &edit);
    ed.buffer.path = editor_path;
    CHECK(!yew_undo_at_save_point(ed.buffer.undo));
    ticks = 0U;
    do {
        yew_git_editor_tick(&ed, 5000);
        hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
        ticks++;
    } while ((hunks == NULL || hunks->h.len != 3U) && ticks < 20000U);
    CHECK(hunks != NULL && hunks->h.len == 3U);

    first = yew_textbuf_line_start(ed.buffer.tb, LINENO(1U));
    last = yew_textbuf_line_start(ed.buffer.tb, LINENO(9U));
    ed.win->cs.curs.data[0].anchor = first;
    ed.win->cs.curs.data[0].pos = BYTEOFF(last.v + 1U);
    stage = yew_cmd_lookup("ed.git.hunk.stage", 17U);
    CHECK(stage.v != 0U);
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    CHECK(yew_ed_invoke(&ed, stage, &cx) == YEW_CMD_OK);
    CHECK(ed.jobs.len == 1U);
    CHECK(ed.msg.active && strcmp(ed.msg.text,
          "staged from an unsaved buffer — the file on disk is still the old version") == 0);
    CHECK(run_editor_jobs_idle(&ed));
    CHECK(run_git(repo, show, NULL, 0U, &indexed));
    CHECK(indexed.len == sizeof(live) - 1U &&
          memcmp(indexed.data, live, sizeof(live) - 1U) == 0);
    CHECK(read_file(repo, "command-selection.txt", &disk));
    CHECK(disk.len == sizeof(base) - 1U &&
          memcmp(disk.data, base, sizeof(base) - 1U) == 0);
    bytes_drop(&disk);
    bytes_drop(&indexed);
    yew_ed_free(&ed);
}

static const GitEntry *find_git_entry(const GitSnapshot *snapshot,
                                      const char *path)
{
    size_t i;

    if (snapshot == NULL)
        return NULL;
    for (i = 0U; i < snapshot->entries.len; i++)
        if (snapshot->entries.data[i].path != NULL &&
            strcmp(snapshot->entries.data[i].path, path) == 0)
            return &snapshot->entries.data[i];
    return NULL;
}

static void check_conflicted_head_base(const char *parent)
{
    static const u8 initial[] = "base\n";
    static const u8 trunk[] = "trunk\n";
    static const u8 side[] = "side\n";
    char repo[4096];
    char full[4096];
    char *branch_trunk[] = {(char *)"checkout", (char *)"-q", (char *)"-B",
                            (char *)"trunk", NULL};
    char *branch_side[] = {(char *)"checkout", (char *)"-q", (char *)"-b",
                           (char *)"side", NULL};
    char *checkout_trunk[] = {(char *)"checkout", (char *)"-q",
                              (char *)"trunk", NULL};
    char *add[] = {(char *)"add", (char *)"--", (char *)"conflict.txt", NULL};
    char *commit_base[] = {(char *)"commit", (char *)"-q", (char *)"-m",
                           (char *)"base", NULL};
    char *commit_side[] = {(char *)"commit", (char *)"-q", (char *)"-am",
                           (char *)"side", NULL};
    char *commit_trunk[] = {(char *)"commit", (char *)"-q", (char *)"-am",
                            (char *)"trunk", NULL};
    char *merge[] = {(char *)"merge", (char *)"--no-edit", (char *)"side",
                     NULL};
    const GitSnapshot *snapshot;
    const GitEntry *entry;
    const HunkList *hunks = NULL;
    i64 start;
    Ed ed;

    CHECK(snprintf(repo, sizeof(repo), "%s/conflict-repo", parent) > 0);
    CHECK(repo_init(repo));
    CHECK(run_git(repo, branch_trunk, NULL, 0U, NULL));
    CHECK(make_file(repo, "conflict.txt", initial, sizeof(initial) - 1U));
    CHECK(run_git(repo, add, NULL, 0U, NULL));
    CHECK(run_git(repo, commit_base, NULL, 0U, NULL));
    CHECK(run_git(repo, branch_side, NULL, 0U, NULL));
    CHECK(make_file(repo, "conflict.txt", side, sizeof(side) - 1U));
    CHECK(run_git(repo, commit_side, NULL, 0U, NULL));
    CHECK(run_git(repo, checkout_trunk, NULL, 0U, NULL));
    CHECK(make_file(repo, "conflict.txt", trunk, sizeof(trunk) - 1U));
    CHECK(run_git(repo, commit_trunk, NULL, 0U, NULL));
    CHECK(!run_git(repo, merge, NULL, 0U, NULL));

    CHECK(snprintf(full, sizeof(full), "%s/conflict.txt", repo) > 0);
    yew_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, repo);
    CHECK(yew_ed_open_memory(&ed, trunk, sizeof(trunk) - 1U,
                             "conflict.txt"));
    ed.buffer.path = arena_strdup(&ed.arena, full);
    CHECK(yew_git_refresh(&ed, true));
    CHECK(run_editor_jobs_idle(&ed));
    snapshot = yew_git_snapshot_cached(&ed);
    entry = find_git_entry(snapshot, "conflict.txt");
    CHECK(entry != NULL && entry->conflicted);
    ed.now_ms = 0;
    yew_git_editor_prepare(&ed, ed.win);
    start = yew_now_ms();
    while (yew_now_ms() - start <= 10000) {
        pump_editor_jobs(&ed);
        yew_git_editor_tick(&ed, 0);
        hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
        if (hunks != NULL && hunks->base_is_head)
            break;
    }
    CHECK(hunks != NULL && hunks->base_is_head);
    CHECK(hunks != NULL && hunks->h.len == 0U);
    yew_ed_free(&ed);
}

int main(int argc, char **argv)
{
    char *root;
    char repo[4096];
    int wrote;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
        return 2;
    }
    root = realpath(argv[1], NULL);
    if (root == NULL || !isolate_git_environment()) {
        (void)fprintf(stderr,
                      "git_hunks: cannot isolate fixture root: %s\n",
                      strerror(errno));
        free(root);
        return 2;
    }
    wrote = snprintf(repo, sizeof(repo), "%s/repo", root);
    if (wrote <= 0 || (size_t)wrote >= sizeof(repo) || !repo_init(repo)) {
        (void)fprintf(stderr, "git_hunks: could not initialize fixture\n");
        free(root);
        return 2;
    }
    check_newline_matrix(repo);
    check_bounds_and_context(repo);
    check_edit_fixture_matrix(repo);
    check_crlf_and_dirty_worktree(repo);
    check_three_way_index_base(repo);
    check_path_rules(repo);
    check_stage_command_selection(repo);
    check_conflicted_head_base(root);
    CHECK(patches_checked == 20U);
    (void)printf("HARNESS_RESULT git_hunks assertions=%u patches=%u "
                 "failures=%u\n", assertions, patches_checked, failures);
    free(root);
    return failures == 0U ? 0 : 1;
}
