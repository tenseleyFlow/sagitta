#include "gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/mode.h"

typedef enum {
    RT_GEN_MOTION,
    RT_GEN_INSERT,
    RT_GEN_DELETE,
    RT_GEN_MODE
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
    {"ed.move.line.home", RT_GEN_MOTION},
    {"ed.move.line.end", RT_GEN_MOTION},
    {"ed.edit.insert.text", RT_GEN_INSERT},
    {"ed.edit.delete.unit", RT_GEN_DELETE},
    {"ed.mode.enter", RT_GEN_MODE}
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
        session->storage = sag_xrealloc(session->storage, cap);
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
                     : roll < 70U ? RT_GEN_INSERT
                     : roll < 92U ? RT_GEN_DELETE
                                   : RT_GEN_MODE;
    u32 start = rnd(state, (u32)SAG_ARRAY_LEN(gen_cmds));
    u32 i;

    for (i = 0U; i < (u32)SAG_ARRAY_LEN(gen_cmds); i++) {
        const RtGenCmd *cmd = &gen_cmds[(start + i) % SAG_ARRAY_LEN(gen_cmds)];

        if (cmd->kind == want)
            return cmd;
    }
    return &gen_cmds[0];
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

    ev.cmd = sag_cmd_lookup(gc->name, (u32)strlen(gc->name));
    desc = sag_cmd_desc(ev.cmd);
    if (desc == NULL)
        return false;
    ev.count = 1U;
    if (gc->kind == RT_GEN_INSERT) {
        u32 p = rnd(state, (u32)SAG_ARRAY_LEN(payloads));

        ev.sarg_len = payloads[p].len;
        ev.sarg = store_bytes(session, payloads[p].bytes, ev.sarg_len);
        if (ev.sarg == NULL)
            return false;
    } else if (gc->kind == RT_GEN_MODE) {
        static const u8 modes[] = {'L', 'W', 'B'};

        ev.sarg_len = 1U;
        ev.sarg = store_bytes(session, &modes[rnd(state, 3U)], 1U);
        if (ev.sarg == NULL)
            return false;
    } else if ((desc->flags & SAG_CMD_REPEATABLE) != 0U) {
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
    u32 i;

    session->seed = seed;
    session->fixture = fixture % 6U;
    session->start_mode = (u8)(SAG_MODE_L + rnd(&state, 3U));
    /* Largest generated payload is 11 bytes and sessions cap at 200. */
    session->storage_cap = 4096U;
    session->storage = sag_xmalloc(session->storage_cap);
    for (i = 0U; i < target; i++) {
        if (!append_generated(session, &state, choose_cmd(&state)))
            return false;
    }
    /* P2 needs an actual edit even when the random session chose motions. */
    if (target != 0U) {
        u32 j;
        bool inserts = false;

        for (j = 0U; j < session->events.len; j++) {
            const CmdDesc *d = sag_cmd_desc(session->events.data[j].cmd);

            inserts = inserts ||
                      (d != NULL && strcmp(d->name,
                                           "ed.edit.insert.text") == 0);
        }
        if (!inserts) {
            const RtGenCmd insert = {"ed.edit.insert.text", RT_GEN_INSERT};

            if (!append_generated(session, &state, &insert))
                return false;
        }
    }
    /*
     * P5 composes S with itself.  Restore the starting unit explicitly so
     * the second direct S begins under the same unit that the emitted
     * macro's mode annotation establishes for its second invocation.
     */
    {
        RtEvent restore = {0};
        static const u8 modes[] = {'L', 'W', 'B'};

        restore.cmd = sag_cmd_lookup("ed.mode.enter",
                                     (u32)strlen("ed.mode.enter"));
        restore.count = 1U;
        restore.sarg_len = 1U;
        restore.sarg = store_bytes(session,
                                   &modes[session->start_mode - SAG_MODE_L],
                                   1U);
        if (restore.sarg == NULL)
            return false;
        RtEventVec_push(&session->events, restore);
    }
    return true;
}

static const char *deny_reason(const CmdDesc *d)
{
    if ((d->flags & SAG_CMD_PROMPTS) != 0U)
        return "prompt/modal input is outside generated committed commands";
    if ((d->flags & SAG_CMD_DEFERRED) != 0U)
        return "deferred command cannot execute successfully";
    if (strncmp(d->name, "ed.file.", 8U) == 0 ||
        strncmp(d->name, "ed.shell.", 9U) == 0 ||
        strncmp(d->name, "ed.job.", 7U) == 0 ||
        strncmp(d->name, "ed.quit", 7U) == 0)
        return "external filesystem/process/lifecycle side effect";
    if (strstr(d->name, ".undo") != NULL ||
        strstr(d->name, ".redo") != NULL ||
        strcmp(d->name, "ed.repeat") == 0)
        return "history-dependent command is not representable in E0";
    if (strcmp(d->name, "ed.edit.insert.at") == 0 ||
        strcmp(d->name, "ed.edit.delete.span") == 0 ||
        strcmp(d->name, "ed.edit.replace.span") == 0)
        return "range/cursor envelope is absent from RecEvent";
    if (strncmp(d->name, "ed.macro.", 9U) == 0)
        return "macro recursion/cache state is tested separately";
    if (strncmp(d->name, "ed.search.", 10U) == 0 ||
        strncmp(d->name, "ed.find.", 8U) == 0)
        return "search query and match state is outside generated E0";
    if (strncmp(d->name, "ed.tab.", 7U) == 0 ||
        strncmp(d->name, "ed.pane.", 8U) == 0 ||
        strncmp(d->name, "ed.win.", 7U) == 0 ||
        strncmp(d->name, "ed.view.", 8U) == 0 ||
        strncmp(d->name, "ed.group.", 9U) == 0)
        return "workspace/window topology is not generated in this sprint";
    if (strncmp(d->name, "ed.select.", 10U) == 0 ||
        strstr(d->name, ".yank") != NULL || strstr(d->name, ".paste") != NULL)
        return "selection/register preconditions need a richer generator";
    if (d->arity != (u8)SAG_ARITY_NONE)
        return "non-motion argument envelope needs a command-specific generator";
    if (strncmp(d->name, "ed.move.", 8U) == 0)
        return "redundant motion alias excluded from the weighted core pool";
    if (strncmp(d->name, "ed.edit.", 8U) == 0)
        return "insert/selection precondition is not total over all fixtures";
    if (strncmp(d->name, "ed.mode.", 8U) == 0)
        return "non-unit mode would make repeated-session composition partial";
    if (strncmp(d->name, "ed.sel.", 7U) == 0)
        return "selection shape is not generated in this bounded campaign";
    if (strncmp(d->name, "ed.cursor.", 10U) == 0)
        return "multi-cursor topology is covered by focused recorder tests";
    return NULL;
}

static bool is_generated(const char *name)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(gen_cmds); i++) {
        if (strcmp(name, gen_cmds[i].name) == 0)
            return true;
    }
    return false;
}

bool rt_generator_coverage(bool verbose)
{
    u32 i;
    bool ok = true;

    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *d = sag_cmd_at(i);
        CmdId id;
        CmdId back;
        const char *reason;

        if (d == NULL || (d->flags & SAG_CMD_RECORDABLE) == 0U)
            continue;
        id = sag_cmd_lookup(d->name, (u32)strlen(d->name));
        back = d->word == NULL ? SAG_CMD_NONE :
               sag_cmd_by_word(d->word, (u32)strlen(d->word));
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
    return ok;
}
