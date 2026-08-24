#ifndef YEW_MOD_PLUG_OVERLAY_H
#define YEW_MOD_PLUG_OVERLAY_H

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

/*
 * Plugin API 1 overlay result:
 *
 *   [{lo: INT, hi: INT, attr: INT}, ...]
 *
 * lo/hi are absolute buffer byte offsets and attr is a SynAttr id returned
 * by ctx.attr().  The callback receives (win, buf, lo_line, hi_line), where
 * hi_line is exclusive.  The host clips spans to that visible line range.
 */
typedef void (*YewPlugOverlayVisit)(void *ctx, Span span, u8 attr);

void yew_plug_overlay_run(Ed *ed, Win *win, LineNo lo_line, LineNo hi_line,
                          YewPlugOverlayVisit visit, void *ctx);

#endif /* YEW_MOD_PLUG_OVERLAY_H */
