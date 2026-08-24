#ifndef YEW_MOD_PLUG_MANIFEST_H
#define YEW_MOD_PLUG_MANIFEST_H

#include <stdbool.h>

#include "fl/diag.h"
#include "util/arena.h"
#include "util/base.h"

enum { YEW_PLUG_API_MAJOR = 1U };

typedef enum YewCap {
    YEW_CAP_FS = 0,
    YEW_CAP_SHELL,
    YEW_CAP_NET,
    YEW_CAP_CLIPBOARD,
    YEW_CAP__N
} YewCap;

typedef struct PlugManifest {
    /* Discovery/lifecycle fill these from their shared interner.  Parsing
     * deliberately leaves them zero: a parser-local interner would return
     * IDs no later owner could resolve. */
    u32 name;
    const char *version;
    const char *entry;
    const char *desc;
    u32 api;
    u32 caps_wanted;
    u32 *events;
    u32 nevents;
    const char *dir;

    /* Arena-owned authoritative text for the two-phase intern step. */
    const char *name_text;
    const char **event_names;
} PlugManifest;

const char *yew_cap_name(YewCap cap);
bool yew_cap_parse(const char *name, size_t len, YewCap *out);
bool yew_plug_event_valid(const char *name, size_t len);

/* False emits at least one diagnostic and leaves OUT entirely zeroed. */
bool yew_plug_manifest_read(Arena *a, const char *dir,
                            PlugManifest *out, DiagCtx *dc);

#endif /* YEW_MOD_PLUG_MANIFEST_H */
