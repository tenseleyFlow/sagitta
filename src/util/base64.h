#ifndef SAG_UTIL_BASE64_H
#define SAG_UTIL_BASE64_H

#include "util/base.h"

u64 sag_base64_len(u64 n);
void sag_base64_encode(const u8 *in, u64 n, u8 *out);

#endif
