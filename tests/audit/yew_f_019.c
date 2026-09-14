/*
 * YEW-F-019 — the porcelain rename mutation control is ineffective.
 *
 * Correct behavior: Sprint 51 and Sprint 58 F13 require the existing rename
 * fixture to fail when both parser passes consume only the destination NUL.
 *
 * Baseline failure: the source pathname began with an unrecognised record
 * byte. A one-NUL mutant therefore skipped it as an unknown record and still
 * exposed the same seven entries asserted by the unit test. The replacement
 * fixture starts that path with a recognised record prefix, making stream
 * desynchronisation observable.
 */
#include "audit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OID_A "0123456789abcdef0123456789abcdef01234567"
#define OID_B "89abcdef0123456789abcdef0123456789abcdef"
#define OID_C "fedcba9876543210fedcba9876543210fedcba98"

static size_t one_nul_mutant_entry_count(const uint8_t *buf, size_t len)
{
    size_t at = 0U;
    size_t count = 0U;

    while (at < len) {
        const uint8_t *nul = memchr(buf + at, 0, len - at);
        uint8_t kind;

        if (nul == NULL)
            return SIZE_MAX;
        kind = buf[at];
        at = (size_t)(nul - buf) + 1U;
        if (kind == (uint8_t)'1' || kind == (uint8_t)'2' ||
            kind == (uint8_t)'u' || kind == (uint8_t)'?' ||
            kind == (uint8_t)'!')
            count++;
    }
    return count;
}

bool test_yew_f_019(char *why, size_t why_cap)
{
    static const uint8_t input[] =
        "2 R. N... 100644 100644 100644 " OID_A " " OID_B
        " R100 renamed\npath\0"
        "1 malformed rename source\0"
        "1 M. N... 100644 100644 100644 " OID_A " " OID_B " one\0"
        "1 .M N... 100644 100644 100644 " OID_A " " OID_B " two\0"
        "1 A. N... 000000 100644 100644 " OID_A " " OID_B " three\0"
        "1 D. N... 100644 000000 000000 " OID_A " " OID_B " four\0"
        "1 T. N... 100644 120000 120000 " OID_A " " OID_B " five\0"
        "2 C. N... 100644 100644 100644 " OID_A " " OID_C
        " C100 copied\0copy source\0";
    size_t mutant_count =
        one_nul_mutant_entry_count(input, sizeof(input) - 1U);

    if (mutant_count != 8U) {
        (void)snprintf(why, why_cap,
                       "one-NUL mutant exposes %zu entries rather than 8",
                       mutant_count);
    }
    return mutant_count == 8U;
}
