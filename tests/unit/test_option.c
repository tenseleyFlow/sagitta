#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/cmdcomp.h"
#include "ui/gutter.h"

static OptVal opt_get(Ed *ed, const char *name)
{
    OptVal value = {0};

    SAG_ASSERT(sag_opt_get(ed, sag_ed_doc(ed), ed->win, name,
                           (u32)strlen(name), &value));
    return value;
}

static bool opt_set(Ed *ed, u8 scope, const char *name, OptVal value,
                    const char **err)
{
    return sag_opt_set(ed, scope, name, (u32)strlen(name), &value, err);
}

void test_option_table_has_frozen_order_types_scopes_and_defaults(void)
{
    static const char *const names[] = {
        "tabwidth", "expandtab", "wrap", "scrolloff", "number",
        "statusline.column", "errorbells", "ambiguous_wide", "subword",
        "chord_timeout_ms", "undo.break_on_newline", "undo.bytes_max",
        "undo.min_nodes", "undo.persist_bytes_max", "registers.ring_depth",
        "registers.ring_bytes_max", "registers.clip_read_max",
        "clipboard.sync", "search.ignorecase", "search.smartcase",
        "hooks.error_limit", "theme", "macro.dir"
    };
    const char *listed[32];
    Ed ed;
    u32 i;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT_EQ_U64(sag_opts_len, SAG_ARRAY_LEN(names));
    SAG_ASSERT_EQ_U64(sag_opt_list(NULL, 0U), SAG_ARRAY_LEN(names));
    SAG_ASSERT_EQ_U64(sag_opt_list(listed, SAG_ARRAY_LEN(listed)),
                      SAG_ARRAY_LEN(names));
    for (i = 0U; i < sag_opts_len; i++) {
        OptVal got = opt_get(&ed, names[i]);

        SAG_ASSERT_EQ_STR(sag_opts[i].name, names[i]);
        SAG_ASSERT_EQ_U64(got.type, sag_opts[i].dflt.type);
        if (got.type == (u8)SAG_OPT_BOOL)
            SAG_ASSERT_EQ_U64(got.as.b, sag_opts[i].dflt.as.b);
        else if (got.type == (u8)SAG_OPT_INT)
            SAG_ASSERT_EQ_I64(got.as.i, sag_opts[i].dflt.as.i);
        else {
            SAG_ASSERT_EQ_U64(got.as.str.len, sag_opts[i].dflt.as.str.len);
            SAG_ASSERT(memcmp(got.as.str.s, sag_opts[i].dflt.as.str.s,
                              got.as.str.len) == 0);
        }
    }
    SAG_ASSERT_EQ_U64(ed.buffer.tabwidth, 4U);
    SAG_ASSERT_EQ_U64(ed.win->vp.scrolloff, 3U);
    SAG_ASSERT_EQ_U64(ed.win->number_style, SAG_NUM_ABS);
    SAG_ASSERT_EQ_U64(ed.chord_timeout_ms, 500U);
    SAG_ASSERT_EQ_U64(ed.regs.ring_depth, SAG_KILL_RING_DEPTH_DEFAULT);
    SAG_ASSERT_EQ_U64(ed.regs.ring_bytes_max, SAG_KILL_RING_BYTES_DEFAULT);
    SAG_ASSERT_EQ_U64(ed.buffer.undo->bytes_max, SAG_UNDO_BYTES_MAX);
    SAG_ASSERT_EQ_U64(ed.buffer.undo->min_nodes, SAG_UNDO_MIN_NODES);
    sag_ed_free(&ed);
}

void test_option_validators_reject_wrong_types_ranges_and_enums(void)
{
    Ed ed;
    const char *err = NULL;
    OptVal boolean = {SAG_OPT_BOOL, {.b = true}};
    OptVal integer = {SAG_OPT_INT, {.i = 1}};
    OptVal string = {SAG_OPT_STR, {.str = {"bogus", 5U}}};
    u32 i;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    for (i = 0U; i < sag_opts_len; i++) {
        const OptDesc *desc = &sag_opts[i];
        const OptVal *wrong = desc->type == (u8)SAG_OPT_BOOL ? &integer :
                              desc->type == (u8)SAG_OPT_INT ? &boolean :
                                                              &integer;

        err = NULL;
        SAG_ASSERT(!opt_set(&ed, SAG_OPT_SCOPE_DECLARED, desc->name,
                            *wrong, &err));
        SAG_ASSERT_NOT_NULL(err);
        if (desc->type == (u8)SAG_OPT_INT) {
            OptVal low = {SAG_OPT_INT, {.i = desc->imin == INT64_MIN ?
                                            desc->imax : desc->imin - 1}};

            if (desc->imin != INT64_MIN) {
                err = NULL;
                SAG_ASSERT(!opt_set(&ed, SAG_OPT_SCOPE_DECLARED,
                                    desc->name, low, &err));
                SAG_ASSERT_NOT_NULL(err);
            }
            if (desc->imax != INT64_MAX) {
                OptVal high = {SAG_OPT_INT, {.i = desc->imax + 1}};

                err = NULL;
                SAG_ASSERT(!opt_set(&ed, SAG_OPT_SCOPE_DECLARED,
                                    desc->name, high, &err));
                SAG_ASSERT_NOT_NULL(err);
            }
        } else if (desc->type == (u8)SAG_OPT_ENUM) {
            err = NULL;
            SAG_ASSERT(!opt_set(&ed, SAG_OPT_SCOPE_DECLARED,
                                desc->name, string, &err));
            SAG_ASSERT_NOT_NULL(err);
        }
    }
    sag_ed_free(&ed);
}

