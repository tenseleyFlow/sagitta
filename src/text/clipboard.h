#ifndef YEW_TEXT_CLIPBOARD_H
#define YEW_TEXT_CLIPBOARD_H

#include <stdbool.h>

#include "text/register.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_CLIPBOARD_TIMEOUT_DEFAULT_MS 1000
#define YEW_CLIPBOARD_READ_MAX_DEFAULT (UINT64_C(64) * 1024U * 1024U)

typedef enum {
    YEW_CLIP_NONE = 0,
    YEW_CLIP_WL,
    YEW_CLIP_XCLIP,
    YEW_CLIP_XSEL,
    YEW_CLIP_PB,
    YEW_CLIP_OSC52,
    YEW_CLIP_CUSTOM
} YewClipBackend;

/* Cached write-side detection. Reads use the same local preference order but
 * intentionally never select OSC 52. */
YewClipBackend yew_clip_detect(void);
YewClipBackend yew_clip_detect_read(void);

/* Writes are copied into a one-item coalescing queue. The future editor loop
 * calls after_render once per input burst, then pump whenever the pipe is
 * writable or its deadline expires. */
bool yew_clip_write(const RegVal *v, u8 target);
void yew_clip_after_render(Bytebuf *terminal_out, i64 now_ms);
void yew_clip_pump(i64 now_ms);
int yew_clip_write_fd(void);
i64 yew_clip_deadline(void);
bool yew_clip_busy(void);
bool yew_clip_pending(void);
void yew_clip_reap(void);
u32 yew_clip_owned_children(void);

/* Clipboard reads are subprocess-only and synchronously bounded. `out` must
 * be initialized by yew_regval_init(). */
bool yew_clip_read(RegVal *out, u8 target);
void yew_clip_set_read_max(u64 max_bytes);

/* Session/test lifecycle. Reset restores defaults and invalidates detection;
 * SIGPIPE intentionally remains ignored process-wide. */
void yew_clip_reset(void);
void yew_clip_shutdown(void);

#endif
