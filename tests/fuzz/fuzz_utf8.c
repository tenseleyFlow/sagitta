#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "unicode/utf8.h"

static bool exhaustive_three_byte_inputs(void)
{
    unsigned int value;

    for (value = 0U; value <= 0xFFFFFFU; value++) {
        u8 input[3];
        u8 encoded[3];
        u32 decoded[3];
        SagU8Dec incremental;
        size_t in_pos = 0U;
        size_t encoded_len = 0U;
        size_t decoded_len = 0U;
        size_t incremental_len = 0U;
        size_t i;

        input[0] = (u8)(value >> 16);
        input[1] = (u8)(value >> 8);
        input[2] = (u8)value;
        while (in_pos < sizeof(input)) {
            u8 bytes[SAG_UTF8_MAX];
            u32 cp;
            size_t consumed = sag_utf8_decode(
                input + in_pos, sizeof(input) - in_pos, &cp);
            size_t produced = sag_utf8_encode(cp, bytes);

            if (consumed == 0U || consumed > sizeof(input) - in_pos ||
                produced != consumed ||
                encoded_len + produced > sizeof(encoded))
                return false;
            (void)memcpy(encoded + encoded_len, bytes, produced);
            encoded_len += produced;
            decoded[decoded_len++] = cp;
            in_pos += consumed;
        }
        if (encoded_len != sizeof(input) ||
            memcmp(encoded, input, sizeof(input)) != 0)
            return false;

        sag_utf8_dec_init(&incremental);
        for (i = 0U; i < sizeof(input); i++) {
            u8 count = sag_utf8_push(&incremental, input[i]);
            u8 j;

            for (j = 0U; j < count; j++) {
                if (incremental_len >= decoded_len ||
                    incremental.out[j] != decoded[incremental_len++])
                    return false;
            }
        }
        {
            u8 count = sag_utf8_finish(&incremental);
            u8 j;

            for (j = 0U; j < count; j++) {
                if (incremental_len >= decoded_len ||
                    incremental.out[j] != decoded[incremental_len++])
                    return false;
            }
        }
        if (incremental_len != decoded_len)
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (!exhaustive_three_byte_inputs()) {
        (void)fprintf(stderr,
                      "fuzz_utf8: exhaustive three-byte sweep failed\n");
        return 1;
    }
    (void)printf("fuzz_utf8: exhaustive-three-byte=16777216 ok\n");
    return sag_fuzz_main(argc, argv, "fuzz_utf8",
                         "tests/unit/fixtures/unicode",
                         sag_fuzz_check_utf8);
}
