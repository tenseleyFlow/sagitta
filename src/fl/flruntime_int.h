#ifndef SAG_FL_FLRUNTIME_INT_H
#define SAG_FL_FLRUNTIME_INT_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct Ed Ed;
typedef struct FlRuntime FlRuntime;

struct FlRuntime {
    Arena arena;
    Interner interner;
    DiagCtx diag;
    FlVm vm;
    Ed *ed;
    char diag_message[256];
    bool diag_error;
    bool ready;
};

CmdStatus fl_runtime_eval(FlRuntime *rt, const char *source, u32 len);

#endif /* SAG_FL_FLRUNTIME_INT_H */
