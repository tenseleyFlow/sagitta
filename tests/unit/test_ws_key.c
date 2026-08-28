/*
 * Sprint 25 §1: workspace identity.
 *
 * Two properties carry this file.
 *
 * STABILITY.  The same directory must key to the same state directory
 * forever.  A hash that drifted between runs — a seed, an ASLR'd
 * pointer, a locale-dependent case fold — would not fail loudly; every
 * restore after the change would just quietly start from a fresh
 * workspace, and the user would conclude the feature does not work.
 *
 * COLLISION.  The `path` record is the whole mitigation, so the probe
 * walk is tested against planted directories rather than argued about.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/xdg.h"
#include "edit/ed.h"
#include "ws/workspace.h"

/*
 * Points XDG_STATE_HOME at a scratch tree so the tests never touch the
 * developer's real state.
 */
typedef struct WsFix {
    char state_home[128];
    char work[128];
    char saved[512];
    bool had_saved;
} WsFix;

static void wsf_make(WsFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/yew-wsstate-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    YEW_ASSERT(yew_test_canonicalize_path(f->state_home,
                                           sizeof(f->state_home)));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/yew-wswork-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->work));
    YEW_ASSERT(yew_test_canonicalize_path(f->work, sizeof(f->work)));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
}

/* Removes the scratch tree; `rm -rf` by hand, since the trees are
 * shallow and known. */
static void wsf_rm_rf(const char *path)
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

static void wsf_remove(WsFix *f)
{
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    wsf_rm_rf(f->state_home);
    wsf_rm_rf(f->work);
}

void test_ws_root_override_requires_and_canonicalizes_a_directory(void)
{
    WsFix f;
    Ed ed;
    char file[192];
    FILE *fp;

    wsf_make(&f);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_set_workspace_root(&ed, f.work));
    YEW_ASSERT_EQ_STR(yew_ws_root(&ed), f.work);
    (void)snprintf(file, sizeof(file), "%s/not-a-dir", f.work);
    fp = fopen(file, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    if (fp != NULL)
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT(!yew_ed_set_workspace_root(&ed, file));
    YEW_ASSERT(!yew_ed_set_workspace_root(&ed, "/no/such/yew-workspace"));
    YEW_ASSERT_EQ_STR(yew_ws_root(&ed), f.work);
    yew_ed_free(&ed);
    (void)unlink(file);
    wsf_remove(&f);
}

