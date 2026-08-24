#ifndef YEW_MOD_PLUG_PLUG_H
#define YEW_MOD_PLUG_PLUG_H

/*
 * Keep the stripped-build diagnostic here so every plugin command-line
 * implementation has one byte-exact source of truth.
 */
static const char yew_plug_module_error[] =
    "yew: error: built without plugin support (MODULES=plugins)\n";

/* argv[0] is "plug", matching the yew_fl_main/yew_syn_main convention. */
int yew_plug_main(int argc, char **argv);

#endif
