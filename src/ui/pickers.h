#ifndef SAG_UI_PICKERS_H
#define SAG_UI_PICKERS_H

/*
 * Sprint 26 §6: the three picker instances.
 *
 * Each is a PickerSpec and nothing else — no new widget, no second
 * event loop, no private key handling.  That is the claim §5 makes and
 * this file is where it is either true or it is not.
 *
 * The undo branch picker closes Sprint 10 §11's deferral.  Its rows come
 * from sag_undo_describe with `now` PASSED IN rather than read from a
 * clock, so "3 minutes ago" is deterministic and the goldens can pin it
 * (invariant 5).  sag_undo_dump stays as the headless surface.
 */

#include "edit/cmd.h"
#include "ui/layout.h"
#include "util/base.h"

typedef struct Ed Ed;

CmdStatus sag_find_cmd_file(CmdCtx *cx);
CmdStatus sag_find_cmd_buffer(CmdCtx *cx);
CmdStatus sag_undo_cmd_branches(CmdCtx *cx);

/*
 * Test seam for the undo picker's clock.  Zero means "read the wall
 * clock", which is what the editor does; a test sets it so the
 * descriptions are stable.
 */
void sag_pickers_set_now(i64 now);

/*
 * Previews a file into `r` WITHOUT creating a Buffer or a TextBuf
 * (DoD 11).  Exposed because the finder's spec points at it and the
 * test counts what it does.
 */
void sag_pickers_preview_file(Ed *ed, void *ctx, i32 payload, Rect r);

/* Test hooks: preview reads performed, and buffers created by them —
 * the second must stay zero. */
u64 sag_pickers_preview_reads(void);
void sag_pickers_preview_reset(void);

/* Releases the shared item table; called from sag_ed_free. */
void sag_pickers_dispose(void);

#endif
