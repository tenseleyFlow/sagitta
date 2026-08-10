#ifndef YEW_FL_MOTION_TAB_H
#define YEW_FL_MOTION_TAB_H

#include <stdbool.h>

#include "fl/value.h"
#include "fl/vm.h"
#include "util/buf.h"

typedef struct Ed Ed;
typedef struct Win Win;

/* Wire values emitted by compile.c from FlMotionKind. */
typedef enum FlMotionExecKind {
    FL_MOTION_UNIT,
    FL_MOTION_ARROW,
    FL_MOTION_HIGHLIGHT,
    FL_MOTION_INSERT,
    FL_MOTION_DEL,
    FL_MOTION_ESC,
    FL_MOTION_WORD
} FlMotionExecKind;

/* Execute one preassembled motion program through the command registry. */
bool fl_motion_exec(FlVm *vm, Ed *ed, Win *win,
                    const FlMotionProg *prog);

/* Compile/store-time command-word validation.  On failure `detail` receives
 * the complete deterministic diagnostic sentence, including a suggestion
 * when one is close enough. */
bool fl_motion_word_validate(const char *word, u32 len, Bytebuf *detail);

/* FlHost.motion-compatible adapter; uses vm->ed and its focused window. */
bool fl_motion_host_dispatch(FlVm *vm, const FlMotionProg *prog);

#endif /* YEW_FL_MOTION_TAB_H */
