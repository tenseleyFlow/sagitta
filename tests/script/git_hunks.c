#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mod/git/gutter.h"

typedef struct Bytes {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

static unsigned assertions;
static unsigned failures;
static unsigned patches_checked;

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

int main(int argc, char **argv)
{
    char repo[4096];
    int wrote;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
        return 2;
    }
    wrote = snprintf(repo, sizeof(repo), "%s/repo", argv[1]);
    if (wrote <= 0 || (size_t)wrote >= sizeof(repo) || !repo_init(repo)) {
        (void)fprintf(stderr, "git_hunks: could not initialize fixture\n");
        return 2;
    }
    check_newline_matrix(repo);
    check_bounds_and_context(repo);
    check_edit_fixture_matrix(repo);
    check_crlf_and_dirty_worktree(repo);
    check_path_rules(repo);
    CHECK(patches_checked == 20U);
    (void)printf("HARNESS_RESULT git_hunks assertions=%u patches=%u "
                 "failures=%u\n", assertions, patches_checked, failures);
    return failures == 0U ? 0 : 1;
}
