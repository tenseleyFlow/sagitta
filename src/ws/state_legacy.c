/*
 * Sprint 36 differential oracle, compiled only as
 * build/tests/unit/state_legacy.o with SAG_STATE_LEGACY.
 *
 * Delete this file in Sprint 58's persistence audit.  It must never be
 * linked into sagitta: production reads and writes through fl/data.c.
 */
#ifdef SAG_STATE_LEGACY
#define SAG_STATE_ACCESSORS_EXTERNAL 1
#include "fl_parse.c"
#include "fl_emit.c"
#endif
