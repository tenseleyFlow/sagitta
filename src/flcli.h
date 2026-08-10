#ifndef YEW_FLCLI_H
#define YEW_FLCLI_H

/*
 * `yew fl ...`, dispatched before the editor's argument parser.
 *
 * `argv` is the subcommand's own: argv[0] is "fl".  Returns a
 * YEW_EXIT_* code.  Sprint 32 documents --list-natives; Sprint 37 adds
 * batch stdio and script arguments.
 */
int yew_fl_main(int argc, char **argv);

#endif
