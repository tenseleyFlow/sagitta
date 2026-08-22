#include "gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/mode.h"

typedef enum {
    RT_GEN_MOTION,
    RT_GEN_INSERT,
    RT_GEN_DELETE,
    RT_GEN_MODE,
    RT_GEN_SELECTION,
    RT_GEN_YANK
} RtGenKind;

typedef struct {
    const char *name;
    RtGenKind kind;
} RtGenCmd;

/*
 * This is intentionally small, not silently permissive.  The property
 * runner regenerates failed operations, so every generated event is a
 * successful, argument-complete command.  Registry coverage below is the
 * complementary guard: a new recordable command must either join this table
 * or match a named exclusion whose state is outside the property envelope.
 */
static const RtGenCmd gen_cmds[] = {
    {"ed.move.unit.next", RT_GEN_MOTION},
    {"ed.move.unit.prev", RT_GEN_MOTION},
    {"ed.move.unit.up", RT_GEN_MOTION},
    {"ed.move.unit.down", RT_GEN_MOTION},
    {"ed.move.unit.next_alt", RT_GEN_MOTION},
    {"ed.move.unit.prev_alt", RT_GEN_MOTION},
    {"ed.move.buf.end", RT_GEN_MOTION},
    {"ed.move.line.home", RT_GEN_MOTION},
    {"ed.move.line.end", RT_GEN_MOTION},
    {"ed.edit.insert.text", RT_GEN_INSERT},
    {"ed.edit.insert.at", RT_GEN_INSERT},
    {"ed.edit.delete.unit", RT_GEN_DELETE},
    {"ed.edit.delete.span", RT_GEN_DELETE},
    {"ed.edit.replace.span", RT_GEN_INSERT},
    {"ed.mode.enter", RT_GEN_MODE},
    {"ed.mode.escape", RT_GEN_SELECTION},
    {"ed.sel.expand", RT_GEN_SELECTION},
    {"ed.sel.yank", RT_GEN_YANK}
};

static u64 xorshift64(u64 *state)
{
    u64 x = *state;

    if (x == 0U)
        x = UINT64_C(0x9e3779b97f4a7c15);
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    *state = x;
    return x;
}

static u32 rnd(u64 *state, u32 n)
{
    return n == 0U ? 0U : (u32)(xorshift64(state) % n);
}

void rt_session_init(RtSession *session)
{
    (void)memset(session, 0, sizeof(*session));
}

void rt_session_free(RtSession *session)
{
    RtEventVec_free(&session->events);
    free(session->storage);
    (void)memset(session, 0, sizeof(*session));
}

static const u8 *store_bytes(RtSession *session, const u8 *bytes, u32 len)
{
    u32 at = session->storage_len;

    if (len > UINT32_MAX - session->storage_len)
        return NULL;
    if (session->storage_len + len > session->storage_cap) {
        u32 cap = session->storage_cap == 0U ? 256U : session->storage_cap;

        while (cap < session->storage_len + len) {
            if (cap > UINT32_MAX / 2U)
                cap = session->storage_len + len;
            else
                cap *= 2U;
        }
        session->storage = yew_xrealloc(session->storage, cap);
        session->storage_cap = cap;
    }
    if (len != 0U)
        (void)memcpy(session->storage + at, bytes, len);
    session->storage_len += len;
    return session->storage + at;
}

static const RtGenCmd *choose_cmd(u64 *state)
{
    u32 roll = rnd(state, 100U);
    RtGenKind want = roll < 45U ? RT_GEN_MOTION
                     : roll < 65U ? RT_GEN_INSERT
                     : roll < 77U ? RT_GEN_DELETE
                     : roll < 87U ? RT_GEN_SELECTION
                     : roll < 95U ? RT_GEN_YANK
                                   : RT_GEN_MODE;
    u32 start = rnd(state, (u32)YEW_ARRAY_LEN(gen_cmds));
    u32 i;

    for (i = 0U; i < (u32)YEW_ARRAY_LEN(gen_cmds); i++) {
        const RtGenCmd *cmd = &gen_cmds[(start + i) % YEW_ARRAY_LEN(gen_cmds)];

        if (cmd->kind == want)
            return cmd;
    }
    return &gen_cmds[0];
}

