#ifndef YEW_MOD_AI_PRESET_H
#define YEW_MOD_AI_PRESET_H

#include <stdbool.h>

typedef struct Ed Ed;

bool yew_ai_preset_load(Ed *ed, const char *name);
bool yew_ai_privacy_open(Ed *ed);

#endif
