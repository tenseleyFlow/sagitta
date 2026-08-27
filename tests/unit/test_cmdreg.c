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
static CmdId probe_invoked_id;

static CmdStatus probe_repeat(CmdCtx *cx)
{
    probe_calls++;
    probe_count = cx->count;
    probe_invoked_id = cx->invoked_id;
    return YEW_CMD_OK;
}

static CmdStatus probe_takes_count(CmdCtx *cx)
{
    probe_calls++;
    probe_count = cx->count;
    return YEW_CMD_OK;
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
    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);

        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_NOT_NULL(desc->name);
        YEW_ASSERT_NOT_NULL(desc->help);
        YEW_ASSERT(desc->help[0] != '\0');
        bytebuf_append(out, desc->name, strlen(desc->name) + 1U);
        bytebuf_append(out, desc->help, strlen(desc->help) + 1U);
    }
}

void test_cmd_registry_builtins_are_deterministic(void)
{
    Bytebuf first;
    Bytebuf second;
    u32 count;

    yew_cmd_shutdown();
    yew_cmd_init();
    count = yew_cmd_count();
    YEW_ASSERT(count >= 40U);
    append_registry(&first);
    yew_cmd_shutdown();
    yew_cmd_init();
    YEW_ASSERT_EQ_U64(yew_cmd_count(), count);
    append_registry(&second);
    YEW_ASSERT_EQ_U64(first.len, second.len);
    YEW_ASSERT_EQ_MEM(first.data, second.data, first.len);
    YEW_ASSERT_NOT_NULL(yew_cmd_desc(yew_cmd_lookup(
        "ed.git.open_split_h", (u32)strlen("ed.git.open_split_h"))));
    YEW_ASSERT_NOT_NULL(yew_cmd_desc(yew_cmd_lookup(
        "ed.git.open_split_v", (u32)strlen("ed.git.open_split_v"))));
    bytebuf_free(&second);
    bytebuf_free(&first);
}

