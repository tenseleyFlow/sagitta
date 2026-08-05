#include "args.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "mod/mods.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

static const char help_text[] =
    "Usage:\n"
    "  sagitta [options] [file ...]\n"
    "\n"
    "Options:\n"
    "  --help           Show this help.\n"
    "  --help-cmds      List named editor commands.\n"
    "  --version        Show version and compiled modules.\n"
    "  --clean          Ignore user configuration (Sprint 36).\n"
    "  --batch <file>   Run a Fletch batch script (Sprint 37).\n"
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
    "  SAG_LOG          Override the log file path.\n"
    "  SAG_LOG_LEVEL    Set debug, info, warn, or error logging.\n"
    "  SAG_TTY_PROBE    Set 0 to disable terminal capability probes.\n"
    "  SAG_PROBE_TIMEOUT_MS  Override the 50 ms probe deadline.\n"
    "  SAG_TRUECOLOR    Set 0 or 1 to override truecolor detection.\n"
    "  SAG_CHORD_TIMEOUT_MS  Set key chord timeout (default 500).\n"
    "  SAG_CLIPBOARD    Set auto, osc52, wl, xclip, xsel, pb, none,\n"
    "                   or cmd:<write-argv>[|<read-argv>].\n"
    "  SAG_CLIPBOARD_TARGET  Set OSC 52 target c, p, or cp.\n"
    "  SAG_CLIPBOARD_TIMEOUT_MS  Set subprocess timeout (default 1000).\n"
    "  SAG_OSC52        Set off, plain, tmux, or screen; plain bypasses\n"
    "                   multiplexer detection.\n"
    "  SAG_OSC52_MAX    Set maximum encoded OSC 52 bytes (default 100000).\n";

static void print_version(void)
{
    SagMod mod;
    bool any = false;

    (void)printf("sagitta %s\nmodules:", SAG_VERSION);
    for (mod = SAG_MOD_LSP; mod < SAG_MOD_COUNT; mod++) {
        if (sag_mod_enabled(mod)) {
            (void)printf(" %s", sag_mod_name(mod));
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

    sag_cmd_init();
    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);

        (void)printf("%-32s %s\n", desc->name, desc->help);
    }
    sag_cmd_shutdown();
}

static int run_driver(const SagArgs *args)
{
    if (args->selftest_bug) {
        SAG_BUG("selftest");
    }
    if (args->version) {
        print_version();
        return SAG_EXIT_OK;
    }
    if (args->help) {
        (void)fputs(help_text, stdout);
        return SAG_EXIT_OK;
    }
    if (args->help_cmds) {
        print_commands();
        return SAG_EXIT_OK;
    }
    if (args->batch_script != NULL) {
        (void)fprintf(stderr,
            "sagitta: error: batch mode is not yet implemented: Sprint 37\n");
        return SAG_EXIT_ERR;
    }
    if (args->clean) {
        (void)fprintf(stderr,
            "sagitta: error: --clean is not yet implemented: Sprint 36\n");
        return SAG_EXIT_ERR;
    }
    if (args->nfiles != 0U &&
        (strcmp(args->files[0], "fl") == 0 ||
         strcmp(args->files[0], "pkg") == 0)) {
        (void)fprintf(stderr, "sagitta: error: unknown argument '%s'\n",
            args->files[0]);
        return SAG_EXIT_ERR;
    }
    if (args->nfiles > 1U) {
        (void)fprintf(stderr,
            "sagitta: error: multiple files are not yet implemented: Sprint 23 (tabs)\n");
        return SAG_EXIT_ERR;
    }
    return sag_ed_driver(args->nfiles == 0U ? NULL : args->files[0]);
}

int main(int argc, char **argv)
{
    Bytebuf err = {0};
    SagArgs args;
    int result = sag_args_parse(&args, argc, argv, &err);

    if (result >= 0) {
        if (result != SAG_EXIT_OK && err.len != 0U) {
            (void)fwrite(err.data, 1U, err.len, stderr);
        }
        bytebuf_free(&err);
        if (result != SAG_EXIT_OK) {
            return result;
        }
        return run_driver(&args);
    }
    bytebuf_free(&err);
    return run_driver(&args);
}
