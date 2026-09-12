/*
 * YEW-F-016 — required 1-based display edges violate the LSP +/-1 gate.
 *
 * Correct behavior: Sprint 46's DoD requires the literal line-number scan to
 * be empty across src/mod/lsp/.
 *
 * Baseline failure: Sprint 47 correctly added 1-based picker and error
 * display conversions inside that directory.  The frozen gate therefore
 * rejects the required implementation even though protocol coordinates stay
 * zero-based.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool count_gate_matches(const char *dir, unsigned *matches)
{
    DIR *stream = opendir(dir);
    struct dirent *entry;

    if (stream == NULL)
        return false;
    while ((entry = readdir(stream)) != NULL) {
        char path[PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) <= 0 ||
            lstat(path, &st) != 0) {
            (void)closedir(stream);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!count_gate_matches(path, matches)) {
                (void)closedir(stream);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            char line[4096];
            FILE *file = fopen(path, "rb");

            if (file == NULL) {
                (void)closedir(stream);
                return false;
            }
            while (fgets(line, sizeof(line), file) != NULL) {
                if (strstr(line, "line + 1") != NULL ||
                    strstr(line, "line - 1") != NULL ||
                    strstr(line, ".v + 1") != NULL)
                    (*matches)++;
            }
            if (ferror(file) || fclose(file) != 0) {
                (void)closedir(stream);
                return false;
            }
        }
    }
    return closedir(stream) == 0;
}

bool test_yew_f_016(char *why, size_t why_cap)
{
    unsigned matches = 0U;

    if (!count_gate_matches("src/mod/lsp", &matches))
        return false;
    if (matches != 0U) {
        (void)snprintf(why, why_cap,
                       "LSP line-number +/-1 gate has %u source matches",
                       matches);
    }
    return matches == 0U;
}
