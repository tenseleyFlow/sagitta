#include "fuzzlib.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod/ai/redact.h"

enum {
    AI_REDACT_LARGE_LINE = 1024 * 1024,
    AI_REDACT_NEIGHBORHOOD = 4096
};

static AiRedactPolicy *redact_policy;

static const char *const secret_shapes[] = {
    "AKIA" "ABCDEFGHIJKLMNOP",
    "AWS_SECRET_" "ACCESS_KEY=0123456789012345678901234567890123456789",
    "-----BEGIN RSA PRIVATE KEY-----",
    "ssh-rsa AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    "Bearer abcdefghijklmnop",
    "Authorization: abcdefghijklmnop",
    "eyJabcdefgh.ijklmnop.qrstuvwx",
    "SERVICE_API_KEY=0123456789",
    "postgres://user:password@host/db",
    "ghp_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "xoxb-1234567890",
    "AIza0123456789ABCDEFGHIJKLMNOPQRSTUVWXY",
    "sk-0123456789ABCDEFGHIJ",
    "sk-ant-0123456789ABCDEFGHIJ",
    "private_key='01234567890123456789012345678901'",
    "$2b$12$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq"
};

static bool fail(char *why, size_t why_cap, const char *message)
{
    (void)snprintf(why, why_cap, "%s", message);
    return false;
}

static bool scan_checked(const u8 *prefix, size_t plen,
                         const u8 *suffix, size_t slen,
                         char *why, size_t why_cap)
{
    AiCtx context;
    RedactHit hit;
    u64 half_len;

    if (plen > UINT32_MAX || slen > UINT32_MAX)
        return fail(why, why_cap, "test context exceeds AiCtx limits");
    (void)memset(&context, 0, sizeof(context));
    (void)memset(&hit, 0, sizeof(hit));
    context.prefix = prefix;
    context.plen = (u32)plen;
    context.suffix = suffix;
    context.slen = (u32)slen;
    context.line_1based = 1U;
    if (!yew_ai_redact_scan(redact_policy, &context, &hit))
        return true;
    half_len = hit.in_prefix ? plen : slen;
    if (hit.rule == NULL || hit.line_1based == 0U)
        return fail(why, why_cap, "redaction hit omitted its identity");
    if (hit.off.v > half_len || (u64)hit.len > half_len - hit.off.v)
        return fail(why, why_cap, "redaction hit escaped its context half");
    return true;
}

static bool scan_neighborhood(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    u8 bytes[AI_REDACT_NEIGHBORHOOD];
    size_t selector = len == 0U ? 0U : data[0];
    const char *shape = secret_shapes[selector % YEW_ARRAY_LEN(secret_shapes)];
    size_t shape_len = strlen(shape);
    size_t before = len > 1U ? data[1] % 32U : 7U;
    size_t after = len > 2U ? data[2] % 32U : 11U;
    size_t at = 0U;
    size_t i;

    for (i = 0U; i < before; i++)
        bytes[at++] = len == 0U ? (u8)'x' : data[i % len];
    (void)memcpy(bytes + at, shape, shape_len);
    if (len > 3U && shape_len != 0U)
        bytes[at + data[3] % shape_len] ^= (u8)(1U << (data[3] & 7U));
    at += shape_len;
    for (i = 0U; i < after; i++)
        bytes[at++] = len == 0U ? (u8)'y' : data[(i + before) % len];
    return scan_checked(bytes, at / 2U, bytes + at / 2U,
                        at - at / 2U, why, why_cap);
}

static bool check_ai_redact(const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    size_t split = len == 0U ? 0U :
                   (size_t)data[0] * len / (size_t)UCHAR_MAX;

    if (redact_policy == NULL)
        return fail(why, why_cap, "shipped redaction policy is unavailable");
    if (!scan_checked(data, split, data + split, len - split,
                      why, why_cap))
        return false;
    return scan_neighborhood(data, len, why, why_cap);
}

static bool preflight(char *why, size_t why_cap)
{
    static const u8 invalid_utf8[] = {
        0x80U, 0xc0U, 0x80U, 0xedU, 0xa0U, 0x80U, 0xffU
    };
    u8 *large = malloc(AI_REDACT_LARGE_LINE);
    u64 state = UINT64_C(0x9e3779b97f4a7c15);
    size_t i;
    bool ok;

    if (large == NULL)
        return fail(why, why_cap, "allocating 1 MiB redaction line failed");
    for (i = 0U; i < AI_REDACT_LARGE_LINE; i++) {
        u8 byte;

        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        byte = (u8)(state * UINT64_C(2685821657736338717));
        large[i] = byte == (u8)'\n' ? 0xffU : byte;
    }
    ok = scan_checked(invalid_utf8, sizeof(invalid_utf8), NULL, 0U,
                      why, why_cap) &&
         scan_checked(large, AI_REDACT_LARGE_LINE, NULL, 0U,
                      why, why_cap);
    free(large);
    return ok;
}

int main(int argc, char **argv)
{
    char why[256] = {0};
    int status;

    redact_policy = yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    if (redact_policy == NULL ||
        yew_ai_redact_policy_len(redact_policy) == 0U) {
        (void)fprintf(stderr,
                      "fuzz_ai_redact: shipped policy construction failed\n");
        yew_ai_redact_policy_free(redact_policy);
        return 2;
    }
    if (!preflight(why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_ai_redact: preflight failed: %s\n", why);
        yew_ai_redact_policy_free(redact_policy);
        redact_policy = NULL;
        return 1;
    }
    status = yew_fuzz_main(argc, argv, "fuzz_ai_redact", NULL,
                           check_ai_redact);
    yew_ai_redact_policy_free(redact_policy);
    redact_policy = NULL;
    return status;
}
