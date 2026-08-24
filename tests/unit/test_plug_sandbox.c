#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/gc.h"
#include "fl/flruntime.h"
#include "fl/origin.h"
#include "fl/vm.h"
#include "mod/plug/internal.h"
#include "ws/trust.h"

typedef struct SandboxFix {
    Ed ed;
    PlugSys sys;
    Plug plug;
    Plug *row;
    FlVm *vm;
    char state_home[160];
    char trust_dir[192];
    char trust_path[224];
    char *old_state_home;
} SandboxFix;

static void sf_open(SandboxFix *f)
{
    const char *old = getenv("XDG_STATE_HOME");

    (void)memset(f, 0, sizeof(*f));
    if (old != NULL)
        f->old_state_home = strdup(old);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/yew-plug-cap-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
    (void)snprintf(f->trust_dir, sizeof(f->trust_dir), "%s/yew",
                   f->state_home);
    (void)snprintf(f->trust_path, sizeof(f->trust_path), "%s/trust.fl",
                   f->trust_dir);

    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    f->vm = yew_fl_vm(&f->ed);
    YEW_ASSERT_NOT_NULL(f->vm);
    f->row = &f->plug;
    f->sys.v = &f->row;
    f->sys.n = 1U;
    f->plug.mf.name_text = "cap-test";
    f->plug.origin_id = fl_origin_register(&f->ed, FL_ORIGIN_PLUGIN,
                                            "cap-test", 0U);
    f->vm->root_origin = (FlOrigin){
        (u8)FL_ORIGIN_PLUGIN,
        yew_intern_cstr(&f->ed.interner, "cap-test"),
        0U,
        f->plug.origin_id
    };
    f->ed.plug = &f->sys;
}

static void sf_close(SandboxFix *f)
{
    f->ed.plug = NULL;
    yew_ed_free(&f->ed);
    if (f->old_state_home != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->old_state_home, 1),
                          0);
        free(f->old_state_home);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    }
    (void)unlink(f->trust_path);
    (void)rmdir(f->trust_dir);
    (void)rmdir(f->state_home);
}

static const char *sf_error_msg(FlVm *vm)
{
    FlValue key;
    FlValue value = FL_NIL_V;

    if (vm->err.t != (u8)FL_MAP)
        return NULL;
    key = FL_OBJ_V(FL_STR, fl_str_new(vm, "msg", 3U));
    if (!fl_map_get((FlMap *)vm->err.as.o, key, &value) ||
        value.t != (u8)FL_STR)
        return NULL;
    return ((FlStr *)value.as.o)->b;
}

static const char *sf_message(const Ed *ed)
{
    return ed->msg.full == NULL ? ed->msg.text : ed->msg.full;
}

void test_plug_sandbox_capability_mappings_are_closed(void)
{
    YEW_ASSERT_EQ_U64(yew_plug_cap_fl_mask(YEW_CAP_FS),
                      FL_CAP_FS_READ | FL_CAP_FS_WRITE);
    YEW_ASSERT_EQ_U64(yew_plug_cap_fl_mask(YEW_CAP_SHELL), FL_CAP_SHELL);
    YEW_ASSERT_EQ_U64(yew_plug_cap_fl_mask(YEW_CAP_NET), FL_CAP_NET);
    YEW_ASSERT_EQ_U64(yew_plug_cap_fl_mask(YEW_CAP_CLIPBOARD),
                      FL_CAP_CLIPBOARD);
    YEW_ASSERT_EQ_U64(yew_plug_cap_fl_mask(YEW_CAP__N), 0U);
    YEW_ASSERT_EQ_I64(yew_plug_trust_cap(YEW_CAP_FS), YEW_PLUGIN_CAP_FS);
    YEW_ASSERT_EQ_I64(yew_plug_trust_cap(YEW_CAP_SHELL),
                      YEW_PLUGIN_CAP_SHELL);
    YEW_ASSERT_EQ_I64(yew_plug_trust_cap(YEW_CAP_NET), YEW_PLUGIN_CAP_NET);
    YEW_ASSERT_EQ_I64(yew_plug_trust_cap(YEW_CAP_CLIPBOARD),
                      YEW_PLUGIN_CAP_CLIPBOARD);
}

void test_plug_sandbox_declared_headless_prompt_and_persistence_policy(void)
{
    SandboxFix f;
    YewTrustDb db;

    sf_open(&f);
    f.plug.mf.caps_wanted =
        (1U << YEW_CAP_FS) | (1U << YEW_CAP_SHELL) |
        (1U << YEW_CAP_CLIPBOARD);

    /* Undeclared is an immediate, exact error and never a question. */
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_NET));
    YEW_ASSERT_EQ_STR(sf_error_msg(f.vm),
                      "plugin \"cap-test\" did not declare net");
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);

    /* Headless declared use denies without touching prompt state. */
    f.ed.headless = true;
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_FS_READ));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    f.ed.headless = false;

    /* The triggering call is denied; lower-case allow authorizes retry. */
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_FS_WRITE));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_PLUGIN_CAP);
    YEW_ASSERT_EQ_STR(
        sf_message(&f.ed),
        "plugin \"cap-test\" requests fs (read/write files outside open "
        "buffers) — [a]llow once  [A]llow always  [d]eny once  "
        "[D]eny always");
    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'a'));
    YEW_ASSERT(fl_cap_check(f.vm, FL_CAP_FS_READ | FL_CAP_FS_WRITE));

    /* Upper-case allow persists and remains effective without session state. */
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_SHELL));
    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'A'));
    f.plug.session_allow &= ~(1U << YEW_CAP_SHELL);
    YEW_ASSERT(fl_cap_check(f.vm, FL_CAP_SHELL));

    /* A once-deny lasts the session; an explicit pregrant overrides it. */
    f.plug.mf.caps_wanted |= 1U << YEW_CAP_NET;
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_NET));
    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'d'));
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_NET));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    YEW_ASSERT(yew_plug_session_grant(&f.ed, "cap-test", "net"));
    YEW_ASSERT(fl_cap_check(f.vm, FL_CAP_NET));

    /* Upper-case deny persists and is consulted after session state clears. */
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_CLIPBOARD));
    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'D'));
    f.plug.session_deny &= ~(1U << YEW_CAP_CLIPBOARD);
    YEW_ASSERT(!fl_cap_check(f.vm, FL_CAP_CLIPBOARD));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);

    yew_trust_db_init(&db);
    YEW_ASSERT(yew_trust_db_load(&db));
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "cap-test", YEW_PLUGIN_CAP_SHELL),
                      YEW_PLUGIN_GRANT_ALLOW);
    YEW_ASSERT_EQ_I64(yew_trust_plugin_capability(
                          &db, "cap-test", YEW_PLUGIN_CAP_CLIPBOARD),
                      YEW_PLUGIN_GRANT_DENY);
    yew_trust_db_free(&db);
    sf_close(&f);
}
