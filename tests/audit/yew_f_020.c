/*
 * YEW-F-020 — the Git formatting grep rejects legitimate display strings.
 *
 * Correct behavior: Sprint 51 requires its sprintf|bytebuf_printf source
 * gate to be empty outside porcelain.c.
 *
 * Baseline failure: seven bytebuf_printf calls format owned UI/detail text,
 * not argv elements.  The semantic argv rule holds, but the literal release
 * gate cannot distinguish it from forbidden pathname formatting.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool format_scan_file(const char *path, unsigned *matches)
{
    char line[4096];
    FILE *file;

    if (strcmp(path, "src/mod/git/porcelain.c") == 0)
        return true;
    file = fopen(path, "rb");
    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "sprintf") != NULL ||
            strstr(line, "bytebuf_printf") != NULL)
            (*matches)++;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

static bool format_scan_tree(const char *dir, unsigned *matches)
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
            if (!format_scan_tree(path, matches)) {
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
        if (!format_scan_file(path, matches)) {
            (void)closedir(stream);
            return false;
        }
    }
    return closedir(stream) == 0;
}

bool test_yew_f_020(char *why, size_t why_cap)
{
    unsigned matches = 0U;

    if (!format_scan_tree("src/mod/git", &matches))
        return false;
    if (matches != 0U) {
        (void)snprintf(why, why_cap,
                       "Git formatting gate has %u non-porcelain matches",
                       matches);
    }
    return matches == 0U;
}