void test_cmd_registry_invocation_and_deferred(void)
{
    Ed fake_ed = {0};
    Win fake_win = {0};
    static const CmdDesc repeat_desc = {
        "ed.ui.toggle", probe_repeat, YEW_ARITY_NONE,
        YEW_CMD_REPEATABLE | YEW_CMD_RECORDABLE, "Toggle the test probe",
        "toggle_probe"
    };
    static const CmdDesc count_desc = {
        "ed.ui.goto", probe_takes_count, YEW_ARITY_INT,
        YEW_CMD_TAKES_COUNT, "Send a count to the test probe", NULL
    };
    static const char *const mode_rows[] = {"L", "I", "W", "B", "H"};
    CmdCtx cx = {0};
    CmdId repeat;
    CmdId takes;
    u32 active_before;
    u32 i;

    yew_cmd_shutdown();
    yew_cmd_init();
    active_before = yew_cmd_active_count();
    fake_ed.win = &fake_win;
    repeat = yew_cmd_register(&repeat_desc);
    takes = yew_cmd_register(&count_desc);
    cx.count = 4U;
    cx.count_given = true;
    cx.source = YEW_SRC_TEST;
    probe_calls = 0U;
    probe_count = 0U;
    probe_invoked_id = YEW_CMD_NONE;
    tap_calls = 0U;
    yew_cmd_set_record_tap(probe_tap);
    YEW_ASSERT_EQ_I64(yew_cmd_invoke(repeat, &cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(probe_calls, 4U);
    YEW_ASSERT_EQ_U64(probe_count, 4U);
    YEW_ASSERT_EQ_U64(tap_calls, 1U);
    YEW_ASSERT_EQ_U64(tap_id.v, repeat.v);
    YEW_ASSERT_EQ_U64(probe_invoked_id.v, repeat.v);

    YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before + 2U);
    YEW_ASSERT(yew_cmd_unregister(repeat));
    YEW_ASSERT(!yew_cmd_unregister(repeat));
    YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before + 1U);
    YEW_ASSERT_EQ_U64(yew_cmd_lookup(repeat_desc.name,
                                     (u32)strlen(repeat_desc.name)).v,
                      YEW_CMD_NONE.v);
    YEW_ASSERT(yew_cmd_desc(repeat) == NULL);
    YEW_ASSERT(yew_cmd_entry(repeat) == NULL);
    {
        CmdId reactivated = yew_cmd_register(&repeat_desc);

        YEW_ASSERT_EQ_U64(reactivated.v, repeat.v);
        YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before + 2U);
        YEW_ASSERT_EQ_U64(yew_cmd_lookup(repeat_desc.name,
                                         (u32)strlen(repeat_desc.name)).v,
                          repeat.v);
    }

    cx.iarg = 7;
    probe_calls = 0U;
    YEW_ASSERT_EQ_I64(yew_cmd_invoke(takes, &cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(probe_calls, 1U);
    YEW_ASSERT_EQ_U64(probe_count, 4U);
    YEW_ASSERT_EQ_U64(tap_calls, 1U);
    yew_cmd_set_record_tap(NULL);

    YEW_ASSERT(yew_cmd_unregister(repeat));
    YEW_ASSERT(yew_cmd_unregister(takes));
    YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before);

    {
        static const char too_long[] =
            "abcdefghijklmnopqrstuvwxyzabcdefg";
        char err[80];
        CmdId plugin = {(u32)999U};
        CmdId reactivated;

        YEW_ASSERT(!yew_cmd_register_plugin(
            "Bad", "toggle", probe_repeat, "Plugin probe", &plugin, err,
            sizeof(err)));
        YEW_ASSERT_EQ_U64(plugin.v, YEW_CMD_NONE.v);
        YEW_ASSERT(err[0] != '\0');
        YEW_ASSERT(!yew_cmd_register_plugin(
            "sample", too_long, probe_repeat, "Plugin probe", &plugin, err,
            sizeof(err)));
        YEW_ASSERT_EQ_U64(plugin.v, YEW_CMD_NONE.v);
        YEW_ASSERT(yew_cmd_register_plugin(
            "sample", "toggle", probe_repeat, "Plugin probe", &plugin, err,
            sizeof(err)));
        YEW_ASSERT(plugin.v != YEW_CMD_NONE.v);
        YEW_ASSERT_EQ_STR(yew_cmd_desc(plugin)->name,
                          "ed.plug.sample.toggle");
        YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before + 1U);
        YEW_ASSERT(!yew_cmd_register_plugin(
            "sample", "toggle", probe_repeat, "Plugin probe", &reactivated,
            err, sizeof(err)));
        YEW_ASSERT_EQ_U64(reactivated.v, YEW_CMD_NONE.v);
        YEW_ASSERT_NOT_NULL(strstr(err, "already registered"));
        YEW_ASSERT(yew_cmd_unregister(plugin));
        YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before);
        YEW_ASSERT(yew_cmd_register_plugin(
            "sample", "toggle", probe_repeat, "Plugin probe", &reactivated,
            err, sizeof(err)));
        YEW_ASSERT_EQ_U64(reactivated.v, plugin.v);
        YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before + 1U);
        YEW_ASSERT(yew_cmd_unregister(reactivated));
        YEW_ASSERT_EQ_U64(yew_cmd_active_count(), active_before);
    }

    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);
        CmdCtx deferred = {0};
        char fake_win;

        if ((desc->flags & YEW_CMD_DEFERRED) == 0U)
            continue;
#if !YEW_WITH_FUSS
        /* The stripped Git boundary deliberately takes precedence over the
         * later Sprint 52/53 refusal. test_git_commands_cross_module_boundary
         * pins that exact module message and status for every Git command. */
        if (strncmp(desc->name, "ed.git.", 7U) == 0)
            continue;
