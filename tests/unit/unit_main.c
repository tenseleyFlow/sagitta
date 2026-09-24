#include "harness.h"

#include <string.h>

#include "edit/job.h"
#include "edit/shsession.h"

int yew_test_run(int argc, char **argv);

int main(int argc, char **argv)
{
    /*
     * Sprint 57.27: the shell session runs yew_job_self_exe() with
     * --yew-env0 after every command, and under this runner that is the
     * runner.  argv[0] is published (and resolved now, before any test
     * changes directory) exactly as src/main.c does for yew.
     */
    if (argc == 2 && strcmp(argv[1], "--yew-env0") == 0)
        return yew_shsession_env0_main();
    yew_job_set_argv0(argc > 0 ? argv[0] : NULL);
    (void)yew_job_self_exe();
    return yew_test_run(argc, argv);
}
