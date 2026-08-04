/* Sprint 19 §9 + DoD 10: sag_shell_quote must round-trip every byte
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

/* Runs `printf %s <quoted>` under /bin/sh and returns its stdout. */
static bool sh_roundtrip(const u8 *src, size_t len, Bytebuf *out)
{
    Bytebuf cmd;
    int fds[2];
    pid_t pid;
    bool ok = true;

    bytebuf_init(&cmd);
    bytebuf_append(&cmd, "printf %s ", 10U);
    sag_shell_quote(&cmd, src, len);
    bytebuf_push_u8(&cmd, 0U);

    if (!sag_pipe_cloexec(fds)) {
        bytebuf_free(&cmd);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        bytebuf_free(&cmd);
        return false;
    }
    if (pid == 0) {
        char *argv[4];

        (void)dup2(fds[1], STDOUT_FILENO);
        (void)close(fds[0]);
        (void)close(fds[1]);
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)cmd.data;
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
    (void)waitpid(pid, NULL, 0);
    bytebuf_free(&cmd);
    return ok;
}

void test_shell_quote_algorithm(void)
{
    Bytebuf out;

    bytebuf_init(&out);
    sag_shell_quote(&out, (const u8 *)"", 0U);
    SAG_ASSERT_EQ_U64((u64)out.len, 2U);
    SAG_ASSERT_EQ_MEM(out.data, "''", 2U);

    out.len = 0U;
    sag_shell_quote(&out, (const u8 *)"plain", 5U);
    SAG_ASSERT_EQ_MEM(out.data, "'plain'", 7U);

    /* The one interesting byte: close, escape, reopen. */
    out.len = 0U;
    sag_shell_quote(&out, (const u8 *)"it's", 4U);
    SAG_ASSERT_EQ_MEM(out.data, "'it'\\''s'", 9U);

    /* Nothing else is escaped — inside '...' sh takes bytes literally. */
    out.len = 0U;
    sag_shell_quote(&out, (const u8 *)"$x `y` \\z", 9U);
    SAG_ASSERT_EQ_MEM(out.data, "'$x `y` \\z'", 11U);
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

    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        Bytebuf got;
        size_t len = strlen(cases[i]);

        bytebuf_init(&got);
        SAG_ASSERT(sh_roundtrip((const u8 *)cases[i], len, &got));
        SAG_ASSERT_EQ_U64((u64)got.len, (u64)len);
        if (len != 0U)
            SAG_ASSERT_EQ_MEM(got.data, cases[i], len);
        bytebuf_free(&got);
    }
}

void test_shell_quote_roundtrips_random_bytes(void)
{
    /* Deterministic LCG: the corpus must be identical on every run
     * (invariant 3), so no time or pid seeding. */
    u64 seed = 0x5A617A19ULL;
    u32 iter;

    /* NUL cannot survive an argv round trip — the shell would truncate —
     * so the generator draws from 1..255, which is what a command line can
     * actually carry. */
    for (iter = 0U; iter < 400U; iter++) {
        u8 src[64];
        size_t len = (size_t)(seed % 33U);
        size_t k;
        Bytebuf got;

        for (k = 0U; k < len; k++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            src[k] = (u8)(1U + (seed >> 33) % 255U);
        }
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        bytebuf_init(&got);
        SAG_ASSERT(sh_roundtrip(src, len, &got));
        SAG_ASSERT_EQ_U64((u64)got.len, (u64)len);
        if (len != 0U)
            SAG_ASSERT_EQ_MEM(got.data, src, len);
        bytebuf_free(&got);
    }
}
