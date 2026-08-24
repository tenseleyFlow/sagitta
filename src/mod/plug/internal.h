#ifndef YEW_MOD_PLUG_INTERNAL_H
#define YEW_MOD_PLUG_INTERNAL_H

#include "fl/vm.h"
#include "mod/plug/plug.h"
#include "ui/picker.h"
#include "util/arena.h"
#include "ws/trust.h"

typedef struct PlugCmd {
    CmdId id;
    u32 origin_id;
    FlValue fn;
    bool active;
} PlugCmd;

typedef struct PlugValueReg {
    u32 handle;
    u32 origin_id;
    u8 kind;
    FlValue value;
    bool active;
} PlugValueReg;

typedef struct PlugPrompt {
    Plug *plug;
    YewCap cap;
    bool active;
    bool retry_enable;
} PlugPrompt;

struct PlugSys {
    Arena arena;
    Plug **v;
    u32 n;
    u32 cap;
    PlugCmd *cmds;
    u32 ncmds;
    u32 capcmds;
    PlugValueReg *regs;
    u32 nregs;
    u32 capregs;
    u32 next_handle;
    PlugPrompt prompt;
    u32 *pending_disable;
    u32 npending_disable;
    u32 cappending_disable;
    u32 ctx_origin;
    bool ctx_registration;
    bool gc_registered;
    bool booted;
    bool draining;
    PickItem *pick_items;
    char *pick_text;
    u32 pick_n;
};


Plug *yew_plug_by_origin(Ed *ed, u32 origin_id);
const char *yew_plug_state_name(PlugState state);
const char *yew_plug_source_name(PlugSource source);

/* discover.c owns allocation/scanning and the standalone porcelain CLI. */
bool yew_plug_discover_with_policy(Ed *ed, bool workspace_trusted,
                                   const YewTrustDb *policy, DiagCtx *dc);

/* hooks.c owns the frozen ctx namespace and origin-tagged registrations. */
bool yew_plug_context_build(Ed *ed, Plug *plug, FlValue *out);
void yew_plug_mark(FlVm *vm, void *ctx);
void yew_plug_drop_origin_regs(Ed *ed, u32 origin_id);

/* sandbox.c owns capability decisions and message-line consent. */
u32 yew_plug_cap_fl_mask(YewCap cap);
YewPluginCapability yew_plug_trust_cap(YewCap cap);

#endif /* YEW_MOD_PLUG_INTERNAL_H */
