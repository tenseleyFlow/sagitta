/*
 * Sprint 31 deliverable 7, spec §13: the capability model.
 *
 * COVERAGE
 * --------
 * delegation gains nothing       caps_a_plugin_gains_nothing_from_a_helper
 * two origins, two module maps   caps_a_plugin_gains_nothing_from_a_helper
 * builtin frames are transparent caps_builtin_frames_are_transparent
 * fs.read granted / denied       caps_each_bit_is_granted_and_denied
 * fs.write granted / denied      caps_each_bit_is_granted_and_denied
 * shell granted / denied         caps_each_bit_is_granted_and_denied
 * net granted / denied           caps_each_bit_is_granted_and_denied
 * a denial is catchable          caps_a_denial_is_catchable
 * the message names the file     caps_a_denial_names_the_origin
 *
 * `shell` and `net` have no stdlib surface in 1.0 -- there is no io.run
 * and no io.http -- so their two cases go through fl_cap_check directly.
 * Asserting them at the C level is the honest option: inventing a native
 * to test a bit with no surface would be inventing the surface.
 */
#define _POSIX_C_SOURCE 200809L

#include "flfix.h"

#include <stdio.h>
#include <string.h>

/*
 * THE MODEL'S REAL TEST IS DELEGATION.
 *
 * A helper file under the config directory is imported by a plugin and
 * calls io.read.  §13 says the plugin gains nothing: the grant comes
 * from the DEFINING module of the calling function, and the helper --
 * loaded under the plugin's origin, because the cache key is
 * (realpath, origin.kind) -- carries the plugin's grants, which are
 * none.
 *
 * With a realpath-only cache the first importer would win and the
 * grants would leak in whichever direction the load order happened to
 * run.  That is an ambient-authority hole no ordinary test notices,
 * which is why this one exists.
 */
void test_fl_caps_a_plugin_gains_nothing_from_a_helper(void)
{
    FlFix f;
    char src[1024];
    char want[1024];
    const char *dir;
    u32 before;
    FlMap *as_config;
    FlMap *as_plugin;

    flfix_open(&f);
    dir = flfix_tmpdir(&f);
    flfix_write(&f, "a.txt", "hi\n");
    flfix_write(&f, "helper.fl",
                "import io\nfn peek(p) { return io.read(p) }\n");
    (void)snprintf(src, sizeof(src),
                   "import \"helper.fl\" as h\nreturn h.peek(\"%s/a.txt\")\n",
                   dir);

    before = f.vm.mods.n;
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    FL_EQ(&f, src, "hi\n");
    as_config = f.vm.mods.v[f.vm.mods.n - 1U].exports;

    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    (void)snprintf(want, sizeof(want),
                   "!capability: fs.read denied to %s/helper.fl", dir);
    FL_EQ(&f, src, want);
    as_plugin = f.vm.mods.v[f.vm.mods.n - 1U].exports;

    /* Twice, because the key is (realpath, origin.kind) -- and the two
     * instances are DISTINCT objects, or the second would be the first
     * one's authority under a new name. */
    SAG_ASSERT_EQ_U64((u64)(f.vm.mods.n - before), 2U);
    SAG_ASSERT(as_config != as_plugin);
    flfix_close(&f);
}

/*
 * `list.map(f, io.read)` must check f's grants, not list's.
 *
 * Builtin frames are transparent for exactly this reason: without it
 * any stdlib function would launder authority for whatever called it,
 * and every capability in the model would be one `list.map` away from
 * being granted.
 */
void test_fl_caps_builtin_frames_are_transparent(void)
{
    FlFix f;
    char src[1024];
    const char *dir;

    flfix_open(&f);
    dir = flfix_tmpdir(&f);
    flfix_write(&f, "a.txt", "hi\n");
    (void)snprintf(src, sizeof(src),
                   "import io\nimport list\n"
                   "return list.map([\"%s/a.txt\"], "
                   "fn(p) { return io.read(p) })[0]\n", dir);

    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    FL_EQ(&f, src, "hi\n");
    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    {
        char want[1024];

        /* The callback's own origin -- this file -- and not `list`'s. */
        (void)snprintf(want, sizeof(want),
                       "!capability: fs.read denied to %s/init.fl", dir);
        FL_EQ(&f, src, want);
    }
    flfix_close(&f);
}

