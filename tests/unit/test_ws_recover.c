/*
 * Sprint 25 §7: a corrupt state file never blocks startup.
 *
 * Every row of the §7 table ends with the same four words — "the editor
 * starts" — and this file asserts each of them individually, because
 * the failure mode is not a crash.  It is an editor that refuses to
 * open, or prompts before its first paint, or deletes the bytes a bug
 * report would have needed, over a CACHE.
 *
 * The rules with teeth:
 *
 * NEVER DELETE THE BAD FILE.  It is set aside under a UTC timestamp and
 * is still there afterwards.  s08's stale-journal doctrine: the user's
 * bytes are theirs even when we cannot make sense of them.
 *
 * NEVER PROMPT, EXACTLY ONE MESSAGE.  A wall of warnings trains people
 * to dismiss the next one, which will matter.
 *
 * RETENTION CAPS AT FIVE, oldest deleted.  "Oldest" is decided
 * lexicographically, which is why the stamp is %Y%m%dT%H%M%SZ and not
 * anything friendlier to read.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/layout.h"
#include "ui/message.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "ws/state.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct RcFix {
    char state_home[128];
    char work[128];
    char saved[512];
    bool had_saved;
    Ed ed;
} RcFix;

static void rc_rm_rf(const char *path)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    /* Assigned, not cast to void: glibc marks system warn_unused_result
     * under _FORTIFY_SOURCE, which Ubuntu's gcc enables by default and
     * Arch's does not — the cast compiled locally and failed CI. */
    {
        int removed = system(cmd);

        (void)removed;
    }
}

static void rc_make(RcFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-rchome-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/sag-rcwork-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->work));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->work);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    sag_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    sag_state_open(&f->ed);
    SAG_ASSERT(f->ed.state.ready);
}

static void rc_remove(RcFix *f)
{
    sag_ed_free(&f->ed);
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    rc_rm_rf(f->state_home);
    rc_rm_rf(f->work);
}

/* Plants a state.fl with exactly these bytes. */
static void rc_plant(const RcFix *f, const char *text, size_t len)
{
    FILE *fp = fopen(sag_ws_state_path(&f->ed.state.key), "wb");

    SAG_ASSERT_NOT_NULL(fp);
    if (len > 0U)
        (void)fwrite(text, 1U, len, fp);
    (void)fclose(fp);
}

static bool rc_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/*
 * Joins through opaque pointers on purpose.  Both halves are PATH_MAX
 * buffers, so a direct snprintf lets -Wformat-truncation prove the
 * result might not fit and -Werror stops the build; the check the
 * warning is asking for is the assert below.
 */
static void rc_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);

    SAG_ASSERT(n > 0 && (size_t)n < cap);
}

/* How many state.fl.corrupt-* files are in the state dir. */
static u32 rc_corrupt_count(const RcFix *f)
{
    DIR *d = opendir(f->ed.state.key.dir);
    struct dirent *ent;
    u32 n = 0U;

    if (d == NULL)
        return 0U;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "state.fl.corrupt-", 17U) == 0)
            n++;
    }
    (void)closedir(d);
    return n;
}

/* The one name in the state dir matching the corrupt prefix. */
static bool rc_corrupt_name(const RcFix *f, char *out, size_t cap)
{
    DIR *d = opendir(f->ed.state.key.dir);
    struct dirent *ent;
    bool found = false;

    if (d == NULL)
        return false;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "state.fl.corrupt-", 17U) != 0)
            continue;
        {
            int n = snprintf(out, cap, "%s", ent->d_name);

            SAG_ASSERT(n > 0 && (size_t)n < cap);
        }
        found = true;
    }
    (void)closedir(d);
    return found;
}

/* ---------------------------------------------------------------- */
/* The §7 rows                                                      */
/* ---------------------------------------------------------------- */

/* Row 1: absent.  Fresh, SILENT, nothing set aside. */
void test_ws_recover_absent_is_silent(void)
{
    RcFix f;

    rc_make(&f);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_FRESH);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 0U);
    /* No message at all: a first run is not an event. */
    SAG_ASSERT(!f.ed.msg.active);
    rc_remove(&f);
}

