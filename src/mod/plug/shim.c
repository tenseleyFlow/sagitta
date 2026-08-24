#include "mod/plug/plug.h"

#include <stdio.h>

#include "util/base.h"

int yew_plug_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)fputs(yew_plug_module_error, stderr);
    return YEW_EXIT_ERR;
}