static bool append_named(RtSession *session, const char *name,
                         const u8 *sarg, u32 sarg_len)
{
    RtEvent ev = {0};

    ev.cmd = yew_cmd_lookup(name, (u32)strlen(name));
    ev.count = 1U;
    ev.sarg_len = sarg_len;
    if (sarg_len != 0U) {
        ev.sarg = store_bytes(session, sarg, sarg_len);
        if (ev.sarg == NULL)
            return false;
    }
    if (ev.cmd.v == 0U)
        return false;
    RtEventVec_push(&session->events, ev);
    return true;
}

static bool append_highlight_scenario(RtSession *session, bool yank)
{
    static const u8 highlight[] = {'H'};

    return append_named(session, "ed.mode.enter", highlight, 1U) &&
           append_named(session, "ed.move.unit.next", NULL, 0U) &&
           append_named(session, yank ? "ed.sel.yank" : "ed.sel.expand",
                        NULL, 0U) &&
           append_named(session, "ed.mode.escape", NULL, 0U);
}

static bool append_generated(RtSession *session, u64 *state,
                             const RtGenCmd *gc)
{
    static const u8 p_ascii[] = "x";
    static const u8 p_cjk[] = "\xe7\x8c\xab";
    static const u8 p_zwj[] = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
    static const u8 p_combining[] = "e\xcc\x81";
    static const u8 p_invalid[] = {0x80U};
    static const struct {
        const u8 *bytes;
        u32 len;
    } payloads[] = {
        {p_ascii, sizeof(p_ascii) - 1U},
        {p_cjk, sizeof(p_cjk) - 1U},
        {p_zwj, sizeof(p_zwj) - 1U},
        {p_combining, sizeof(p_combining) - 1U},
        {p_invalid, sizeof(p_invalid)}
    };
    RtEvent ev = {0};
    const CmdDesc *desc;
    u32 count_roll;

    ev.cmd = yew_cmd_lookup(gc->name, (u32)strlen(gc->name));
    desc = yew_cmd_desc(ev.cmd);
    if (desc == NULL)
        return false;
    ev.count = 1U;
    if (gc->kind == RT_GEN_INSERT) {
        u32 p = rnd(state, (u32)YEW_ARRAY_LEN(payloads));

        ev.sarg_len = payloads[p].len;
        ev.sarg = store_bytes(session, payloads[p].bytes, ev.sarg_len);
        if (ev.sarg == NULL)
            return false;
        if (strcmp(gc->name, "ed.edit.insert.at") == 0)
            ev.iarg = 0;
        if (strcmp(gc->name, "ed.edit.replace.span") == 0) {
            ev.range.given = true;
            ev.range.tok = (Span){0U, 0U};
        }
    } else if (strcmp(gc->name, "ed.edit.delete.span") == 0) {
        /* Empty span is valid for every fixture; nonempty range semantics
         * are locked by the integration suite without making generation
         * depend on the session's evolving buffer length. */
        ev.range.given = true;
        ev.range.tok = (Span){0U, 0U};
    } else if (gc->kind == RT_GEN_MODE) {
        static const u8 modes[] = {'L', 'W', 'B'};

        ev.sarg_len = 1U;
        ev.sarg = store_bytes(session, &modes[rnd(state, 3U)], 1U);
        if (ev.sarg == NULL)
            return false;
    } else if ((desc->flags &
                (YEW_CMD_REPEATABLE | YEW_CMD_TAKES_COUNT)) != 0U) {
        count_roll = rnd(state, 100U);
        if (count_roll >= 60U) {
            ev.count_given = true;
            ev.count = count_roll < 90U ? 2U + rnd(state, 8U)
                                         : 10U + rnd(state, 90U);
        }
    }
    RtEventVec_push(&session->events, ev);
    return true;
}

