#ifndef YEW_UTIL_RUNTIME_ASSET_H
#define YEW_UTIL_RUNTIME_ASSET_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

#ifndef YEW_EMBED_RUNTIME
#define YEW_EMBED_RUNTIME 0
#endif

size_t yew_runtime_asset_count(void);
const char *yew_runtime_asset_name(size_t index);
bool yew_runtime_asset_has(const char *path);
bool yew_runtime_asset_read(const char *path, Bytebuf *out);
char *yew_runtime_asset_resolve(const char *path);

#endif