/*
 * Row 2: unreadable.  Fresh with a message, and emphatically NOT set
 * aside — we could not read it, so we have no grounds to call it
 * corrupt, and renaming a file we merely lack permission for moves
 * somebody else's data.
 */
void test_ws_recover_unreadable_is_not_set_aside(void)
{
    RcFix f;
    char path[PATH_MAX];

    rc_make(&f);
    rc_plant(&f, "{ version: 1, }\n", 16U);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_state_path(&f.ed.state.key));
    if (chmod(path, 0000) != 0 || geteuid() == 0U) {
        /* root reads anything; the row is untestable as root. */
        rc_remove(&f);
        return;
    }
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_FRESH);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 0U);
    SAG_ASSERT(rc_exists(path));
    SAG_ASSERT(f.ed.msg.active);
    (void)chmod(path, 0600);
    rc_remove(&f);
}

/* Row 3a: a parse error.  Set aside, never deleted, editor starts. */
void test_ws_recover_parse_error_is_set_aside(void)
{
    RcFix f;
    char name[256];
    char path[PATH_MAX];

    rc_make(&f);
    rc_plant(&f, "{ version: 1, tabs: [ { oh no", 28U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    /* state.fl is gone from its own name... */
    SAG_ASSERT(!rc_exists(sag_ws_state_path(&f.ed.state.key)));
    /* ...because it is here, under a UTC stamp.  NOT deleted. */
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    SAG_ASSERT(rc_corrupt_name(&f, name, sizeof(name)));
    rc_join(path, sizeof(path), f.ed.state.key.dir, name);
    SAG_ASSERT(rc_exists(path));
    /* The stamp is %Y%m%dT%H%M%SZ: 8 digits, T, 6 digits, Z. */
    SAG_ASSERT_EQ_U64(strlen(name), 17U + 16U);
    SAG_ASSERT_EQ_I64(name[17 + 8], 'T');
    SAG_ASSERT_EQ_I64(name[17 + 15], 'Z');
    /* One message, and no prompt before the first paint. */
    SAG_ASSERT(f.ed.msg.active);
    SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_NONE);
    /* The editor started: a scratch tab, exit code untouched. */
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 1);
    SAG_ASSERT(!f.ed.quit);
    SAG_ASSERT_EQ_I64(f.ed.exit_code, SAG_EXIT_OK);
    rc_remove(&f);
}

/* Row 3b: a version this build does not speak. */
void test_ws_recover_bad_version_is_set_aside(void)
{
    RcFix f;

    rc_make(&f);
    rc_plant(&f, "{ version: 99, tabs: [ ], }\n", 28U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 1);
    rc_remove(&f);
}

