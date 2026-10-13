/*
 * YEW-F-084 — Fletch rejects safe quoted patterns before the regex compiler.
 *
 * Correct behavior: the re module must admit the core compiler's bounded
 * fourfold textual representation so re.test(s, re.escape(s)) can reach the
 * documented 64 KiB semantic limit.
 *
 * Baseline failure: compile_cached rejects every pattern above the old
 * 64 KiB textual cap before yew_re_compile can validate the decoded size.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool source_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    char buf[32768];
    size_t n;

    if (f == NULL)
        return false;
    n = fread(buf, 1U, sizeof(buf) - 1U, f);
    (void)fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

bool test_yew_f_084(char *why, size_t why_cap)
{
    bool passed = source_has("src/fl/stdre.c",
                             "YEW_RE_MAX_PATTERN * 4U");

    if (!passed)
        (void)snprintf(why, why_cap,
                       "Fletch still imposes the stale 64 KiB text cap");
    return passed;
}
