/*
 * YEW-F-136 — the tutor Git lesson inherits global commit policy.
 * Correct behavior: the isolated tutor repository owns a deterministic local
 * identity, disables signing, and disables hooks before the interactive FUSS
 * commit can run.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool file_has(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char line[2048];

    if (file == NULL)
        return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            (void)fclose(file);
            return true;
        }
    }
    (void)fclose(file);
    return false;
}

bool test_yew_f_136(char *why, size_t why_cap)
{
    bool signing = file_has("src/tutor.c", "commit.gpgsign");
    bool identity = file_has("src/tutor.c", "user.name");
    bool hooks = file_has("src/tutor.c", "core.hooksPath");
    bool pty = file_has("tests/pty/tutor_runner.c",
                        "tutor Git repository retained inherited policy");

    if (signing && identity && hooks && pty)
        return true;
    (void)snprintf(why, why_cap,
                   "tutor Git lesson inherits global commit policy");
    return false;
}
