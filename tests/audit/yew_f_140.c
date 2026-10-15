/*
 * YEW-F-140 — nil Fletch evaluation passes NULL to fwrite.
 * Correct behavior: a zero-length rendered value must not call fwrite with
 * Bytebuf's NULL empty-storage pointer.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool mode_eval_write_is_guarded(void)
{
    FILE *file = fopen("src/flcli.c", "rb");
    char line[2048];
    bool saw_render = false;

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (saw_render) {
            bool guarded = strstr(line, "if (bb.len != 0U)") != NULL;

            (void)fclose(file);
            return guarded;
        }
        if (strstr(line, "yew_fl_print_result(&r->vm, out, &bb);") != NULL)
            saw_render = true;
    }
    (void)fclose(file);
    return false;
}

bool test_yew_f_140(char *why, size_t why_cap)
{
    if (mode_eval_write_is_guarded())
        return true;
    (void)snprintf(why, why_cap,
                   "mode_eval writes empty Bytebuf storage without a guard");
    return false;
}
