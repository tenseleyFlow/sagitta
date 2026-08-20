/* Sprint 48: the Fletch front door for owned AI backend definitions. */

#include "fl/std.h"

#include "edit/ed.h"
#include "mod/ai/ai.h"

static bool ai_backend(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    const FlStr *name;
    FlMap *config;
    char err[256];

    (void)nargs;
    if (vm == NULL)
        return false;
    if (vm->ed == NULL)
        return fl_raise(vm, "handle", "no editor is attached");
    if (!fl_arg_str(vm, args, 0U, &name) ||
        !fl_arg_map(vm, args, 1U, &config))
        return false;
    if (!yew_ai_backend_define(vm->ed, name, config, err, sizeof(err)))
        return fl_raise(vm, "user", "%s", err);
    *out = FL_NIL_V;
    return true;
}

static const FlNativeDef AI_DEFS[] = {
    {"backend", ai_backend, 2U, 2U, 0U, "(name, config) -> nil"}
};

const FlModuleDef fl_mod_ai = {
    "ai", AI_DEFS, YEW_ARRAY_LEN(AI_DEFS), NULL, 0U
};
