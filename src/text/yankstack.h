#ifndef YEW_TEXT_YANKSTACK_H
#define YEW_TEXT_YANKSTACK_H

/*
 * Sprint 57.28 §1: the yank stack -- readline's kill ring.
 *
 * The readline kills (C-w, A-<bs>, A-d, C-u, C-k) push here and C-y / A-y
 * read from here, in Insert mode and in the `:` prompt alike.  It is
 * FULLY SEPARATE from yew's registers: a kill never writes the unnamed
 * register, the register ring (text/register.h) or the system clipboard,
 * so no register paste ever pastes a kill, and no yank, delete or cut
 * into a register changes what C-y yanks.
 * Two stores, two key families; neither can clobber the other.
 *
 * CONSECUTIVE kills join, the way readline's do: C-k C-k is one entry,
 * A-<bs> A-<bs> one entry with the earlier word in front.  "Consecutive"
 * is decided by the dispatcher's command sequence number (Ed.invoke_seq,
 * bumped once per yew_ed_invoke) and the identity of the Win the kill ran
 * in: the kill extends the newest entry when the previous command the
 * dispatcher ran was a kill in the same Win.  Every other command --
 * a motion, a typed character, a mode change -- breaks the chain simply
 * by taking a sequence number, so no command has to clear a flag.
 *
 * SESSION-ONLY.  Nothing here is saved in workspace state.
 */

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"
#include "util/buf.h"

#define YEW_YANK_MAX 32U
#define YEW_YANK_BYTES_MAX (UINT64_C(1) << 20)

typedef enum {
    YEW_KILL_FORWARD,   /* extends by appending (C-k, A-d)               */
    YEW_KILL_BACKWARD,  /* extends by prepending (C-w, A-<bs>, C-u)      */
    /* A kill over several cursors: always its own entry, and never
     * extended -- joining a 3-cursor kill onto a 1-cursor one means
     * nothing. */
    YEW_KILL_ALONE
} YewKillDir;

typedef struct YewYankStack {
    Bytebuf entry[YEW_YANK_MAX];  /* newest at `head`                    */
    u32 head, len;
    u64 bytes, bytes_max;         /* oldest evicted past bytes_max       */
    u64 kill_seq;                 /* Ed.invoke_seq of the last kill      */
    const void *kill_owner;       /* the Win it ran in (identity only)   */
    bool kill_open;               /* the newest entry may be extended    */
    /* The last yank, for A-y: what it inserted at each caret (the bytes
     * before every caret, `yank_len` long) and which entry it was. */
    u64 yank_seq;
    const void *yank_owner;
    u64 yank_len;
    u32 yank_k;
} YewYankStack;

void yew_yank_init(YewYankStack *y);
void yew_yank_free(YewYankStack *y);
/*
 * Record a kill of `n` bytes.  `seq` is the dispatcher's sequence number
 * of the command doing it and `owner` the Win it ran in: when `seq` is
 * exactly one past `kill_seq` in the same owner and neither kill is
 * YEW_KILL_ALONE, the kill EXTENDS the newest entry -- appended for
 * FORWARD, prepended for BACKWARD.  The newest entry is never evicted,
 * even alone over `bytes_max`: losing what was just killed is data loss.
 */
void yew_yank_kill(YewYankStack *y, const u8 *b, size_t n, YewKillDir d,
                   u64 seq, const void *owner);
/* Entry `k` back from the newest (0 = newest), or NULL. */
const Bytebuf *yew_yank_at(const YewYankStack *y, u32 k);

#endif
