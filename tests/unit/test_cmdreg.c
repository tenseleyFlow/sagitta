#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "util/buf.h"

static u32 probe_calls;
static u32 probe_count;
static u32 tap_calls;
static CmdId tap_id;

static CmdStatus probe_repeat(CmdCtx *cx)
{
    probe_calls++;
    probe_count = cx->count;
    return SAG_CMD_OK;
}

static CmdStatus probe_takes_count(CmdCtx *cx)
{
    probe_calls++;
    probe_count = cx->count;
    return SAG_CMD_OK;
}

static void probe_tap(CmdId id, const CmdCtx *cx)
{
    tap_calls++;
    tap_id = id;
    probe_count = cx->count;
}

static void append_registry(Bytebuf *out)
{
    u32 i;

    bytebuf_init(out);
    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);

        SAG_ASSERT_NOT_NULL(desc);
        SAG_ASSERT_NOT_NULL(desc->name);
        SAG_ASSERT_NOT_NULL(desc->help);
        SAG_ASSERT(desc->help[0] != '\0');
        bytebuf_append(out, desc->name, strlen(desc->name) + 1U);
        bytebuf_append(out, desc->help, strlen(desc->help) + 1U);
    }
}

void test_cmd_registry_builtins_are_deterministic(void)
{
    Bytebuf first;
    Bytebuf second;
    u32 count;

    sag_cmd_shutdown();
    sag_cmd_init();
    count = sag_cmd_count();
    SAG_ASSERT(count >= 40U);
    append_registry(&first);
    sag_cmd_shutdown();
    sag_cmd_init();
    SAG_ASSERT_EQ_U64(sag_cmd_count(), count);
    append_registry(&second);
    SAG_ASSERT_EQ_U64(first.len, second.len);
    SAG_ASSERT_EQ_MEM(first.data, second.data, first.len);
    bytebuf_free(&second);
    bytebuf_free(&first);
}

