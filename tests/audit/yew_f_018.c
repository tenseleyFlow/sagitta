/*
 * YEW-F-018 — FUSS picker detail bypasses the module clock discipline.
 *
 * Correct behavior: Sprint 51's source gate and Sprint 58 F13 require the Git
 * module to use yew's injected/monotonic clock surfaces and contain no
 * time(2), clock(3), or cpu_time call.
 *
 * Baseline failure: fuss_detail_relative calls time(NULL) directly, making
 * picker detail depend on an uninjectable system-clock read and causing the
 * mandatory module-wide source gate to fail.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool has_word_time_call(const char *line)
{
    const char *hit = line;

    while ((hit = strstr(hit, "time(")) != NULL) {
        if (hit == line || !((hit[-1] >= 'A' && hit[-1] <= 'Z') ||
                             (hit[-1] >= 'a' && hit[-1] <= 'z') ||
                             (hit[-1] >= '0' && hit[-1] <= '9') ||
                             hit[-1] == '_'))
            return true;
        hit += sizeof("time(") - 1U;
    }
    return false;
}

static bool clock_scan_file(const char *path, unsigned *matches)
{
    char line[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        /* Match Sprint 51's literal grep, including its clock-suffixed
         * helper-name false positives; only `time(` has a word boundary. */
        if (strstr(line, "clock(") != NULL ||
            strstr(line, "cpu_time") != NULL || has_word_time_call(line))
            (*matches)++;
    }
    if (ferror(file) || fclose(file) != 0)
        return false;
    return true;
}

static bool clock_scan_tree(const char *dir, unsigned *matches)
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
            if (!clock_scan_tree(path, matches)) {
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
        if (!clock_scan_file(path, matches)) {
            (void)closedir(stream);
            return false;
        }
    }
    return closedir(stream) == 0;
}

bool test_yew_f_018(char *why, size_t why_cap)
{
    unsigned matches = 0U;

    if (!clock_scan_tree("src/mod/git", &matches))
        return false;
    if (matches != 0U)
        (void)snprintf(why, why_cap,
                       "Git module forbidden clock matches=%u", matches);
    return matches == 0U;
}
