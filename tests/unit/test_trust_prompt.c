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
    SagTrustDb db;
    SagTrustProbe probe;
    Ed ed;
} TrustPromptFix;

typedef struct TrustPromptResult {
    u32 calls;
    SagTrustAnswer answer;
} TrustPromptResult;

static Key tp_key(u32 code)
{
    Key key = {0};

    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.code = code;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static void tp_done(Ed *ed, SagTrustAnswer answer, void *ctx)
{
    TrustPromptResult *result = ctx;

    (void)ed;
    result->calls++;
    result->answer = answer;
}

static void tp_make(TrustPromptFix *f)
{
    const char *old = getenv("XDG_STATE_HOME");
    FILE *fp;

    (void)memset(f, 0, sizeof(*f));
    f->had_state_home = old != NULL;
    if (old != NULL)
        (void)snprintf(f->saved_state_home, sizeof(f->saved_state_home),
                       "%s", old);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-trust-prompt-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
    (void)snprintf(f->config, sizeof(f->config), "%s/.sagitta.fl",
                   f->state_home);
    fp = fopen(f->config, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT_EQ_U64(fwrite("set({wrap: true})\n", 1U, 18U, fp), 18U);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);

    sag_trust_db_init(&f->db);
    sag_trust_probe_init(&f->probe);
    f->probe.has_config = true;
    (void)snprintf(f->probe.workspace, sizeof(f->probe.workspace), "%s",
                   f->state_home);
    (void)snprintf(f->probe.config_path, sizeof(f->probe.config_path), "%s",
                   f->config);
    bytebuf_append(&f->probe.bytes, (const u8 *)"set({wrap: true})\n", 18U);

    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
}

static void tp_remove(TrustPromptFix *f)
{
    char trust[192];
    char sagitta[160];

    sag_ed_free(&f->ed);
    sag_trust_probe_free(&f->probe);
    sag_trust_db_free(&f->db);
    if (f->had_state_home)
        (void)setenv("XDG_STATE_HOME", f->saved_state_home, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    (void)snprintf(sagitta, sizeof(sagitta), "%s/sagitta", f->state_home);
    (void)snprintf(trust, sizeof(trust), "%s/trust.fl", sagitta);
    (void)unlink(trust);
    (void)rmdir(sagitta);
    (void)unlink(f->config);
    (void)rmdir(f->state_home);
}

void test_trust_prompt_routes_all_terminal_answers_through_event_loop(void)
{
    static const u32 keys[] = {'t', 'o', 'n', 's', SAG_KEY_ESCAPE};
    static const SagTrustAnswer answers[] = {
        SAG_TRUST_ALWAYS, SAG_TRUST_ONCE, SAG_TRUST_NEVER,
        SAG_TRUST_SKIP, SAG_TRUST_SKIP
    };
    TrustPromptFix f;
    u32 i;

    tp_make(&f);
    for (i = 0U; i < (u32)(sizeof(keys) / sizeof(keys[0])); i++) {
        TrustPromptResult result = {0};

        SAG_ASSERT(sag_trust_prompt_begin(
            &f.ed, &f.db, &f.probe, SAG_TRUST_PROMPT_NEW,
            tp_done, &result));
        SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_WORKSPACE_TRUST);
        SAG_ASSERT(f.ed.msg.prompt);
        SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.full == NULL ? f.ed.msg.text :
                                  f.ed.msg.full,
                                  "[t]rust always [o]nce [n]ever [v]iew [s]kip"));

        sag_ed_handle_key(&f.ed, tp_key(keys[i]), 10);
        SAG_ASSERT_EQ_U64(result.calls, 1U);
        SAG_ASSERT_EQ_I64(result.answer, answers[i]);
        SAG_ASSERT(!f.ed.trust_prompt.active);
        SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_NONE);
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
    SAG_ASSERT(sag_trust_prompt_begin(
        &f.ed, &f.db, &f.probe, SAG_TRUST_PROMPT_CHANGED,
        tp_done, &result));
    sag_ed_handle_key(&f.ed, tp_key((u32)'v'), 10);
    SAG_ASSERT(f.ed.trust_prompt.active);
    SAG_ASSERT(f.ed.trust_prompt.viewing);
    SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_NONE);
    SAG_ASSERT_NOT_NULL(f.ed.win);
    SAG_ASSERT_NOT_NULL(f.ed.win->buf);
    view_id = f.ed.win->buf->id;
    SAG_ASSERT_EQ_U64(view_id, f.ed.trust_prompt.view_buffer_id);
    SAG_ASSERT(sag_buf_readonly(f.ed.win->buf));
    SAG_ASSERT_EQ_U64(sag_buf_len(f.ed.win->buf), f.probe.bytes.len);

    close.ed = &f.ed;
    close.win = f.ed.win;
    SAG_ASSERT_EQ_I64(sag_file_cmd_buf_close(&close), SAG_CMD_OK);
    SAG_ASSERT(f.ed.trust_prompt.active);
    SAG_ASSERT(!f.ed.trust_prompt.viewing);
    SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_WORKSPACE_TRUST);
    SAG_ASSERT_EQ_U64(result.calls, 0U);

    sag_ed_handle_key(&f.ed, tp_key((u32)'o'), 20);
    SAG_ASSERT_EQ_U64(result.calls, 1U);
    SAG_ASSERT_EQ_I64(result.answer, SAG_TRUST_ONCE);
    tp_remove(&f);
}
