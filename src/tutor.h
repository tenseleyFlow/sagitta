#ifndef YEW_TUTOR_H
#define YEW_TUTOR_H

#include <stdbool.h>

#include "util/base.h"

enum {
    YEW_TUTOR_CHAPTERS = 7,
    YEW_TUTOR_TASKS_PER_CHAPTER = 7
};

typedef struct YewTutorProgress {
    u8 done[YEW_TUTOR_CHAPTERS];
    bool skipped[YEW_TUTOR_CHAPTERS];
} YewTutorProgress;

/* argv[0] is "tutor". */
int yew_tutor_main(int argc, char **argv);

bool yew_tutor_chapter_parse(const char *text, u8 *chapter);
char *yew_tutor_root(bool ensure);
bool yew_tutor_seed(const char *root);
bool yew_tutor_reset(const char *root);
bool yew_tutor_progress_load(const char *root, YewTutorProgress *out,
                             bool *missing);
bool yew_tutor_progress_save(const char *root,
                             const YewTutorProgress *progress);

#endif /* YEW_TUTOR_H */
