#include "ws/trust_prompt.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "edit/ed.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/message.h"

static bool trust_reason_prompts(YewTrustDecision reason)
{
    return reason == YEW_TRUST_PROMPT_NEW ||
           reason == YEW_TRUST_PROMPT_CHANGED ||
           reason == YEW_TRUST_PROMPT_REPLACED;
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
    YewTrustPrompt *prompt = &ed->trust_prompt;
    char size[32];
    char age[48];

    ed->prompt = YEW_PROMPT_WORKSPACE_TRUST;
    trust_size_text(prompt->probe->bytes.len, size);
    trust_age_text(prompt->probe->config_path, age);
    yew_msg(ed, YEW_MSG_WARN,
            "workspace config: %s (%s, modified %s); %s; "
            "[t]rust always [o]nce [n]ever [v]iew [s]kip",
            prompt->probe->config_path, size, age,
            yew_trust_decision_reason(prompt->reason));
    ed->msg.prompt = true;
}

static void trust_prompt_finish(Ed *ed, YewTrustAnswer answer)
{
    YewTrustPromptDone done = ed->trust_prompt.done;
    void *ctx = ed->trust_prompt.ctx;

    (void)memset(&ed->trust_prompt, 0, sizeof(ed->trust_prompt));
    ed->prompt = YEW_PROMPT_NONE;
    yew_msg_clear(ed);
    if (done != NULL)
        done(ed, answer, ctx);
}

static bool trust_prompt_persist(Ed *ed, YewTrustAnswer answer)
{
    YewTrustPrompt *prompt = &ed->trust_prompt;
    time_t now = time(NULL);

    if (!yew_trust_answer(prompt->db, prompt->probe, answer, now) ||
        !yew_trust_db_write(prompt->db, now,
                            YEW_TRUST_PRUNE_DAYS_DEFAULT)) {
        yew_msg(ed, YEW_MSG_ERROR,
                "could not persist workspace trust decision; "
                "[t]rust always [o]nce [n]ever [v]iew [s]kip");
        ed->msg.prompt = true;
        return false;
    }
    return true;
}

static bool trust_prompt_view(Ed *ed)
{
    YewTrustPrompt *prompt = &ed->trust_prompt;
    Buffer *view;
    char name[PATH_MAX + 32U];

    if (!ed->model_ready)
        return false;
    (void)snprintf(name, sizeof(name), "*trust: %s*",
                   prompt->probe->config_path);
    view = yew_ws_scratch_new(ed, name,
                              YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (view == NULL)
        return false;
    yew_textbuf_insert(view->tb, BYTEOFF(0U), prompt->probe->bytes.data,
                       (u64)prompt->probe->bytes.len);
    yew_undo_mark_saved(view->undo);
    if (!yew_ed_show_buffer(ed, view)) {
        yew_ws_scratch_drop(ed, view);
        return false;
    }
    prompt->viewing = true;
    prompt->view_buffer_id = view->id;
    ed->prompt = YEW_PROMPT_NONE;
    yew_msg_clear(ed);
    yew_msg(ed, YEW_MSG_INFO,
            "viewing workspace config; close this buffer to answer trust prompt");
    return true;
}

bool yew_trust_prompt_begin(Ed *ed, YewTrustDb *db,
                            const YewTrustProbe *probe,
                            YewTrustDecision reason,
                            YewTrustPromptDone done, void *ctx)
{
    if (ed == NULL || db == NULL || probe == NULL || !probe->has_config ||
        !trust_reason_prompts(reason) || ed->trust_prompt.active ||
        ed->prompt != YEW_PROMPT_NONE)
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

bool yew_trust_prompt_key(Ed *ed, u8 answer)
{
    YewTrustPrompt *prompt;

    if (ed == NULL || !ed->trust_prompt.active ||
        ed->trust_prompt.viewing)
        return false;
    prompt = &ed->trust_prompt;
    switch (answer) {
    case 't':
        if (trust_prompt_persist(ed, YEW_TRUST_ALWAYS))
            trust_prompt_finish(ed, YEW_TRUST_ALWAYS);
        return true;
    case 'o':
        if (yew_trust_answer(prompt->db, prompt->probe, YEW_TRUST_ONCE,
                             time(NULL)))
            trust_prompt_finish(ed, YEW_TRUST_ONCE);
        return true;
    case 'n':
        if (trust_prompt_persist(ed, YEW_TRUST_NEVER))
            trust_prompt_finish(ed, YEW_TRUST_NEVER);
        return true;
    case 'v':
        if (!trust_prompt_view(ed))
            trust_prompt_show(ed);
        return true;
    case 's':
    case 0x1BU:
        (void)yew_trust_answer(prompt->db, prompt->probe, YEW_TRUST_SKIP,
                               time(NULL));
        trust_prompt_finish(ed, YEW_TRUST_SKIP);
        return true;
    default:
        trust_prompt_show(ed);
        return true;
    }
}

void yew_trust_prompt_buffer_closed(Ed *ed, u32 buffer_id)
{
    if (ed == NULL || !ed->trust_prompt.active ||
        !ed->trust_prompt.viewing ||
        ed->trust_prompt.view_buffer_id != buffer_id)
        return;
    ed->trust_prompt.viewing = false;
    ed->trust_prompt.view_buffer_id = 0U;
    trust_prompt_show(ed);
}

void yew_trust_prompt_cancel(Ed *ed)
{
    if (ed == NULL)
        return;
    if (ed->prompt == YEW_PROMPT_WORKSPACE_TRUST)
        ed->prompt = YEW_PROMPT_NONE;
    (void)memset(&ed->trust_prompt, 0, sizeof(ed->trust_prompt));
}
