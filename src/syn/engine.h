#ifndef YEW_SYN_ENGINE_H
#define YEW_SYN_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

#include "search/regex.h"
#include "syn/attr.h"
#include "text/coords.h"
#include "util/base.h"
#include "util/intern.h"

typedef struct TextBuf TextBuf;

#define YEW_SYN_DEPTH_MAX 16U
#define YEW_SYN_LOST_MAX 255U
#define YEW_SYN_MAX_STATES 65536U
#define YEW_SYN_MAX_SPANS 4096U
#define YEW_SYN_LINE_BYTE_CAP (64U * 1024U)
#define YEW_SYN_LINE_STEPS(len) (4096ULL + 8ULL * (u64)(len))
#define YEW_SYN_FRAME_BUDGET_US 1000
#define YEW_SYN_IDLE_BUDGET_US 4000
#define YEW_SYN_CLOCK_EVERY 256U
#define YEW_SYN_SETTLING_MS 250U
#define YEW_SYN_SPAN_CACHE 256U

#define YEW_SYN_STATE_UNKNOWN 0U
#define YEW_SYN_STATE_ROOT 1U
#define YEW_LANG_NONE 0U

enum {
    YEW_SYN_F_VALUE = 1U << 0,
    YEW_SYN_F_STRIP = 1U << 1,
    YEW_SYN_F_DEGRADED = 1U << 2
};

enum {
    YEW_SPAN_TRUNCATED = 1U << 0
};

typedef enum SynStop {
    YEW_SYN_STOP_OK = 0,
    YEW_SYN_STOP_SPANS,
    YEW_SYN_STOP_BYTES,
    YEW_SYN_STOP_STEPS
} SynStop;

typedef struct SynState {
    u16 ctx[YEW_SYN_DEPTH_MAX];
    u32 aux;
    u8 depth;
    u8 lost;
    u8 def;
    u8 flags;
} SynState;

_Static_assert(sizeof(SynState) == 40, "state tuple size");

typedef struct SynSpan {
    u32 start;
    u16 len;
    u8 attr;
    u8 flags;
} SynSpan;

_Static_assert(sizeof(SynSpan) == 8, "span size");

typedef struct SynLineOut {
    SynSpan *spans;
    u32 n;
    u32 cap;
    u32 exit_state;
    u8 stop;
} SynLineOut;

typedef enum SynOp {
    SYN_OP_STAY = 0,
    SYN_OP_PUSH,
    SYN_OP_POP,
    SYN_OP_SET
} SynOp;

typedef enum SynAuxMatch {
    SYN_AUXM_NONE = 0,
    SYN_AUXM_LINE_EQ,
    SYN_AUXM_LITERAL,
    SYN_AUXM_FENCE_CLOSE,
    SYN_AUXM_INDENT_LT
} SynAuxMatch;

enum {
    YEW_SYN_RULE_SET_AUX = 1U << 0,
    YEW_SYN_RULE_SET_VALUE = 1U << 1,
    YEW_SYN_RULE_CLR_VALUE = 1U << 2,
    YEW_SYN_RULE_STRIP = 1U << 3,
    YEW_SYN_RULE_ZERO_POP = 1U << 4,
    YEW_SYN_RULE_AUX_INT = 1U << 5,
    YEW_SYN_RULE_CLR_AUX = 1U << 6
};

enum {
    YEW_SYN_CTX_UNIT_SPAN = 1U << 0,
    YEW_SYN_CTX_UNIT_ATOM = 1U << 1
};

typedef struct SynRule {
    const YewRe *re;
    u8 attr;
    u8 op;
    u8 nop;
    u8 aux_match;
    u16 target;
    u8 consume;
    u8 flags;
    u8 caps[8];
    u8 aux_group;
    u32 aux_pre;             /* interned literal affix, 0 = empty */
    u32 aux_post;            /* interned literal affix, 0 = empty */
    u16 push[4];             /* multi-push outermost to innermost */
    u8 npush;                /* 0 uses target as the single push */
    u8 first[32];
} SynRule;

