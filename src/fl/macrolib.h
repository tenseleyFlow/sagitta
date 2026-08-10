#ifndef SAG_FL_MACROLIB_H
#define SAG_FL_MACROLIB_H

/* Sprint 38: deterministic, reloadable Fletch macro library. */

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct MacroLib MacroLib;

typedef struct SagMacroText {
    const char *s;                 /* borrowed; not necessarily NUL-ended */
    u32 len;
    bool present;
} SagMacroText;

typedef struct SagMacroHeader {
    u32 schema;
    bool has_schema;
    SagMacroText recorded_with;
    SagMacroText keymap;
    SagMacroText recorded;
    SagMacroText describe;
} SagMacroHeader;

typedef enum SagMacroHeaderStatus {
    SAG_MACRO_HEADER_OK = 0,
    SAG_MACRO_HEADER_UNSUPPORTED
} SagMacroHeaderStatus;

/* No header is valid.  A present schema other than 1 is unsupported. */
SagMacroHeaderStatus sag_macro_header_parse(const char *source, size_t len,
                                             SagMacroHeader *out);

typedef struct SagMacroEntryView {
    const char *name;              /* canonical <stem>.<binding>          */
    const char *alias;             /* bare stem alias, or NULL            */
    const char *binding;
    const char *stem;
    const char *path;
    const char *source;
    size_t source_len;
    SagMacroHeader header;
    u32 events;                    /* library source has no event count   */
    u8 arity;
    bool replayable;               /* callable and zero-argument          */
} SagMacroEntryView;

MacroLib *sag_macrolib_new(Ed *ed);
void sag_macrolib_free(Ed *ed, MacroLib *lib);

/* Candidate-and-swap rescan.  Returns the number of exported functions. */
u32 sag_macrolib_scan(Ed *ed, DiagCtx *dc);
u32 sag_macrolib_count(const Ed *ed);
bool sag_macrolib_at(const Ed *ed, u32 index, SagMacroEntryView *out);
bool sag_macrolib_find(const Ed *ed, const char *name,
                       SagMacroEntryView *out);
/* Calls a zero-argument library macro through the ordinary Fletch entry. */
CmdStatus sag_macrolib_call(Ed *ed, const char *name);

/* Effective scan directory.  Borrowed until macro.dir changes/rescans. */
const char *sag_macrolib_dir(const Ed *ed);
/* Enables startup scanning; later macro.dir changes rescan automatically. */
void sag_macrolib_enable(Ed *ed);
void sag_macrolib_option_changed(Ed *ed);

#endif /* SAG_FL_MACROLIB_H */
