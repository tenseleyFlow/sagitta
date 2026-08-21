#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/flconf.h"

typedef struct FlconfAiFix {
    Ed ed;
    char root[160];
    char state[192];
    char workspace[192];
    char *old_state;
} FlconfAiFix;

static char *flconf_ai_env_copy(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = yew_xmalloc(strlen(value) + 1U);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void flconf_ai_init(FlconfAiFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-flconf-ai-XXXXXX");
    if (mkdtemp(f->root) == NULL)
        YEW_BUG("AI trust config fixture mkdtemp failed");
    (void)snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    (void)snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    if (mkdir(f->state, 0700) != 0 || mkdir(f->workspace, 0700) != 0)
        YEW_BUG("AI trust config fixture mkdir failed");
    f->old_state = flconf_ai_env_copy("XDG_STATE_HOME");
    if (setenv("XDG_STATE_HOME", f->state, 1) != 0)
        YEW_BUG("AI trust config fixture environment failed");
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    if (!yew_ed_open_scratch(&f->ed))
        YEW_BUG("AI trust config fixture editor failed");
    yew_config_init(&f->ed, NULL);
}

static void flconf_ai_free(FlconfAiFix *f)
{
    char trust_dir[224];
    char trust_file[256];

    yew_ed_free(&f->ed);
    (void)snprintf(trust_dir, sizeof(trust_dir), "%s/yew", f->state);
    (void)snprintf(trust_file, sizeof(trust_file), "%s/trust.fl", trust_dir);
    (void)unlink(trust_file);
    (void)rmdir(trust_dir);
    (void)rmdir(f->workspace);
    (void)rmdir(f->state);
    (void)rmdir(f->root);
    if (f->old_state == NULL)
        (void)unsetenv("XDG_STATE_HOME");
    else {
        (void)setenv("XDG_STATE_HOME", f->old_state, 1);
        free(f->old_state);
    }
}

static void flconf_ai_reopen_config(FlconfAiFix *f)
{
    yew_config_free(&f->ed);
    yew_config_init(&f->ed, NULL);
}

void test_flconf_ai_workspace_grants_follow_config_lifecycle(void)
{
    FlconfAiFix f;

    flconf_ai_init(&f);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&f.ed),
                      YEW_AI_WS_UNSET);

    YEW_ASSERT(yew_config_ai_workspace_set(&f.ed, YEW_AI_WS_DENY));
    flconf_ai_reopen_config(&f);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&f.ed),
                      YEW_AI_WS_DENY);

    YEW_ASSERT(yew_config_ai_workspace_set(&f.ed, YEW_AI_WS_ALLOW));
    flconf_ai_reopen_config(&f);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&f.ed),
                      YEW_AI_WS_ALLOW);

    YEW_ASSERT(yew_config_ai_workspace_set(&f.ed, YEW_AI_WS_UNSET));
    flconf_ai_reopen_config(&f);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&f.ed),
                      YEW_AI_WS_UNSET);

    YEW_ASSERT(yew_config_ai_workspace_set(&f.ed, YEW_AI_WS_ALLOW));
    YEW_ASSERT(yew_config_ai_workspace_forget(&f.ed));
    flconf_ai_reopen_config(&f);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&f.ed),
                      YEW_AI_WS_UNSET);
    flconf_ai_free(&f);
}

void test_flconf_ai_workspace_grants_require_config_state(void)
{
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(NULL),
                      YEW_AI_WS_UNSET);
    YEW_ASSERT_EQ_I64(yew_config_ai_workspace_grant(&ed),
                      YEW_AI_WS_UNSET);
    YEW_ASSERT(!yew_config_ai_workspace_set(NULL, YEW_AI_WS_ALLOW));
    YEW_ASSERT(!yew_config_ai_workspace_set(&ed, YEW_AI_WS_ALLOW));
    YEW_ASSERT(!yew_config_ai_workspace_forget(NULL));
    YEW_ASSERT(!yew_config_ai_workspace_forget(&ed));
    yew_ed_free(&ed);
}
