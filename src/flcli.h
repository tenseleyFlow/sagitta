#ifndef SAG_FLCLI_H
#define SAG_FLCLI_H

/*
 * `sag fl ...`, dispatched before the editor's argument parser.
 *
 * `argv` is the subcommand's own: argv[0] is "fl".  Returns a
 * SAG_EXIT_* code.  Sprint 32 documents --list-natives; Sprint 37 adds
 * batch stdio and script arguments.
 */
int sag_fl_main(int argc, char **argv);

#endif