bool rt_session_generate(RtSession *session, u64 seed, u32 fixture,
                         u32 forced_len)
{
    u64 state = seed ^ (UINT64_C(0xd1b54a32d192ed03) * (fixture + 1U));
    u32 target = forced_len == 0U ? 1U + rnd(&state, 200U) : forced_len;
    bool has_text_insert = false;

    session->seed = seed;
    session->fixture = fixture % 6U;
    session->generated_len = target;
    session->start_mode = (u8)(YEW_MODE_L + rnd(&state, 3U));
    /* Largest generated payload is 11 bytes and sessions cap at 200. */
    session->storage_cap = 4096U;
    session->storage = yew_xmalloc(session->storage_cap);
    while (session->events.len + (has_text_insert ? 1U : 2U) < target) {
        const RtGenCmd *chosen = choose_cmd(&state);
        u32 reserve = has_text_insert ? 1U : 2U;
        u32 available = target - (u32)session->events.len - reserve;

        if ((chosen->kind == RT_GEN_SELECTION ||
             chosen->kind == RT_GEN_YANK) && available >= 4U) {
            if (!append_highlight_scenario(session,
                                           chosen->kind == RT_GEN_YANK))
                return false;
        } else {
            const RtGenCmd *one = chosen;

            if (chosen->kind == RT_GEN_SELECTION ||
                chosen->kind == RT_GEN_YANK)
                one = &gen_cmds[0];
            if (!append_generated(session, &state, one))
                return false;
            if (strcmp(one->name, "ed.edit.insert.text") == 0)
                has_text_insert = true;
        }
    }
    /* P2 needs an actual edit even when the random session chose motions. */
    if (!has_text_insert) {
        const RtGenCmd insert = {"ed.edit.insert.text", RT_GEN_INSERT};

        if (!append_generated(session, &state, &insert))
            return false;
    }
    /*
     * P5 composes S with itself.  Restore the starting unit explicitly so
     * the second direct S begins under the same unit that the emitted
     * macro's mode annotation establishes for its second invocation.
     */
    if (session->events.len < target) {
        RtEvent restore = {0};
        static const u8 modes[] = {'L', 'W', 'B'};

        restore.cmd = yew_cmd_lookup("ed.mode.enter",
                                     (u32)strlen("ed.mode.enter"));
        restore.count = 1U;
        restore.sarg_len = 1U;
        restore.sarg = store_bytes(session,
                                   &modes[session->start_mode - YEW_MODE_L],
                                   1U);
        if (restore.sarg == NULL)
            return false;
        RtEventVec_push(&session->events, restore);
    }
    return session->events.len == target;
}

typedef struct RtDenied {
    const char *name;
    const char *reason;
} RtDenied;

