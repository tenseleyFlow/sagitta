#include <inttypes.h>
#include <stdio.h>

#include "edit/option.h"

static const char *scope_name(u8 scope)
{
    switch ((OptScope)scope) {
    case YEW_OPT_GLOBAL:
        return "global";
    case YEW_OPT_BUFFER:
        return "buffer";
    case YEW_OPT_WINDOW:
        return "window";
    case YEW_OPT_SCOPE_DECLARED:
        return "declared";
    }
    return "invalid";
}

int main(void)
{
    u32 i;

    for (i = 0U; i < yew_opts_len; i++) {
        const OptDesc *desc = &yew_opts[i];

        printf("%s|", desc->name);
        switch ((OptValType)desc->dflt.type) {
        case YEW_OPT_BOOL:
            fputs(desc->dflt.as.b ? "true" : "false", stdout);
            break;
        case YEW_OPT_INT:
            printf("%" PRId64, desc->dflt.as.i);
            break;
        case YEW_OPT_STR:
        case YEW_OPT_ENUM:
            printf("\"%.*s\"", (int)desc->dflt.as.str.len,
                   desc->dflt.as.str.s);
            break;
        case YEW_OPT_STRLIST:
            if (desc->dflt.as.list.len != 0U) {
                fputs("nonempty-list", stdout);
                return 1;
            }
            fputs("[]", stdout);
            break;
        }
        printf("|%s\n", scope_name(desc->scope));
    }
    return ferror(stdout) ? 1 : 0;
}
