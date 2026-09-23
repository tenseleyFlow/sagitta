#include "text/yankstack.h"

#include <string.h>

void yew_yank_init(YewYankStack *y)
{
    u32 i;

    if (y == NULL)
        return;
    (void)memset(y, 0, sizeof(*y));
    for (i = 0U; i < YEW_YANK_MAX; i++)
        bytebuf_init(&y->entry[i]);
    y->bytes_max = YEW_YANK_BYTES_MAX;
}

void yew_yank_free(YewYankStack *y)
{
    u32 i;

    if (y == NULL)
        return;
    for (i = 0U; i < YEW_YANK_MAX; i++)
        bytebuf_free(&y->entry[i]);
    yew_yank_init(y);
}

static u32 yank_slot(const YewYankStack *y, u32 k)
{
    return (y->head + YEW_YANK_MAX - k) % YEW_YANK_MAX;
}

/* Drop the oldest entry.  Never the newest: see the header. */
static void yank_evict_oldest(YewYankStack *y)
{
    Bytebuf *oldest = &y->entry[yank_slot(y, y->len - 1U)];

    y->bytes -= (u64)oldest->len;
    bytebuf_free(oldest);
    y->len--;
}

static void yank_trim(YewYankStack *y)
{
    while (y->len > 1U && y->bytes > y->bytes_max)
        yank_evict_oldest(y);
}

static void yank_extend(YewYankStack *y, const u8 *b, size_t n,
                        YewKillDir d)
{
    Bytebuf *newest = &y->entry[y->head];

    if (d == YEW_KILL_FORWARD) {
        bytebuf_append(newest, b, n);
    } else {
        Bytebuf joined;

        bytebuf_init(&joined);
        bytebuf_reserve(&joined, n + newest->len);
        bytebuf_append(&joined, b, n);
        bytebuf_append(&joined, newest->data, newest->len);
        bytebuf_free(newest);
        *newest = joined;
    }
}

static void yank_push(YewYankStack *y, const u8 *b, size_t n)
{
    if (y->len == YEW_YANK_MAX)
        yank_evict_oldest(y);
    y->head = y->len == 0U ? 0U : (y->head + 1U) % YEW_YANK_MAX;
    y->entry[y->head].len = 0U;
    bytebuf_append(&y->entry[y->head], b, n);
    y->len++;
}

void yew_yank_kill(YewYankStack *y, const u8 *b, size_t n, YewKillDir d,
                   u64 seq, const void *owner)
{
    bool extend;

    if (y == NULL || b == NULL || n == 0U)
        return;
    extend = y->len != 0U && y->kill_open && d != YEW_KILL_ALONE &&
             owner == y->kill_owner && seq == y->kill_seq + 1U;
    if (extend)
        yank_extend(y, b, n, d);
    else
        yank_push(y, b, n);
    y->bytes += (u64)n;
    y->kill_seq = seq;
    y->kill_owner = owner;
    y->kill_open = d != YEW_KILL_ALONE;
    yank_trim(y);
}

const Bytebuf *yew_yank_at(const YewYankStack *y, u32 k)
{
    if (y == NULL || k >= y->len)
        return NULL;
    return &y->entry[yank_slot(y, k)];
}