#endif
        deferred.count = 1U;
        deferred.source = YEW_SRC_TEST;
        deferred.win = (Win *)(void *)&fake_win;
        if (desc->arity == YEW_ARITY_INT ||
            desc->arity == YEW_ARITY_OPT_INT)
            deferred.iarg = 1;
        if (desc->arity == YEW_ARITY_STR ||
            desc->arity == YEW_ARITY_OPT_STR) {
            deferred.sarg = "x";
            deferred.sarg_len = 1U;
        }
        yew_test_capture_log();
        YEW_ASSERT_EQ_I64(yew_cmd_invoke((CmdId){i + 1U}, &deferred),
                          YEW_CMD_ERR_DEFERRED);
        YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR, desc->name));
        YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR, "Sprint"));
    }
    /* Sprint 38 closes Sprint 18.5's command-palette deferral. */
    {
        CmdId palette = yew_cmd_lookup("ed.find.command", 15U);
        const CmdDesc *desc = yew_cmd_desc(palette);

        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT((desc->flags & YEW_CMD_PROMPTS) != 0U);
        YEW_ASSERT_EQ_STR(desc->help, "Open the command palette");
    }
    {
        CmdId theme = yew_cmd_lookup("ed.theme.set", 12U);
        const CmdDesc *desc = yew_cmd_desc(theme);

        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT_EQ_U64(desc->arity, YEW_ARITY_STR);
        YEW_ASSERT_NOT_NULL(yew_cmd_desc(
            yew_cmd_lookup("ed.theme.toggle", 15U)));
    }
    {
        static const struct {
            const char *compat;
            const char *canonical;
        } aliases[] = {
            {"ed.file.open", "ed.buf.open"},
            {"ed.file.close", "ed.buf.close"},
            {"ed.buf.next", "ed.tab.next"},
            {"ed.buf.prev", "ed.tab.prev"},
            {"ed.group.next", "ed.file.next"},
            {"ed.group.prev", "ed.file.prev"},
            {"ed.find.symbol", "ed.lsp.symbols"},
            {"ed.win.next", "ed.pane.focus_next"},
            {"ed.win.prev", "ed.pane.focus_prev"}
        };
        size_t alias_i;

        for (alias_i = 0U; alias_i < YEW_ARRAY_LEN(aliases); alias_i++) {
            const CmdDesc *compat = yew_cmd_desc(yew_cmd_lookup(
                aliases[alias_i].compat, strlen(aliases[alias_i].compat)));
            const CmdDesc *canonical = yew_cmd_desc(yew_cmd_lookup(
                aliases[alias_i].canonical,
                strlen(aliases[alias_i].canonical)));

            YEW_ASSERT_NOT_NULL(compat);
            YEW_ASSERT_NOT_NULL(canonical);
            YEW_ASSERT((compat->flags & YEW_CMD_DEFERRED) == 0U);
            YEW_ASSERT(compat->fn == canonical->fn);
        }
    }
    for (i = 0U; i < YEW_ARRAY_LEN(mode_rows); i++) {
        CmdCtx mode = {0};
        CmdId enter = yew_cmd_lookup("ed.mode.enter", 13U);

        mode.count = 1U;
        mode.ed = &fake_ed;
        mode.source = YEW_SRC_TEST;
        mode.sarg = mode_rows[i];
        mode.sarg_len = 1U;
        YEW_ASSERT_EQ_I64(yew_cmd_invoke(enter, &mode), YEW_CMD_OK);
        YEW_ASSERT_EQ_U64(
            fake_ed.mode,
            mode_rows[i][0] == 'L' ? YEW_MODE_L :
            mode_rows[i][0] == 'I' ? YEW_MODE_I :
            mode_rows[i][0] == 'W' ? YEW_MODE_W :
            mode_rows[i][0] == 'B' ? YEW_MODE_B : YEW_MODE_H);
    }
    {
        const CmdDesc *stage = yew_cmd_desc(
            yew_cmd_lookup("ed.git.stage", 12U));
        const CmdDesc *hunk = yew_cmd_desc(
            yew_cmd_lookup("ed.git.hunk.stage", 17U));

        YEW_ASSERT_NOT_NULL(stage);
        YEW_ASSERT((stage->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT((stage->flags & YEW_CMD_RECORDABLE) != 0U);
        YEW_ASSERT_NOT_NULL(stage->word);
        YEW_ASSERT(stage->help[0] != '\0');
        YEW_ASSERT_NOT_NULL(hunk);
        YEW_ASSERT((hunk->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT((hunk->flags & YEW_CMD_MULTI_AGGREGATE) != 0U);
        YEW_ASSERT_NOT_NULL(hunk->help);
        YEW_ASSERT(hunk->help[0] != '\0');
    }
    yew_keymap_free(&fake_ed.mode_keys[YEW_MODE_H]);
    yew_cmd_shutdown();
}

static int descriptor_child_exit(const CmdDesc *desc, bool register_twice)
{
    pid_t child;
    pid_t waited;
    int status;

    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(STDERR_FILENO);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        (void)yew_cmd_register(desc);
        if (register_twice)
            (void)yew_cmd_register(desc);
        _exit(99);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_cmd_registry_rejects_invalid_descriptors(void)
{
    static const char *const bad_names[] = {
        "ed", "ed.", "ed.Move.x", "ed.move.a.b.c.d",
        "ed.move.abcdefghijklmnopq", "ed.rogue.open", "ed.ui.unknown",
    };
    CmdDesc desc = {
        "ed.ui.toggle", probe_repeat, YEW_ARITY_NONE, 0U, "test command", NULL
    };
    size_t i;

    yew_cmd_shutdown();
    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(bad_names); i++) {
        desc.name = bad_names[i];
        YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    }
    desc.name = "ed.ui.toggle";
    desc.flags = YEW_CMD_REPEATABLE | YEW_CMD_TAKES_COUNT;
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.flags = 0U;
    desc.help = "";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.help = "test command";
    desc.fn = NULL;
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.fn = probe_repeat;
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, true), YEW_EXIT_BUG);
}

/*
 * Sprint 34 §3 / DoD 6: the CMDWORD bijection.
 *
 * Sprint 35's round-trip law says a recorded macro and a hand-typed
 * motion block are the same thing to the editor.  That needs word ->
 * command to be one-to-one over the recordable set, and the only
 * durable way to keep it so is to refuse the registration -- a law
 * checked in the recorder is one that the next command silently
 * breaks.
 */
void test_cmd_registry_enforces_cmdwords(void)
{
    CmdDesc desc = {
        "ed.ui.toggle", probe_repeat, YEW_ARITY_NONE, YEW_CMD_RECORDABLE,
        "test command", NULL
    };

    yew_cmd_shutdown();
    yew_cmd_init();
    /* Recordable without a word. */
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    /* A word on a command that is not recordable: the reverse map
     * would then hold a word no recording can ever produce. */
    desc.flags = 0U;
    desc.word = "toggle_it";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    /* Bad shapes. */
    desc.flags = YEW_CMD_RECORDABLE;
    desc.word = "Toggle";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.word = "9lives";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.word = "has.dot";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.word = "";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    desc.word = "abcdefghijklmnopq";      /* 17 */
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    /* Colliding with a word a builtin already owns. */
    desc.word = "yank";
    YEW_ASSERT_EQ_I64(descriptor_child_exit(&desc, false), YEW_EXIT_BUG);
    /* A good one registers, and round-trips. */
    desc.word = "toggle_it";
    {
        CmdId id = yew_cmd_register(&desc);

        YEW_ASSERT(id.v != 0U);
        YEW_ASSERT_EQ_U64(yew_cmd_by_word("toggle_it", 9U).v, id.v);
    }
    yew_cmd_shutdown();
    yew_cmd_init();
}

/*
 * Every registered command round-trips through its own word, and every
 * recordable one has a word.  This is the assertion DoD 6 asks for, and
 * it covers the whole table rather than a sample.
 */
void test_cmd_registry_word_roundtrip(void)
{
    u32 i;
    u32 n;
    u32 recordable = 0U;
    u32 worded = 0U;

    yew_cmd_shutdown();
    yew_cmd_init();
    n = yew_cmd_count();
    YEW_ASSERT(n > 100U);
    for (i = 0U; i < n; i++) {
        const CmdDesc *d = yew_cmd_at(i);
        CmdId back;

        if ((d->flags & YEW_CMD_RECORDABLE) != 0U) {
            recordable++;
            YEW_ASSERT_NOT_NULL(d->word);
        }
        if (d->word == NULL)
            continue;
        worded++;
        back = yew_cmd_by_word(d->word, (u32)strlen(d->word));
        /* Same DESCRIPTOR, not merely a command with that word: an
         * alias pair sharing a word would pass an id comparison
         * against either one. */
        YEW_ASSERT(yew_cmd_desc(back) == d);
    }
    /* No unrecordable command carries one, so the two counts agree and
     * the map has no entries a recording could not produce. */
    YEW_ASSERT_EQ_U64(worded, recordable);
    YEW_ASSERT_EQ_U64(yew_cmd_by_word("no_such_word", 12U).v, 0U);
    YEW_ASSERT_EQ_U64(yew_cmd_by_word(NULL, 0U).v, 0U);
}
