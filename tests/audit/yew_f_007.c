/*
 * YEW-F-007 — workspace restore reorders group members by tab array order.
 *
 * Correct behavior: a tab group's saved group_ordinal values determine its
 * member order after restore, even when the enclosing tabs list is ordered
 * 3, 2, 1. Reopening a workspace must preserve the user's group traversal
 * and member-strip order exactly.
 *
 * Baseline failure: apply_tabs attaches each record and immediately calls
 * yew_group_set_ordinal. An early ordinal 3 clamps to the partial group,
 * losing its intended position before ordinals 2 and 1 arrive. The restored
 * order becomes f0, f2, f1 instead of f0, f1, f2.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "text/file.h"
#include "ui/groups.h"
#include "ui/tabs.h"
#include "util/buf.h"
#include "ws/state.h"

enum { F007_FILES = 3 };

typedef struct F007Files {
    char dir[128];
    char paths[F007_FILES][192];
} F007Files;

static bool f007_files_make(F007Files *files)
{
    static const char body[] = "audit fixture\n";
    int i;

    (void)memset(files, 0, sizeof(*files));
    (void)snprintf(files->dir, sizeof(files->dir), "/tmp/yew-f007-XXXXXX");
    if (mkdtemp(files->dir) == NULL)
        return false;
    for (i = 0; i < F007_FILES; i++) {
        FILE *stream;

        (void)snprintf(files->paths[i], sizeof(files->paths[i]),
                       "%s/f%d.txt", files->dir, i);
        stream = fopen(files->paths[i], "wb");
        if (stream == NULL)
            return false;
        if (fwrite(body, 1U, sizeof(body) - 1U, stream) !=
            sizeof(body) - 1U) {
            (void)fclose(stream);
            return false;
        }
        if (fclose(stream) != 0)
            return false;
    }
    return true;
}

static void f007_files_remove(const F007Files *files)
{
    int i;

    for (i = 0; i < F007_FILES; i++) {
        if (files->paths[i][0] != '\0')
            (void)unlink(files->paths[i]);
    }
    if (files->dir[0] != '\0')
        (void)rmdir(files->dir);
}

static bool f007_group_order_is(const Ed *ed, const F007Files *files)
{
    int members[F007_FILES];
    int i;

    if (ed == NULL || ed->groups.v.len != 1U)
        return false;
    if (yew_group_members(ed, ed->groups.v.data[0].id, members,
                          F007_FILES) != F007_FILES)
        return false;
    for (i = 0; i < F007_FILES; i++) {
        const Tab *tab = yew_tab_at((Ed *)ed, members[i]);

        if (tab == NULL || !yew_file_same_identity(tab->path,
                                                    files->paths[i]))
            return false;
    }
    return true;
}

bool test_yew_f_007(char *why, size_t why_cap)
{
    Ed before;
    Ed after;
    F007Files files;
    Bytebuf document;
    bool before_ready = false;
    bool after_ready = false;
    bool document_ready = false;
    bool setup_failed = false;
    bool correct = false;
    u32 gid;
    int i;

    (void)memset(&before, 0, sizeof(before));
    (void)memset(&after, 0, sizeof(after));
    if (!f007_files_make(&files)) {
        f007_files_remove(&files);
        return true; /* Infrastructure failure must be a hard XPASS. */
    }

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&before);
    before_ready = true;
    if (!yew_ed_open_scratch(&before)) {
        setup_failed = true;
        goto done;
    }
    for (i = 0; i < F007_FILES; i++) {
        if (yew_tab_open(&before, files.paths[i]) < 0) {
            setup_failed = true;
            goto done;
        }
    }
    gid = yew_group_create(&before, files.dir, "audit");
    if (gid == 0U) {
        setup_failed = true;
        goto done;
    }
    for (i = 0; i < F007_FILES; i++)
        yew_group_add_member(&before, gid, i + 1);
    /* Emit tab records in descending member ordinal, without changing the
     * group's user-visible f0, f1, f2 order. */
    yew_tab_reorder(&before, 3, 1);
    yew_tab_reorder(&before, 3, 2);
    if (!f007_group_order_is(&before, &files)) {
        setup_failed = true;
        goto done;
    }

    bytebuf_init(&document);
    document_ready = true;
    yew_state_emit(&before, &document);
    yew_ed_free(&before);
    before_ready = false;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&after);
    after_ready = true;
    if (!yew_ed_open_scratch(&after) ||
        yew_state_apply(&after, document.data, document.len) !=
            YEW_WS_RESTORED) {
        setup_failed = true;
        goto done;
    }
    correct = f007_group_order_is(&after, &files);
    if (!correct && why != NULL && why_cap > 0U) {
        (void)snprintf(why, why_cap,
                       "saved f0,f1,f2 restored as f0,f2,f1");
    }

done:
    if (after_ready)
        yew_ed_free(&after);
    if (before_ready)
        yew_ed_free(&before);
    if (document_ready)
        bytebuf_free(&document);
    f007_files_remove(&files);
    return setup_failed ? true : correct;
}
