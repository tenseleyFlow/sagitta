#ifndef SAG_TEXT_COORDS_H
#define SAG_TEXT_COORDS_H

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

#endif
