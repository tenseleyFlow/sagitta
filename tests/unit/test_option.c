#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/cmdcomp.h"
#include "ui/gutter.h"
#include "util/xdg.h"

static OptVal opt_get(Ed *ed, const char *name)
{
    OptVal value = {0};

    YEW_ASSERT(yew_opt_get(ed, yew_ed_doc(ed), ed->win, name,
                           (u32)strlen(name), &value));
    return value;
}

static bool opt_set(Ed *ed, u8 scope, const char *name, OptVal value,
                    const char **err)
{
    return yew_opt_set(ed, scope, name, (u32)strlen(name), &value, err);
}

void test_option_table_has_frozen_order_types_scopes_and_defaults(void)
{
    static const char *const names[] = {
        "tabwidth", "expandtab", "wrap", "scrolloff", "number",
        "statusline.column", "errorbells", "ambiguous_wide", "subword",
        "fortran_form", "chord_timeout_ms", "undo.break_on_newline", "undo.bytes_max",
        "undo.min_nodes", "undo.persist_bytes_max", "registers.ring_depth",
        "registers.ring_bytes_max", "registers.clip_read_max",
        "clipboard.sync", "search.ignorecase", "search.smartcase",
        "hooks.error_limit", "theme", "theme_auto", "macro.dir",
        "shadow.enable", "shadow.providers", "shadow.max_lines",
        "shadow.midline", "shadow.lsp_debounce_ms",
        "shadow.ai_debounce_ms", "compl.auto_trigger",
        "compl.trigger_chars"
    };
    const char *listed[34];
    Ed ed;
    u32 i;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT_EQ_U64(yew_opts_len, YEW_ARRAY_LEN(names));
    YEW_ASSERT_EQ_U64(yew_opt_list(NULL, 0U), YEW_ARRAY_LEN(names));
    YEW_ASSERT_EQ_U64(yew_opt_list(listed, YEW_ARRAY_LEN(listed)),
                      YEW_ARRAY_LEN(names));
    for (i = 0U; i < yew_opts_len; i++) {
        OptVal got = opt_get(&ed, names[i]);

        YEW_ASSERT_EQ_STR(yew_opts[i].name, names[i]);
        YEW_ASSERT_EQ_U64(got.type, yew_opts[i].dflt.type);
        if (got.type == (u8)YEW_OPT_BOOL)
            YEW_ASSERT_EQ_U64(got.as.b, yew_opts[i].dflt.as.b);
        else if (got.type == (u8)YEW_OPT_INT)
            YEW_ASSERT_EQ_I64(got.as.i, yew_opts[i].dflt.as.i);
        else {
            char *config = strcmp(names[i], "macro.dir") == 0 ?
                           yew_xdg_config_dir() : NULL;

            if (config != NULL) {
                size_t config_len = strlen(config);

                YEW_ASSERT_EQ_U64(got.as.str.len,
                                  config_len + sizeof("/macros") - 1U);
                YEW_ASSERT_EQ_MEM(got.as.str.s, config, config_len);
                YEW_ASSERT_EQ_MEM(got.as.str.s + config_len, "/macros",
                                  sizeof("/macros") - 1U);
            } else {
                YEW_ASSERT_EQ_U64(got.as.str.len,
                                  yew_opts[i].dflt.as.str.len);
                YEW_ASSERT(memcmp(got.as.str.s, yew_opts[i].dflt.as.str.s,
                                  got.as.str.len) == 0);
            }
            free(config);
        }
    }
    YEW_ASSERT_EQ_U64(ed.buffer.tabwidth, 4U);
    YEW_ASSERT_EQ_U64(ed.win->vp.scrolloff, 3U);
    YEW_ASSERT_EQ_U64(ed.win->number_style, YEW_NUM_HYBRID);
    YEW_ASSERT_EQ_U64(ed.chord_timeout_ms, 500U);
    YEW_ASSERT_EQ_U64(ed.regs.ring_depth, YEW_KILL_RING_DEPTH_DEFAULT);
    YEW_ASSERT_EQ_U64(ed.regs.ring_bytes_max, YEW_KILL_RING_BYTES_DEFAULT);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->bytes_max, YEW_UNDO_BYTES_MAX);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->min_nodes, YEW_UNDO_MIN_NODES);
    yew_ed_free(&ed);
}

