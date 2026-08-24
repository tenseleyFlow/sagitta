#ifndef YEW_MOD_PLUG_PLUG_H
#define YEW_MOD_PLUG_PLUG_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "mod/plug/manifest.h"
#include "util/base.h"

/*
 * Keep the stripped-build diagnostic here so every plugin command-line
 * implementation has one byte-exact source of truth.
 */
static const char yew_plug_module_error[] =
    "yew: error: built without plugin support (MODULES=plugins)\n";

/*
 * Plugins are Fletch running in the same VM, same process, same address
 * space as your editor.  There is no memory isolation and no resource
 * isolation: an enabled plugin can read any open buffer, burn CPU, or
 * allocate until the GC sweats.  Capability gates cover exactly the flapi
 * I/O surface -- fs, shell, net, clipboard -- nothing else.  Enabling a
 * plugin is an act of trust in its author; capabilities limit blast radius,
 * they do not create a sandbox.  Read the source: it is Fletch, and it is
 * short.
 */

typedef struct Ed Ed;
typedef struct FlVm FlVm;
typedef struct FlRegistration FlRegistration;
typedef struct PlugSys PlugSys;

typedef enum PlugState {
    PLUG_DISCOVERED = 0,
    PLUG_LOADED,
    PLUG_ENABLED,
    PLUG_DISABLED,
    PLUG_ERROR,
    PLUG_BLOCKED,
    PLUG_SHADOWED
} PlugState;

typedef enum PlugSource {
    PLUG_SOURCE_DATA = 0,
    PLUG_SOURCE_CONFIG,
    PLUG_SOURCE_WORKSPACE
} PlugSource;

typedef struct Plug {
    PlugManifest mf;
    PlugState st;
    PlugSource source;
    u32 origin_id;
    u32 err_count;
    FlValue module;
    u32 session_allow;
    u32 session_deny;
    char *last_error;
    bool winner;
    bool rooted;
#ifndef NDEBUG
    u32 residue_before[6];
    bool residue_snapshot;
#endif
} Plug;

/* Discovery is separate from enabling so --batch can install repeatable
 * session grants before any plugin code runs. */
bool yew_plug_discover(Ed *ed, DiagCtx *dc);
bool yew_plug_enable_desired(Ed *ed, DiagCtx *dc);
bool yew_plug_boot(Ed *ed);
bool yew_plug_startup_pending(const Ed *ed);
void yew_plug_pump(Ed *ed);
void yew_plug_free(Ed *ed);
bool yew_plug_session_grant(Ed *ed, const char *plugin, const char *cap);

u32 yew_plug_count(const Ed *ed);
Plug *yew_plug_at(Ed *ed, u32 index);
Plug *yew_plug_find(Ed *ed, const char *name);

bool yew_plug_enable(Ed *ed, Plug *p, DiagCtx *dc);
bool yew_plug_disable(Ed *ed, Plug *p);
bool yew_plug_reload(Ed *ed, Plug *p, DiagCtx *dc);

/* Fletch/runtime seams. */
bool yew_plug_event_allowed(Ed *ed, u32 origin_id, const char *name,
                            size_t len);
bool yew_plug_ctx_registration_allowed(const Ed *ed, u32 origin_id);
void yew_plug_hook_error(Ed *ed, u32 origin_id, FlValue err);
void yew_plug_drain_pending(Ed *ed);
bool yew_plug_cap_check(FlVm *vm, u32 need);
bool yew_plug_registration_remove(Ed *ed, const FlRegistration *reg);
bool yew_plug_prompt_key(Ed *ed, u32 code);
void yew_plug_error_limit_set(Ed *ed, u32 limit);

CmdStatus yew_plug_cmd_list(CmdCtx *cx);
CmdStatus yew_plug_cmd_enable(CmdCtx *cx);
CmdStatus yew_plug_cmd_disable(CmdCtx *cx);
CmdStatus yew_plug_cmd_reload(CmdCtx *cx);
CmdStatus yew_plug_cmd_info(CmdCtx *cx);

/* argv[0] is "plug", matching the yew_fl_main/yew_syn_main convention. */
int yew_plug_main(int argc, char **argv);

#endif
