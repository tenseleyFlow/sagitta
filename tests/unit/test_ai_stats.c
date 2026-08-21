#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/ai/stats.h"

static const char *stats_message(const Ed *ed)
{
    return ed->msg.full == NULL ? ed->msg.text : ed->msg.full;
}

static CmdStatus stats_show(Ed *ed)
{
    CmdCtx context = {0};

    context.ed = ed;
    context.win = ed->win;
    context.count = 1U;
    context.source = YEW_SRC_TEST;
    return yew_ai_cmd_stats(&context);
}

static char *stats_env_save(void)
{
    const char *value = getenv("XDG_STATE_HOME");

    return value == NULL ? NULL : strdup(value);
}

static void stats_env_restore(char *saved)
{
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
}

void test_ai_stats_persist_and_render_local_fletch_data(void)
{
    char root[] = "/tmp/yew-ai-stats-XXXXXX";
    char dir[sizeof(root) + sizeof("/yew")];
    char file[sizeof(dir) + sizeof("/ai_stats.fl")];
    char *saved = stats_env_save();
    Ed first;
    Ed second;
    const char *message;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    yew_ed_init(&first);
    yew_ai_stats_request(&first, "local");
    yew_ai_stats_request(&first, "local");
    yew_ai_stats_delivery(&first, "local", 40U);
    yew_ai_stats_accept(&first, "local", 0U, 7U);
    yew_ai_stats_finish(&first, "local", 104, 11, 13);
    YEW_ASSERT_EQ_I64(stats_show(&first), YEW_CMD_OK);
    message = stats_message(&first);
    YEW_ASSERT_NOT_NULL(strstr(message, "backend  requests"));
    YEW_ASSERT_NOT_NULL(strstr(message, "local"));
    YEW_ASSERT_NOT_NULL(strstr(message, "      2"));
    YEW_ASSERT_NOT_NULL(strstr(message, "100.0%"));
    YEW_ASSERT_NOT_NULL(strstr(message, "104 ms"));
    yew_ed_free(&first);

    (void)snprintf(dir, sizeof(dir), "%s/yew", root);
    (void)snprintf(file, sizeof(file), "%s/ai_stats.fl", dir);
    YEW_ASSERT_EQ_I64(access(file, R_OK), 0);
    yew_ed_init(&second);
    YEW_ASSERT_EQ_I64(stats_show(&second), YEW_CMD_OK);
    message = stats_message(&second);
    YEW_ASSERT_NOT_NULL(strstr(message, "local"));
    YEW_ASSERT_NOT_NULL(strstr(message, "104 ms"));
    YEW_ASSERT_NOT_NULL(strstr(message, "         13"));
    yew_ed_free(&second);

    YEW_ASSERT_EQ_I64(unlink(file), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    stats_env_restore(saved);
}

void test_ai_stats_missing_file_is_an_empty_success(void)
{
    char root[] = "/tmp/yew-ai-stats-empty-XXXXXX";
    char *saved = stats_env_save();
    Ed ed;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", root, 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(stats_show(&ed), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(stats_message(&ed),
                               "(no AI requests recorded)"));
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    stats_env_restore(saved);
}
