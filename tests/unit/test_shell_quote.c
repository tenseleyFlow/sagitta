/* Sprint 19 §9 + DoD 10: yew_shell_quote must round-trip every byte
 * through a real /bin/sh.  The quoting rule is the whole security story
 * for user-composed :! lines carrying s18 %-expansions. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/job.h"
#include "util/base.h"
#include "util/buf.h"

/* Runs one NUL-terminated command under /bin/sh and captures stdout. */
static bool sh_capture(const Bytebuf *cmd, Bytebuf *out)
{
    int fds[2];
    pid_t pid;
    int status = 0;
    bool ok = true;

    if (!yew_pipe_cloexec(fds)) {
        return false;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    if (pid == 0) {
        char *argv[4];

        (void)dup2(fds[1], STDOUT_FILENO);
        (void)close(fds[0]);
        (void)close(fds[1]);
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)cmd->data;
        argv[3] = NULL;
        (void)execv("/bin/sh", argv);
        _exit(127);
    }
    (void)close(fds[1]);
    for (;;) {
        u8 chunk[4096];
        ssize_t got = read(fds[0], chunk, sizeof(chunk));

        if (got > 0) {
            bytebuf_append(out, chunk, (size_t)got);
            continue;
        }
        if (got == 0)
            break;
        ok = false;
        break;
    }
    (void)close(fds[0]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        ok = false;
    return ok;
}

/* Runs `printf %s <quoted>` under /bin/sh and returns its stdout. */
static bool sh_roundtrip(const u8 *src, size_t len, Bytebuf *out)
{
    Bytebuf cmd;
    bool ok;

    bytebuf_init(&cmd);
    bytebuf_append(&cmd, "printf %s ", 10U);
    yew_shell_quote(&cmd, src, len);
    bytebuf_push_u8(&cmd, 0U);
    ok = sh_capture(&cmd, out);
    bytebuf_free(&cmd);
    return ok;
}

void test_shell_quote_algorithm(void)
{
    Bytebuf out;

    bytebuf_init(&out);
    yew_shell_quote(&out, (const u8 *)"", 0U);
    YEW_ASSERT_EQ_U64((u64)out.len, 2U);
    YEW_ASSERT_EQ_MEM(out.data, "''", 2U);

    out.len = 0U;
    yew_shell_quote(&out, (const u8 *)"plain", 5U);
    YEW_ASSERT_EQ_MEM(out.data, "'plain'", 7U);

    /* The one interesting byte: close, escape, reopen. */
    out.len = 0U;
    yew_shell_quote(&out, (const u8 *)"it's", 4U);
    YEW_ASSERT_EQ_MEM(out.data, "'it'\\''s'", 9U);

    /* Nothing else is escaped — inside '...' sh takes bytes literally. */
    out.len = 0U;
    yew_shell_quote(&out, (const u8 *)"$x `y` \\z", 9U);
    YEW_ASSERT_EQ_MEM(out.data, "'$x `y` \\z'", 11U);
    bytebuf_free(&out);
}

void test_shell_quote_roundtrips_hard_cases(void)
{
    static const char *const cases[] = {
        "",
        "plain",
        "with space",
        "it's",
        "'",
        "''",
        "'''",
        "$HOME",
        "`whoami`",
        "back\\slash",
        "semi;colon",
        "pipe|char",
        "amp&sand",
        "new\nline",
        "tab\there",
        "quote\"double",
        "star*glob?",
        "paren(s)",
        "brace{s}",
        "dollar$(cmd)",
        "emoji \xF0\x9F\x98\x80 here",
        "cjk \xE6\x97\xA5\xE6\x9C\xAC",
        "combining e\xCC\x81",
        "-leading-dash",
        "--flag=value",
        "trailing space ",
        " leading space",
        "mixed '$(x)' \"y\" `z`"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        Bytebuf got;
        size_t len = strlen(cases[i]);

        bytebuf_init(&got);
        YEW_ASSERT(sh_roundtrip((const u8 *)cases[i], len, &got));
        YEW_ASSERT_EQ_U64((u64)got.len, (u64)len);
        if (len != 0U)
            YEW_ASSERT_EQ_MEM(got.data, cases[i], len);
        bytebuf_free(&got);
    }
}

void test_shell_quote_roundtrips_random_bytes(void)
{
    enum { CASES = 100000, BATCH_CASES = 256 };
    /* Deterministic LCG: the corpus must be identical on every run
     * (invariant 3), so no time or pid seeding. */
    u64 seed = 0x5A617A19ULL;
    u32 iter;
    Bytebuf cmd;
    Bytebuf expected;

    /* NUL cannot survive an argv round trip — the shell would truncate —
     * so the generator draws from 1..255, which is what a command line can
     * actually carry. */
    bytebuf_init(&cmd);
    bytebuf_init(&expected);
    for (iter = 0U; iter < CASES; iter++) {
        u8 src[64];
        size_t len = (size_t)(seed % 33U);
        size_t k;

        for (k = 0U; k < len; k++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            src[k] = (u8)(1U + (seed >> 33) % 255U);
        }
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;

        /* One real shell parses every emitted form.  The NUL is generated
         * by printf rather than embedded in argv and makes each case's
         * boundary part of the byte-exact comparison. */
        bytebuf_append(&cmd, "printf %s ", 10U);
        yew_shell_quote(&cmd, src, len);
        bytebuf_append(&cmd, "; printf '\\000'\n",
                       sizeof("; printf '\\000'\n") - 1U);
        bytebuf_append(&expected, src, len);
        bytebuf_push_u8(&expected, 0U);

        if ((iter + 1U) % BATCH_CASES == 0U || iter + 1U == CASES) {
            Bytebuf got;

            bytebuf_init(&got);
            bytebuf_push_u8(&cmd, 0U);
            YEW_ASSERT(sh_capture(&cmd, &got));
            YEW_ASSERT_EQ_U64((u64)got.len, (u64)expected.len);
            YEW_ASSERT_EQ_MEM(got.data, expected.data, expected.len);
            bytebuf_free(&got);
            cmd.len = 0U;
            expected.len = 0U;
        }
    }
    YEW_ASSERT_EQ_U64(iter, CASES);
    bytebuf_free(&expected);
    bytebuf_free(&cmd);
}