/* A missing version is version 0, which is not 1. */
void test_ws_recover_absent_version_is_set_aside(void)
{
    RcFix f;

    rc_make(&f);
    rc_plant(&f, "{ tabs: [ ], }\n", 15U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    rc_remove(&f);
}

/* A root that is not a map is not a document. */
void test_ws_recover_non_map_root_is_set_aside(void)
{
    RcFix f;

    rc_make(&f);
    rc_plant(&f, "[ 1, 2, 3, ]\n", 13U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    rc_remove(&f);
}

/* An empty file parses as nothing, which is not a v1 document. */
void test_ws_recover_empty_file_is_set_aside(void)
{
    RcFix f;

    rc_make(&f);
    rc_plant(&f, "", 0U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 1);
    rc_remove(&f);
}

/*
 * Row 3c: past the 8 MiB cap.  Rejected on the STAT, so an oversized
 * file costs one syscall rather than reading it to find out.
 */
void test_ws_recover_oversize_is_set_aside_without_reading(void)
{
    RcFix f;
    FILE *fp;
    char path[PATH_MAX];

    rc_make(&f);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_state_path(&f.ed.state.key));
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    /* Sparse: 9 MiB of address space, a few blocks of disk. */
    SAG_ASSERT_EQ_I64(fseek(fp, 9L * 1024L * 1024L, SEEK_SET), 0);
    (void)fputc('x', fp);
    (void)fclose(fp);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    rc_remove(&f);
}

/*
 * Row 4: a single malformed record is DROPPED and the rest is kept.
 *
 * One bad field must not cost someone their whole arrangement — the
 * document is a cache, and the readers take defaults rather than
 * failing (fllit.h).
 */
void test_ws_recover_one_bad_tab_record_keeps_the_rest(void)
{
    RcFix f;
    char pa[192];
    char pc[192];
    char text[1024];
    FILE *fp;

    rc_make(&f);
    (void)snprintf(pa, sizeof(pa), "%s/a.txt", f.work);
    (void)snprintf(pc, sizeof(pc), "%s/c.txt", f.work);
    fp = fopen(pa, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("alpha\n", fp);
    (void)fclose(fp);
    fp = fopen(pc, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("charlie\n", fp);
    (void)fclose(fp);
    /* The middle record has a nil path — structurally fine, nothing to
     * open.  It is dropped; its neighbours are not. */
    (void)snprintf(text, sizeof(text),
                   "{\n  version: 1,\n  groups: [\n  ],\n  tabs: [\n"
                   "    { id: 1, path: \"%s\", },\n"
                   "    { id: 2, path: nil, },\n"
                   "    { id: 3, path: \"%s\", },\n"
                   "  ],\n  active_tab: 3,\n}\n",
                   pa, pc);
    rc_plant(&f, text, strlen(text));
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RESTORED);
    /* Scratch plus the two good records. */
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 3);
    SAG_ASSERT_EQ_STR(sag_tab_at(&f.ed, 1)->path, pa);
    SAG_ASSERT_EQ_STR(sag_tab_at(&f.ed, 2)->path, pc);
    /* And nothing was set aside: a droppable record is not corruption
     * of the document. */
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 0U);
    rc_remove(&f);
}

/* A wrong-typed field takes its default rather than failing the file. */
void test_ws_recover_wrong_typed_fields_take_defaults(void)
{
    RcFix f;
    char pa[192];
    char text[1024];
    FILE *fp;

    rc_make(&f);
    (void)snprintf(pa, sizeof(pa), "%s/a.txt", f.work);
    fp = fopen(pa, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("alpha\n", fp);
    (void)fclose(fp);
    /* group is a string, group_ordinal is a bool, panes is an int. */
    (void)snprintf(text, sizeof(text),
                   "{\n  version: 1,\n  tabs: [\n"
                   "    { id: 1, path: \"%s\", group: \"nope\","
                   " group_ordinal: true, panes: 7, },\n"
                   "  ],\n  active_tab: 1,\n}\n",
                   pa);
    rc_plant(&f, text, strlen(text));
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RESTORED);
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 2);
    SAG_ASSERT_EQ_U64(sag_tab_at(&f.ed, 1)->group_id, 0U);
    SAG_ASSERT_NOT_NULL(sag_tab_at(&f.ed, 1)->root);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 0U);
    rc_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Retention                                                        */
/* ---------------------------------------------------------------- */

/*
 * At most five corrupt copies survive, and the ones kept are the
 * NEWEST — which is the lexicographically largest, which is the whole
 * reason for the timestamp format.
 */
void test_ws_recover_retention_caps_at_five(void)
{
    RcFix f;
    u32 i;
    char path[PATH_MAX];
    char newest[PATH_MAX];
    char oldest[PATH_MAX];

    rc_make(&f);
    /* Eight plants, oldest first by stamp. */
    for (i = 0U; i < 8U; i++) {
        FILE *fp;

        char leaf[64];

        (void)snprintf(leaf, sizeof(leaf),
                       "state.fl.corrupt-2026010%uT000000Z", (unsigned)i);
        rc_join(path, sizeof(path), f.ed.state.key.dir, leaf);
        fp = fopen(path, "wb");
        SAG_ASSERT_NOT_NULL(fp);
        (void)fputs("old\n", fp);
        (void)fclose(fp);
    }
    rc_join(oldest, sizeof(oldest), f.ed.state.key.dir,
            "state.fl.corrupt-20260100T000000Z");
    rc_join(newest, sizeof(newest), f.ed.state.key.dir,
            "state.fl.corrupt-20260107T000000Z");
    SAG_ASSERT(rc_exists(oldest));
    SAG_ASSERT(rc_exists(newest));

    /* A ninth failure triggers the prune. */
    rc_plant(&f, "not a document", 14U);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), (u32)SAG_STATE_CORRUPT_KEEP);
    /* The oldest went; a newer one stayed. */
    SAG_ASSERT(!rc_exists(oldest));
    SAG_ASSERT(rc_exists(newest));
    rc_remove(&f);
}

