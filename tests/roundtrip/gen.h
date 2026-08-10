#ifndef SAG_TEST_ROUNDTRIP_GEN_H
#define SAG_TEST_ROUNDTRIP_GEN_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct RtEvent {
    CmdId cmd;
    u32 count;
    bool count_given;
    const u8 *sarg;
    u32 sarg_len;
} RtEvent;

VEC_DECL(RtEventVec, RtEvent);

typedef struct RtSession {
    RtEventVec events;
    u8 *storage;
    u32 storage_len;
    u32 storage_cap;
    u64 seed;
    u32 fixture;
    u8 start_mode;
} RtSession;

void rt_session_init(RtSession *session);
void rt_session_free(RtSession *session);
bool rt_session_generate(RtSession *session, u64 seed, u32 fixture,
                         u32 forced_len);

/* Audits both halves of Sprint 35 DoD 9/10. */
bool rt_generator_coverage(bool verbose);

#endif /* SAG_TEST_ROUNDTRIP_GEN_H */