void test_cmd_registry_invocation_and_deferred(void)
{
    Ed fake_ed = {0};
    Win fake_win = {0};
    static const CmdDesc repeat_desc = {
        "ed.ui.toggle", probe_repeat, SAG_ARITY_NONE,
        SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE, "Toggle the test probe",
    };
    static const CmdDesc count_desc = {
        "ed.ui.goto", probe_takes_count, SAG_ARITY_INT,
        SAG_CMD_TAKES_COUNT, "Send a count to the test probe",
    };
    static const struct {
        const char *mode;
        const char *sprint;
    } mode_rows[] = {
        {"L", "Sprint 14"}, {"I", "Sprint 14"},
        {"W", "Sprint 16"}, {"B", "Sprint 16"},
        {"H", "Sprint 17"}, {"E", "Sprint 18"},
        {"F", "Sprint 52"},
    };
    CmdCtx cx = {0};
    CmdId repeat;
    CmdId takes;
    u32 i;

    sag_cmd_shutdown();
    sag_cmd_init();
    fake_ed.win = &fake_win;
    repeat = sag_cmd_register(&repeat_desc);
    takes = sag_cmd_register(&count_desc);
    cx.count = 4U;
    cx.count_given = true;
    cx.source = SAG_SRC_TEST;
    probe_calls = 0U;
    probe_count = 0U;
    tap_calls = 0U;
    sag_cmd_set_record_tap(probe_tap);
    SAG_ASSERT_EQ_I64(sag_cmd_invoke(repeat, &cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(probe_calls, 4U);
    SAG_ASSERT_EQ_U64(probe_count, 4U);
    SAG_ASSERT_EQ_U64(tap_calls, 1U);
    SAG_ASSERT_EQ_U64(tap_id.v, repeat.v);

    cx.iarg = 7;
    probe_calls = 0U;
    SAG_ASSERT_EQ_I64(sag_cmd_invoke(takes, &cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(probe_calls, 1U);
    SAG_ASSERT_EQ_U64(probe_count, 4U);
    SAG_ASSERT_EQ_U64(tap_calls, 1U);
    sag_cmd_set_record_tap(NULL);

    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);
        CmdCtx deferred = {0};
        char fake_win;

        if ((desc->flags & SAG_CMD_DEFERRED) == 0U)
            continue;
        deferred.count = 1U;
        deferred.source = SAG_SRC_TEST;
        deferred.win = (Win *)(void *)&fake_win;
        if (desc->arity == SAG_ARITY_INT ||
            desc->arity == SAG_ARITY_OPT_INT)
            deferred.iarg = 1;
        if (desc->arity == SAG_ARITY_STR ||
            desc->arity == SAG_ARITY_OPT_STR) {
            deferred.sarg = "x";
            deferred.sarg_len = 1U;
        }
        sag_test_capture_log();
        SAG_ASSERT_EQ_I64(sag_cmd_invoke((CmdId){i + 1U}, &deferred),
                          SAG_CMD_ERR_DEFERRED);
        SAG_ASSERT(sag_test_log_contains(SAG_LOG_ERROR, desc->name));
        SAG_ASSERT(sag_test_log_contains(SAG_LOG_ERROR, "Sprint"));
    }
    for (i = 0U; i < SAG_ARRAY_LEN(mode_rows); i++) {
        CmdCtx mode = {0};
        CmdId enter = sag_cmd_lookup("ed.mode.enter", 13U);

        mode.count = 1U;
        mode.ed = &fake_ed;
        mode.source = SAG_SRC_TEST;
        mode.sarg = mode_rows[i].mode;
        mode.sarg_len = 1U;
        sag_test_capture_log();
        if (i < 5U) {
            SAG_ASSERT_EQ_I64(sag_cmd_invoke(enter, &mode), SAG_CMD_OK);
            SAG_ASSERT_EQ_U64(
                fake_ed.mode,
                mode_rows[i].mode[0] == 'L' ? SAG_MODE_L :
                mode_rows[i].mode[0] == 'I' ? SAG_MODE_I :
                mode_rows[i].mode[0] == 'W' ? SAG_MODE_W :
                mode_rows[i].mode[0] == 'B' ? SAG_MODE_B : SAG_MODE_H);
        } else {
            SAG_ASSERT_EQ_I64(sag_cmd_invoke(enter, &mode),
                              SAG_CMD_ERR_DEFERRED);
            SAG_ASSERT(sag_test_log_contains(SAG_LOG_ERROR,
                                             mode_rows[i].sprint));
        }
    }
    sag_keymap_free(&fake_ed.mode_keys[SAG_MODE_H]);
    sag_cmd_shutdown();
}

static int descriptor_child_exit(const CmdDesc *desc, bool register_twice)
{
    pid_t child;
    pid_t waited;
    int status;

    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(STDERR_FILENO);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        (void)sag_cmd_register(desc);
        if (register_twice)
            (void)sag_cmd_register(desc);
        _exit(99);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_cmd_registry_rejects_invalid_descriptors(void)
{
    static const char *const bad_names[] = {
        "ed", "ed.", "ed.Move.x", "ed.move.a.b.c.d",
        "ed.move.abcdefghijklmnopq", "ed.rogue.open", "ed.ui.unknown",
    };
    CmdDesc desc = {
        "ed.ui.toggle", probe_repeat, SAG_ARITY_NONE, 0U, "test command",
    };
    size_t i;

    sag_cmd_shutdown();
    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(bad_names); i++) {
        desc.name = bad_names[i];
        SAG_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), SAG_EXIT_BUG);
    }
    desc.name = "ed.ui.toggle";
    desc.flags = SAG_CMD_REPEATABLE | SAG_CMD_TAKES_COUNT;
    SAG_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), SAG_EXIT_BUG);
    desc.flags = 0U;
    desc.help = "";
    SAG_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), SAG_EXIT_BUG);
    desc.help = "test command";
    desc.fn = NULL;
    SAG_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), SAG_EXIT_BUG);
    desc.fn = probe_repeat;
    SAG_ASSERT_EQ_I64(descriptor_child_exit(&desc, true), SAG_EXIT_BUG);
}
