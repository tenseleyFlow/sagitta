#ifndef YEW_TEXT_COORDS_H
#define YEW_TEXT_COORDS_H

#include "util/base.h"

typedef struct {
    u64 v;
} ByteOff;

typedef struct {
    u64 v;
} LineNo;

typedef struct {
    u64 lo;
    u64 hi;
} Span;

#define BYTEOFF(x) ((ByteOff){(x)})
#define LINENO(x) ((LineNo){(x)})

/* YEW-F-016: presentation edges name their zero-based-to-human conversion
 * here, outside protocol modules, so wire coordinates never acquire an
 * accidental adjustment.  A u32 input guarantees the u64 result cannot
 * overflow. */
static inline u64 yew_coord_display(u32 zero_based)
{
    return (u64)zero_based + 1U;
}

#endif
