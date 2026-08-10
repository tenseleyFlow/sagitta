#ifndef YEW_SYNCLI_H
#define YEW_SYNCLI_H

#include <stdbool.h>

/* argv[0] is "syn".  Returns a YEW_EXIT_* code. */
int yew_syn_main(int argc, char **argv, bool clean);

#endif
