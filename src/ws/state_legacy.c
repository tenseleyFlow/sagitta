/*
 * Sprint 36 differential oracle, compiled only as
 * build/tests/unit/state_legacy.o with YEW_STATE_LEGACY.
 *
 * Delete this file in Sprint 58's persistence audit.  It must never be
 * linked into yew: production reads and writes through fl/data.c.
 */
#ifdef YEW_STATE_LEGACY
#define YEW_STATE_ACCESSORS_EXTERNAL 1
#include "fl_parse.c"
#include "fl_emit.c"
#endif