void test_option_scope_and_side_effects_share_one_setter(void)
{
    Ed ed;
    const char *err = NULL;
    OptVal eight = {SAG_OPT_INT, {.i = 8}};
    OptVal yes = {SAG_OPT_BOOL, {.b = true}};
    OptVal both = {SAG_OPT_STR, {.str = {"both", 4U}}};

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT(!opt_set(&ed, SAG_OPT_GLOBAL, "tabwidth", eight, &err));
    SAG_ASSERT_NOT_NULL(err);
    err = NULL;
    SAG_ASSERT(!opt_set(&ed, SAG_OPT_BUFFER, "errorbells", yes, &err));
    SAG_ASSERT_NOT_NULL(err);
    SAG_ASSERT(opt_set(&ed, SAG_OPT_SCOPE_DECLARED, "tabwidth", eight,
                       &err));
    SAG_ASSERT_EQ_U64(ed.buffer.tabwidth, 8U);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 8);
    SAG_ASSERT(opt_set(&ed, SAG_OPT_SCOPE_DECLARED, "wrap", yes, &err));
    SAG_ASSERT(ed.win->vp.wrap);
    SAG_ASSERT(opt_set(&ed, SAG_OPT_SCOPE_DECLARED, "number", both, &err));
    SAG_ASSERT_EQ_U64(ed.win->number_style, SAG_NUM_HYBRID);
    SAG_ASSERT(opt_set(&ed, SAG_OPT_SCOPE_DECLARED, "errorbells", yes,
                       &err));
    SAG_ASSERT(ed.errorbells);
    sag_opt_reset(&ed);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    SAG_ASSERT_EQ_U64(ed.buffer.tabwidth, 4U);
    SAG_ASSERT(!ed.win->vp.wrap);
    SAG_ASSERT_EQ_U64(ed.win->number_style, SAG_NUM_ABS);
    SAG_ASSERT(!ed.errorbells);
    sag_ed_free(&ed);
}

void test_option_fletch_set_map_is_atomic_and_cmdline_is_identical(void)
{
    static const char bad[] = "set({tabwidth: 8, tabwith: 2})";
    static const char good[] = "set({tabwidth: 8, wrap: true})";
    static const char later[] = "set({tabwidth: 10})";
    Ed ed;
    CmdCtx cx = {0};
    char *argv[] = {"ed.opt.set_many", "tabwidth", "6"};
    u32 before;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    before = ed.hooks.ledger.n;
    SAG_ASSERT(sag_fl_eval(&ed, bad, (u32)(sizeof(bad) - 1U)) !=
               SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    SAG_ASSERT_EQ_U64(ed.hooks.ledger.n, before);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, good, (u32)(sizeof(good) - 1U)),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 8);
    SAG_ASSERT(opt_get(&ed, "wrap").as.b);
    SAG_ASSERT_EQ_U64(ed.hooks.ledger.n, before + 2U);
    SAG_ASSERT_EQ_U64(ed.hooks.ledger.v[before].kind, REG_OPTION);
    SAG_ASSERT_EQ_U64(ed.hooks.ledger.v[before].origin_id,
                      FL_ORIGIN_ID_CONFIG);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, later,
                                  (u32)(sizeof(later) - 1U)), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 10);
    SAG_ASSERT_EQ_U64(ed.hooks.ledger.n, before + 3U);
    SAG_ASSERT(sag_opt_remove(&ed, before + 3U));
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 8);
    SAG_ASSERT(sag_opt_remove(&ed, before + 2U));
    SAG_ASSERT(!opt_get(&ed, "wrap").as.b);
    SAG_ASSERT(sag_opt_remove(&ed, before + 1U));
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    SAG_ASSERT(!sag_opt_remove(&ed, before + 1U));
    cx.ed = &ed;
    cx.argv = (CmdArgv){argv, 3U};
    SAG_ASSERT_EQ_I64(sag_opt_cmdline_set(&cx), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 6);
    SAG_ASSERT_EQ_U64(ed.buffer.tabwidth, 6U);
    sag_ed_free(&ed);
}

void test_option_completion_uses_declaration_order_inventory(void)
{
    Ed ed;
    Vec_CompItem items = {0};
    u32 total;

    sag_ed_init(&ed);
    total = sag_comp_enumerate(&ed, SAG_COMP_OPTION, "tab", &items);
    SAG_ASSERT(total >= 1U);
    SAG_ASSERT(items.len >= 1U);
    SAG_ASSERT_EQ_STR(items.data[0].text, "tabwidth");
    Vec_CompItem_free(&items);
    total = sag_comp_enumerate(&ed, SAG_COMP_VALUE, "gcol", &items);
    SAG_ASSERT_EQ_U64(total, 2U);
    SAG_ASSERT(items.len >= 2U);
    Vec_CompItem_free(&items);
    sag_ed_free(&ed);
}
