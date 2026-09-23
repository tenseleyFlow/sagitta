#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.32: cd-aware completion -- the consumers.
 *
 * The lexer's half (which directory the caret's command runs in) is the
 * corpus in shctx_corpus.h.  This file proves every consumer of a
 * directory follows it: the path source, `./` executables, generators,
 * fish, the help layer, and every cache keyed on a directory.
 */

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/cmdcomp.h"
#include "ui/cmdparse.h"

typedef struct CdFix {
    char root[128];
    Ed ed;
} CdFix;

static void cd_path(const CdFix *f, const char *rel, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", f->root, rel);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

static void cd_touch(const CdFix *f, const char *rel, mode_t mode)
{
    char path[512];
    int fd;

    cd_path(f, rel, path, sizeof(path));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void cd_mkdir(const CdFix *f, const char *rel)
{
    char path[512];

    cd_path(f, rel, path, sizeof(path));
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void cd_rm(const CdFix *f, const char *rel, bool dir)
{
    char path[512];

    cd_path(f, rel, path, sizeof(path));
    YEW_ASSERT_EQ_I64(dir ? rmdir(path) : unlink(path), 0);
}

static void cd_fix_init(CdFix *f)
{
    (void)strcpy(f->root, "/tmp/yew-cdaware-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)memset(&f->ed, 0, sizeof(f->ed));
    arena_init(&f->ed.arena);
    f->ed.ws.dir = f->root;
}

static void cd_fix_drop(CdFix *f)
{
    arena_free_all(&f->ed.arena);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
}

static const CompItem *cd_find(const Vec_CompItem *items, const char *text)
{
    size_t i;

    for (i = 0U; i < items->len; i++) {
        if (strcmp(items->data[i].text, text) == 0)
            return &items->data[i];
    }
    return NULL;
}

/*
 * §3's pitfall, written before the fix: complete `ls s‸`, then insert
 * `cd sub && ` before `ls` with the menu still open.  Head (""), pattern
 * ("s"), row, position and argv[0] are all unchanged, so a cache that
 * does not key on the EFFECTIVE directory re-ranks the prompt
 * directory's rows -- names from the wrong place.
 */
void test_cdaware_cache_rekeys_on_inserted_cd(void)
{
    static const char before[] = ":!ls s";
    static const char after[] = ":!cd sub && ls s";
    CdFix f;
    Arena scratch;
    Arena arena;
    CompFilter filter;
    YewCompQuery q;
    Vec_CompItem rows = {0};

    cd_fix_init(&f);
    cd_mkdir(&f, "sub");
    cd_touch(&f, "sa-top", 0600);
    cd_touch(&f, "sub/sb-inner", 0600);
    arena_init(&scratch);
    arena_init(&arena);
    yew_comp_filter_init(&filter);

    YEW_ASSERT(yew_comp_query(&f.ed, before, strlen(before), strlen(before),
                              &scratch, &q));
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));

    YEW_ASSERT(yew_comp_query(&f.ed, after, strlen(after), strlen(after),
                              &scratch, &q));
    YEW_ASSERT_EQ_STR(q.stem, "s");
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sb-inner"));
    YEW_ASSERT_NULL(cd_find(&rows, "sa-top"));

    /* And back: deleting the `cd` re-keys again. */
    YEW_ASSERT(yew_comp_query(&f.ed, before, strlen(before), strlen(before),
                              &scratch, &q));
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));

    Vec_CompItem_free(&rows);
    yew_comp_filter_free(&filter);
    yew_comp_listing_invalidate();
    arena_free_all(&arena);
    arena_free_all(&scratch);
    cd_rm(&f, "sub/sb-inner", false);
    cd_rm(&f, "sub", true);
    cd_rm(&f, "sa-top", false);
    cd_fix_drop(&f);
}
