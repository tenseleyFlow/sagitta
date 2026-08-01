#ifndef SAG_TEXT_CLIPBOARD_H
#define SAG_TEXT_CLIPBOARD_H

#include <stdbool.h>

#include "text/register.h"
#include "util/base.h"
#include "util/buf.h"

#define SAG_CLIPBOARD_TIMEOUT_DEFAULT_MS 1000
#define SAG_CLIPBOARD_READ_MAX_DEFAULT (UINT64_C(64) * 1024U * 1024U)

typedef enum {
    SAG_CLIP_NONE = 0,
    SAG_CLIP_WL,
    SAG_CLIP_XCLIP,
    SAG_CLIP_XSEL,
    SAG_CLIP_PB,
    SAG_CLIP_OSC52,
    SAG_CLIP_CUSTOM
} SagClipBackend;

/* Cached write-side detection. Reads use the same local preference order but
 * intentionally never select OSC 52. */
SagClipBackend sag_clip_detect(void);
SagClipBackend sag_clip_detect_read(void);

/* Writes are copied into a one-item coalescing queue. The future editor loop
 * calls after_render once per input burst, then pump whenever the pipe is
 * writable or its deadline expires. */
bool sag_clip_write(const RegVal *v, u8 target);
void sag_clip_after_render(Bytebuf *terminal_out, i64 now_ms);
void sag_clip_pump(i64 now_ms);
int sag_clip_write_fd(void);
i64 sag_clip_deadline(void);
bool sag_clip_busy(void);
bool sag_clip_pending(void);
void sag_clip_reap(void);
u32 sag_clip_owned_children(void);

/* Clipboard reads are subprocess-only and synchronously bounded. `out` must
 * be initialized by sag_regval_init(). */
bool sag_clip_read(RegVal *out, u8 target);
void sag_clip_set_read_max(u64 max_bytes);

/* Session/test lifecycle. Reset restores defaults and invalidates detection;
 * SIGPIPE intentionally remains ignored process-wide. */
void sag_clip_reset(void);
void sag_clip_shutdown(void);

#endif