typedef struct SynCtx {
    u32 first_rule;
    u32 nrules;
    u8 dflt_attr;
    u8 at_eol;
    u8 eol_nop;
    u8 flags;
    u8 first[32];
    u16 eol_target;          /* SYN_OP_SET target; zero otherwise */
} SynCtx;

typedef struct SynDef {
    const char *name;
    u16 root;
    u16 nctxs;
    u32 nrules;
    SynCtx *ctxs;
    SynRule *rules;
    Interner *aux;
} SynDef;

typedef struct SynStateTab SynStateTab;
typedef struct SynEngine SynEngine;

SynStateTab *yew_syn_state_tab_new(u16 root_ctx);
void yew_syn_state_tab_free(SynStateTab *tab);
u32 yew_syn_state_intern(SynStateTab *tab, const SynState *state);
const SynState *yew_syn_state_get(const SynStateTab *tab, u32 id);
u32 yew_syn_state_count(const SynStateTab *tab);
bool yew_syn_state_exhausted(const SynStateTab *tab);

/* Direct transition seams make the depth/lost laws independently testable. */
void yew_syn_state_push(SynState *state, u16 ctx);
void yew_syn_state_pop(SynState *state, u8 count);
void yew_syn_state_set(SynState *state, u16 ctx);

SynEngine *yew_syn_engine_new(SynDef *def);
void yew_syn_engine_free(SynEngine *engine);
void yew_syn_engine_set_def(SynEngine *engine, SynDef *def);
SynStateTab *yew_syn_engine_states(SynEngine *engine);
const SynDef *yew_syn_engine_def(const SynEngine *engine);
u64 yew_syn_engine_line_calls(const SynEngine *engine);
void yew_syn_engine_reset_counters(SynEngine *engine);

void yew_syn_line(SynEngine *engine, u32 entry_state, const u8 *line,
                  u32 len, SynLineOut *out);
bool yew_syn_stack_at(SynEngine *engine, u32 entry_state, const u8 *line,
                      u32 len, u32 p, SynState *out);

typedef i64 (*SynClockFn)(void *ctx);

typedef struct SynU32Vec {
    u32 *data;
    size_t len;
    size_t cap;
} SynU32Vec;

typedef struct SynBuf {
    u32 lang;
    SynU32Vec entry;
    LineNo settled_to;
    LineNo wave;
    u64 buf_gen;
    bool settling;
    bool degraded;
    bool edit_spliced;
    bool spec_valid;
    LineNo spec_from;
    LineNo must_reach;
    SynEngine *engine;
    u64 engine_gen;
    i64 edit_us;
    SynClockFn clock;
    void *clock_ctx;
    void *private_cache;
    u64 splice_count;
    u64 provisional_corrections;
} SynBuf;

typedef struct SynSettleReport {
    u64 lines;
    u64 us;
    bool fixpoint;
    bool hit_view;
    bool provisional;
    LineNo damage_lo;
    LineNo damage_hi;
} SynSettleReport;

void yew_syn_buf_init(SynBuf *syn);
void yew_syn_attach(SynBuf *syn, u32 lang, const TextBuf *tb);
void yew_syn_detach(SynBuf *syn);
void yew_syn_buf_bind(SynBuf *syn, SynEngine *engine);
void yew_syn_buf_set_clock(SynBuf *syn, SynClockFn clock, void *ctx);
void yew_syn_edit(SynBuf *syn, LineNo lo, u64 removed, u64 inserted);
void yew_syn_settle(SynBuf *syn, const TextBuf *tb, LineNo view_lo,
                    LineNo view_hi, i64 budget_us, SynSettleReport *report);
void yew_syn_spans(SynBuf *syn, const TextBuf *tb, LineNo line,
                   SynLineOut *out);
bool yew_syn_status_visible(const SynBuf *syn);
void yew_syn_status(const SynBuf *syn, u64 line_count, char *dst,
                    size_t cap);

#endif
