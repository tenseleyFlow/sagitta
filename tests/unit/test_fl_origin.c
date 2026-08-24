#define _POSIX_C_SOURCE 200809L

/* Sprint 34 §2 and DoD 10: stable origin ids and defining-origin caps. */

#include "flfix.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/origin.h"
#include "fl/value.h"
#include "fl/vm.h"

static const u32 ALL_CAPS = FL_CAP_ALL;

static void origin_ed_open(Ed *ed)
{
    (void)memset(ed, 0, sizeof(*ed));
    fl_origin_reg_init(&ed->origins);
}

static void origin_ed_close(Ed *ed)
{
    fl_origin_reg_free(&ed->origins);
}

void test_fl_origin_registry_reserves_config_zero(void)
{
    static const FlOriginKind kinds[] = {
        FL_ORIGIN_BUILTIN, FL_ORIGIN_WORKSPACE, FL_ORIGIN_PLUGIN,
        FL_ORIGIN_CLI, FL_ORIGIN_REPL
    };
    static const char *const labels[] = {
        "a builtin", "workspace.fl", "plugin.fl", "the command line",
        "the REPL"
    };
    Ed ed;
    u32 ids[YEW_ARRAY_LEN(kinds)];
    size_t i;
    size_t j;

    origin_ed_open(&ed);
    YEW_ASSERT_EQ_U64(ed.origins.n, 1U);
    YEW_ASSERT_EQ_U64(FL_ORIGIN_ID_CONFIG, 0U);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, FL_ORIGIN_ID_CONFIG),
                      "the user config");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, FL_ORIGIN_ID_CONFIG), ALL_CAPS);

    for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
        ids[i] = fl_origin_register(&ed, kinds[i], labels[i], (u32)(1U << i));
        YEW_ASSERT(ids[i] != FL_ORIGIN_ID_CONFIG);
        YEW_ASSERT(ids[i] != FL_ORIGIN_ID_NONE);
        YEW_ASSERT_EQ_U64(ids[i], i + 1U);
        YEW_ASSERT_EQ_STR(fl_origin_label(&ed, ids[i]), labels[i]);
        YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, ids[i]), (u32)(1U << i));
        for (j = 0U; j < i; j++)
            YEW_ASSERT(ids[i] != ids[j]);
    }
    YEW_ASSERT_EQ_U64(ed.origins.n, YEW_ARRAY_LEN(kinds) + 1U);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, FL_ORIGIN_ID_NONE),
                      "an unknown origin");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, FL_ORIGIN_ID_NONE), 0U);
    YEW_ASSERT_EQ_STR(fl_origin_label(NULL, 0U), "an unknown origin");
    YEW_ASSERT_EQ_U64(fl_origin_caps(NULL, 0U), 0U);
    YEW_ASSERT_EQ_U64(fl_origin_register(NULL, FL_ORIGIN_PLUGIN, "x", 0U),
                      FL_ORIGIN_ID_NONE);
    origin_ed_close(&ed);
}

void test_fl_origin_registry_is_idempotent_and_updates_caps(void)
{
    Ed ed;
    u32 config;
    u32 first;
    u32 again;
    u32 other_kind;
    u32 unnamed;

    origin_ed_open(&ed);
    config = fl_origin_register(&ed, FL_ORIGIN_CONFIG, "the user config",
                                (u32)FL_CAP_FS_READ);
    YEW_ASSERT_EQ_U64(config, FL_ORIGIN_ID_CONFIG);
    YEW_ASSERT_EQ_U64(ed.origins.n, 1U);
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, config), FL_CAP_FS_READ);
    first = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "same.fl",
                               (u32)FL_CAP_FS_READ);
    again = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "same.fl",
                               (u32)FL_CAP_NET);
    YEW_ASSERT_EQ_U64(again, first);
    YEW_ASSERT_EQ_U64(ed.origins.n, 2U);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, first), "same.fl");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, first), FL_CAP_NET);

    /* Kind is part of identity: the same path under config authority is
     * not the plugin's ledger owner. */
    other_kind = fl_origin_register(&ed, FL_ORIGIN_WORKSPACE, "same.fl",
                                    (u32)FL_CAP_FS_WRITE);
    YEW_ASSERT(other_kind != first);
    YEW_ASSERT_EQ_U64(ed.origins.n, 3U);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, other_kind), "same.fl");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, other_kind), FL_CAP_FS_WRITE);

    unnamed = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, NULL, 0U);
    YEW_ASSERT(unnamed != FL_ORIGIN_ID_CONFIG);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, unnamed), "an unnamed origin");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, unnamed), 0U);
    origin_ed_close(&ed);
}

