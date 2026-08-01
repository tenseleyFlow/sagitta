#include "harness.h"

#include <stdlib.h>

#include "../pty/harness.h"

void test_pty_environment_exact(void)
{
    static const char *const expected[] = {
        "TERM=xterm-256color",
        "SAG_COLORS=truecolor",
        "SAG_TTY_PROBE=1",
        "SAG_PROBE_TIMEOUT_MS=500",
        "SAG_ESC_TIMEOUT_MS=25",
        "XDG_STATE_HOME=/tmp/sagitta-pty-state",
        "LANG=C.UTF-8",
        "LC_ALL=C.UTF-8",
        "SAG_LOG_LEVEL=debug"
    };
    char *envp[SAG_PTY_ENV_COUNT + 1U] = {0};
    size_t i;

    SAG_ASSERT(ptc_env_build(envp, "truecolor", "/tmp/sagitta-pty-state"));
    for (i = 0U; i < SAG_PTY_ENV_COUNT; i++)
        SAG_ASSERT_EQ_STR(envp[i], expected[i]);
    SAG_ASSERT_NULL(envp[SAG_PTY_ENV_COUNT]);
    ptc_env_free(envp);
    for (i = 0U; i <= SAG_PTY_ENV_COUNT; i++)
        SAG_ASSERT_NULL(envp[i]);
}
