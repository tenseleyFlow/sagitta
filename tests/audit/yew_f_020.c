/*
 * YEW-F-020 — the Git formatting grep rejects legitimate display strings.
 *
 * Correct behavior: Git argv construction copies discrete caller-owned
 * elements byte-exactly. Formatting owned FUSS and gutter display buffers is
 * permitted and is not evidence that a pathname was interpolated into argv.
 *
 * Baseline failure: the literal sprintf|bytebuf_printf gate matched six
 * display-only calls outside porcelain.c. This semantic control pins the
 * structural builder and registered hostile-filename matrix instead.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

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

static bool span_contains(const char *begin, const char *end,
                          const char *needle)
{
    const char *hit;

    if (begin == NULL || end == NULL || begin >= end)
        return false;
    hit = strstr(begin, needle);
    return hit != NULL && hit < end;
}

bool test_yew_f_020(char *why, size_t why_cap)
{
    char git[100000];
    char unit[160000];
    char script[100000];
    const char *builder;
    const char *builder_end;
    const char *matrix;
    const char *matrix_end;
    bool structural_copy;
    bool unformatted;
    bool unit_registered;
    bool hostile_names;
    bool path_assertion;
    bool surfaces;

    if (!read_source("src/mod/git/git.c", git, sizeof(git)) ||
        !read_source("tests/unit/registry.c", unit, sizeof(unit)) ||
        !read_source("tests/script/fuss_commands.c", script,
                     sizeof(script)))
        return false;
    builder = strstr(git, "static char **git_build_argv(");
    builder_end = builder == NULL ? NULL :
                  strstr(builder, "static GitStatusCode git_auth_state(");
    structural_copy = span_contains(builder, builder_end,
                                    "memcpy(argv + at, tail,");
    unformatted = builder != NULL && builder_end != NULL &&
                  !span_contains(builder, builder_end, "sprintf(") &&
                  !span_contains(builder, builder_end, "snprintf(") &&
                  !span_contains(builder, builder_end, "bytebuf_printf(");
    unit_registered = strstr(unit,
        "T(gitcache_verb_table_and_argv_are_structural)") != NULL;
    matrix = strstr(script,
        "static void test_f13_filename_matrix_roundtrips_every_git_surface(");
    matrix_end = matrix == NULL ? NULL :
                 strstr(matrix,
                        "static void test_stage_all_stages_every_change(");
    hostile_names = span_contains(matrix, matrix_end, "\"a b\"") &&
                    span_contains(matrix, matrix_end, "\"a\\nb\"") &&
                    span_contains(matrix, matrix_end, "\"a\\\"b\"") &&
                    span_contains(matrix, matrix_end, "invalid_name");
    path_assertion = span_contains(matrix, matrix_end,
        "argv_index(&capture, names[i]) > dash");
    surfaces = span_contains(matrix, matrix_end, "ed.git.stage") &&
               span_contains(matrix, matrix_end, "ed.git.unstage") &&
               span_contains(matrix, matrix_end, "ed.git.diff") &&
               span_contains(matrix, matrix_end, "ed.git.blame");
    if (!structural_copy || !unformatted || !unit_registered ||
        !hostile_names || !path_assertion || !surfaces) {
        (void)snprintf(why, why_cap,
                       "Git argv copy=%u unformatted=%u unit=%u names=%u path=%u surfaces=%u",
                       structural_copy ? 1U : 0U, unformatted ? 1U : 0U,
                       unit_registered ? 1U : 0U, hostile_names ? 1U : 0U,
                       path_assertion ? 1U : 0U, surfaces ? 1U : 0U);
    }
    return structural_copy && unformatted && unit_registered &&
           hostile_names && path_assertion && surfaces;
}
