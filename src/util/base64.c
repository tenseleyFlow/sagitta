#include "util/base64.h"

#include <stdint.h>

#include "util/log.h"

static const u8 yew_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

u64 yew_base64_len(u64 n)
{
    u64 groups = n / 3u;

    if (groups > UINT64_MAX / 4u ||
        (n % 3u != 0u && groups == UINT64_MAX / 4u))
        YEW_BUG("base64 encoded length overflow");
    return groups * 4u + (n % 3u != 0u ? 4u : 0u);
}

void yew_base64_encode(const u8 *in, u64 n, u8 *out)
{
    u64 src = 0u;
    u64 dst = 0u;

    while (n - src >= 3u) {
        u32 bits = ((u32)in[src] << 16u) |
                   ((u32)in[src + 1u] << 8u) |
                   (u32)in[src + 2u];

        out[dst++] = yew_base64_alphabet[(bits >> 18u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[(bits >> 12u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[(bits >> 6u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[bits & 0x3fu];
        src += 3u;
    }
    if (n - src == 1u) {
        u32 bits = (u32)in[src] << 16u;

        out[dst++] = yew_base64_alphabet[(bits >> 18u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[(bits >> 12u) & 0x3fu];
        out[dst++] = (u8)'=';
        out[dst] = (u8)'=';
    } else if (n - src == 2u) {
        u32 bits = ((u32)in[src] << 16u) | ((u32)in[src + 1u] << 8u);

        out[dst++] = yew_base64_alphabet[(bits >> 18u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[(bits >> 12u) & 0x3fu];
        out[dst++] = yew_base64_alphabet[(bits >> 6u) & 0x3fu];
        out[dst] = (u8)'=';
    }
}