void test_fl_origin_masking_is_idempotent(void)
{
    Ed ed;
    u32 a;
    u32 b;

    origin_ed_open(&ed);
    a = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "a.fl", 0U);
    b = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "b.fl", 0U);
    YEW_ASSERT(!fl_origin_masked(&ed, a));
    YEW_ASSERT(!fl_origin_masked(&ed, b));
    YEW_ASSERT(!fl_origin_masked(NULL, a));

    fl_origin_mask(&ed, a);
    YEW_ASSERT(fl_origin_masked(&ed, a));
    YEW_ASSERT(!fl_origin_masked(&ed, b));
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 1U);
    fl_origin_mask(&ed, a);
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 1U);

    fl_origin_mask(&ed, b);
    YEW_ASSERT(fl_origin_masked(&ed, a));
    YEW_ASSERT(fl_origin_masked(&ed, b));
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 2U);
    fl_origin_unmask(&ed, a);
    YEW_ASSERT(!fl_origin_masked(&ed, a));
    YEW_ASSERT(fl_origin_masked(&ed, b));
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 1U);
    fl_origin_unmask(&ed, a);
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 1U);
    fl_origin_unmask(&ed, b);
    YEW_ASSERT(!fl_origin_masked(&ed, b));
    YEW_ASSERT_EQ_U64(ed.origins.nmasked, 0U);
    fl_origin_mask(NULL, a);
    fl_origin_unmask(NULL, a);
    origin_ed_close(&ed);
}

void test_fl_origin_of_frame_uses_attached_root_origin(void)
{
    FlFix f;
    Ed ed;
    u32 path;
    u32 id;

    flfix_open(&f);
    YEW_ASSERT_EQ_U64(fl_origin_of_frame(&f.vm), FL_ORIGIN_ID_NONE);
    origin_ed_open(&ed);
    f.vm.ed = &ed;
    path = yew_intern(&f.in, "/tmp/root-plugin.fl", 19U);
    f.vm.root_origin.kind = (u8)FL_ORIGIN_PLUGIN;
    f.vm.root_origin.path_id = path;
    f.vm.root_origin.caps = (u32)FL_CAP_FS_READ;

    id = fl_origin_of_frame(&f.vm);
    YEW_ASSERT(id != FL_ORIGIN_ID_CONFIG);
    YEW_ASSERT(id != FL_ORIGIN_ID_NONE);
    YEW_ASSERT_EQ_STR(fl_origin_label(&ed, id), "/tmp/root-plugin.fl");
    YEW_ASSERT_EQ_U64(fl_origin_caps(&ed, id), FL_CAP_FS_READ);
    YEW_ASSERT_EQ_U64(fl_origin_of_frame(&f.vm), id);
    YEW_ASSERT_EQ_U64(ed.origins.n, 2U);

    f.vm.ed = NULL;
    origin_ed_close(&ed);
    flfix_close(&f);
}

/*
 * Load the same helper first as trusted config and then as an unprivileged
 * plugin.  The second call must not reuse the first module's authority;
 * its denial is caught in Fletch, proving both the defining-origin walk
 * and the ordinary error value contract.
 */
void test_fl_origin_plugin_helper_chain_denial_is_catchable(void)
{
    FlFix f;
    const char *dir;
    char trusted[1024];
    char denied[1200];

    flfix_open(&f);
    dir = flfix_tmpdir(&f);
    flfix_write(&f, "payload.txt", "secret\n");
    flfix_write(&f, "helper.fl",
                "import io\nfn peek(p) { return io.read(p) }\n");

    (void)snprintf(trusted, sizeof(trusted),
                   "import \"helper.fl\" as h\n"
                   "return h.peek(\"%s/payload.txt\")\n", dir);
    flfix_as(&f, (u8)FL_ORIGIN_CONFIG, (u32)FL_CAP_FS_READ);
    FL_EQ(&f, trusted, "secret\n");

    (void)snprintf(denied, sizeof(denied),
                   "import \"helper.fl\" as h\n"
                   "try { h.peek(\"%s/payload.txt\") }\n"
                   "catch e { return e.kind + \":\" + e.msg }\n"
                   "return \"authority leaked\"\n", dir);
    flfix_as(&f, (u8)FL_ORIGIN_PLUGIN, 0U);
    {
        char want[1200];

        (void)snprintf(want, sizeof(want),
                       "capability:fs.read denied to %s/helper.fl", dir);
        FL_EQ(&f, denied, want);
    }
    YEW_ASSERT_EQ_U64(f.vm.mods.n, 2U);
    YEW_ASSERT(f.vm.mods.v[0].exports != f.vm.mods.v[1].exports);

    flfix_close(&f);
}

void test_fl_origin_cap_check_requires_every_requested_bit(void)
{
    FlFix f;
    FlValue kind = FL_NIL_V;
    FlStr *key;

    flfix_open(&f);
    f.vm.root_origin.kind = (u8)FL_ORIGIN_PLUGIN;
    f.vm.root_origin.caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_SHELL;
    YEW_ASSERT(fl_cap_check(&f.vm, (u32)FL_CAP_FS_READ));
    YEW_ASSERT(fl_cap_check(&f.vm, (u32)FL_CAP_SHELL));
    YEW_ASSERT(fl_cap_check(&f.vm,
                            (u32)FL_CAP_FS_READ | (u32)FL_CAP_SHELL));
    YEW_ASSERT(!fl_cap_check(&f.vm,
                             (u32)FL_CAP_FS_READ | (u32)FL_CAP_NET));
    key = fl_str_new(&f.vm, "kind", 4U);
    YEW_ASSERT(fl_map_get((FlMap *)f.vm.err.as.o, FL_OBJ_V(FL_STR, key),
                          &kind));
    YEW_ASSERT_EQ_U64(kind.t, FL_STR);
    YEW_ASSERT_EQ_STR(((FlStr *)kind.as.o)->b, "capability");
    flfix_close(&f);
}
