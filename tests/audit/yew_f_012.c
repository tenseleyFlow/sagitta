/*
 * YEW-F-012 — pending embeds occupy a canonical state tail slot.
 *
 * Correct behavior: Sprint 41.5 and Sprint 58 F10 require every interned
 * SynState to zero aux[ndef..YEW_SYN_DEF_MAX).  Pending guest identity was
 * specified outside that tail so equivalent logical states keep canonical
 * byte identity.
 *
 * Baseline failure: an HTML script opener whose JavaScript definition is not
 * yet resident stores the pending language id in aux[ndef].  The interner
 * explicitly preserves that future slot, producing a nonzero canonical tail.
 */
#include "audit.h"

#include <stdio.h>

#include "syn/defs.h"
#include "syn/engine.h"

bool test_yew_f_012(char *why, size_t why_cap)
{
    static const u8 line[] = "<script>";
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named("html"));
    SynSpan spans[16];
    SynLineOut out = {spans, 0U, 16U, 0U, 0U};
    SynEngine *engine;
    const SynState *exit;
    u8 slot;
    u8 bad_slot = UINT8_MAX;
    u32 bad_value = 0U;
    bool correct;

    if (def == NULL) {
        (void)snprintf(why, why_cap, "could not load html definition");
        return false;
    }
    engine = yew_syn_engine_new((SynDef *)def);
    if (engine == NULL) {
        (void)snprintf(why, why_cap, "could not create html engine");
        return false;
    }
    yew_syn_line(engine, YEW_SYN_STATE_ROOT, line,
                 (u32)(sizeof(line) - 1U), &out);
    exit = yew_syn_state_get(yew_syn_engine_states(engine), out.exit_state);
    if (exit != NULL) {
        for (slot = exit->ndef; slot < YEW_SYN_DEF_MAX; slot++) {
            if (exit->aux[slot] != 0U) {
                bad_slot = slot;
                bad_value = exit->aux[slot];
                break;
            }
        }
    }
    correct = exit != NULL && out.stop == YEW_SYN_STOP_OK &&
              bad_slot == UINT8_MAX;
    if (!correct) {
        (void)snprintf(why, why_cap,
                       "exit=%u ndef=%u flags=0x%02x aux[%u]=%u; canonical tail is nonzero",
                       out.exit_state, exit == NULL ? 0U : exit->ndef,
                       exit == NULL ? 0U : exit->flags,
                       bad_slot == UINT8_MAX ? 0U : bad_slot, bad_value);
    }
    yew_syn_engine_free(engine);
    return correct;
}
