#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "util/base64.h"

static i32 base64_value(u8 c)
{
    if (c >= (u8)'A' && c <= (u8)'Z')
        return (i32)(c - (u8)'A');
    if (c >= (u8)'a' && c <= (u8)'z')
        return (i32)(c - (u8)'a') + 26;
    if (c >= (u8)'0' && c <= (u8)'9')
        return (i32)(c - (u8)'0') + 52;
    if (c == (u8)'+')
        return 62;
    if (c == (u8)'/')
        return 63;
    return -1;
}

static size_t base64_decode(const u8 *in, size_t n, u8 *out)
{
    size_t src;
    size_t dst = 0u;

    for (src = 0u; src < n; src += 4u) {
        i32 a = base64_value(in[src]);
        i32 b = base64_value(in[src + 1u]);
        i32 c = in[src + 2u] == (u8)'=' ? 0 : base64_value(in[src + 2u]);
        i32 d = in[src + 3u] == (u8)'=' ? 0 : base64_value(in[src + 3u]);
        u32 bits = ((u32)a << 18u) | ((u32)b << 12u) |
                   ((u32)c << 6u) | (u32)d;

        out[dst++] = (u8)(bits >> 16u);
        if (in[src + 2u] != (u8)'=')
            out[dst++] = (u8)(bits >> 8u);
        if (in[src + 3u] != (u8)'=')
            out[dst++] = (u8)bits;
    }
    return dst;
}

static u64 base64_rng(u64 *state)
{
    u64 x = *state;

    x ^= x << 13u;
    x ^= x >> 7u;
    x ^= x << 17u;
    *state = x;
    return x;
}

void test_base64_rfc4648_vectors(void)
{
    static const struct {
        const char *input;
        const char *encoded;
    } cases[] = {
        {"", ""},       {"f", "Zg=="},     {"fo", "Zm8="},
        {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"foobar", "Zm9vYmFy"}
    };
    size_t i;

    for (i = 0u; i < SAG_ARRAY_LEN(cases); i++) {
        u64 input_len = (u64)strlen(cases[i].input);
        u64 encoded_len = sag_base64_len(input_len);
        u8 *encoded = malloc((size_t)(encoded_len == 0u ? 1u : encoded_len));

        SAG_ASSERT_NOT_NULL(encoded);
        sag_base64_encode((const u8 *)cases[i].input, input_len, encoded);
        SAG_ASSERT_EQ_U64(encoded_len, strlen(cases[i].encoded));
        SAG_ASSERT_EQ_MEM(encoded, cases[i].encoded, (size_t)encoded_len);
        free(encoded);
    }
}

void test_base64_lengths_and_alphabet(void)
{
    u8 input[1024];
    u8 encoded[1368];
    u64 n;

    memset(input, 0xa5, sizeof(input));
    for (n = 0u; n <= sizeof(input); n++) {
        u64 encoded_len = sag_base64_len(n);
        u64 i;

        SAG_ASSERT_EQ_U64(encoded_len, 4u * ((n + 2u) / 3u));
        sag_base64_encode(input, n, encoded);
        for (i = 0u; i < encoded_len; i++) {
            u8 c = encoded[i];

            SAG_ASSERT(base64_value(c) >= 0 || c == (u8)'=');
            SAG_ASSERT(c != (u8)'\n');
            SAG_ASSERT(c != (u8)'\r');
        }
    }
}

void test_base64_binary_roundtrip_fuzz(void)
{
    u8 input[1024];
    u8 encoded[1368];
    u8 decoded[1024];
    u64 state = UINT64_C(0x5a17d3c9e2046b8f);
    u32 iteration;

    for (iteration = 0u; iteration < 10000u; iteration++) {
        size_t n = (size_t)(base64_rng(&state) % (sizeof(input) + 1u));
        size_t encoded_len = (size_t)sag_base64_len((u64)n);
        size_t i;
        size_t decoded_len;

        for (i = 0u; i < n; i++)
            input[i] = (u8)base64_rng(&state);
        sag_base64_encode(input, (u64)n, encoded);
        decoded_len = base64_decode(encoded, encoded_len, decoded);
        SAG_ASSERT_EQ_U64(decoded_len, n);
        SAG_ASSERT_EQ_MEM(decoded, input, n);
    }
}
