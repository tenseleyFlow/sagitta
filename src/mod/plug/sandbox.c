#define _POSIX_C_SOURCE 200809L

/*
 * Plugin capability policy.
 *
 * Plugins share the editor process and VM.  These checks cover only the
 * four named host I/O surfaces; they provide neither memory nor resource
 * isolation.  The current call is denied before an interactive question is
 * shown.  An allow answer authorizes a later retry, so no Fletch frame is
 * resumed after its transaction has already rolled back.
 */

#include "mod/plug/internal.h"

#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "fl/origin.h"
#include "fl/vm.h"
#include "ui/message.h"

u32 yew_plug_cap_fl_mask(YewCap cap)
{
    switch (cap) {
    case YEW_CAP_FS:        return FL_CAP_FS_READ | FL_CAP_FS_WRITE;
    case YEW_CAP_SHELL:     return FL_CAP_SHELL;
    case YEW_CAP_NET:       return FL_CAP_NET;
    case YEW_CAP_CLIPBOARD: return FL_CAP_CLIPBOARD;
    default:                return 0U;
    }
}

YewPluginCapability yew_plug_trust_cap(YewCap cap)
{
    switch (cap) {
    case YEW_CAP_FS:        return YEW_PLUGIN_CAP_FS;
    case YEW_CAP_SHELL:     return YEW_PLUGIN_CAP_SHELL;
    case YEW_CAP_NET:       return YEW_PLUGIN_CAP_NET;
    case YEW_CAP_CLIPBOARD: return YEW_PLUGIN_CAP_CLIPBOARD;
    default:                return YEW_PLUGIN_CAP_FS;
    }
}

static Plug *plug_by_origin(Ed *ed, u32 origin_id)
{
    u32 i;

    if (ed == NULL || ed->plug == NULL)
        return NULL;
    for (i = 0U; i < ed->plug->n; i++) {
        Plug *plug = ed->plug->v[i];

        if (plug != NULL && plug->origin_id == origin_id)
            return plug;
    }
    return NULL;
}

static const char *cap_detail(YewCap cap)
{
    switch (cap) {
    case YEW_CAP_FS:        return "read/write files outside open buffers";
    case YEW_CAP_SHELL:     return "run shell commands";
    case YEW_CAP_NET:       return "access the network";
    case YEW_CAP_CLIPBOARD: return "read/write the system clipboard";
    default:                return "use a host I/O surface";
    }
}

static YewPluginGrant persisted_grant(const Plug *plug, YewCap cap)
{
    YewTrustDb db;
    YewPluginGrant grant = YEW_PLUGIN_GRANT_UNSET;

    if (plug == NULL || plug->mf.name_text == NULL)
        return grant;
    yew_trust_db_init(&db);
    if (yew_trust_db_load(&db))
        grant = yew_trust_plugin_capability(&db, plug->mf.name_text,
                                            yew_plug_trust_cap(cap));
    yew_trust_db_free(&db);
    return grant;
}

static bool persist_grant(const Plug *plug, YewCap cap,
                          YewPluginGrant grant)
{
    YewTrustDb db;
    time_t now = time(NULL);
    bool ok = false;

    if (plug == NULL || plug->mf.name_text == NULL || now == (time_t)-1)
        return false;
    yew_trust_db_init(&db);
    if (yew_trust_db_load(&db) &&
        yew_trust_plugin_set_capability(&db, plug->mf.name_text,
                                        yew_plug_trust_cap(cap), grant)) {
        ok = yew_trust_db_write(&db, now,
                                YEW_TRUST_PRUNE_DAYS_DEFAULT);
    }
    yew_trust_db_free(&db);
    return ok;
}

static bool cap_denied(FlVm *vm, const Plug *plug, YewCap cap)
{
    return fl_raise(vm, "capability", "plugin \"%s\" denied %s",
                    plug->mf.name_text, yew_cap_name(cap));
}

static bool cap_one(FlVm *vm, Plug *plug, YewCap cap)
{
    Ed *ed = vm->ed;
    u32 bit = 1U << (u32)cap;
    YewPluginGrant persisted;

    if ((plug->mf.caps_wanted & bit) == 0U) {
        return fl_raise(vm, "capability",
                        "plugin \"%s\" did not declare %s",
                        plug->mf.name_text, yew_cap_name(cap));
    }
    if ((plug->session_allow & bit) != 0U)
        return true;
    if ((plug->session_deny & bit) != 0U)
        return cap_denied(vm, plug, cap);
    persisted = persisted_grant(plug, cap);
    if (persisted == YEW_PLUGIN_GRANT_ALLOW)
        return true;
    if (persisted == YEW_PLUGIN_GRANT_DENY || ed->headless)
        return cap_denied(vm, plug, cap);
    if (ed->plug->prompt.active || ed->prompt != YEW_PROMPT_NONE)
        return cap_denied(vm, plug, cap);

    ed->plug->prompt.plug = plug;
    ed->plug->prompt.cap = cap;
    ed->plug->prompt.active = true;
    ed->plug->prompt.retry_enable = false;
    yew_ed_prompt(ed, YEW_PROMPT_PLUGIN_CAP);
    yew_msg(ed, YEW_MSG_WARN,
            "plugin \"%s\" requests %s (%s) — [a]llow once  "
            "[A]llow always  [d]eny once  [D]eny always",
            plug->mf.name_text, yew_cap_name(cap), cap_detail(cap));
    return cap_denied(vm, plug, cap);
}

