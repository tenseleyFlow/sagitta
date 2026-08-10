#ifndef YEW_SYN_DEFS_H
#define YEW_SYN_DEFS_H

#include <stdbool.h>
#include <stddef.h>

#include "fl/diag.h"
#include "syn/engine.h"
#include "util/arena.h"
#include "util/base.h"

#define YEW_SYN_TABLE_VERSION 3U
#define YEW_SYN_CACHE_MAGIC "SAGSYN\0\0"
#define YEW_SYN_CACHE_HEADER_SIZE 64U

typedef struct SynDefErr {
    FlSpan sp;
    u32 pat_off;
    const char *msg;
} SynDefErr;

typedef struct SynComment {
    const char *line;
    const char *block_open;
    const char *block_close;
} SynComment;

typedef struct SynLangDesc {
    u32 id;
    const char *name;
    const char *source;
    const char **extensions;
    u32 nextensions;
    const char **filenames;
    u32 nfilenames;
    const char **shebangs;
    u32 nshebangs;
    const char *first_line;
    i32 priority;
    SynComment comment;
} SynLangDesc;

/* Pointer-free input used by the checked-in generated detection table. */
typedef struct SynLangSeed {
    u32 id;
    const char *name;
    const char *source;
    const char *const *extensions;
    u32 nextensions;
    const char *const *filenames;
    u32 nfilenames;
    const char *const *shebangs;
    u32 nshebangs;
    const char *first_line;
    i32 priority;
    SynComment comment;
} SynLangSeed;

SynDef *yew_syn_def_compile(Arena *a, DiagCtx *dc, const u8 *src, size_t n,
                            u32 file_id, u32 *n_err, u32 *n_warn);
SynDef *yew_syn_def_load(Arena *a, DiagCtx *dc, const char *path);
void yew_syn_def_dispose(SynDef *def);

u32 yew_syn_lang_for(const char *path, const u8 *line1, u32 l1_len);
const SynDef *yew_syn_def_for(u32 lang);
SynEngine *yew_syn_engine_for(u32 lang);
const SynLangDesc *yew_syn_lang_desc(u32 lang);
u32 yew_syn_lang_named(const char *name);
u32 yew_syn_lang_count(void);
/* Forget definitions discovered under $XDG_CONFIG_HOME/yew/syntax and make
 * the next registry lookup rescan that directory.  Primarily useful to
 * process-lifetime owners and environment-isolated tests. */
void yew_syn_discovery_reset(void);
/* Set before the first registry lookup; --clean uses this to suppress user
 * definition discovery without changing the built-in table. */
void yew_syn_discovery_set_bypass(bool bypass);
const char *yew_syn_ctx_name(const SynDef *def, u16 ctx);
const char *yew_syn_rule_pattern(const SynDef *def, u32 rule);
const char *yew_syn_attr_name(u8 attr);
bool yew_syn_attr_id(const char *name, size_t n, u8 *out);
bool yew_syn_def_firstbyte_check(const SynDef *def, u32 *bad_rule,
                                 u8 *bad_byte);

/* Observable only for cache/laziness tests. */
u64 yew_syn_compile_count(void);
void yew_syn_compile_count_reset(void);

char *yew_syn_cache_dir(void);
/* Return the cache entry for a definition source path.  The basename keeps
 * entries recognizable; the full path hash prevents equal stems in distinct
 * directories from sharing an entry. */
char *yew_syn_cache_path(const char *source);
bool yew_syn_cache_clear(void);
void yew_syn_cache_set_bypass(bool bypass);

#endif
