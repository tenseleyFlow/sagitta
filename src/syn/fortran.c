#include "syn/fortran.h"

#include <string.h>

static u8 fold_ascii(u8 c)
{
    return c >= (u8)'A' && c <= (u8)'Z' ?
           (u8)(c + ((u8)'a' - (u8)'A')) : c;
}

static bool word_is(const u8 *line, size_t len, const char *word)
{
    size_t n = strlen(word);
    size_t i;

    if (len < n)
        return false;
    for (i = 0U; i < n; i++)
        if (fold_ascii(line[i]) != (u8)word[i])
            return false;
    return len == n || line[n] == (u8)' ' || line[n] == (u8)'\t' ||
           line[n] == (u8)'(' || line[n] == (u8)':';
}

static bool statement_at_column_one(const u8 *line, size_t len)
{
    static const char *const words[] = {
        "program", "module", "submodule", "subroutine", "function",
        "block", "associate", "select", "interface", "contains",
        "use", "implicit", "integer", "real", "complex", "logical",
        "character", "type", "class", "procedure", "call", "allocate",
        "deallocate", "if", "do", "where", "forall", "read", "write",
        "print", "open", "close", "format", "return", "stop", "end"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(words); i++)
        if (word_is(line, len, words[i]))
            return true;
    return false;
}

void yew_syn_fortran_score_init(SynFortranScore *score)
{
    if (score != NULL)
        (void)memset(score, 0, sizeof(*score));
}

void yew_syn_fortran_score_line(SynFortranScore *score,
                                const u8 *line, size_t len)
{
    size_t lo = 0U;
    size_t hi = len;
    size_t i;
    bool columns_1_5 = true;
    bool statement_after_comment = false;

    if (score == NULL || line == NULL || score->nonblank >= 100U)
        return;
    while (hi != 0U && (line[hi - 1U] == (u8)'\n' ||
                        line[hi - 1U] == (u8)'\r'))
        hi--;
    while (lo < hi && (line[lo] == (u8)' ' || line[lo] == (u8)'\t'))
        lo++;
    if (lo == hi)
        return;
    score->nonblank++;

    if (line[0] == (u8)'C' || line[0] == (u8)'c' ||
        line[0] == (u8)'*' || line[0] == (u8)'!') {
        for (i = 1U; i < hi; i++)
            if (line[i] != (u8)' ' && line[i] != (u8)'\t') {
                statement_after_comment = true;
                break;
            }
        if (statement_after_comment) {
            score->fixed_form += 2;
            score->signals++;
        }
    }
    if (hi > 5U) {
        for (i = 0U; i < 5U; i++)
            if (line[i] != (u8)' ' &&
                (line[i] < (u8)'0' || line[i] > (u8)'9'))
                columns_1_5 = false;
        if (columns_1_5 && line[5] != (u8)' ' && line[5] != (u8)'\t' &&
            line[5] != (u8)'0') {
            score->fixed_form += 3;
            score->signals++;
        }
    }
    if (hi != 0U) {
        size_t last = hi;

        while (last != 0U &&
               (line[last - 1U] == (u8)' ' || line[last - 1U] == (u8)'\t'))
            last--;
        if (last != 0U && line[last - 1U] == (u8)'&') {
            score->free_form += 3;
            score->signals++;
        }
    }
    for (i = 1U; i < hi; i++)
        if (line[i] == (u8)'!') {
            score->free_form += 2;
            score->signals++;
            break;
        }
    if (lo == 0U && statement_at_column_one(line, hi)) {
        score->free_form += 1;
        score->signals++;
    }
    for (i = 72U; i < hi; i++)
        if (line[i] != (u8)' ' && line[i] != (u8)'\t') {
            score->fixed_form -= 2;
            score->signals++;
            break;
        }
}

SynFortranForm yew_syn_fortran_score_result(const SynFortranScore *score)
{
    if (score == NULL || score->signals == 0U)
        return YEW_FORTRAN_AUTO;
    return score->fixed_form > score->free_form ? YEW_FORTRAN_FIXED :
                                                  YEW_FORTRAN_FREE;
}

SynFortranForm yew_syn_fortran_score_bytes(const u8 *text, size_t len,
                                           SynFortranScore *score_out)
{
    SynFortranScore local;
    size_t lo = 0U;

    yew_syn_fortran_score_init(&local);
    while (lo < len && local.nonblank < 100U) {
        size_t hi = lo;

        while (hi < len && text[hi] != (u8)'\n')
            hi++;
        yew_syn_fortran_score_line(&local, text + lo, hi - lo);
        lo = hi < len ? hi + 1U : hi;
    }
    if (score_out != NULL)
        *score_out = local;
    return yew_syn_fortran_score_result(&local);
}

static const char *base_name(const char *path)
{
    const char *slash;

    if (path == NULL)
        return "";
    slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool extension_is(const char *path, const char *want)
{
    const char *base = base_name(path);
    const char *dot = strrchr(base, '.');
    size_t n;
    size_t i;

    if (dot == NULL || dot[1] == '\0')
        return want[0] == '\0';
    dot++;
    n = strlen(dot);
    if (n != strlen(want))
        return false;
    for (i = 0U; i < n; i++)
        if (fold_ascii((u8)dot[i]) != (u8)want[i])
            return false;
    return true;
}

bool yew_syn_fortran_ambiguous_path(const char *path)
{
    const char *base = base_name(path);

    return strchr(base, '.') == NULL || extension_is(path, "inc") ||
           extension_is(path, "i");
}

bool yew_syn_fortran_legacy_path(const char *path)
{
    return extension_is(path, "f") || extension_is(path, "for");
}
