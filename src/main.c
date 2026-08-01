#include "args.h"
#include "mod/mods.h"
#include "term/tty.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char help_text[] =
    "Usage:\n"
    "  sagitta [options] [file ...]\n"
    "\n"
    "Options:\n"
    "  --help           Show this help.\n"
    "  --version        Show version and compiled modules.\n"
    "  --clean          Ignore user configuration (Sprint 36).\n"
    "  --batch <file>   Run a Fletch batch script (Sprint 37).\n"
    "\n"
    "Environment:\n"
    "  SAG_LOG          Override the log file path.\n"
    "  SAG_LOG_LEVEL    Set debug, info, warn, or error logging.\n"
    "  SAG_TTY_PROBE    Set 0 to disable terminal capability probes.\n"
    "  SAG_PROBE_TIMEOUT_MS  Override the 50 ms probe deadline.\n"
    "  SAG_TRUECOLOR    Set 0 or 1 to override truecolor detection.\n";

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

static int run_driver(const SagArgs *args)
{
    Tty tty;

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
    if (args->nfiles == 0U) {
        errno = 0;
        if (!sag_tty_open(&tty))
            return errno == ENOTTY ? SAG_EXIT_ERR : SAG_EXIT_IO;
        sag_tty_close(&tty);
    }
    (void)fprintf(stderr,
        "sagitta: error: the editor is not yet implemented: Sprint 14 (modes L and I)\n");
    return SAG_EXIT_ERR;
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
