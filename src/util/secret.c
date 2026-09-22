#include "util/secret.h"

#include <string.h>

#include "util/base.h"

/*
 * Kept in the SAME spelling as the redactor's alternation, with its one
 * optional byte expanded: `API_?KEY` is both API_KEY and APIKEY.
 */
static const char *const fragments[] = {
    "SECRET", "TOKEN", "PASSWORD", "PASSWD", "PRIVATE_KEY", "API_KEY",
    "APIKEY", "CREDENTIAL"
};

static char upper_ascii(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* ASCII case folding only: environment names are bytes, and a locale-
 * dependent toupper() would make the answer depend on LC_CTYPE. */
static bool contains_folded(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    for (; *hay != '\0'; hay++) {
        for (i = 0U; i < n; i++) {
            if (hay[i] == '\0' || upper_ascii(hay[i]) != needle[i])
                break;
        }
        if (i == n)
            return true;
    }
    return false;
}

bool yew_secret_name(const char *name)
{
    size_t i;

    if (name == NULL)
        return false;
    for (i = 0U; i < YEW_ARRAY_LEN(fragments); i++) {
        if (contains_folded(name, fragments[i]))
            return true;
    }
    return false;
}

const char *const *yew_secret_fragments(size_t *n)
{
    if (n != NULL)
        *n = YEW_ARRAY_LEN(fragments);
    return fragments;
}
