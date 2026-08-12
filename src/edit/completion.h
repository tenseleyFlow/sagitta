#ifndef YEW_EDIT_COMPLETION_H
#define YEW_EDIT_COMPLETION_H

#include <stdbool.h>

typedef struct Ed Ed;
typedef struct Win Win;

/* Sprint 43 owns only menu/ghost arbitration.  Sprint 44 grows this state. */
typedef struct Completion {
    bool open;
} Completion;

void yew_compl_open(Ed *ed, Win *win);
void yew_compl_close(Ed *ed, Win *win);

#endif