bool yew_plug_cap_check(FlVm *vm, u32 need)
{
    Plug *plug;
    u32 origin_id;
    u32 covered = 0U;
    u32 cap;

    if (vm == NULL || vm->ed == NULL || vm->ed->plug == NULL)
        return false;
    origin_id = fl_origin_of_frame(vm);
    plug = plug_by_origin(vm->ed, origin_id);
    if (plug == NULL || plug->mf.name_text == NULL)
        return fl_raise(vm, "capability", "%s denied to a plugin",
                        fl_cap_name(need));
    for (cap = 0U; cap < (u32)YEW_CAP__N; cap++) {
        u32 mask = yew_plug_cap_fl_mask((YewCap)cap);

        covered |= mask;
        if ((need & mask) != 0U && !cap_one(vm, plug, (YewCap)cap))
            return false;
    }
    if ((need & ~covered) != 0U)
        return fl_raise(vm, "capability", "%s denied to plugin \"%s\"",
                        fl_cap_name(need), plug->mf.name_text);
    return true;
}

bool yew_plug_session_grant(Ed *ed, const char *plugin, const char *cap)
{
    Plug *p;
    YewCap parsed;
    u32 bit;

    if (ed == NULL || plugin == NULL || cap == NULL ||
        !yew_cap_parse(cap, strlen(cap), &parsed))
        return false;
    p = NULL;
    {
        u32 i;

        for (i = 0U; ed->plug != NULL && i < ed->plug->n; i++) {
            Plug *candidate = ed->plug->v[i];

            if (candidate != NULL && candidate->mf.name_text != NULL &&
                strcmp(candidate->mf.name_text, plugin) == 0) {
                p = candidate;
                break;
            }
        }
    }
    bit = 1U << (u32)parsed;
    if (p == NULL || (p->mf.caps_wanted & bit) == 0U)
        return false;
    p->session_allow |= bit;
    p->session_deny &= ~bit;
    return true;
}

static void prompt_close(Ed *ed)
{
    (void)memset(&ed->plug->prompt, 0, sizeof(ed->plug->prompt));
    yew_ed_prompt(ed, YEW_PROMPT_NONE);
}

bool yew_plug_prompt_key(Ed *ed, u32 code)
{
    PlugPrompt *prompt;
    Plug *plug;
    u32 bit;
    bool allow;
    bool always;

    if (ed == NULL || ed->plug == NULL || !ed->plug->prompt.active)
        return false;
    prompt = &ed->plug->prompt;
    plug = prompt->plug;
    if (plug == NULL) {
        prompt_close(ed);
        return true;
    }
    if (code == 0x1BU) {
        prompt_close(ed);
        return true;
    }
    if (code != (u32)'a' && code != (u32)'A' &&
        code != (u32)'d' && code != (u32)'D') {
        yew_msg(ed, YEW_MSG_WARN,
                "plugin \"%s\" requests %s (%s) — [a]llow once  "
                "[A]llow always  [d]eny once  [D]eny always",
                plug->mf.name_text, yew_cap_name(prompt->cap),
                cap_detail(prompt->cap));
        return true;
    }
    allow = code == (u32)'a' || code == (u32)'A';
    always = code == (u32)'A' || code == (u32)'D';
    if (always && !persist_grant(plug, prompt->cap,
                                 allow ? YEW_PLUGIN_GRANT_ALLOW :
                                         YEW_PLUGIN_GRANT_DENY)) {
        yew_msg(ed, YEW_MSG_ERROR,
                "cannot persist %s decision for plugin \"%s\"",
                yew_cap_name(prompt->cap), plug->mf.name_text);
        return true;
    }
    bit = 1U << (u32)prompt->cap;
    if (allow) {
        plug->session_allow |= bit;
        plug->session_deny &= ~bit;
    } else {
        plug->session_deny |= bit;
        plug->session_allow &= ~bit;
    }
    prompt_close(ed);
    return true;
}
