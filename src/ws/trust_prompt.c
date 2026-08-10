#include "ws/trust_prompt.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "edit/ed.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/message.h"

static bool trust_reason_prompts(SagTrustDecision reason)
{
    return reason == SAG_TRUST_PROMPT_NEW ||
           reason == SAG_TRUST_PROMPT_CHANGED ||
           reason == SAG_TRUST_PROMPT_REPLACED;
}

static void trust_size_text(size_t bytes, char out[32])
{
    if (bytes < 1024U) {
        (void)snprintf(out, 32U, "%lu B", (unsigned long)bytes);
    } else {
        unsigned long tenths = ((unsigned long)bytes * 10UL + 512UL) /
                               1024UL;

        (void)snprintf(out, 32U, "%lu.%lu KiB", tenths / 10UL,
                       tenths % 10UL);
    }
}

static void trust_age_text(const char *path, char out[48])
{
    struct stat st;
    time_t now = time(NULL);
    time_t age;

    if (stat(path, &st) != 0 || now == (time_t)-1) {
        (void)snprintf(out, 48U, "unknown");
        return;
    }
    age = now > st.st_mtime ? now - st.st_mtime : 0;
    if (age < (time_t)60)
        (void)snprintf(out, 48U, "just now");
    else if (age < (time_t)(60 * 60))
        (void)snprintf(out, 48U, "%ld minutes ago", (long)(age / 60));
    else if (age < (time_t)(24 * 60 * 60))
        (void)snprintf(out, 48U, "%ld hours ago",
                       (long)(age / (60 * 60)));
    else
        (void)snprintf(out, 48U, "%ld days ago",
                       (long)(age / (24 * 60 * 60)));
}

static void trust_prompt_show(Ed *ed)
{
    SagTrustPrompt *prompt = &ed->trust_prompt;
    char size[32];
    char age[48];

    ed->prompt = SAG_PROMPT_WORKSPACE_TRUST;
    trust_size_text(prompt->probe->bytes.len, size);
    trust_age_text(prompt->probe->config_path, age);
    sag_msg(ed, SAG_MSG_WARN,
            "workspace config: %s (%s, modified %s); %s; "
            "[t]rust always [o]nce [n]ever [v]iew [s]kip",
            prompt->probe->config_path, size, age,
            sag_trust_decision_reason(prompt->reason));
    ed->msg.prompt = true;
}

static void trust_prompt_finish(Ed *ed, SagTrustAnswer answer)
{
    SagTrustPromptDone done = ed->trust_prompt.done;
    void *ctx = ed->trust_prompt.ctx;

    (void)memset(&ed->trust_prompt, 0, sizeof(ed->trust_prompt));
    ed->prompt = SAG_PROMPT_NONE;
    sag_msg_clear(ed);
    if (done != NULL)
        done(ed, answer, ctx);
}

static bool trust_prompt_persist(Ed *ed, SagTrustAnswer answer)
{
    SagTrustPrompt *prompt = &ed->trust_prompt;
    time_t now = time(NULL);

    if (!sag_trust_answer(prompt->db, prompt->probe, answer, now) ||
        !sag_trust_db_write(prompt->db, now,
                            SAG_TRUST_PRUNE_DAYS_DEFAULT)) {
        sag_msg(ed, SAG_MSG_ERROR,
                "could not persist workspace trust decision; "
                "[t]rust always [o]nce [n]ever [v]iew [s]kip");
        ed->msg.prompt = true;
        return false;
    }
    return true;
}

static bool trust_prompt_view(Ed *ed)
{
    SagTrustPrompt *prompt = &ed->trust_prompt;
    Buffer *view;
    char name[PATH_MAX + 32U];

    if (!ed->model_ready)
        return false;
    (void)snprintf(name, sizeof(name), "*trust: %s*",
                   prompt->probe->config_path);
    view = sag_ws_scratch_new(ed, name,
                              SAG_BUF_NOUNDO | SAG_BUF_READONLY);
    if (view == NULL)
        return false;
    sag_textbuf_insert(view->tb, BYTEOFF(0U), prompt->probe->bytes.data,
                       (u64)prompt->probe->bytes.len);
    sag_undo_mark_saved(view->undo);
    if (!sag_ed_show_buffer(ed, view)) {
        sag_ws_scratch_drop(ed, view);
        return false;
    }
    prompt->viewing = true;
    prompt->view_buffer_id = view->id;
    ed->prompt = SAG_PROMPT_NONE;
    sag_msg_clear(ed);
    sag_msg(ed, SAG_MSG_INFO,
            "viewing workspace config; close this buffer to answer trust prompt");
    return true;
}

bool sag_trust_prompt_begin(Ed *ed, SagTrustDb *db,
                            const SagTrustProbe *probe,
                            SagTrustDecision reason,
                            SagTrustPromptDone done, void *ctx)
{
    if (ed == NULL || db == NULL || probe == NULL || !probe->has_config ||
        !trust_reason_prompts(reason) || ed->trust_prompt.active ||
        ed->prompt != SAG_PROMPT_NONE)
        return false;
    ed->trust_prompt.db = db;
    ed->trust_prompt.probe = probe;
    ed->trust_prompt.done = done;
    ed->trust_prompt.ctx = ctx;
    ed->trust_prompt.reason = reason;
    ed->trust_prompt.active = true;
    trust_prompt_show(ed);
    return true;
}

bool sag_trust_prompt_key(Ed *ed, u8 answer)
{
    SagTrustPrompt *prompt;

    if (ed == NULL || !ed->trust_prompt.active ||
        ed->trust_prompt.viewing)
        return false;
    prompt = &ed->trust_prompt;
    switch (answer) {
    case 't':
        if (trust_prompt_persist(ed, SAG_TRUST_ALWAYS))
            trust_prompt_finish(ed, SAG_TRUST_ALWAYS);
        return true;
    case 'o':
        if (sag_trust_answer(prompt->db, prompt->probe, SAG_TRUST_ONCE,
                             time(NULL)))
            trust_prompt_finish(ed, SAG_TRUST_ONCE);
        return true;
    case 'n':
        if (trust_prompt_persist(ed, SAG_TRUST_NEVER))
            trust_prompt_finish(ed, SAG_TRUST_NEVER);
        return true;
    case 'v':
        if (!trust_prompt_view(ed))
            trust_prompt_show(ed);
        return true;
    case 's':
    case 0x1BU:
        (void)sag_trust_answer(prompt->db, prompt->probe, SAG_TRUST_SKIP,
                               time(NULL));
        trust_prompt_finish(ed, SAG_TRUST_SKIP);
        return true;
    default:
        trust_prompt_show(ed);
        return true;
    }
}

void sag_trust_prompt_buffer_closed(Ed *ed, u32 buffer_id)
{
    if (ed == NULL || !ed->trust_prompt.active ||
        !ed->trust_prompt.viewing ||
        ed->trust_prompt.view_buffer_id != buffer_id)
        return;
    ed->trust_prompt.viewing = false;
    ed->trust_prompt.view_buffer_id = 0U;
    trust_prompt_show(ed);
}

void sag_trust_prompt_cancel(Ed *ed)
{
    if (ed == NULL)
        return;
    if (ed->prompt == SAG_PROMPT_WORKSPACE_TRUST)
        ed->prompt = SAG_PROMPT_NONE;
    (void)memset(&ed->trust_prompt, 0, sizeof(ed->trust_prompt));
}
