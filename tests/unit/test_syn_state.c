#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syn/engine.h"
#include "syn/defs.h"
#include "syn/theme.h"
#include "util/buf.h"

typedef void (*SynBugCase)(void);

static void syn_expect_bug(SynBugCase run, const char *needle)
{
    Bytebuf output;
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    ssize_t count;
    u8 chunk[256];

    bytebuf_init(&output);
    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    YEW_ASSERT_EQ_I64(pipe(pipefd), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        run();
        _exit(99);
    }
    (void)close(pipefd[1]);
    for (;;) {
        count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), YEW_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    YEW_ASSERT(strstr((const char *)output.data, needle) != NULL);
    bytebuf_free(&output);
}

static void syn_load_theme_bug(void)
{
    yew_theme_load("test.theme");
}

static void syn_embedded_def_bug(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = {{0}, 0U, 1U, 0U, 1U, 0U};

    (void)yew_syn_state_intern(tab, &state);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_root_is_reserved_and_canonical(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(7U);
    const SynState *root = yew_syn_state_get(tab, YEW_SYN_STATE_ROOT);
    SynState copy;

    YEW_ASSERT_NOT_NULL(tab);
    YEW_ASSERT_NULL(yew_syn_state_get(tab, YEW_SYN_STATE_UNKNOWN));
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_U64(root->depth, 1U);
    YEW_ASSERT_EQ_U64(root->ctx[0], 7U);
    YEW_ASSERT_EQ_U64(root->lost, 0U);
    YEW_ASSERT_EQ_U64(root->def, 0U);
    copy = *root;
    YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &copy), YEW_SYN_STATE_ROOT);
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 2U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_equal_tuples_share_identity(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = {{0}, 91U, 1U, 0U, 0U, YEW_SYN_F_VALUE};
    u32 first;
    u32 i;

    first = yew_syn_state_intern(tab, &state);
    YEW_ASSERT(first > YEW_SYN_STATE_ROOT);
    for (i = 0U; i < 40U; i++)
        YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &state), first);
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 3U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_different_tuples_have_different_identities(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = {{0}, 0U, 1U, 0U, 0U, 0U};
    u32 ids[40];
    u32 i;
    u32 j;

    for (i = 0U; i < YEW_ARRAY_LEN(ids); i++) {
        state.aux = i + 1U;
        ids[i] = yew_syn_state_intern(tab, &state);
        YEW_ASSERT(ids[i] > YEW_SYN_STATE_ROOT);
        for (j = 0U; j < i; j++)
            YEW_ASSERT(ids[i] != ids[j]);
    }
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), 42U);
    yew_syn_state_tab_free(tab);
}

void test_syn_state_depth_cap_balances_refused_pushes(void)
{
    SynState state = {{0}, 0U, 1U, 0U, 0U, 0U};
    SynState entry = state;
    u32 i;

    for (i = 0U; i < 40U; i++)
        yew_syn_state_push(&state, (u16)(i + 1U));
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(state.lost, 40U - (YEW_SYN_DEPTH_MAX - 1U));
    for (i = 0U; i < 40U; i++)
        yew_syn_state_pop(&state, 1U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
    /* Removing the lost-first pop branch makes this exact balance fail. */
}

void test_syn_state_lost_saturates_and_marks_degraded(void)
{
    SynState state = {{0}, 0U, YEW_SYN_DEPTH_MAX, 0U, 0U, 0U};
    u32 i;

    for (i = 0U; i < YEW_SYN_DEPTH_MAX; i++)
        state.ctx[i] = (u16)i;
    for (i = 0U; i < 300U; i++)
        yew_syn_state_push(&state, 99U);
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(state.lost, YEW_SYN_LOST_MAX);
    YEW_ASSERT((state.flags & YEW_SYN_F_DEGRADED) != 0U);
    yew_syn_state_pop(&state, 4U);
    YEW_ASSERT_EQ_U64(state.lost, YEW_SYN_LOST_MAX - 4U);
    YEW_ASSERT_EQ_U64(state.depth, YEW_SYN_DEPTH_MAX);
}

void test_syn_state_pop_never_removes_root(void)
{
    SynState state = {{17U}, 12U, 1U, 0U, 0U, YEW_SYN_F_VALUE};
    SynState entry = state;

    yew_syn_state_pop(&state, 1U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
    yew_syn_state_pop(&state, 4U);
    YEW_ASSERT_EQ_MEM(&state, &entry, sizeof(state));
}

void test_syn_state_set_replaces_top_without_changing_depth(void)
{
    SynState state = {{3U, 4U, 5U}, 0U, 3U, 0U, 0U, 0U};

    yew_syn_state_set(&state, 77U);
    YEW_ASSERT_EQ_U64(state.depth, 3U);
    YEW_ASSERT_EQ_U64(state.ctx[0], 3U);
    YEW_ASSERT_EQ_U64(state.ctx[1], 4U);
    YEW_ASSERT_EQ_U64(state.ctx[2], 77U);
}

void test_syn_state_table_exhaustion_degrades_to_root(void)
{
    SynStateTab *tab = yew_syn_state_tab_new(0U);
    SynState state = {{0}, 0U, 1U, 0U, 0U, 0U};
    u32 last = 0U;
    u32 i;

    for (i = 1U; i < YEW_SYN_MAX_STATES + 8U; i++) {
        state.aux = i;
        last = yew_syn_state_intern(tab, &state);
    }
    YEW_ASSERT(yew_syn_state_exhausted(tab));
    YEW_ASSERT_EQ_U64(yew_syn_state_count(tab), YEW_SYN_MAX_STATES);
    YEW_ASSERT_EQ_U64(last, YEW_SYN_STATE_ROOT);
    YEW_ASSERT_NOT_NULL(yew_syn_state_get(tab, YEW_SYN_STATE_ROOT));
    yew_syn_state_tab_free(tab);
}

void test_syn_deferred_surfaces_fail_loudly(void)
{
    const SynDef *ini;

    YEW_ASSERT_EQ_U64(yew_syn_lang_for("example.xyz", NULL, 0U),
                      YEW_LANG_NONE);
    YEW_ASSERT_EQ_U64(yew_syn_lang_for("example.ini", NULL, 0U), 1U);
    ini = yew_syn_def_for(1U);
    YEW_ASSERT_NOT_NULL(ini);
    YEW_ASSERT_EQ_STR(ini->name, "ini");
    syn_expect_bug(syn_load_theme_bug, "Sprint 41");
    syn_expect_bug(syn_embedded_def_bug, "embedded definitions");
}