#define D(name_, reason_) {name_, reason_}
static const RtDenied denied[] = {
    D("ed.move.unit.up_alt", "redundant motion alias"),
    D("ed.move.unit.down_alt", "redundant motion alias"),
    D("ed.move.buf.home", "redundant motion alias"),
    D("ed.move.line.up", "redundant motion alias"),
    D("ed.move.line.down", "redundant motion alias"),
    D("ed.move.line.first_nonblank", "redundant motion alias"),
    D("ed.move.line.last_nonblank", "redundant motion alias"),
    D("ed.move.line.half_page_up", "viewport geometry is outside E0"),
    D("ed.move.line.half_page_down", "viewport geometry is outside E0"),
    D("ed.move.unit.home", "redundant motion alias"),
    D("ed.move.unit.end", "redundant motion alias"),
    D("ed.move.unit.home_alt", "redundant motion alias"),
    D("ed.move.unit.end_alt", "redundant motion alias"),
    D("ed.move.block.match_prev", "delimiter precondition is fixture-specific"),
    D("ed.move.block.match_next", "delimiter precondition is fixture-specific"),
    D("ed.move.word.sub_prev", "redundant motion alias"),
    D("ed.move.word.sub_next", "redundant motion alias"),
    D("ed.move.char.prev", "redundant motion alias"),
    D("ed.move.char.next", "redundant motion alias"),
    D("ed.move.char.left", "redundant motion alias"),
    D("ed.move.char.right", "redundant motion alias"),
    D("ed.edit.insert.newline", "native EOL metadata needs a dedicated pool"),
    D("ed.edit.insert.tab", "tab-width state is outside generated E0"),
    D("ed.edit.insert.after", "insert-mode transaction is not mode-closed"),
    D("ed.edit.line.open_below", "insert-mode transaction is not mode-closed"),
    D("ed.edit.line.open_above", "insert-mode transaction is not mode-closed"),
    D("ed.edit.delete.grapheme_left", "redundant delete alias"),
    D("ed.edit.delete.grapheme", "redundant delete alias"),
    D("ed.edit.line.delete", "line register semantics need a seeded paste command"),
    D("ed.edit.delete.prev", "redundant delete alias"),
    D("ed.edit.delete.next", "redundant delete alias"),
    D("ed.edit.undo", "history-dependent command"),
    D("ed.edit.redo", "history-dependent command"),
    D("ed.shadow.accept_word", "requires live suggestion state"),
    D("ed.shadow.accept_word_alt", "requires live suggestion state"),
    D("ed.shadow.accept_line", "requires live suggestion state"),
    D("ed.shadow.accept_all", "requires live suggestion state"),
    D("ed.sel.contract", "selection stack history is outside E0"),
    D("ed.sel.unit.expand", "alias of generated ed.sel.expand"),
    D("ed.sel.unit.contract", "selection stack history is outside E0"),
    D("ed.sel.kind", "argument-specific geometry needs a dedicated pool"),
    D("ed.sel.swap_ends", "redundant selection permutation"),
    D("ed.sel.delete", "covered indirectly; register deletion needs seeded paste"),
    D("ed.sel.change", "enters insert mode and is not mode-closed"),
    D("ed.sel.case_upper", "locale/case fixture needs a dedicated pool"),
    D("ed.sel.case_lower", "locale/case fixture needs a dedicated pool"),
    D("ed.sel.case_toggle", "locale/case fixture needs a dedicated pool"),
    D("ed.sel.indent", "indent option is outside generated E0"),
    D("ed.sel.dedent", "indent option is outside generated E0"),
    D("ed.sel.shift_left", "indent option is outside generated E0"),
    D("ed.sel.shift_right", "indent option is outside generated E0"),
    D("ed.sel.join", "linewise selection precondition is fixture-specific"),
    D("ed.sel.replace_char", "argument-specific selection edit"),
    D("ed.edit.rect.insert", "rectangular geometry needs a dedicated fixture"),
    D("ed.edit.rect.append", "rectangular geometry needs a dedicated fixture"),
    D("ed.cursor.lift.lines", "multi-cursor topology is separately tested"),
    D("ed.cursor.lift.matches", "search and multi-cursor state are outside E0"),
    D("ed.cursor.lift.ends", "multi-cursor topology is separately tested"),
    D("ed.cursor.add.above", "multi-cursor topology is separately tested"),
    D("ed.cursor.add.below", "multi-cursor topology is separately tested"),
    D("ed.cursor.drop", "multi-cursor topology is separately tested"),
    D("ed.cursor.collapse", "multi-cursor topology is separately tested"),
    D("ed.view.center", "viewport geometry is outside compared E0"),
    D("ed.view.top", "viewport geometry is outside compared E0"),
    D("ed.view.bottom", "viewport geometry is outside compared E0"),
    D("ed.view.scroll.up", "viewport geometry is outside compared E0"),
    D("ed.view.scroll.down", "viewport geometry is outside compared E0"),
    D("ed.view.up", "viewport geometry is outside compared E0"),
    D("ed.view.down", "viewport geometry is outside compared E0"),
    D("ed.view.page_up", "viewport geometry is outside compared E0"),
    D("ed.view.page_down", "viewport geometry is outside compared E0"),
    D("ed.view.half_page_up", "viewport geometry is outside compared E0"),
    D("ed.view.half_page_down", "viewport geometry is outside compared E0"),
    D("ed.view.goto_line", "TAKES_COUNT is covered by emitter unit tests"),
    D("ed.view.toggle_wrap", "viewport option is outside compared E0"),
    D("ed.view.number_style", "viewport option is outside compared E0"),
    D("ed.search.next", "search query state is outside E0"),
    D("ed.search.prev", "search query state is outside E0"),
    D("ed.search.word_next", "search query state is outside E0"),
    D("ed.search.word_prev", "search query state is outside E0"),
    D("ed.macro.replay", "macro recursion/cache is separately tested"),
    D("ed.macro.replay_last", "macro recursion/cache is separately tested"),
    D("ed.shell.run", "external process side effect"),
    D("ed.shell.run_bg", "external process side effect"),
    D("ed.shell.read", "external process side effect"),
    D("ed.shell.filter", "external process side effect"),
    /* These commands still participate in the word bijection, but the
     * editor-state round-trip property deliberately has no repo, F-mode tree,
     * picker, confirmation prompt, or subprocess oracle.  Their command/macro
     * reachability is covered by the registry and F-mode table; their effects
     * are covered by the real-Git and pty suites. */
    D("ed.group.from_dir", "requires a live workspace walk and layout state"),
    D("ed.git.refresh", "requires a live F-mode repository"),
    D("ed.git.init", "external Git side effect"),
    D("ed.git.mode.leave", "requires live F-mode layout state"),
    D("ed.git.tree.all", "requires a live F-mode tree"),
    D("ed.git.tree.hidden", "requires a live F-mode tree"),
    D("ed.git.nav.prev", "requires a live F-mode tree"),
    D("ed.git.nav.next", "requires a live F-mode tree"),
    D("ed.git.nav.parent", "requires a live F-mode tree"),
    D("ed.git.nav.enter", "requires a live F-mode tree"),
    D("ed.git.nav.toggle", "requires a live F-mode tree"),
    D("ed.git.nav.row_prev", "requires a live F-mode tree"),
    D("ed.git.nav.row_next", "requires a live F-mode tree"),
    D("ed.git.jump.arm", "requires a live F-mode tree"),
    D("ed.git.stage", "external Git side effect"),
    D("ed.git.unstage", "external Git side effect"),
    D("ed.git.stage.all", "external Git side effect"),
    D("ed.git.unstage.all", "external Git side effect"),
    D("ed.git.commit", "interactive Git side effect"),
    D("ed.git.commit.amend", "interactive Git side effect"),
    D("ed.git.push", "network Git side effect"),
    D("ed.git.push.force", "network Git side effect"),
    D("ed.git.pull", "network Git side effect"),
    D("ed.git.fetch", "network Git side effect"),
    D("ed.git.diff", "requires a live F-mode viewer"),
    D("ed.git.status", "requires a live F-mode viewer"),
    D("ed.git.blame", "requires a live F-mode viewer"),
    D("ed.git.history", "requires a live F-mode viewer"),
    D("ed.git.reflog", "requires a live F-mode viewer"),
    D("ed.git.view", "requires a live F-mode viewer"),
    D("ed.git.branch.switch", "interactive Git side effect"),
    D("ed.git.branch.create", "interactive Git side effect"),
    D("ed.git.branch.delete", "interactive Git side effect"),
    D("ed.git.merge", "interactive Git side effect"),
    D("ed.git.reset", "interactive Git side effect"),
    D("ed.git.rebase.interactive", "terminal handover side effect"),
    D("ed.git.rebase.continue", "external Git side effect"),
    D("ed.git.rebase.abort", "external Git side effect"),
    D("ed.git.cherry_pick", "interactive Git side effect"),
    D("ed.git.revert", "interactive Git side effect"),
    D("ed.git.stash.push", "interactive Git side effect"),
    D("ed.git.stash.pop", "interactive Git side effect"),
    D("ed.git.tag", "interactive Git side effect"),
    D("ed.git.discard", "destructive Git side effect"),
    D("ed.git.file.delete", "destructive file side effect"),
    D("ed.git.file.rename", "file-system side effect"),
    D("ed.git.open", "requires live F-mode layout state"),
    D("ed.git.diff.view", "requires live Git workspace and layout state")
};
#undef D

