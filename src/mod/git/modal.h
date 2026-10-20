#ifndef YEW_MOD_GIT_MODAL_H
#define YEW_MOD_GIT_MODAL_H

#include "util/base.h"

typedef struct Ed Ed;

typedef struct GitModalBridge {
    char *dir;
    char *editor_env;
    char *sequence_env;
    u32 job_id;
    bool waiting;
    bool sequence;
    bool timeout_paused;
    i64 timeout_left;
    i64 next_poll;
} GitModalBridge;

bool yew_git_modal_start(Ed *ed, GitModalBridge *bridge);
void yew_git_modal_tick(Ed *ed, GitModalBridge *bridge, i64 now_ms);
void yew_git_modal_close(Ed *ed, GitModalBridge *bridge);
/* Private subprocess entry; no terminal or editor state is initialized. */
int yew_git_modal_helper_main(int argc, char **argv);

#endif
