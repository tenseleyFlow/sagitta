/*
 * YEW-F-024 — the cross-surface XFAIL debt table stops at F004.
 *
 * Correct behavior: Sprint 58 section 3 and F15 q1 require every live
 * finding to have one row in the authoritative cross-surface debt table.
 *
 * Baseline failure: findings.md contains 79 retained IDs, while
 * xfail-debt.md contains only F001 through F004.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool collect_ids(const char *path, bool ids[1000], size_t *count,
                        size_t *duplicates)
{
    char line[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned id;

        if (sscanf(line, "| YEW-F-%3u |", &id) == 1 && id < 1000U) {
            if (ids[id]) {
                (*duplicates)++;
            } else {
                ids[id] = true;
                (*count)++;
            }
        }
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

bool test_yew_f_024(char *why, size_t why_cap)
{
    bool findings[1000] = {false};
    bool debt[1000] = {false};
    size_t finding_count = 0U;
    size_t debt_count = 0U;
    size_t finding_duplicates = 0U;
    size_t debt_duplicates = 0U;
    size_t missing = 0U;
    size_t unexpected = 0U;
    size_t i;

    if (!collect_ids(".docs/audits/findings.md", findings, &finding_count,
                     &finding_duplicates) ||
        !collect_ids(".docs/audits/xfail-debt.md", debt, &debt_count,
                     &debt_duplicates))
        return false;
    for (i = 0U; i < 1000U; i++) {
        if (findings[i] && !debt[i])
            missing++;
        if (debt[i] && !findings[i])
            unexpected++;
    }
    if (missing != 0U || unexpected != 0U || finding_duplicates != 0U ||
        debt_duplicates != 0U)
        (void)snprintf(why, why_cap,
                       "finding rows=%lu debt rows=%lu missing=%lu "
                       "unexpected=%lu duplicates=%lu/%lu",
                       (unsigned long)finding_count,
                       (unsigned long)debt_count,
                       (unsigned long)missing, (unsigned long)unexpected,
                       (unsigned long)finding_duplicates,
                       (unsigned long)debt_duplicates);
    return finding_count != 0U && finding_count == debt_count &&
           missing == 0U && unexpected == 0U &&
           finding_duplicates == 0U && debt_duplicates == 0U;
}