static const char *deny_reason(const CmdDesc *d)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(denied); i++)
        if (strcmp(d->name, denied[i].name) == 0)
            return denied[i].reason;
    return NULL;
}

static bool is_generated(const char *name)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(gen_cmds); i++) {
        if (strcmp(name, gen_cmds[i].name) == 0)
            return true;
    }
    return false;
}

bool rt_generator_coverage(bool verbose)
{
    u32 i;
    bool ok = true;

    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *d = yew_cmd_at(i);
        CmdId id;
        CmdId back;
        const char *reason;

        if (d == NULL || (d->flags & YEW_CMD_RECORDABLE) == 0U)
            continue;
        id = yew_cmd_lookup(d->name, (u32)strlen(d->name));
        back = d->word == NULL ? YEW_CMD_NONE :
               yew_cmd_by_word(d->word, (u32)strlen(d->word));
        if (id.v == 0U || back.v != id.v) {
            (void)fprintf(stderr,
                          "roundtrip: word bijection failed: %s -> %s\n",
                          d->name, d->word == NULL ? "<null>" : d->word);
            ok = false;
        }
        if (is_generated(d->name)) {
            if (verbose)
                (void)printf("generate %s\n", d->name);
            continue;
        }
        reason = deny_reason(d);
        if (reason == NULL || reason[0] == '\0') {
            (void)fprintf(stderr, "roundtrip: uncovered command: %s\n",
                          d->name);
            ok = false;
        } else if (verbose) {
            (void)printf("deny %s: %s\n", d->name, reason);
        }
    }
    /* Sprint text names paste, but this registry currently exposes no
     * recordable paste command.  Once one lands it is not auto-denied: the
     * exact-table audit above fails until the generator handles it or a
     * specific reviewed exclusion is added. */
    if (verbose && yew_cmd_by_word("paste", 5U).v == 0U)
        (void)printf("unavailable paste: no recordable registry command\n");
    return ok;
}