void test_option_validators_reject_wrong_types_ranges_and_enums(void)
{
    Ed ed;
    const char *err = NULL;
    OptVal boolean = {YEW_OPT_BOOL, {.b = true}};
    OptVal integer = {YEW_OPT_INT, {.i = 1}};
    OptVal string = {YEW_OPT_STR, {.str = {"bogus", 5U}}};
    u32 i;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    for (i = 0U; i < yew_opts_len; i++) {
        const OptDesc *desc = &yew_opts[i];
        const OptVal *wrong = desc->type == (u8)YEW_OPT_BOOL ? &integer :
                              desc->type == (u8)YEW_OPT_INT ? &boolean :
                                                              &integer;

        err = NULL;
        YEW_ASSERT(!opt_set(&ed, YEW_OPT_SCOPE_DECLARED, desc->name,
                            *wrong, &err));
        YEW_ASSERT_NOT_NULL(err);
        if (desc->type == (u8)YEW_OPT_INT) {
            OptVal low = {YEW_OPT_INT, {.i = desc->imin == INT64_MIN ?
                                            desc->imax : desc->imin - 1}};

            if (desc->imin != INT64_MIN) {
                err = NULL;
                YEW_ASSERT(!opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                                    desc->name, low, &err));
                YEW_ASSERT_NOT_NULL(err);
            }
            if (desc->imax != INT64_MAX) {
                OptVal high = {YEW_OPT_INT, {.i = desc->imax + 1}};

                err = NULL;
                YEW_ASSERT(!opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                                    desc->name, high, &err));
                YEW_ASSERT_NOT_NULL(err);
            }
        } else if (desc->type == (u8)YEW_OPT_ENUM) {
            err = NULL;
            YEW_ASSERT(!opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                                desc->name, string, &err));
            YEW_ASSERT_NOT_NULL(err);
        }
    }
    yew_ed_free(&ed);
}

void test_option_scope_and_side_effects_share_one_setter(void)
{
    Ed ed;
    const char *err = NULL;
    OptVal eight = {YEW_OPT_INT, {.i = 8}};
    OptVal yes = {YEW_OPT_BOOL, {.b = true}};
    OptVal both = {YEW_OPT_STR, {.str = {"both", 4U}}};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT(!opt_set(&ed, YEW_OPT_GLOBAL, "tabwidth", eight, &err));
    YEW_ASSERT_NOT_NULL(err);
    err = NULL;
    YEW_ASSERT(!opt_set(&ed, YEW_OPT_BUFFER, "errorbells", yes, &err));
    YEW_ASSERT_NOT_NULL(err);
    YEW_ASSERT(opt_set(&ed, YEW_OPT_SCOPE_DECLARED, "tabwidth", eight,
                       &err));
    YEW_ASSERT_EQ_U64(ed.buffer.tabwidth, 8U);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 8);
    YEW_ASSERT(opt_set(&ed, YEW_OPT_SCOPE_DECLARED, "wrap", yes, &err));
    YEW_ASSERT(ed.win->vp.wrap);
    YEW_ASSERT(opt_set(&ed, YEW_OPT_SCOPE_DECLARED, "number", both, &err));
    YEW_ASSERT_EQ_U64(ed.win->number_style, YEW_NUM_HYBRID);
    YEW_ASSERT(opt_set(&ed, YEW_OPT_SCOPE_DECLARED, "errorbells", yes,
                       &err));
    YEW_ASSERT(ed.errorbells);
    yew_opt_reset(&ed);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    YEW_ASSERT_EQ_U64(ed.buffer.tabwidth, 4U);
    YEW_ASSERT(!ed.win->vp.wrap);
    YEW_ASSERT_EQ_U64(ed.win->number_style, YEW_NUM_HYBRID);
    YEW_ASSERT(!ed.errorbells);
    yew_ed_free(&ed);
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

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    before = ed.hooks.ledger.n;
    YEW_ASSERT(yew_fl_eval(&ed, bad, (u32)(sizeof(bad) - 1U)) !=
               YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.n, before);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, good, (u32)(sizeof(good) - 1U)),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 8);
    YEW_ASSERT(opt_get(&ed, "wrap").as.b);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.n, before + 2U);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.v[before].kind, REG_OPTION);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.v[before].origin_id,
                      FL_ORIGIN_ID_CONFIG);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, later,
                                  (u32)(sizeof(later) - 1U)), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 10);
    YEW_ASSERT_EQ_U64(ed.hooks.ledger.n, before + 2U);
    YEW_ASSERT(yew_opt_remove(&ed, before + 2U));
    YEW_ASSERT(!opt_get(&ed, "wrap").as.b);
    YEW_ASSERT(yew_opt_remove(&ed, before + 1U));
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 4);
    YEW_ASSERT(!yew_opt_remove(&ed, before + 1U));
    cx.ed = &ed;
    cx.argv = (CmdArgv){argv, 3U};
    YEW_ASSERT_EQ_I64(yew_opt_cmdline_set(&cx), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(opt_get(&ed, "tabwidth").as.i, 6);
    YEW_ASSERT_EQ_U64(ed.buffer.tabwidth, 6U);
    yew_ed_free(&ed);
}

void test_option_completion_uses_declaration_order_inventory(void)
{
    Ed ed;
    Vec_CompItem items = {0};
    u32 total;

    yew_ed_init(&ed);
    total = yew_comp_enumerate(&ed, YEW_COMP_OPTION, "tab", &items);
    YEW_ASSERT(total >= 1U);
    YEW_ASSERT(items.len >= 1U);
    YEW_ASSERT_EQ_STR(items.data[0].text, "tabwidth");
    Vec_CompItem_free(&items);
    total = yew_comp_enumerate(&ed, YEW_COMP_VALUE, "gcol", &items);
    YEW_ASSERT_EQ_U64(total, 2U);
    YEW_ASSERT(items.len >= 2U);
    Vec_CompItem_free(&items);
    yew_ed_free(&ed);
}
