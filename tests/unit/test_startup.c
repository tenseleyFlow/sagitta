#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"

typedef struct StartFix {
    char dir[PATH_MAX];
    char link[PATH_MAX];
} StartFix;

static void start_fix_make(StartFix *f)
{
    const char *tmp = getenv("TMPDIR");
    char *dir;
    int n;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(f->dir, sizeof(f->dir), "%s/yew-start-XXXXXX", tmp);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->dir));
    dir = mkdtemp(f->dir);
    YEW_ASSERT_NOT_NULL(dir);
    n = snprintf(f->link, sizeof(f->link), "%s-link", dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->link));
    YEW_ASSERT_EQ_I64(symlink(dir, f->link), 0);
}

static void start_fix_remove(StartFix *f)
{
    YEW_ASSERT_EQ_I64(unlink(f->link), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->dir), 0);
}

void test_startup_plan_resolves_resume_files_and_explicit_workspace(void)
{
    StartFix f;
    YewStartPlan plan;
    const char *files[] = {"alpha.txt", "elsewhere/beta.txt"};
    char cwd[PATH_MAX];
    char error[256];

    start_fix_make(&f);
    YEW_ASSERT_NOT_NULL(realpath(".", cwd));
    YEW_ASSERT(yew_start_plan_resolve(&plan, NULL, 0U, NULL,
                                      error, sizeof(error)));
    YEW_ASSERT_EQ_U64(plan.kind, YEW_START_RESUME);
    YEW_ASSERT_EQ_STR(plan.workspace, cwd);
    YEW_ASSERT_NULL(plan.files);
    YEW_ASSERT_EQ_U64(plan.nfiles, 0U);
    YEW_ASSERT(!plan.enter_fuss);

    YEW_ASSERT(yew_start_plan_resolve(&plan, files, 2U, NULL,
                                      error, sizeof(error)));
    YEW_ASSERT_EQ_U64(plan.kind, YEW_START_FILES);
    YEW_ASSERT(plan.files == files);
    YEW_ASSERT_EQ_U64(plan.nfiles, 2U);
    YEW_ASSERT_EQ_STR(plan.workspace, cwd);
    YEW_ASSERT(!plan.enter_fuss);

    YEW_ASSERT(yew_start_plan_resolve(&plan, files, 2U, f.link,
                                      error, sizeof(error)));
    YEW_ASSERT_EQ_U64(plan.kind, YEW_START_FILES);
    YEW_ASSERT_EQ_STR(plan.workspace, f.dir);
    YEW_ASSERT(plan.files == files);
    YEW_ASSERT_EQ_U64(plan.nfiles, 2U);
    start_fix_remove(&f);
}

void test_startup_plan_canonicalizes_directory_and_symlink_launches(void)
{
    StartFix f;
    YewStartPlan plan;
    const char *paths[1];
    char dotted[PATH_MAX];
    char error[256];
    size_t dir_len;

    start_fix_make(&f);
    dir_len = strlen(f.dir);
    YEW_ASSERT(dir_len + 2U < sizeof(dotted));
    (void)memcpy(dotted, f.dir, dir_len);
    (void)memcpy(dotted + dir_len, "/.", 3U);
    paths[0] = dotted;
    YEW_ASSERT(yew_start_plan_resolve(&plan, paths, 1U, NULL,
                                      error, sizeof(error)));
    YEW_ASSERT_EQ_U64(plan.kind, YEW_START_DIRECTORY);
    YEW_ASSERT_EQ_STR(plan.workspace, f.dir);
    YEW_ASSERT_NULL(plan.files);
    YEW_ASSERT_EQ_U64(plan.nfiles, 0U);
    YEW_ASSERT(plan.enter_fuss);

    paths[0] = f.link;
    YEW_ASSERT(yew_start_plan_resolve(&plan, paths, 1U, NULL,
                                      error, sizeof(error)));
    YEW_ASSERT_EQ_U64(plan.kind, YEW_START_DIRECTORY);
    YEW_ASSERT_EQ_STR(plan.workspace, f.dir);
    YEW_ASSERT(plan.enter_fuss);
    start_fix_remove(&f);
}

void test_startup_plan_rejects_mixed_and_ambiguous_directories(void)
{
    StartFix f;
    YewStartPlan plan;
    const char *mixed[2];
    const char *directory[1];
    char error[256];

    start_fix_make(&f);
    mixed[0] = "alpha.txt";
    mixed[1] = f.link;
    YEW_ASSERT(!yew_start_plan_resolve(&plan, mixed, 2U, NULL,
                                       error, sizeof(error)));
    YEW_ASSERT_NOT_NULL(strstr(error,
                              "directory argument cannot be combined"));

    directory[0] = f.dir;
    YEW_ASSERT(!yew_start_plan_resolve(&plan, directory, 1U, f.dir,
                                       error, sizeof(error)));
    YEW_ASSERT_NOT_NULL(strstr(error, "--workspace"));
    YEW_ASSERT(!yew_start_plan_resolve(&plan, NULL, 0U,
                                       "/no/such/yew-workspace",
                                       error, sizeof(error)));
    YEW_ASSERT_NOT_NULL(strstr(error, "workspace is not a directory"));
    start_fix_remove(&f);
}
