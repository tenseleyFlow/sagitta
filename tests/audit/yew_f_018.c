/*
 * YEW-F-018 — FUSS picker detail bypasses the module clock discipline.
 *
 * Correct behavior: Sprint 51's source gate and Sprint 58 F13 require the Git
 * module to use yew's injected/monotonic clock surfaces and contain no actual
 * time(2), clock(3), or cpu_time call.
 *
 * Baseline failure: fuss_detail_relative calls time(NULL) directly, making
 * picker detail depend on an uninjectable system-clock read. The original
 * literal gate also mistook clock-suffixed helper names for forbidden calls;
 * this control tokenizes identifiers and ignores comments and strings.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool read_source(const char *path, char *buf, size_t cap)
{
    FILE *file = fopen(path, "rb");
    size_t len;

    if (file == NULL || cap == 0U)
        return false;
    len = fread(buf, 1U, cap - 1U, file);
    if (ferror(file) || fclose(file) != 0)
        return false;
    buf[len] = '\0';
    return true;
}

static bool ident_start(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool ident_continue(int c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

static bool ascii_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static bool forbidden_ident(const char *word, size_t len)
{
    return (len == sizeof("time") - 1U &&
            memcmp(word, "time", sizeof("time") - 1U) == 0) ||
           (len == sizeof("clock") - 1U &&
            memcmp(word, "clock", sizeof("clock") - 1U) == 0) ||
           (len == sizeof("cpu_time") - 1U &&
            memcmp(word, "cpu_time", sizeof("cpu_time") - 1U) == 0);
}

static bool skip_quoted(FILE *file, int quote)
{
    int c;
    bool escaped = false;

    while ((c = fgetc(file)) != EOF) {
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == quote) {
            return true;
        }
    }
    return !ferror(file);
}

static bool clock_scan_file(const char *path, unsigned *matches)
{
    FILE *file = fopen(path, "rb");
    int c;

    if (file == NULL)
        return false;
    while ((c = fgetc(file)) != EOF) {
        if (c == '/') {
            int next = fgetc(file);

            if (next == '/') {
                while ((c = fgetc(file)) != EOF && c != '\n')
                    ;
                continue;
            }
            if (next == '*') {
                int prev = 0;

                while ((c = fgetc(file)) != EOF) {
                    if (prev == '*' && c == '/')
                        break;
                    prev = c;
                }
                continue;
            }
            if (next != EOF)
                (void)ungetc(next, file);
        } else if (c == '"' || c == '\'') {
            if (!skip_quoted(file, c)) {
                (void)fclose(file);
                return false;
            }
        } else if (ident_start(c)) {
            char word[16];
            size_t len = 0U;

            do {
                if (len < sizeof(word))
                    word[len] = (char)c;
                len++;
                c = fgetc(file);
            } while (c != EOF && ident_continue(c));
            if (forbidden_ident(word, len)) {
                while (c != EOF && ascii_space(c))
                    c = fgetc(file);
                if (c == '(')
                    (*matches)++;
            }
            if (c != EOF)
                (void)ungetc(c, file);
        }
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
    char fuss[200000];
    const char *formatter;
    const char *formatter_end;
    const char *detail;
    const char *detail_end;
    const char *hit;
    unsigned matches = 0U;
    bool anchored;
    bool delegated;

    if (!clock_scan_tree("src/mod/git", &matches) ||
        !read_source("src/mod/git/fussmode.c", fuss, sizeof(fuss)))
        return false;
    formatter = strstr(fuss, "size_t yew_fuss_relative_time(");
    formatter_end = formatter == NULL ? NULL :
                    strstr(formatter, "static void fuss_detail_relative(");
    detail = formatter_end;
    detail_end = detail == NULL ? NULL :
                 strstr(detail, "static bool fuss_parse_records(");
    hit = formatter == NULL ? NULL :
          strstr(formatter, "yew_git_editor_wall_now");
    anchored = formatter_end != NULL && hit != NULL && hit < formatter_end;
    hit = detail == NULL ? NULL :
          strstr(detail, "yew_fuss_relative_time");
    delegated = detail_end != NULL && hit != NULL && hit < detail_end;
    if (matches != 0U || !anchored || !delegated)
        (void)snprintf(why, why_cap,
                       "Git forbidden clock calls=%u anchored=%u delegated=%u",
                       matches, anchored ? 1U : 0U, delegated ? 1U : 0U);
    return matches == 0U && anchored && delegated;
}
