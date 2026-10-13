/*
 * YEW-F-087 — the long-literal piece-seam regression is false-green.
 *
 * Correct behavior: the named unit fixture proves that its subject crosses
 * many live TextBuf pieces before exercising the streaming matcher.
 *
 * Baseline failure: inserting successive chunks at the end coalesces them
 * into one add-store piece, despite the test claiming hundreds of seams.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

static bool source_has(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char line[1024];

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

bool test_yew_f_087(char *why, size_t why_cap)
{
    if (source_has("tests/unit/test_re_match.c",
                   "yew_textbuf_piece_count(tb) >= 100U"))
        return true;
    (void)snprintf(why, why_cap,
                   "long-literal fixture does not prove real fragmentation");
    return false;
}
