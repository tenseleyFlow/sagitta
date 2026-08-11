#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/file_cmds.h"
#include "ws/trust_prompt.h"

typedef struct TrustPromptFix {
    char state_home[128];
    char config[192];
    char saved_state_home[512];
    bool had_state_home;
    YewTrustDb db;
    YewTrustProbe probe;
    Ed ed;
} TrustPromptFix;

typedef struct TrustPromptResult {
    u32 calls;
    YewTrustAnswer answer;
} TrustPromptResult;

static Key tp_key(u32 code)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = code;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static void tp_done(Ed *ed, YewTrustAnswer answer, void *ctx)
{
    TrustPromptResult *result = ctx;

    (void)ed;
    result->calls++;
    result->answer = answer;
}

static void tp_make(TrustPromptFix *f)
{
    static const char state_template[] = "/tmp/yew-trust-prompt-XXXXXX";
    const char *old = getenv("XDG_STATE_HOME");
    FILE *fp;

    _Static_assert(sizeof(state_template) <= sizeof(f->state_home),
                   "trust prompt state template exceeds fixture storage");
    (void)memset(f, 0, sizeof(*f));
    f->had_state_home = old != NULL;
    if (old != NULL)
        (void)snprintf(f->saved_state_home, sizeof(f->saved_state_home),
                       "%s", old);
    (void)memcpy(f->state_home, state_template, sizeof(state_template));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
    (void)snprintf(f->config, sizeof(f->config), "%s/.yew.fl",
                   f->state_home);
    fp = fopen(f->config, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite("set({wrap: true})\n", 1U, 18U, fp), 18U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);

    yew_trust_db_init(&f->db);
    yew_trust_probe_init(&f->probe);
    f->probe.has_config = true;
    (void)snprintf(f->probe.workspace, sizeof(f->probe.workspace), "%s",
                   f->state_home);
    (void)snprintf(f->probe.config_path, sizeof(f->probe.config_path), "%s",
                   f->config);
    bytebuf_append(&f->probe.bytes, (const u8 *)"set({wrap: true})\n", 18U);

    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
}

static void tp_remove(TrustPromptFix *f)
{
    char trust[192];
    char yew[160];

    yew_ed_free(&f->ed);
    yew_trust_probe_free(&f->probe);
    yew_trust_db_free(&f->db);
    if (f->had_state_home)
        (void)setenv("XDG_STATE_HOME", f->saved_state_home, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    (void)snprintf(yew, sizeof(yew), "%s/yew", f->state_home);
    (void)snprintf(trust, sizeof(trust), "%s/trust.fl", yew);
    (void)unlink(trust);
    (void)rmdir(yew);
    (void)unlink(f->config);
    (void)rmdir(f->state_home);
}

void test_trust_prompt_routes_all_terminal_answers_through_event_loop(void)
{
    static const u32 keys[] = {'t', 'o', 'n', 's', YEW_KEY_ESCAPE};
    static const YewTrustAnswer answers[] = {
        YEW_TRUST_ALWAYS, YEW_TRUST_ONCE, YEW_TRUST_NEVER,
        YEW_TRUST_SKIP, YEW_TRUST_SKIP
    };
    TrustPromptFix f;
    u32 i;

    tp_make(&f);
    for (i = 0U; i < (u32)(sizeof(keys) / sizeof(keys[0])); i++) {
        TrustPromptResult result = {0};

        YEW_ASSERT(yew_trust_prompt_begin(
            &f.ed, &f.db, &f.probe, YEW_TRUST_PROMPT_NEW,
            tp_done, &result));
        YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_WORKSPACE_TRUST);
        YEW_ASSERT(f.ed.msg.prompt);
        YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.full == NULL ? f.ed.msg.text :
                                  f.ed.msg.full,
                                  "[t]rust always [o]nce [n]ever [v]iew [s]kip"));

        yew_ed_handle_key(&f.ed, tp_key(keys[i]), 10);
        YEW_ASSERT_EQ_U64(result.calls, 1U);
        YEW_ASSERT_EQ_I64(result.answer, answers[i]);
        YEW_ASSERT(!f.ed.trust_prompt.active);
        YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    }
    tp_remove(&f);
}

void test_trust_prompt_view_is_readonly_and_reprompts_after_close(void)
{
    TrustPromptFix f;
    TrustPromptResult result = {0};
    CmdCtx close = {0};
    u32 view_id;

    tp_make(&f);
    YEW_ASSERT(yew_trust_prompt_begin(
        &f.ed, &f.db, &f.probe, YEW_TRUST_PROMPT_CHANGED,
        tp_done, &result));
    yew_ed_handle_key(&f.ed, tp_key((u32)'v'), 10);
    YEW_ASSERT(f.ed.trust_prompt.active);
    YEW_ASSERT(f.ed.trust_prompt.viewing);
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    YEW_ASSERT_NOT_NULL(f.ed.win);
    YEW_ASSERT_NOT_NULL(f.ed.win->buf);
    view_id = f.ed.win->buf->id;
    YEW_ASSERT_EQ_U64(view_id, f.ed.trust_prompt.view_buffer_id);
    YEW_ASSERT(yew_buf_readonly(f.ed.win->buf));
    YEW_ASSERT_EQ_U64(yew_buf_len(f.ed.win->buf), f.probe.bytes.len);

    close.ed = &f.ed;
    close.win = f.ed.win;
    YEW_ASSERT_EQ_I64(yew_file_cmd_buf_close(&close), YEW_CMD_OK);
    YEW_ASSERT(f.ed.trust_prompt.active);
    YEW_ASSERT(!f.ed.trust_prompt.viewing);
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_WORKSPACE_TRUST);
    YEW_ASSERT_EQ_U64(result.calls, 0U);

    yew_ed_handle_key(&f.ed, tp_key((u32)'o'), 20);
    YEW_ASSERT_EQ_U64(result.calls, 1U);
    YEW_ASSERT_EQ_I64(result.answer, YEW_TRUST_ONCE);
    tp_remove(&f);
}
