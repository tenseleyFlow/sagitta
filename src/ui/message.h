#ifndef YEW_UI_MESSAGE_H
#define YEW_UI_MESSAGE_H

#include <stddef.h>

#include "edit/loop.h"
#include "ui/statusline.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef enum {
    YEW_MSG_INFO,
    YEW_MSG_WARN,
    YEW_MSG_ERROR
} MsgSev;

typedef struct Msg {
    char text[512];
    char *full;
    size_t len;
    MsgSev sev;
    TimerId expiry;
    bool active;
    bool prompt;
    bool truncated;
    bool expanded;
} Msg;

void yew_msg(Ed *ed, MsgSev sev, const char *fmt, ...);
/* Low-priority persistent text.  A real message temporarily displaces it. */
void yew_msg_hint(Ed *ed, MsgSev sev, const char *fmt, ...);
void yew_msg_hint_clear(Ed *ed);
bool yew_msg_visible(const Ed *ed);
/* Event-loop/test variant: schedules expiry relative to the supplied clock. */
void yew_msg_at(Ed *ed, MsgSev sev, i64 now_ms, const char *fmt, ...);
void yew_msg_clear(Ed *ed);
bool yew_msg_expand(Ed *ed);
bool yew_msg_dismiss_overlay(Ed *ed);

/* Clip at grapheme boundaries and reserve one cell for an ellipsis. */
size_t yew_message_clip(const char *text, size_t len, u16 max_cells,
                        char *out, size_t out_cap, bool *truncated);
YewUiStyle yew_message_style(const Ed *ed);
void yew_message_draw(Ed *ed, Win *w);

#endif
