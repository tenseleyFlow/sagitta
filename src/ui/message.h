#ifndef SAG_UI_MESSAGE_H
#define SAG_UI_MESSAGE_H

#include <stddef.h>

#include "edit/loop.h"
#include "ui/statusline.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef enum {
    SAG_MSG_INFO,
    SAG_MSG_WARN,
    SAG_MSG_ERROR
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

void sag_msg(Ed *ed, MsgSev sev, const char *fmt, ...);
/* Event-loop/test variant: schedules expiry relative to the supplied clock. */
void sag_msg_at(Ed *ed, MsgSev sev, i64 now_ms, const char *fmt, ...);
void sag_msg_clear(Ed *ed);
bool sag_msg_expand(Ed *ed);
bool sag_msg_dismiss_overlay(Ed *ed);

/* Clip at grapheme boundaries and reserve one cell for an ellipsis. */
size_t sag_message_clip(const char *text, size_t len, u16 max_cells,
                        char *out, size_t out_cap, bool *truncated);
SagUiStyle sag_message_style(const Ed *ed);
void sag_message_draw(Ed *ed, Win *w);

#endif
