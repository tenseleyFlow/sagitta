#ifndef YEW_UTIL_RUNTIME_BLOB_H
#define YEW_UTIL_RUNTIME_BLOB_H

#include <stddef.h>

#include "util/base.h"

typedef struct YewRuntimeBlobEntry {
    const char *name;
    u32 offset;
    u32 packed_len;
    u32 raw_len;
} YewRuntimeBlobEntry;

const u8 *yew_runtime_blob_data(size_t *len);
const YewRuntimeBlobEntry *yew_runtime_blob_index(size_t *len);

#endif
