/*
 * YEW-F-015 — the snippet-policy grep gate matches unrelated core code.
 *
 * Correct behavior: Sprint 47 and Sprint 58 F11 require the repository-wide
 * tabstop|placeholder scan to hit only the LSP downgrade-policy paragraph.
 *
 * Baseline failure: several ordinary core implementation comments and names
 * use "placeholder", so the literal release gate cannot produce its promised
 * one-policy-line result.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct MatchCount {
    unsigned total;
    unsigned policy;
} MatchCount;

static bool scan_file(const char *path, MatchCount *count)
{
    char line[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "tabstop") == NULL &&
            strstr(line, "placeholder") == NULL)
            continue;
        count->total++;
        if (strcmp(path, "src/mod/lsp/features.c") == 0 &&
            strstr(line, "tab-stop mode, placeholder") != NULL)
            count->policy++;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

static bool scan_tree(const char *dir, MatchCount *count)
{
    DIR *stream = opendir(dir);
    struct dirent *entry;

    if (stream == NULL)
        return false;
    while ((entry = readdir(stream)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        size_t len;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) <= 0 ||
            lstat(path, &st) != 0) {
            (void)closedir(stream);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!scan_tree(path, count)) {
                (void)closedir(stream);
                return false;
            }
            continue;
        }
        len = strlen(path);
        if (!S_ISREG(st.st_mode) || len < 2U ||
            (strcmp(path + len - 2U, ".c") != 0 &&
             strcmp(path + len - 2U, ".h") != 0))
            continue;
        if (!scan_file(path, count)) {
            (void)closedir(stream);
            return false;
        }
    }
    return closedir(stream) == 0;
}

bool test_yew_f_015(char *why, size_t why_cap)
{
    MatchCount count = {0};

    if (!scan_tree("src", &count))
        return false;
    if (count.total != 1U || count.policy != 1U) {
        (void)snprintf(why, why_cap,
                       "tabstop|placeholder source matches: total=%u policy=%u",
                       count.total, count.policy);
    }
    return count.total == 1U && count.policy == 1U;
}
