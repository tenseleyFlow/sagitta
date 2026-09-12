/*
 * YEW-F-009 — the recorder folding self-test no longer reaches its fault.
 *
 * Correct behavior: Sprint 35's pinned self-test seed generates two adjacent,
 * uncounted ed.move.buf.end events followed by insertion of "x".  The
 * self-test can then corrupt that prefix into `2buf_end`, execute it through
 * the VM, shrink the divergence to three events, and name the right command.
 *
 * Baseline failure: later generator-pool growth changed the deterministic
 * command selection for seed 20764.  YEW_RT_SELFTEST therefore exits 2 before
 * fault injection and prints none of the required shrinker evidence.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "roundtrip/gen.h"

static bool event_is(const RtSession *session, u32 at, const char *name)
{
    const CmdDesc *desc;

    if (at >= session->events.len)
        return false;
    desc = yew_cmd_desc(session->events.data[at].cmd);
    return desc != NULL && strcmp(desc->name, name) == 0;
}

bool test_yew_f_009(char *why, size_t why_cap)
{
    const u64 seed = UINT64_C(20764);
    RtSession session;
    bool correct;

    rt_session_init(&session);
    correct = rt_session_generate(&session, seed, 2U, 96U) &&
              session.events.len >= 3U &&
              event_is(&session, 0U, "ed.move.buf.end") &&
              event_is(&session, 1U, "ed.move.buf.end") &&
              !session.events.data[0].count_given &&
              !session.events.data[1].count_given &&
              event_is(&session, 2U, "ed.edit.insert.text") &&
              !session.events.data[2].count_given &&
              session.events.data[2].sarg_len == 1U &&
              session.events.data[2].sarg != NULL &&
              session.events.data[2].sarg[0] == (u8)'x';
    if (!correct) {
        const CmdDesc *first = session.events.len == 0U ? NULL :
                               yew_cmd_desc(session.events.data[0].cmd);

        (void)snprintf(why, why_cap,
                       "seed 20764 begins with %s; self-test exits 2 before fault injection",
                       first == NULL ? "<no command>" : first->name);
    }
    rt_session_free(&session);
    return correct;
}
