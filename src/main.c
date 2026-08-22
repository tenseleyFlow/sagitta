#include "args.h"
#include "flcli.h"
#include "syncli.h"
#include "edit/batch.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/lsp/lsp.h"
#include "mod/ai/ai.h"
#include "mod/mods.h"
#include "syn/defs.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"
#include "ws/symshadow.h"

#include <stdio.h>
#include <string.h>

static const char help_text[] =
    "Usage:\n"
    "  yew [options] [file ...]\n"
    "\n"
    "Options:\n"
    "  --help           Show this help.\n"
    "  --help-cmds      List named editor commands.\n"
    "  --version        Show version and compiled modules.\n"
    "  --clean          Load only the panic keymap; keep no state/history.\n"
    "  --config PATH    Use PATH instead of the user init.fl.\n"
    "  --theme NAME     Override the configured theme for this run.\n"
    "  --no-workspace-config  Do not load .yew.fl.\n"
    "  --trust-workspace      Pre-grant this workspace configuration.\n"
    "  --batch SCRIPT   Run SCRIPT headlessly; no tty or grid.\n"
    "  --test           Add the t.* script-test assertions (batch only).\n"
    "  --quiet          Suppress batch warnings; errors still print.\n"
    "  --grant NAME:CAP Reserve a plugin capability grant (Sprint 54).\n"
    "  --               Pass every remaining argument to the batch script.\n"
    "\n"
    "Subcommands:\n"
    "  yew fl FILE      Run a Fletch script headlessly.\n"
    "  yew syn COMMAND   Inspect and compile syntax definitions.\n"
    "\n"
    "Command line:\n"
    "  Lines beginning with a space are not saved in command history.\n"
    "\n"
    "Search and replace:\n"
    "  /pat  ?pat        Incremental search, forward and backward.\n"
    "  n  N              Repeat in the search's own direction, and the\n"
    "                    reverse of it -- after ?pat, n goes backward.\n"
    "  *  #              Search for the word under the cursor.\n"
    "  :[range]s/pat/rep/flags   Substitute; flags g c n i I p e.\n"
    "                    In the replacement, & is a LITERAL ampersand;\n"
    "                    use \\0 for the whole match.  This differs from\n"
    "                    vi and sed on purpose.\n"
    "  Multi-file search is :!rg <pattern> (or any tool you prefer),\n"
    "  whose output lands in a job buffer you can navigate.  An\n"
    "  in-editor indexed project search is deliberately not a 1.0\n"
    "  feature.\n"
    "\n"
    "Environment:\n"
    "  YEW_LOG          Override the log file path.\n"
    "  YEW_LOG_LEVEL    Set debug, info, warn, or error logging.\n"
    "  YEW_NO_SYN_CACHE Set to 1 to bypass the syntax table cache.\n"
    "  YEW_THEME        Override the configured theme (loses to --theme).\n"
    "  YEW_TTY_PROBE    Set 0 to disable terminal capability probes.\n"
    "  YEW_PROBE_TIMEOUT_MS  Override the 50 ms probe deadline.\n"
    "  YEW_TRUECOLOR    Set 0 or 1 to override truecolor detection.\n"
    "  YEW_CHORD_TIMEOUT_MS  Set key chord timeout (default 500).\n"
    "  YEW_CLIPBOARD    Set auto, osc52, wl, xclip, xsel, pb, none,\n"
    "                   or cmd:<write-argv>[|<read-argv>].\n"
    "  YEW_CLIPBOARD_TARGET  Set OSC 52 target c, p, or cp.\n"
    "  YEW_CLIPBOARD_TIMEOUT_MS  Set subprocess timeout (default 1000).\n"
    "  YEW_OSC52        Set off, plain, tmux, or screen; plain bypasses\n"
    "                   multiplexer detection.\n"
    "  YEW_OSC52_MAX    Set maximum encoded OSC 52 bytes (default 100000).\n";