/* Plants a state directory carrying a `path` record naming `owner`. */
static void wsf_plant(const WsFix *f, u64 hash, unsigned probe,
                      const char *owner)
{
    char dir[512];
    char record[640];
    FILE *fp;

    if (probe == 0U)
        (void)snprintf(dir, sizeof(dir), "%s/yew/workspaces/%016lx",
                       f->state_home, (unsigned long)hash);
    else
        (void)snprintf(dir, sizeof(dir), "%s/yew/workspaces/%016lx-%u",
                       f->state_home, (unsigned long)hash, probe);
    YEW_ASSERT(yew_mkdirs(dir, 0700U));
    (void)snprintf(record, sizeof(record), "%s/path", dir);
    fp = fopen(record, "w");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fprintf(fp, "%s\n", owner);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

/* ---------------------------------------------------------------- */

/*
 * The published FNV-1a 64 vectors.  Pinned to the algorithm rather than
 * to whatever this build happens to produce, so a "harmless"
 * refactoring of the hash cannot silently orphan every existing
 * workspace on disk.
 */
void test_ws_key_hash_matches_the_published_vectors(void)
{
    YEW_ASSERT_EQ_U64(yew_fnv1a64((const u8 *)"", 0U),
                      0xcbf29ce484222325ULL);
    YEW_ASSERT_EQ_U64(yew_fnv1a64((const u8 *)"a", 1U),
                      0xaf63dc4c8601ec8cULL);
    YEW_ASSERT_EQ_U64(yew_fnv1a64((const u8 *)"foobar", 6U),
                      0x85944171f73967e8ULL);
    /* Bytes, not text: an invalid UTF-8 sequence hashes without a
     * decoder that could refuse it. */
    {
        static const u8 bad[] = {0xC3U, 0x28U, 0x00U, 0xFFU};

        YEW_ASSERT(yew_fnv1a64(bad, 4U) != 0U);
        /* The embedded NUL is part of the input, not a terminator. */
        YEW_ASSERT(yew_fnv1a64(bad, 4U) != yew_fnv1a64(bad, 2U));
    }
}

/* Same directory -> same key, every time, with no seed. */
void test_ws_key_is_stable_across_calls(void)
{
    WsFix f;
    WsKey a;
    WsKey b;

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&a, f.work));
    YEW_ASSERT(yew_ws_key(&b, f.work));
    YEW_ASSERT_EQ_U64(a.hash, b.hash);
    YEW_ASSERT_EQ_STR(a.dir, b.dir);
    YEW_ASSERT_EQ_STR(a.realpath, b.realpath);
    YEW_ASSERT(!a.stateless);
    /* The hash is of the REALPATH, so a non-canonical spelling of the
     * same directory keys identically. */
    {
        char dotted[256];
        WsKey c;

        (void)snprintf(dotted, sizeof(dotted), "%s/./", f.work);
        YEW_ASSERT(yew_ws_key(&c, dotted));
        YEW_ASSERT_EQ_U64(c.hash, a.hash);
        YEW_ASSERT_EQ_STR(c.dir, a.dir);
    }
    wsf_remove(&f);
}

void test_ws_key_paths_hang_off_the_state_dir(void)
{
    WsFix f;
    WsKey k;
    char want[PATH_MAX + 32];

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    (void)snprintf(want, sizeof(want), "%sstate.fl", k.dir);
    YEW_ASSERT_EQ_STR(yew_ws_state_path(&k), want);
    (void)snprintf(want, sizeof(want), "%sundo/", k.dir);
    YEW_ASSERT_EQ_STR(yew_ws_undo_dir(&k), want);
    (void)snprintf(want, sizeof(want), "%slock", k.dir);
    YEW_ASSERT_EQ_STR(yew_ws_lock_path(&k), want);
    /* Nothing lands in the workspace itself (DoD 9). */
    YEW_ASSERT(strstr(yew_ws_state_path(&k), f.work) == NULL);
    wsf_remove(&f);
}

/* The `path` record is written at directory creation and reads back as
 * the realpath — it is what makes the hex tree auditable. */
void test_ws_key_writes_the_path_record(void)
{
    WsFix f;
    WsKey k;
    char record[PATH_MAX + 32];
    char line[PATH_MAX + 32];
    FILE *fp;

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT(yew_ws_ensure_dir(&k));
    (void)snprintf(record, sizeof(record), "%spath", k.dir);
    fp = fopen(record, "r");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    /* LF-terminated, one line, the realpath bytes. */
    YEW_ASSERT_EQ_U64(strlen(line), strlen(k.realpath) + 1U);
    YEW_ASSERT_EQ_I64(line[strlen(line) - 1U], '\n');
    line[strlen(line) - 1U] = '\0';
    YEW_ASSERT_EQ_STR(line, k.realpath);
    wsf_remove(&f);
}

/*
 * Probe 0 is taken by a DIFFERENT workspace, so we land on probe 1.
 *
 * Without the `path` record this is undetectable: two directories would
 * share one state file and each would see the other's tabs.
 */