void test_fl_caps_each_bit_is_granted_and_denied(void)
{
    FlFix f;
    char src[1024];
    char want[1024];
    const char *dir;

    flfix_open(&f);
    dir = flfix_tmpdir(&f);
    flfix_write(&f, "a.txt", "hi\n");

    (void)snprintf(src, sizeof(src), "import io\nreturn io.read(\"%s/a.txt\")\n",
                   dir);
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    FL_EQ(&f, src, "hi\n");
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, 0U);
    (void)snprintf(want, sizeof(want),
                   "!capability: fs.read denied to %s/init.fl", dir);
    FL_EQ(&f, src, want);

    (void)snprintf(src, sizeof(src),
                   "import io\nio.write(\"%s/w.txt\", \"x\")\nreturn 1\n", dir);
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_WRITE);
    FL_EQ(&f, src, "1");
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    (void)snprintf(want, sizeof(want),
                   "!capability: fs.write denied to %s/init.fl", dir);
    FL_EQ(&f, src, want);

    /*
     * shell and net exist so Sprint 54 can prompt for them and have no
     * 1.0 surface, so the check itself is what gets tested.  With no
     * Fletch frame on the stack, fl_cap_origin falls back to
     * root_origin -- the host deciding what it grants before it calls
     * in, which is §13.1.
     */
    f.vm.root_origin.kind = (u8)FL_ORIGIN_PLUGIN;
    f.vm.root_origin.path_id = 0U;
    f.vm.root_origin.caps = (u32)FL_CAP_SHELL;
    SAG_ASSERT(fl_cap_check(&f.vm, (u32)FL_CAP_SHELL));
    SAG_ASSERT(!fl_cap_check(&f.vm, (u32)FL_CAP_NET));
    f.vm.root_origin.caps = (u32)FL_CAP_NET;
    SAG_ASSERT(fl_cap_check(&f.vm, (u32)FL_CAP_NET));
    SAG_ASSERT(!fl_cap_check(&f.vm, (u32)FL_CAP_SHELL));
    /* Both at once, because a check for two bits must want both. */
    f.vm.root_origin.caps = (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;
    SAG_ASSERT(fl_cap_check(&f.vm, (u32)FL_CAP_SHELL | (u32)FL_CAP_NET));
    f.vm.root_origin.caps = (u32)FL_CAP_SHELL;
    SAG_ASSERT(!fl_cap_check(&f.vm, (u32)FL_CAP_SHELL | (u32)FL_CAP_NET));
    flfix_close(&f);
}

void test_fl_caps_a_denial_is_catchable(void)
{
    FlFix f;
    char src[1024];

    flfix_open(&f);
    (void)snprintf(src, sizeof(src),
                   "import io\n"
                   "try { io.read(\"%s/nope\") }\n"
                   "catch e { return e.kind }\n"
                   "return \"ran\"\n", flfix_tmpdir(&f));
    /* Catchable, so a plugin can degrade gracefully rather than dying
     * on a grant it turned out not to have. */
    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    FL_EQ(&f, src, "capability");
    flfix_close(&f);
}

void test_fl_caps_a_denial_names_the_origin(void)
{
    FlFix f;
    char src[1024];

    flfix_open(&f);
    (void)snprintf(src, sizeof(src), "import io\nreturn io.read(\"%s/a\")\n",
                   flfix_tmpdir(&f));
    /*
     * The PATH when there is one.  "denied to plugin" tells a user with
     * four plugins nothing, and the point of the message is that they
     * can go and look at the file.
     */
    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    {
        char want[1024];

        (void)snprintf(want, sizeof(want),
                       "!capability: fs.read denied to %s/init.fl",
                       flfix_tmpdir(&f));
        FL_EQ(&f, src, want);
    }
    flfix_close(&f);

    /* And the kind of origin when there is not. */
    flfix_open(&f);
    flfix_as(&f, (u8)FL_ORIGIN_WORKSPACE, 0U);
    FL_EQ(&f, "import io\nreturn io.read(\"/nope\")\n",
          "!capability: fs.read denied to the workspace config");
    flfix_close(&f);
}