static void print_version(void)
{
    YewMod mod;
    bool any = false;

    (void)printf("yew %s\nmodules:", YEW_VERSION);
    for (mod = YEW_MOD_LSP; mod < YEW_MOD_COUNT; mod++) {
        if (yew_mod_enabled(mod)) {
            (void)printf(" %s", yew_mod_name(mod));
            any = true;
        }
    }
    if (!any) {
        (void)printf(" none");
    }
    (void)putchar('\n');
}

static void print_commands(void)
{
    u32 i;

    yew_cmd_init();
    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);

        (void)printf("%-32s %s\n", desc->name, desc->help);
    }
    yew_cmd_shutdown();
}

static int run_driver(const YewArgs *args)
{
    YewEdStartup startup;

    yew_symshadow_install();
    yew_lsp_shadow_install();
    yew_ai_shadow_init(NULL);
    yew_syn_cache_set_bypass(args->clean);
    yew_syn_discovery_set_bypass(args->clean);
    if (args->selftest_bug) {
        YEW_BUG("selftest");
    }
    if (args->version) {
        print_version();
        return YEW_EXIT_OK;
    }
    if (args->help) {
        (void)fputs(help_text, stdout);
        return YEW_EXIT_OK;
    }
    if (args->help_cmds) {
        print_commands();
        return YEW_EXIT_OK;
    }
    if (args->batch_script != NULL) {
        BatchOpts batch;

        if (args->ngrants != 0U) {
            (void)fprintf(stderr,
                "yew: error: --grant enforcement lands in Sprint 54\n");
            return YEW_EXIT_ERR;
        }
        batch = (BatchOpts){args->batch_script, args->files, args->nfiles,
                            args->batch_args, args->nbatch_args,
                            args->config_path, args->clean,
                            args->no_workspace_config,
                            args->trust_workspace, args->test, args->quiet};
        return yew_batch_run(&batch);
    }
    if (args->nfiles != 0U && strcmp(args->files[0], "pkg") == 0) {
        (void)fprintf(stderr, "yew: error: unknown argument '%s'\n",
            args->files[0]);
        return YEW_EXIT_ERR;
    }
    if (args->nfiles > 1U) {
        (void)fprintf(stderr,
            "yew: error: multiple files are not yet implemented: Sprint 23 (tabs)\n");
        return YEW_EXIT_ERR;
    }
    startup = (YewEdStartup){args->config_path, args->theme, args->clean,
                             args->no_workspace_config,
                             args->trust_workspace};
    return yew_ed_driver_opts(args->nfiles == 0U ? NULL : args->files[0],
                              &startup);
}

int main(int argc, char **argv)
{
    Bytebuf err = {0};
    int exit_code;

    yew_job_set_argv0(argc > 0 ? argv[0] : NULL);

    /*
     * `yew fl` is handled BEFORE the editor's parser: its options are
     * not the editor's, and threading --list-natives through one parser
     * would make it a top-level flag that means nothing anywhere else.
     */
    if (argc >= 2 && strcmp(argv[1], "fl") == 0)
        return yew_fl_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "syn") == 0)
        return yew_syn_main(argc - 1, argv + 1, false);
    if (argc >= 3 && strcmp(argv[1], "--clean") == 0 &&
        strcmp(argv[2], "syn") == 0)
        return yew_syn_main(argc - 2, argv + 2, true);

    YewArgs args;
    int result = yew_args_parse(&args, argc, argv, &err);

    if (result >= 0) {
        if (result != YEW_EXIT_OK && err.len != 0U) {
            (void)fwrite(err.data, 1U, err.len, stderr);
        }
        bytebuf_free(&err);
        if (result != YEW_EXIT_OK) {
            return result;
        }
        exit_code = run_driver(&args);
        yew_args_free(&args);
        return exit_code;
    }
    bytebuf_free(&err);
    exit_code = run_driver(&args);
    yew_args_free(&args);
    return exit_code;
}
