#ifndef YEW_FL_MACROLIB_H
#define YEW_FL_MACROLIB_H

/* Sprint 38: deterministic, reloadable Fletch macro library. */

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct MacroLib MacroLib;

typedef struct YewMacroText {
    const char *s;                 /* borrowed; not necessarily NUL-ended */
    u32 len;
    bool present;
} YewMacroText;

typedef struct YewMacroHeader {
    u32 schema;
    bool has_schema;
    YewMacroText recorded_with;
    YewMacroText keymap;
    YewMacroText recorded;
    YewMacroText describe;
} YewMacroHeader;

typedef enum YewMacroHeaderStatus {
    YEW_MACRO_HEADER_OK = 0,
    YEW_MACRO_HEADER_UNSUPPORTED
} YewMacroHeaderStatus;

/* No header is valid.  A present schema other than 1 is unsupported. */
YewMacroHeaderStatus yew_macro_header_parse(const char *source, size_t len,
                                             YewMacroHeader *out);

typedef struct YewMacroEntryView {
    const char *name;              /* canonical <stem>.<binding>          */
    const char *alias;             /* bare stem alias, or NULL            */
    const char *binding;
    const char *stem;
    const char *path;
    const char *source;
    size_t source_len;
    YewMacroHeader header;
    u32 events;                    /* library source has no event count   */
    u8 arity;
    bool replayable;               /* callable and zero-argument          */
} YewMacroEntryView;

MacroLib *yew_macrolib_new(Ed *ed);
void yew_macrolib_free(Ed *ed, MacroLib *lib);

/* Candidate-and-swap rescan.  Returns the number of exported functions. */
u32 yew_macrolib_scan(Ed *ed, DiagCtx *dc);
u32 yew_macrolib_count(const Ed *ed);
bool yew_macrolib_at(const Ed *ed, u32 index, YewMacroEntryView *out);
bool yew_macrolib_find(const Ed *ed, const char *name,
                       YewMacroEntryView *out);
/* Calls a zero-argument library macro through the ordinary Fletch entry. */
CmdStatus yew_macrolib_call(Ed *ed, const char *name);

/* Effective scan directory.  Borrowed until macro.dir changes/rescans. */
const char *yew_macrolib_dir(const Ed *ed);
/* Enables startup scanning; later macro.dir changes rescan automatically. */
void yew_macrolib_enable(Ed *ed);
void yew_macrolib_option_changed(Ed *ed);

#endif /* YEW_FL_MACROLIB_H */
