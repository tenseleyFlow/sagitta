#include "edit/completion.h"

#include "edit/shadow.h"
#include "ui/win.h"

void yew_compl_open(Ed *ed, Win *win)
{
    if (win == NULL)
        return;
    yew_shadow_dismiss(ed, win);
    win->shadow.suppressed = true;
    win->compl.open = true;
}

void yew_compl_close(Ed *ed, Win *win)
{
    if (win == NULL)
        return;
    win->compl.open = false;
    win->shadow.suppressed = false;
    yew_shadow_arm(ed, win);
}