void test_ws_key_walks_past_a_collision(void)
{
    WsFix f;
    WsKey k;

    wsf_make(&f);
    /* Learn our own hash first, then plant a squatter on it. */
    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT_EQ_U64(k.probe, 0U);
    wsf_plant(&f, k.hash, 0U, "/some/other/workspace");

    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT_EQ_U64(k.probe, 1U);
    YEW_ASSERT(!k.stateless);
    YEW_ASSERT_NOT_NULL(strstr(k.dir, "-1/"));

    /* Planting OUR record at probe 1 makes it a match rather than a
     * collision, so we keep landing there. */
    wsf_plant(&f, k.hash, 1U, k.realpath);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT_EQ_U64(k.probe, 1U);
    wsf_remove(&f);
}

/* A run of squatters walks to the first free probe, not past it. */
void test_ws_key_takes_the_first_free_probe(void)
{
    WsFix f;
    WsKey k;
    unsigned i;

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    for (i = 0U; i <= 3U; i++)
        wsf_plant(&f, k.hash, i, "/not/us");
    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT_EQ_U64(k.probe, 4U);
    wsf_remove(&f);
}

/*
 * Exhaustion runs STATELESS rather than guessing.
 *
 * Writing into a directory that belongs to a different workspace would
 * destroy that workspace's layout; running without a cache costs
 * nothing that cannot be rebuilt.
 */
void test_ws_key_exhausted_probes_run_stateless(void)
{
    WsFix f;
    WsKey k;
    unsigned i;

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    for (i = 0U; i <= (unsigned)YEW_WS_PROBE_MAX; i++)
        wsf_plant(&f, k.hash, i, "/not/us");
    YEW_ASSERT(!yew_ws_key(&k, f.work));
    YEW_ASSERT(k.stateless);
    /* And a stateless key hands out no paths to write to. */
    YEW_ASSERT_EQ_STR(yew_ws_state_path(&k), "");
    YEW_ASSERT(!yew_ws_ensure_dir(&k));
    wsf_remove(&f);
}

/* A half-written directory — created, then killed before the record
 * landed — is free, not a permanent collision. */
void test_ws_key_empty_path_record_is_free(void)
{
    WsFix f;
    WsKey k;
    char dir[PATH_MAX];
    char record[PATH_MAX + 32];
    FILE *fp;

    wsf_make(&f);
    YEW_ASSERT(yew_ws_key(&k, f.work));
    (void)snprintf(dir, sizeof(dir), "%s/yew/workspaces/%016lx",
                   f.state_home, (unsigned long)k.hash);
    YEW_ASSERT(yew_mkdirs(dir, 0700U));
    (void)snprintf(record, sizeof(record), "%s/path", dir);
    fp = fopen(record, "w");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);

    YEW_ASSERT(yew_ws_key(&k, f.work));
    YEW_ASSERT_EQ_U64(k.probe, 0U);
    wsf_remove(&f);
}

/* No state home means no state — and an editor that still starts. */
void test_ws_key_without_a_state_home_is_stateless(void)
{
    WsFix f;
    WsKey k;
    char saved_home[512];
    const char *home;

    wsf_make(&f);
    home = getenv("HOME");
    (void)snprintf(saved_home, sizeof(saved_home), "%s",
                   home == NULL ? "" : home);
    YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    YEW_ASSERT_EQ_I64(unsetenv("HOME"), 0);
    YEW_ASSERT(!yew_ws_key(&k, f.work));
    YEW_ASSERT(k.stateless);
    if (saved_home[0] != '\0')
        (void)setenv("HOME", saved_home, 1);
    wsf_remove(&f);
}

/* A path that does not resolve keys to nothing rather than to a
 * plausible-looking directory. */
void test_ws_key_refuses_an_unresolvable_path(void)
{
    WsFix f;
    WsKey k;

    wsf_make(&f);
    YEW_ASSERT(!yew_ws_key(&k, "/tmp/yew-does-not-exist-at-all-12345"));
    YEW_ASSERT(k.stateless);
    YEW_ASSERT(!yew_ws_key(&k, NULL));
    YEW_ASSERT(!yew_ws_key(&k, ""));
    wsf_remove(&f);
}
