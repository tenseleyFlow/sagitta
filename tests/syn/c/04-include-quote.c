# include "local.h"
#pragma include "pragma-path.h"
#if defined(FEATURE) && 1
#define TEXT "value" // trailing comment
#define BLOCK /* comment */ 2
#endif