/*
 * The set-aside REFUSES to clobber.  The stamp has one-second
 * resolution, so two failures inside one second share a name — and
 * plain rename(2) would destroy the first with no record it existed.
 */
void test_ws_recover_set_aside_never_overwrites(void)
{
    RcFix f;
    char name[256];
    char path[PATH_MAX];
    char body[64];
    FILE *fp;
    size_t n;

    rc_make(&f);
    rc_plant(&f, "first bad document", 18U);
    SAG_ASSERT(sag_state_set_aside(&f.ed, name, sizeof(name)));
    rc_join(path, sizeof(path), f.ed.state.key.dir, name);

    /* A second failure in the same second reuses the name. */
    rc_plant(&f, "second bad document", 19U);
    /* Refused, so the first copy is untouched... */
    SAG_ASSERT(!sag_state_set_aside(&f.ed, NULL, 0U));
    fp = fopen(path, "rb");
    SAG_ASSERT_NOT_NULL(fp);
    n = fread(body, 1U, sizeof(body) - 1U, fp);
    body[n] = '\0';
    (void)fclose(fp);
    SAG_ASSERT_EQ_STR(body, "first bad document");
    /* ...and the second is still at state.fl rather than lost. */
    SAG_ASSERT(rc_exists(sag_ws_state_path(&f.ed.state.key)));
    rc_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Never exits, never prompts                                       */
/* ---------------------------------------------------------------- */

/*
 * DoD 7 in one place: every §7 input leaves an editor that started,
 * with exit 0, no prompt, and one message.
 */
void test_ws_recover_every_bad_input_still_starts(void)
{
    static const char *const inputs[] = {
        "",
        "\x00\x01\x02\x03",
        "{",
        "}",
        "{ version: 1",
        "{ version: \"one\", }",
        "[ ]",
        "nil",
        "{ version: 1, tabs: nil, }",
        "{ version: 1, tabs: [ nil, ], }",
        "{ version: 1, groups: 5, tabs: [ ], }",
        "############\n",
        "{ version: 1, } trailing garbage"
    };
    u32 i;

    for (i = 0U; i < SAG_ARRAY_LEN(inputs); i++) {
        RcFix f;
        SagWsResult r;

        rc_make(&f);
        rc_plant(&f, inputs[i], strlen(inputs[i]));
        r = sag_ws_restore(&f.ed);
        /* Whatever it decided, the editor is alive and usable. */
        SAG_ASSERT(r == SAG_WS_FRESH || r == SAG_WS_RESTORED ||
                   r == SAG_WS_RECOVERED);
        SAG_ASSERT(!f.ed.quit);
        SAG_ASSERT_EQ_I64(f.ed.exit_code, SAG_EXIT_OK);
        SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_NONE);
        SAG_ASSERT(sag_tab_count(&f.ed) >= 1);
        rc_remove(&f);
    }
}

/*
 * A reader (someone else holds the lock) still sets a bad file aside.
 *
 * The rename is not a state WRITE — refusing it would leave the second
 * session parsing the same broken document on every start with no way
 * out that does not involve the shell.
 */
void test_ws_recover_a_reader_still_sets_aside(void)
{
    RcFix f;

    rc_make(&f);
    rc_plant(&f, "definitely not fletch", 21U);
    /* Demote without disturbing the key. */
    f.ed.state.writer = false;
    f.ed.state.owner_pid = 1;
    SAG_ASSERT_EQ_I64(sag_ws_restore(&f.ed), SAG_WS_RECOVERED);
    SAG_ASSERT_EQ_U64(rc_corrupt_count(&f), 1U);
    rc_remove(&f);
}
