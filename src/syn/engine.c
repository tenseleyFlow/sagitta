#define _POSIX_C_SOURCE 200809L

#include "syn/engine.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "search/regex_internal.h"
#include "syn/defs.h"
#include "syn/theme.h"
#include "text/piece.h"
#include "unicode/utf8.h"
#include "util/log.h"

typedef struct SynCacheEnt {
    u64 line;
    u64 gen;
    u32 entry_state;
    u32 exit_state;
    u32 span_off;
    u32 n;
    u8 stop;
    bool valid;
} SynCacheEnt;

typedef struct SynCache {
    SynCacheEnt slots[YEW_SYN_SPAN_CACHE];
    u32 hand;
    u32 slab_hand;
    SynSpan *slab;
    u32 slab_cap;
    SynSpan scratch[YEW_SYN_MAX_SPANS];
    u8 *line;
    size_t line_cap;
} SynCache;

typedef struct SynWordSlot {
    u64 hash;
    u32 off;
    u32 len;
} SynWordSlot;

typedef struct SynWordSet {
    SynWordSlot *slots;
    u8 *bytes;
    u32 cap;
    u32 count;
    u32 bytes_len;
    u32 bytes_cap;
    u32 collect_steps;
    bool fold_ascii;
    bool overflow;
} SynWordSet;

typedef enum SynWordProbeStatus {
    SYN_WORD_PROBE_UNSET,
    SYN_WORD_PROBE_NO_MATCH,
    SYN_WORD_PROBE_FALLBACK,
    SYN_WORD_PROBE_READY
} SynWordProbeStatus;

typedef struct SynWordProbe {
    u64 exact_hash;
    u64 folded_hash;
    u32 hi;
    u8 status;
} SynWordProbe;

typedef struct SynFortranWordSlot {
    u64 hash;
    u32 off;
    u32 len;
    u32 rule;
} SynFortranWordSlot;

typedef struct SynFiniteLit {
    u32 off;
    u16 len;
    u16 cap_lo;
    u16 cap_hi;
} SynFiniteLit;

typedef struct SynFiniteSet {
    SynFiniteLit *lits;
    u8 *bytes;
    u32 count;
    u32 cap;
    u32 bytes_len;
    u32 bytes_cap;
    u8 ngroups;
} SynFiniteSet;

typedef struct SynResident {
    u32 lang;
    struct SynEngine *runtime;
} SynResident;

typedef struct SynIdentifierSpec {
    u8 prefix[2];
    u8 nprefix;
    u8 start_kind;
    u8 continue_kind;
    bool boundary_before;
    bool boundary_after;
} SynIdentifierSpec;

enum {
    SYN_CANDIDATE_BYTES = 256,
    SYN_CANDIDATE_STRIDE = SYN_CANDIDATE_BYTES + 1,
    SYN_CANDIDATE_RULE_BYTES_MAX = 8 * 1024 * 1024,
    SYN_WORD_LITERAL_PROG_MAX = 4096,
    SYN_WORD_LITERAL_COUNT_MAX = 4096,
    SYN_WORD_LITERAL_BYTES_MAX = 256 * 1024,
    SYN_WORD_LITERAL_STEPS_MAX = 64 * 1024,
    SYN_FINITE_LITERAL_MAX = 512,
    SYN_FINITE_BYTES_MAX = 32 * 1024,
    SYN_FINITE_LENGTH_MAX = 256
};

enum {
    SYN_FORTRAN_NONE,
    SYN_FORTRAN_FREE,
    SYN_FORTRAN_FIXED
};

struct SynStateTab {
    SynState *states;
    u32 len;
    u32 cap;
    u32 *slots;
    u32 slots_cap;
    bool exhausted;
};

typedef struct SynMergedFirst {
    u16 active_ctx;
    u16 bridge_ctx;
    u8 active_def;
    u8 bridge_def;
    u8 bol;
    bool valid;
    u8 bits[32];
    u8 end_bits[32];
} SynMergedFirst;

typedef struct SynLangName {
    char *name;
    u32 lang;
} SynLangName;

struct SynEngine {
    SynDef *def;
    SynStateTab *states;
    u8 *ctx_aux;
    u8 *rule_bol;
    u8 *rule_wordb;
    u8 *rule_word_literal;
    u8 *rule_identifier_suffix;
    u8 *rule_json_key;
    u8 *rule_yaml_block_key;
    SynIdentifierSpec *rule_identifiers;
    SynWordSet *word_sets;
    u32 word_sets_len;
    SynFiniteSet *finite_sets;
    u32 finite_sets_len;
    u8 (*rule_first)[32];
    u8 (*ctx_first_nonbol)[32];
    u32 *candidate_offsets;
    u32 *candidate_rules;
    YewReWorkspace re_workspace;
    bool has_first_line;
    bool identifier_fast_enabled;
    bool word_literal_fast_enabled;
    bool finite_literal_fast_enabled;
    bool fortran_fast_enabled;
    bool json_key_fast_enabled;
    bool yaml_block_key_fast_enabled;
    u8 fortran_form;
    u8 fortran_bol_rules;
    u8 *rule_fortran_word;
    SynFortranWordSlot *fortran_words;
    u32 fortran_words_cap;
    u32 fortran_words_count;
    SynCoverage *coverage;
    u64 line_calls;
    u64 generation;
    SynResident defs[YEW_SYN_RESIDENT_MAX];
    u16 ndefs;
    const char **ctx_names;
    SynMergedFirst merged_first[64];
    u8 embed_end_first[32];
    u8 embed_bol_end_first[32];
    u8 embed_local_end_first[32];
    u8 embed_local_bol_end_first[32];
    SynLangName *lang_names;
    u32 nlang_names;
};

static const SynCtx *checked_ctx(SynEngine *master, const SynFrame *frame,
                                 SynEngine **runtime_out);

static void engine_index_fortran_bol(SynEngine *engine)
{
    static const char *const fixed_patterns[] = {
        "^[Cc*!].*$",
        "^#.*$",
        "^(.{72})(.+)$",
        "^(\\t)([1-9])",
        "^\\t",
        "^((?:[0-9][ 0-9]{4}| [0-9][ 0-9]{3}|  [0-9][ 0-9]{2}|   [0-9][ 0-9]|    [0-9]))([^ 0])",
        "^((?:[0-9][ 0-9]{4}| [0-9][ 0-9]{3}|  [0-9][ 0-9]{2}|   [0-9][ 0-9]|    [0-9]))[ 0]",
        "^( {5})([^ 0])",
        "^ {5}[ 0]"
    };
    static const char *const free_patterns[] = {
        "^\\s*#.*$",
        "^(\\s*)(&)",
        "^(\\s*)([0-9]{1,5})(\\s+)"
    };
    const char *const *patterns;
    const SynCtx *root;
    u8 count;
    u32 i;

    engine->fortran_form = SYN_FORTRAN_NONE;
    engine->fortran_bol_rules = 0U;
    if (engine->def == NULL || engine->def->name == NULL ||
        engine->def->ctxs == NULL || engine->def->root >= engine->def->nctxs ||
        engine->rule_bol == NULL)
        return;
    if (strcmp(engine->def->name, "fortran-fixed") == 0) {
        engine->fortran_form = SYN_FORTRAN_FIXED;
        count = (u8)YEW_ARRAY_LEN(fixed_patterns);
        patterns = fixed_patterns;
    } else if (strcmp(engine->def->name, "fortran") == 0) {
        engine->fortran_form = SYN_FORTRAN_FREE;
        count = (u8)YEW_ARRAY_LEN(free_patterns);
        patterns = free_patterns;
    } else {
        return;
    }
    root = &engine->def->ctxs[engine->def->root];
    if (root->nrules < count || root->first_rule > engine->def->nrules - count)
        goto reject;
    for (i = 0U; i < count; i++) {
        u32 index = root->first_rule + i;
        const char *pattern = yew_syn_rule_pattern(engine->def, index);

        if (engine->rule_bol[index] == 0U || pattern == NULL ||
            strcmp(pattern, patterns[i]) != 0 ||
            engine->def->rules[index].aux_match != SYN_AUXM_NONE ||
            engine->def->rules[index].value_pred != SYN_VALUE_ANY ||
            (engine->def->rules[index].flags &
             YEW_SYN_RULE_FIRST_LINE) != 0U)
            goto reject;
    }
    engine->fortran_bol_rules = count;
    return;
reject:
    engine->fortran_form = SYN_FORTRAN_NONE;
}

static bool ascii_identifier_start(u8 byte)
{
    return byte == (u8)'_' || (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'a' && byte <= (u8)'z');
}

static bool ascii_identifier_continue(u8 byte)
{
    return ascii_identifier_start(byte) ||
           (byte >= (u8)'0' && byte <= (u8)'9');
}

static bool ascii_space(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t' || byte == (u8)'\n' ||
           byte == (u8)'\v' || byte == (u8)'\f' || byte == (u8)'\r';
}

static bool ascii_yaml_key_start(u8 byte)
{
    return !ascii_space(byte) && byte != (u8)'#';
}

static bool ascii_yaml_key_continue(u8 byte)
{
    return byte != (u8)':' && byte != (u8)'#';
}

static bool ascii_hex(u8 byte)
{
    return (byte >= (u8)'0' && byte <= (u8)'9') ||
           (byte >= (u8)'A' && byte <= (u8)'F') ||
           (byte >= (u8)'a' && byte <= (u8)'f');
}

static bool ascii_json_escape(u8 byte)
{
    return byte == (u8)'"' || byte == (u8)'\\' || byte == (u8)'/' ||
           byte == (u8)'b' || byte == (u8)'f' || byte == (u8)'n' ||
           byte == (u8)'r' || byte == (u8)'t';
}

static bool ascii_json_key_byte(u8 byte)
{
    return byte >= 0x20U && byte != (u8)'"' && byte != (u8)'\\';
}

enum {
    SYN_IDENT_CLASS_NONE,
    SYN_IDENT_CLASS_START,
    SYN_IDENT_CLASS_CONTINUE,
    SYN_IDENT_CLASS_UPPER,
    SYN_IDENT_CLASS_UPPER_CONTINUE,
    SYN_IDENT_CLASS_CAMEL_CONTINUE
};

static bool ascii_upper(u8 byte)
{
    return byte >= (u8)'A' && byte <= (u8)'Z';
}

static bool ascii_upper_continue(u8 byte)
{
    return ascii_upper(byte) || byte == (u8)'_' ||
           (byte >= (u8)'0' && byte <= (u8)'9');
}

static bool ascii_camel_continue(u8 byte)
{
    return (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'a' && byte <= (u8)'z') ||
           (byte >= (u8)'0' && byte <= (u8)'9');
}

typedef bool (*AsciiPred)(u8 byte);

static bool bitset_has(const u8 bits[32], u8 byte);

static bool class_ascii_equals(const YewRe *re, u32 index, AsciiPred pred)
{
    u32 byte;

    if (index >= re->nclasses)
        return false;
    for (byte = 0U; byte < 128U; byte++) {
        if (yew_re_class_has(&re->classes[index], byte) != pred((u8)byte))
            return false;
    }
    return true;
}

static u8 identifier_class_kind(const YewRe *re, u32 index)
{
    u32 range;

    if (index >= re->nclasses)
        return SYN_IDENT_CLASS_NONE;
    for (range = 0U; range < re->classes[index].n; range++) {
        if (re->classes[index].r[range].hi >= 0x80U)
            return SYN_IDENT_CLASS_NONE;
    }
    if (class_ascii_equals(re, index, ascii_identifier_start))
        return SYN_IDENT_CLASS_START;
    if (class_ascii_equals(re, index, ascii_identifier_continue))
        return SYN_IDENT_CLASS_CONTINUE;
    if (class_ascii_equals(re, index, ascii_upper))
        return SYN_IDENT_CLASS_UPPER;
    if (class_ascii_equals(re, index, ascii_upper_continue))
        return SYN_IDENT_CLASS_UPPER_CONTINUE;
    if (class_ascii_equals(re, index, ascii_camel_continue))
        return SYN_IDENT_CLASS_CAMEL_CONTINUE;
    return SYN_IDENT_CLASS_NONE;
}

static bool identifier_kind_has(u8 kind, u8 byte)
{
    switch (kind) {
    case SYN_IDENT_CLASS_START: return ascii_identifier_start(byte);
    case SYN_IDENT_CLASS_CONTINUE: return ascii_identifier_continue(byte);
    case SYN_IDENT_CLASS_UPPER: return ascii_upper(byte);
    case SYN_IDENT_CLASS_UPPER_CONTINUE: return ascii_upper_continue(byte);
    case SYN_IDENT_CLASS_CAMEL_CONTINUE: return ascii_camel_continue(byte);
    default: break;
    }
    return false;
}

static bool regex_ascii_identifier(const YewRe *re, SynIdentifierSpec *out)
{
    SynIdentifierSpec spec = {{0U, 0U}, 0U, 0U, 0U, false, false};
    u32 pc = 0U;
    u32 split;

    if (re == NULL || out == NULL || re->ngroups != 1U ||
        re->nprog < 7U || (re->flags & YEW_RE_ICASE) != 0U ||
        (ReOp)re->prog[pc].op != RE_SAVE || re->prog[pc].arg != 0U)
        return false;
    pc++;
    if ((ReOp)re->prog[pc].op == RE_WORDB) {
        spec.boundary_before = true;
        pc++;
    }
    while (pc < re->nprog && (ReOp)re->prog[pc].op == RE_CHAR &&
           spec.nprefix < YEW_ARRAY_LEN(spec.prefix)) {
        if (re->prog[pc].arg >= 0x80U)
            return false;
        spec.prefix[spec.nprefix++] = (u8)re->prog[pc].arg;
        pc++;
    }
    if (pc >= re->nprog || (ReOp)re->prog[pc].op != RE_CLASS)
        return false;
    spec.start_kind = identifier_class_kind(re, re->prog[pc].arg);
    if (spec.start_kind == SYN_IDENT_CLASS_NONE)
        return false;
    pc++;
    split = pc;
    if (pc + 2U >= re->nprog ||
        (ReOp)re->prog[pc].op != RE_SPLIT ||
        re->prog[pc].x != pc + 1U || re->prog[pc].y != pc + 3U ||
        (ReOp)re->prog[pc + 1U].op != RE_CLASS ||
        (ReOp)re->prog[pc + 2U].op != RE_JMP ||
        re->prog[pc + 2U].x != split)
        return false;
    spec.continue_kind =
        identifier_class_kind(re, re->prog[pc + 1U].arg);
    if (spec.continue_kind == SYN_IDENT_CLASS_NONE)
        return false;
    pc += 3U;
    if ((ReOp)re->prog[pc].op == RE_WORDB) {
        spec.boundary_after = true;
        pc++;
    }
    if (pc + 1U >= re->nprog || (ReOp)re->prog[pc].op != RE_SAVE ||
        re->prog[pc].arg != 1U ||
        (ReOp)re->prog[pc + 1U].op != RE_MATCH ||
        pc + 2U != re->nprog)
        return false;
    *out = spec;
    return true;
}

static u8 regex_identifier_suffix(const YewRe *re)
{
    const ReInst *p;

    if (re == NULL || re->nprog != 13U || re->ngroups != 2U ||
        (re->flags & YEW_RE_ICASE) != 0U)
        return 0U;
    p = re->prog;
    if ((ReOp)p[0].op != RE_SAVE || p[0].arg != 0U ||
        (ReOp)p[1].op != RE_SAVE || p[1].arg != 2U ||
        (ReOp)p[2].op != RE_CLASS ||
        (ReOp)p[3].op != RE_SPLIT || p[3].x != 4U || p[3].y != 6U ||
        (ReOp)p[4].op != RE_CLASS ||
        (ReOp)p[5].op != RE_JMP || p[5].x != 3U ||
        (ReOp)p[6].op != RE_SAVE || p[6].arg != 3U ||
        (ReOp)p[7].op != RE_SPLIT || p[7].x != 8U || p[7].y != 10U ||
        (ReOp)p[8].op != RE_CLASS ||
        (ReOp)p[9].op != RE_JMP || p[9].x != 7U ||
        (ReOp)p[10].op != RE_CHAR ||
        (p[10].arg != (u32)'(' && p[10].arg != (u32)':') ||
        (ReOp)p[11].op != RE_SAVE || p[11].arg != 1U ||
        (ReOp)p[12].op != RE_MATCH ||
        !class_ascii_equals(re, p[2].arg, ascii_identifier_start) ||
        !class_ascii_equals(re, p[4].arg, ascii_identifier_continue) ||
        !class_ascii_equals(re, p[8].arg, ascii_space))
        return 0U;
    return (u8)p[10].arg;
}

static bool regex_json_key(const YewRe *re)
{
    const ReInst *p;

    if (re == NULL || re->nprog != 28U || re->ngroups != 3U ||
        re->flags != 0U)
        return false;
    p = re->prog;
    if ((ReOp)p[0].op != RE_SAVE || p[0].arg != 0U ||
        (ReOp)p[1].op != RE_SAVE || p[1].arg != 2U ||
        (ReOp)p[2].op != RE_CHAR || p[2].arg != (u32)'"' ||
        (ReOp)p[3].op != RE_SPLIT || p[3].x != 4U || p[3].y != 20U ||
        (ReOp)p[4].op != RE_SAVE || p[4].arg != 4U ||
        (ReOp)p[5].op != RE_SPLIT || p[5].x != 6U || p[5].y != 17U ||
        (ReOp)p[6].op != RE_SPLIT || p[6].x != 7U || p[6].y != 10U ||
        (ReOp)p[7].op != RE_CHAR || p[7].arg != (u32)'\\' ||
        (ReOp)p[8].op != RE_CLASS ||
        (ReOp)p[9].op != RE_JMP || p[9].x != 16U ||
        (ReOp)p[10].op != RE_CHAR || p[10].arg != (u32)'\\' ||
        (ReOp)p[11].op != RE_CHAR || p[11].arg != (u32)'u' ||
        (ReOp)p[12].op != RE_CLASS ||
        (ReOp)p[13].op != RE_CLASS ||
        (ReOp)p[14].op != RE_CLASS ||
        (ReOp)p[15].op != RE_CLASS ||
        (ReOp)p[16].op != RE_JMP || p[16].x != 18U ||
        (ReOp)p[17].op != RE_CLASS ||
        (ReOp)p[18].op != RE_SAVE || p[18].arg != 5U ||
        (ReOp)p[19].op != RE_JMP || p[19].x != 3U ||
        (ReOp)p[20].op != RE_CHAR || p[20].arg != (u32)'"' ||
        (ReOp)p[21].op != RE_SAVE || p[21].arg != 3U ||
        (ReOp)p[22].op != RE_SPLIT || p[22].x != 23U || p[22].y != 25U ||
        (ReOp)p[23].op != RE_CLASS ||
        (ReOp)p[24].op != RE_JMP || p[24].x != 22U ||
        (ReOp)p[25].op != RE_CHAR || p[25].arg != (u32)':' ||
        (ReOp)p[26].op != RE_SAVE || p[26].arg != 1U ||
        (ReOp)p[27].op != RE_MATCH ||
        !class_ascii_equals(re, p[8].arg, ascii_json_escape) ||
        !class_ascii_equals(re, p[12].arg, ascii_hex) ||
        p[13].arg != p[12].arg || p[14].arg != p[12].arg ||
        p[15].arg != p[12].arg ||
        !class_ascii_equals(re, p[17].arg, ascii_json_key_byte) ||
        !class_ascii_equals(re, p[23].arg, ascii_space))
        return false;
    return true;
}

static bool regex_yaml_block_key(const YewRe *re)
{
    const ReInst *p;

    if (re == NULL || re->nprog != 24U || re->ngroups != 5U ||
        re->flags != 0U)
        return false;
    p = re->prog;
    return (ReOp)p[0].op == RE_SAVE && p[0].arg == 0U &&
           (ReOp)p[1].op == RE_BOL &&
           (ReOp)p[2].op == RE_SAVE && p[2].arg == 2U &&
           (ReOp)p[3].op == RE_SPLIT && p[3].x == 4U && p[3].y == 6U &&
           (ReOp)p[4].op == RE_CLASS &&
           (ReOp)p[5].op == RE_JMP && p[5].x == 3U &&
           (ReOp)p[6].op == RE_SAVE && p[6].arg == 3U &&
           (ReOp)p[7].op == RE_SAVE && p[7].arg == 4U &&
           (ReOp)p[8].op == RE_CLASS &&
           (ReOp)p[9].op == RE_SPLIT && p[9].x == 10U && p[9].y == 12U &&
           (ReOp)p[10].op == RE_CLASS &&
           (ReOp)p[11].op == RE_JMP && p[11].x == 9U &&
           (ReOp)p[12].op == RE_SAVE && p[12].arg == 5U &&
           (ReOp)p[13].op == RE_SAVE && p[13].arg == 6U &&
           (ReOp)p[14].op == RE_CHAR && p[14].arg == (u32)':' &&
           (ReOp)p[15].op == RE_SAVE && p[15].arg == 7U &&
           (ReOp)p[16].op == RE_SAVE && p[16].arg == 8U &&
           (ReOp)p[17].op == RE_SPLIT && p[17].x == 18U &&
           p[17].y == 20U &&
           (ReOp)p[18].op == RE_CLASS &&
           (ReOp)p[19].op == RE_JMP && p[19].x == 21U &&
           (ReOp)p[20].op == RE_EOL &&
           (ReOp)p[21].op == RE_SAVE && p[21].arg == 9U &&
           (ReOp)p[22].op == RE_SAVE && p[22].arg == 1U &&
           (ReOp)p[23].op == RE_MATCH &&
           class_ascii_equals(re, p[4].arg, ascii_space) &&
           class_ascii_equals(re, p[8].arg, ascii_yaml_key_start) &&
           class_ascii_equals(re, p[10].arg, ascii_yaml_key_continue) &&
           class_ascii_equals(re, p[18].arg, ascii_space);
}

static void engine_index_aux(SynEngine *engine)
{
    u32 i;
    free(engine->ctx_aux);
    engine->ctx_aux = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    if (engine->def->ctxs == NULL ||
        (engine->def->nrules != 0U && engine->def->rules == NULL))
        YEW_BUG("syntax: malformed compiled definition");
    engine->ctx_aux = yew_xcalloc(engine->def->nctxs,
                                  sizeof(*engine->ctx_aux));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;
        for (j = 0U; j < ctx->nrules; j++) {
            u32 rule = ctx->first_rule + j;
            if (rule < engine->def->nrules &&
                engine->def->rules[rule].aux_match != SYN_AUXM_NONE) {
                engine->ctx_aux[i] = 1U;
                break;
            }
        }
    }
}

static bool regex_requires_bol(const YewRe *re)
{
    u32 pc = 0U;
    u32 guard = 0U;

    if (re == NULL)
        return false;
    while (pc < re->nprog && guard++ <= re->nprog) {
        const ReInst *ins = &re->prog[pc];

        switch ((ReOp)ins->op) {
        case RE_BOL:
            return true;
        case RE_JMP:
            pc = ins->x;
            break;
        case RE_SAVE:
        case RE_BOT:
        case RE_EOT:
        case RE_WORDB:
        case RE_NWORDB:
            pc++;
            break;
        case RE_CHAR:
        case RE_CLASS:
        case RE_ANY:
        case RE_SPLIT:
        case RE_EOL:
        case RE_MATCH:
            return false;
        }
    }
    return false;
}

static bool regex_requires_wordb(const YewRe *re)
{
    u32 pc = 0U;
    u32 guard = 0U;

    if (re == NULL)
        return false;
    while (pc < re->nprog && guard++ <= re->nprog) {
        const ReInst *ins = &re->prog[pc];

        switch ((ReOp)ins->op) {
        case RE_WORDB:
            return true;
        case RE_JMP:
            pc = ins->x;
            break;
        case RE_SAVE:
        case RE_BOL:
        case RE_BOT:
        case RE_EOT:
        case RE_NWORDB:
            pc++;
            break;
        case RE_CHAR:
        case RE_CLASS:
        case RE_ANY:
        case RE_SPLIT:
        case RE_EOL:
        case RE_MATCH:
            return false;
        }
    }
    return false;
}

static bool regex_is_word_literal(const YewRe *re)
{
    u32 suffix;
    u32 stack[SYN_WORD_LITERAL_PROG_MAX];
    u8 seen[SYN_WORD_LITERAL_PROG_MAX];
    u32 nstack = 0U;

    if (re == NULL || re->nprog < 5U ||
        re->nprog > SYN_WORD_LITERAL_PROG_MAX ||
        re->ngroups > 2U || (ReOp)re->prog[0].op != RE_SAVE ||
        re->prog[0].arg != 0U || (ReOp)re->prog[1].op != RE_WORDB)
        return false;
    suffix = re->nprog - 3U;
    if (re->ngroups == 2U) {
        if (re->nprog < 7U || (ReOp)re->prog[2].op != RE_SAVE ||
            re->prog[2].arg != 2U ||
            (ReOp)re->prog[re->nprog - 4U].op != RE_SAVE ||
            re->prog[re->nprog - 4U].arg != 3U)
            return false;
        suffix--;
    }
    if ((ReOp)re->prog[re->nprog - 3U].op != RE_WORDB ||
        (ReOp)re->prog[re->nprog - 2U].op != RE_SAVE ||
        re->prog[re->nprog - 2U].arg != 1U ||
        (ReOp)re->prog[re->nprog - 1U].op != RE_MATCH)
        return false;
    (void)memset(seen, 0, sizeof(seen));
    stack[nstack++] = re->ngroups == 2U ? 3U : 2U;
    while (nstack != 0U) {
        u32 pc = stack[--nstack];
        const ReInst *ins;

        if (pc == suffix)
            continue;
        if (pc >= suffix)
            return false;
        if (seen[pc] != 0U)
            continue;
        seen[pc] = 1U;
        ins = &re->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR:
            if (ins->arg >= 0x80U)
                return false;
            if (nstack == YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = pc + 1U;
            break;
        case RE_CLASS:
            if (ins->arg >= re->nclasses ||
                nstack == YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = pc + 1U;
            break;
        case RE_JMP:
            if (ins->x <= pc)
                return false;
            if (nstack == YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = ins->x;
            break;
        case RE_SPLIT:
            if (ins->x <= pc || ins->y <= pc)
                return false;
            if (nstack + 2U > YEW_ARRAY_LEN(stack))
                return false;
            stack[nstack++] = ins->x;
            stack[nstack++] = ins->y;
            break;
        default:
            return false;
        }
    }
    return true;
}

static u8 ascii_fold(u8 byte)
{
    return byte >= (u8)'A' && byte <= (u8)'Z' ?
        (u8)(byte + ((u8)'a' - (u8)'A')) : byte;
}

static u64 word_hash(const u8 *word, u32 len, bool fold_ascii)
{
    u64 hash = UINT64_C(14695981039346656037);
    u32 i;

    for (i = 0U; i < len; i++) {
        u8 byte = fold_ascii ? ascii_fold(word[i]) : word[i];

        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0U ? 1U : hash;
}

static bool class_word_byte(const YewRe *re, u32 index, u8 *out)
{
    u8 bytes[2];
    u32 count = 0U;
    u32 byte;

    if (index >= re->nclasses)
        return false;
    for (byte = 0U; byte < 0x80U; byte++) {
        if (yew_re_class_has(&re->classes[index], byte)) {
            if (count == YEW_ARRAY_LEN(bytes))
                return false;
            bytes[count++] = (u8)byte;
        }
    }
    if (count == 1U && ascii_identifier_continue(bytes[0])) {
        *out = bytes[0];
        return true;
    }
    if (count == 2U && ascii_identifier_continue(bytes[0]) &&
        ascii_identifier_continue(bytes[1]) &&
        ascii_fold(bytes[0]) == ascii_fold(bytes[1])) {
        *out = ascii_fold(bytes[0]);
        return true;
    }
    return false;
}

static void word_set_free(SynWordSet *set)
{
    free(set->slots);
    free(set->bytes);
    (void)memset(set, 0, sizeof(*set));
}

static void word_set_rehash(SynWordSet *set, u32 cap)
{
    SynWordSlot *slots = yew_xcalloc(cap, sizeof(*slots));
    u32 i;

    for (i = 0U; i < set->cap; i++) {
        SynWordSlot slot = set->slots[i];
        u32 at;

        if (slot.hash == 0U)
            continue;
        at = (u32)slot.hash & (cap - 1U);
        while (slots[at].hash != 0U)
            at = (at + 1U) & (cap - 1U);
        slots[at] = slot;
    }
    free(set->slots);
    set->slots = slots;
    set->cap = cap;
}

static void word_set_insert(SynWordSet *set, const u8 *word, u32 len)
{
    u64 hash;
    u32 at;

    if (set->count >= SYN_WORD_LITERAL_COUNT_MAX ||
        len > SYN_WORD_LITERAL_BYTES_MAX - set->bytes_len) {
        set->overflow = true;
        return;
    }
    hash = word_hash(word, len, set->fold_ascii);
    if (set->cap == 0U || (set->count + 1U) * 4U >= set->cap * 3U)
        word_set_rehash(set, set->cap == 0U ? 16U : set->cap * 2U);
    at = (u32)hash & (set->cap - 1U);
    while (set->slots[at].hash != 0U) {
        const SynWordSlot *slot = &set->slots[at];

        if (slot->hash == hash && slot->len == len &&
            memcmp(set->bytes + slot->off, word, len) == 0)
            return;
        at = (at + 1U) & (set->cap - 1U);
    }
    if (len > UINT32_MAX - set->bytes_len)
        YEW_BUG("syntax: word literal table exceeds 4 GiB");
    if (set->bytes_len + len > set->bytes_cap) {
        u32 cap = set->bytes_cap == 0U ? 256U : set->bytes_cap;

        while (cap < set->bytes_len + len) {
            if (cap > UINT32_MAX / 2U)
                cap = UINT32_MAX;
            else
                cap *= 2U;
        }
        set->bytes = yew_xrealloc(set->bytes, cap);
        set->bytes_cap = cap;
    }
    set->slots[at] = (SynWordSlot){hash, set->bytes_len, len};
    if (len != 0U)
        (void)memcpy(set->bytes + set->bytes_len, word, len);
    set->bytes_len += len;
    set->count++;
}

static bool word_set_collect(SynWordSet *set, const YewRe *re, u32 pc,
                             u32 suffix, u8 word[SYN_WORD_LITERAL_PROG_MAX],
                             u32 len, u32 depth)
{
    const ReInst *ins;
    u8 byte;

    if (set->overflow || set->collect_steps >= SYN_WORD_LITERAL_STEPS_MAX) {
        set->overflow = true;
        return false;
    }
    set->collect_steps++;
    if (pc == suffix) {
        word_set_insert(set, word, len);
        return !set->overflow;
    }
    if (pc >= suffix || depth >= re->nprog ||
        len >= SYN_WORD_LITERAL_PROG_MAX)
        return false;
    ins = &re->prog[pc];
    switch ((ReOp)ins->op) {
    case RE_CHAR:
        if (ins->arg >= 0x80U ||
            !ascii_identifier_continue((u8)ins->arg))
            return false;
        word[len] = set->fold_ascii ? ascii_fold((u8)ins->arg) :
                                     (u8)ins->arg;
        return word_set_collect(set, re, pc + 1U, suffix, word, len + 1U,
                                depth + 1U);
    case RE_CLASS:
        if (!class_word_byte(re, ins->arg, &byte))
            return false;
        word[len] = set->fold_ascii ? ascii_fold(byte) : byte;
        return word_set_collect(set, re, pc + 1U, suffix, word, len + 1U,
                                depth + 1U);
    case RE_JMP:
        return word_set_collect(set, re, ins->x, suffix, word, len,
                                depth + 1U);
    case RE_SPLIT:
        return word_set_collect(set, re, ins->x, suffix, word, len,
                                depth + 1U) &&
               word_set_collect(set, re, ins->y, suffix, word, len,
                                depth + 1U);
    default:
        return false;
    }
}

static bool word_set_build(SynWordSet *set, const YewRe *re)
{
    u8 word[SYN_WORD_LITERAL_PROG_MAX];
    u32 start = re->ngroups == 2U ? 3U : 2U;
    u32 suffix = re->nprog - (re->ngroups == 2U ? 4U : 3U);

    set->fold_ascii = (re->flags & YEW_RE_ICASE) != 0U;
    if (!word_set_collect(set, re, start, suffix, word, 0U, 0U) ||
        set->count == 0U) {
        word_set_free(set);
        return false;
    }
    return true;
}

static void word_set_collect_partial(
    SynWordSet *set, const YewRe *re, u32 pc, u32 suffix,
    u8 word[SYN_WORD_LITERAL_PROG_MAX], u32 len, u32 depth)
{
    const ReInst *ins;
    u8 byte;

    if (set->overflow || set->collect_steps >= SYN_WORD_LITERAL_STEPS_MAX) {
        set->overflow = true;
        return;
    }
    set->collect_steps++;
    if (pc == suffix) {
        if (len != 0U)
            word_set_insert(set, word, len);
        return;
    }
    if (pc >= suffix || depth >= re->nprog ||
        len >= SYN_WORD_LITERAL_PROG_MAX)
        return;
    ins = &re->prog[pc];
    switch ((ReOp)ins->op) {
    case RE_CHAR:
        if (ins->arg >= 0x80U ||
            !ascii_identifier_continue((u8)ins->arg))
            return;
        word[len] = set->fold_ascii ? ascii_fold((u8)ins->arg) :
                                     (u8)ins->arg;
        word_set_collect_partial(set, re, pc + 1U, suffix, word, len + 1U,
                                 depth + 1U);
        return;
    case RE_CLASS:
        if (!class_word_byte(re, ins->arg, &byte))
            return;
        word[len] = set->fold_ascii ? ascii_fold(byte) : byte;
        word_set_collect_partial(set, re, pc + 1U, suffix, word, len + 1U,
                                 depth + 1U);
        return;
    case RE_JMP:
        word_set_collect_partial(set, re, ins->x, suffix, word, len,
                                 depth + 1U);
        return;
    case RE_SPLIT:
        word_set_collect_partial(set, re, ins->x, suffix, word, len,
                                 depth + 1U);
        word_set_collect_partial(set, re, ins->y, suffix, word, len,
                                 depth + 1U);
        return;
    default:
        return;
    }
}

static bool word_set_build_partial(SynWordSet *set, const YewRe *re)
{
    u8 word[SYN_WORD_LITERAL_PROG_MAX];
    u32 start;
    u32 suffix;

    if (re == NULL || re->nprog < 6U || re->ngroups < 1U ||
        re->ngroups > 2U)
        return false;
    start = re->ngroups == 2U ? 3U : 2U;
    suffix = re->nprog - (re->ngroups == 2U ? 4U : 3U);
    set->fold_ascii = (re->flags & YEW_RE_ICASE) != 0U;
    word_set_collect_partial(set, re, start, suffix, word, 0U, 0U);
    if (set->overflow || set->count == 0U) {
        word_set_free(set);
        return false;
    }
    return true;
}

static void engine_free_word_sets(SynEngine *engine)
{
    u32 i;

    for (i = 0U; i < engine->word_sets_len; i++)
        word_set_free(&engine->word_sets[i]);
    free(engine->word_sets);
    engine->word_sets = NULL;
    engine->word_sets_len = 0U;
}

static void finite_set_free(SynFiniteSet *set)
{
    free(set->lits);
    free(set->bytes);
    (void)memset(set, 0, sizeof(*set));
}

static bool finite_set_add(SynFiniteSet *set, const u8 *bytes, u32 len,
                           u16 cap_lo, u16 cap_hi)
{
    if (len == 0U || len > SYN_FINITE_LENGTH_MAX ||
        set->count == SYN_FINITE_LITERAL_MAX ||
        len > SYN_FINITE_BYTES_MAX - set->bytes_len)
        return false;
    if (set->count == set->cap) {
        u32 cap = set->cap == 0U ? 16U : set->cap * 2U;

        if (cap > SYN_FINITE_LITERAL_MAX)
            cap = SYN_FINITE_LITERAL_MAX;
        set->lits = yew_xreallocarray(set->lits, cap,
                                      sizeof(*set->lits));
        set->cap = cap;
    }
    if (set->bytes_len + len > set->bytes_cap) {
        u32 cap = set->bytes_cap == 0U ? 256U : set->bytes_cap;

        while (cap < set->bytes_len + len)
            cap *= 2U;
        set->bytes = yew_xrealloc(set->bytes, cap);
        set->bytes_cap = cap;
    }
    set->lits[set->count++] =
        (SynFiniteLit){set->bytes_len, (u16)len, cap_lo, cap_hi};
    (void)memcpy(set->bytes + set->bytes_len, bytes, len);
    set->bytes_len += len;
    return true;
}

static bool finite_class_ascii(const ReClass *cls)
{
    u32 i;

    if (cls == NULL || cls->n == 0U)
        return false;
    for (i = 0U; i < cls->n; i++) {
        if (cls->r[i].lo > cls->r[i].hi || cls->r[i].hi >= 0x80U)
            return false;
    }
    return true;
}

static bool finite_set_collect(SynFiniteSet *set, const YewRe *re, u32 pc,
                               u8 bytes[SYN_FINITE_LENGTH_MAX], u32 len,
                               u16 cap_lo, u16 cap_hi,
                               u8 active[SYN_WORD_LITERAL_PROG_MAX])
{
    const ReInst *ins;
    bool ok = false;

    if (pc >= re->nprog || active[pc] != 0U)
        return false;
    active[pc] = 1U;
    ins = &re->prog[pc];
    switch ((ReOp)ins->op) {
    case RE_CHAR:
        if (ins->arg < 0x80U && len < SYN_FINITE_LENGTH_MAX) {
            bytes[len] = (u8)ins->arg;
            ok = finite_set_collect(set, re, pc + 1U, bytes, len + 1U,
                                    cap_lo, cap_hi, active);
        }
        break;
    case RE_CLASS:
        if (ins->arg < re->nclasses &&
            finite_class_ascii(&re->classes[ins->arg]) &&
            len < SYN_FINITE_LENGTH_MAX) {
            const ReClass *cls = &re->classes[ins->arg];
            u32 range;

            ok = true;
            for (range = 0U; range < cls->n && ok; range++) {
                u32 byte;

                for (byte = cls->r[range].lo;
                     byte <= cls->r[range].hi && ok; byte++) {
                    bytes[len] = (u8)byte;
                    ok = finite_set_collect(set, re, pc + 1U, bytes,
                                            len + 1U, cap_lo, cap_hi,
                                            active);
                }
            }
        }
        break;
    case RE_JMP:
        ok = finite_set_collect(set, re, ins->x, bytes, len, cap_lo,
                                cap_hi, active);
        break;
    case RE_SPLIT:
        ok = finite_set_collect(set, re, ins->x, bytes, len, cap_lo,
                                cap_hi, active) &&
             finite_set_collect(set, re, ins->y, bytes, len, cap_lo,
                                cap_hi, active);
        break;
    case RE_SAVE:
        if (ins->arg <= 3U) {
            if (ins->arg == 2U)
                cap_lo = (u16)len;
            else if (ins->arg == 3U)
                cap_hi = (u16)len;
            ok = finite_set_collect(set, re, pc + 1U, bytes, len, cap_lo,
                                    cap_hi, active);
        }
        break;
    case RE_MATCH:
        ok = (cap_lo == UINT16_MAX) == (cap_hi == UINT16_MAX) &&
             (cap_lo == UINT16_MAX || cap_lo <= cap_hi) &&
             finite_set_add(set, bytes, len, cap_lo, cap_hi);
        break;
    default:
        break;
    }
    active[pc] = 0U;
    return ok;
}

static bool finite_set_build(SynFiniteSet *set, const YewRe *re)
{
    u8 bytes[SYN_FINITE_LENGTH_MAX];
    u8 active[SYN_WORD_LITERAL_PROG_MAX] = {0};

    if (re == NULL || re->nprog > SYN_WORD_LITERAL_PROG_MAX ||
        re->ngroups == 0U || re->ngroups > 2U ||
        !finite_set_collect(set, re, 0U, bytes, 0U, UINT16_MAX,
                            UINT16_MAX, active) ||
        set->count == 0U) {
        finite_set_free(set);
        return false;
    }
    set->ngroups = (u8)re->ngroups;
    return true;
}

static void engine_free_finite_sets(SynEngine *engine)
{
    u32 i;

    for (i = 0U; i < engine->finite_sets_len; i++)
        finite_set_free(&engine->finite_sets[i]);
    free(engine->finite_sets);
    engine->finite_sets = NULL;
    engine->finite_sets_len = 0U;
}

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static void regex_effective_first(const YewRe *re, u8 first[32])
{
    u32 *stack;
    u8 *seen;
    u32 nstack = 0U;

    (void)memset(first, 0, 32U);
    if (re == NULL || re->nprog == 0U) {
        (void)memset(first, 0xff, 32U);
        return;
    }
    stack = yew_xcalloc((size_t)re->nprog * 2U + 1U, sizeof(*stack));
    seen = yew_xcalloc(re->nprog, sizeof(*seen));
    stack[nstack++] = 0U;
    while (nstack != 0U) {
        u32 pc = stack[--nstack];
        const ReInst *ins;

        if (pc >= re->nprog || seen[pc] != 0U)
            continue;
        seen[pc] = 1U;
        ins = &re->prog[pc];
        switch ((ReOp)ins->op) {
        case RE_CHAR: {
            u8 encoded[YEW_UTF8_MAX];
            size_t n = yew_utf8_encode(ins->arg, encoded);

            if (n == 0U)
                (void)memset(first, 0xff, 32U);
            else
                first_add(first, encoded[0]);
            break;
        }
        case RE_CLASS: {
            u32 byte;
            const ReClass *cls;

            if (ins->arg >= re->nclasses) {
                (void)memset(first, 0xff, 32U);
                break;
            }
            cls = &re->classes[ins->arg];
            for (byte = 0U; byte < 128U; byte++) {
                if (yew_re_class_has(cls, byte))
                    first_add(first, (u8)byte);
            }
            for (byte = 128U; byte < 256U; byte++) {
                if (yew_re_class_has(cls, yew_utf8_escape_of((u8)byte)))
                    first_add(first, (u8)byte);
            }
            for (byte = 0U; byte < cls->n; byte++) {
                u32 lead;

                if (cls->r[byte].hi < 128U)
                    continue;
                for (lead = 0xc2U; lead <= 0xf4U; lead++)
                    first_add(first, (u8)lead);
                break;
            }
            break;
        }
        case RE_ANY:
            (void)memset(first, 0xff, 32U);
            if (ins->arg == 0U)
                first[(u8)'\n' >> 3U] &=
                    (u8)~(1U << ((u8)'\n' & 7U));
            break;
        case RE_SPLIT:
            stack[nstack++] = ins->x;
            stack[nstack++] = ins->y;
            break;
        case RE_JMP:
            stack[nstack++] = ins->x;
            break;
        case RE_SAVE:
        case RE_BOL:
        case RE_EOL:
        case RE_BOT:
        case RE_EOT:
        case RE_WORDB:
        case RE_NWORDB:
            stack[nstack++] = pc + 1U;
            break;
        case RE_MATCH:
            (void)memset(first, 0xff, 32U);
            break;
        }
    }
    free(seen);
    free(stack);
}

static void engine_index_bol(SynEngine *engine)
{
    u32 i;

    free(engine->rule_bol);
    engine->rule_bol = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_bol = yew_xcalloc(engine->def->nrules,
                                   sizeof(*engine->rule_bol));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_requires_bol(engine->def->rules[i].re))
            engine->rule_bol[i] = 1U;
    }
}

static void engine_index_first_line(SynEngine *engine)
{
    u32 i;

    engine->has_first_line = false;
    if (engine->def == NULL)
        return;
    for (i = 0U; i < engine->def->nrules; i++) {
        if ((engine->def->rules[i].flags & YEW_SYN_RULE_FIRST_LINE) != 0U) {
            engine->has_first_line = true;
            return;
        }
    }
}

static void engine_index_wordb(SynEngine *engine)
{
    u32 i;

    free(engine->rule_wordb);
    engine->rule_wordb = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_wordb = yew_xcalloc(engine->def->nrules,
                                     sizeof(*engine->rule_wordb));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_requires_wordb(engine->def->rules[i].re))
            engine->rule_wordb[i] = 1U;
    }
}

static void engine_index_word_literals(SynEngine *engine)
{
    u32 i;

    engine_free_word_sets(engine);
    free(engine->rule_word_literal);
    engine->rule_word_literal = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_word_literal = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_word_literal));
    engine->word_sets = yew_xcalloc(engine->def->nrules,
                                    sizeof(*engine->word_sets));
    engine->word_sets_len = engine->def->nrules;
    for (i = 0U; i < engine->def->nrules; i++) {
        if (regex_is_word_literal(engine->def->rules[i].re) &&
            word_set_build(&engine->word_sets[i],
                           engine->def->rules[i].re))
            engine->rule_word_literal[i] = 1U;
    }
}

static void engine_free_fortran_words(SynEngine *engine)
{
    free(engine->rule_fortran_word);
    free(engine->fortran_words);
    engine->rule_fortran_word = NULL;
    engine->fortran_words = NULL;
    engine->fortran_words_cap = 0U;
    engine->fortran_words_count = 0U;
}

static void fortran_words_rehash(SynEngine *engine, u32 cap)
{
    SynFortranWordSlot *slots = yew_xcalloc(cap, sizeof(*slots));
    u32 i;

    for (i = 0U; i < engine->fortran_words_cap; i++) {
        SynFortranWordSlot slot = engine->fortran_words[i];
        u32 at;

        if (slot.hash == 0U)
            continue;
        at = (u32)slot.hash & (cap - 1U);
        while (slots[at].hash != 0U)
            at = (at + 1U) & (cap - 1U);
        slots[at] = slot;
    }
    free(engine->fortran_words);
    engine->fortran_words = slots;
    engine->fortran_words_cap = cap;
}

static void fortran_words_insert(SynEngine *engine, u32 rule,
                                 const SynWordSlot *word)
{
    const SynWordSet *set = &engine->word_sets[rule];
    u32 at;

    if (engine->fortran_words_cap == 0U ||
        (engine->fortran_words_count + 1U) * 4U >=
            engine->fortran_words_cap * 3U)
        fortran_words_rehash(engine, engine->fortran_words_cap == 0U ?
                            64U : engine->fortran_words_cap * 2U);
    at = (u32)word->hash & (engine->fortran_words_cap - 1U);
    while (engine->fortran_words[at].hash != 0U) {
        const SynFortranWordSlot *slot = &engine->fortran_words[at];
        const SynWordSet *prior = &engine->word_sets[slot->rule];

        if (slot->hash == word->hash && slot->len == word->len &&
            memcmp(prior->bytes + slot->off, set->bytes + word->off,
                   word->len) == 0)
            return;
        at = (at + 1U) & (engine->fortran_words_cap - 1U);
    }
    engine->fortran_words[at] = (SynFortranWordSlot){
        word->hash, word->off, word->len, rule};
    engine->fortran_words_count++;
}

static void engine_index_fortran_words(SynEngine *engine)
{
    const SynCtx *root;
    u32 i;

    engine_free_fortran_words(engine);
    if (engine->fortran_form == SYN_FORTRAN_NONE || engine->def == NULL ||
        engine->word_sets == NULL || engine->rule_word_literal == NULL)
        return;
    root = &engine->def->ctxs[engine->def->root];
    engine->rule_fortran_word = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_fortran_word));
    for (i = 0U; i < root->nrules; i++) {
        u32 rule = root->first_rule + i;
        const SynWordSet *set;
        u32 slot;

        if (rule >= engine->def->nrules)
            continue;
        if (engine->rule_word_literal[rule] == 0U) {
            if (engine->rule_wordb == NULL ||
                engine->rule_wordb[rule] == 0U ||
                !word_set_build_partial(&engine->word_sets[rule],
                                        engine->def->rules[rule].re))
                continue;
            engine->rule_fortran_word[rule] = 2U;
        } else {
            engine->rule_fortran_word[rule] = 1U;
        }
        set = &engine->word_sets[rule];
        if (!set->fold_ascii)
            continue;
        for (slot = 0U; slot < set->cap; slot++) {
            if (set->slots[slot].hash != 0U)
                fortran_words_insert(engine, rule, &set->slots[slot]);
        }
    }
}

static void engine_index_finite_literals(SynEngine *engine)
{
    u32 i;

    engine_free_finite_sets(engine);
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->finite_sets = yew_xcalloc(engine->def->nrules,
                                      sizeof(*engine->finite_sets));
    engine->finite_sets_len = engine->def->nrules;
    for (i = 0U; i < engine->def->nrules; i++) {
        if ((engine->rule_word_literal == NULL ||
             engine->rule_word_literal[i] == 0U) &&
            engine->def->rules[i].aux_match == SYN_AUXM_NONE)
            (void)finite_set_build(&engine->finite_sets[i],
                                   engine->def->rules[i].re);
    }
}

static void engine_index_identifier_suffixes(SynEngine *engine)
{
    u32 i;

    free(engine->rule_identifier_suffix);
    free(engine->rule_identifiers);
    engine->rule_identifier_suffix = NULL;
    engine->rule_identifiers = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_identifier_suffix = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_identifier_suffix));
    engine->rule_identifiers = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_identifiers));
    for (i = 0U; i < engine->def->nrules; i++) {
        engine->rule_identifier_suffix[i] =
            regex_identifier_suffix(engine->def->rules[i].re);
        (void)regex_ascii_identifier(engine->def->rules[i].re,
                                     &engine->rule_identifiers[i]);
    }
}

static void engine_index_json_keys(SynEngine *engine)
{
    u32 i;

    free(engine->rule_json_key);
    engine->rule_json_key = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_json_key = yew_xcalloc(engine->def->nrules,
                                        sizeof(*engine->rule_json_key));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->def->rules[i].aux_match == SYN_AUXM_NONE &&
            regex_json_key(engine->def->rules[i].re))
            engine->rule_json_key[i] = 1U;
    }
}

static void engine_index_yaml_block_keys(SynEngine *engine)
{
    static const char *const yaml_root_prefix[] = {
        "^\\s*(---|\\.\\.\\.)\\s*(#.*)?$",
        "^\\s*%(YAML|TAG)\\b.*$",
        "^(\\s*)([^\\s#][^:#]*)(:)(\\s*)([|>][+-]?([1-9]?)[+-]?)(\\s*#.*)?$",
        "^(\\s*)(-)\\s+([|>][+-]?([1-9]?)[+-]?)(\\s*#.*)?$"
    };
    u32 i;

    free(engine->rule_yaml_block_key);
    engine->rule_yaml_block_key = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_yaml_block_key = yew_xcalloc(
        engine->def->nrules, sizeof(*engine->rule_yaml_block_key));
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->def->rules[i].aux_match == SYN_AUXM_NONE &&
            regex_yaml_block_key(engine->def->rules[i].re)) {
            engine->rule_yaml_block_key[i] = 1U;
            if (strcmp(engine->def->name, "yaml") == 0 &&
                engine->def->root < engine->def->nctxs) {
                const SynCtx *root =
                    &engine->def->ctxs[engine->def->root];
                u32 prefix;

                if (i >= root->first_rule &&
                    i - root->first_rule ==
                        YEW_ARRAY_LEN(yaml_root_prefix) &&
                    engine->def->rules[i].value_pred == SYN_VALUE_ANY &&
                    (engine->def->rules[i].flags &
                     YEW_SYN_RULE_FIRST_LINE) == 0U) {
                    for (prefix = 0U;
                         prefix < YEW_ARRAY_LEN(yaml_root_prefix);
                         prefix++) {
                        const char *pattern = yew_syn_rule_pattern(
                            engine->def, root->first_rule + prefix);

                        if (pattern == NULL ||
                            strcmp(pattern, yaml_root_prefix[prefix]) != 0)
                            break;
                    }
                    if (prefix == YEW_ARRAY_LEN(yaml_root_prefix))
                        engine->rule_yaml_block_key[i] = 2U;
                }
            }
        }
    }
}

static void engine_index_first(SynEngine *engine)
{
    u32 i;

    free(engine->rule_first);
    engine->rule_first = NULL;
    if (engine->def == NULL || engine->def->nrules == 0U)
        return;
    engine->rule_first = yew_xcalloc(engine->def->nrules,
                                     sizeof(*engine->rule_first));
    for (i = 0U; i < engine->def->nrules; i++)
        regex_effective_first(engine->def->rules[i].re,
                              engine->rule_first[i]);
}

static void engine_index_ctx_first_nonbol(SynEngine *engine)
{
    u32 i;

    free(engine->ctx_first_nonbol);
    engine->ctx_first_nonbol = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    engine->ctx_first_nonbol = yew_xcalloc(
        engine->def->nctxs, sizeof(*engine->ctx_first_nonbol));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;

        for (j = 0U; j < ctx->nrules; j++) {
            u32 index = ctx->first_rule + j;
            u32 byte;

            if (index >= engine->def->nrules)
                YEW_BUG("syntax: context rule range exceeds definition");
            if (engine->rule_bol != NULL &&
                engine->rule_bol[index] != 0U)
                continue;
            for (byte = 0U; byte < 32U; byte++)
                engine->ctx_first_nonbol[i][byte] |=
                    engine->rule_first[index][byte];
        }
    }
}

static void engine_index_embed_end_first(SynEngine *engine)
{
    u16 ctx_id;

    (void)memset(engine->embed_local_end_first, 0,
                 sizeof(engine->embed_local_end_first));
    (void)memset(engine->embed_local_bol_end_first, 0,
                 sizeof(engine->embed_local_bol_end_first));
    if (engine->def != NULL) {
        for (ctx_id = 0U; ctx_id < engine->def->nctxs; ctx_id++) {
            const SynCtx *ctx = &engine->def->ctxs[ctx_id];
            u8 *first;
            u32 i;

            if (ctx->embed.end == SYN_EMBED_END_INLINE ||
                ctx->embed.end == SYN_EMBED_END_INLINE_ROOT)
                first = engine->embed_local_end_first;
            else if (ctx->embed.end == SYN_EMBED_END_LINE ||
                     ctx->embed.end == SYN_EMBED_END_LINE_CONTINUATION)
                first = engine->embed_local_bol_end_first;
            else
                continue;
            for (i = 0U; i < ctx->nrules; i++) {
                u32 index = ctx->first_rule + i;
                const SynRule *rule = &engine->def->rules[index];
                u32 byte;

                if (rule->end == 0U)
                    continue;
                if (rule->aux_match != SYN_AUXM_NONE) {
                    (void)memset(first, 0xff,
                                 sizeof(engine->embed_local_end_first));
                    break;
                }
                for (byte = 0U;
                     byte < sizeof(engine->embed_local_end_first); byte++)
                    first[byte] |= engine->rule_first == NULL ?
                        rule->first[byte] : engine->rule_first[index][byte];
            }
        }
    }
    (void)memcpy(engine->embed_end_first,
                 engine->embed_local_end_first,
                 sizeof(engine->embed_end_first));
    (void)memcpy(engine->embed_bol_end_first,
                 engine->embed_local_bol_end_first,
                 sizeof(engine->embed_bol_end_first));
}

static void engine_index_candidates(SynEngine *engine)
{
    u32 *offsets;
    u64 total = 0U;
    size_t noffsets;
    u32 out = 0U;
    u32 i;

    free(engine->candidate_offsets);
    free(engine->candidate_rules);
    engine->candidate_offsets = NULL;
    engine->candidate_rules = NULL;
    if (engine->def == NULL || engine->def->nctxs == 0U)
        return;
    noffsets = (size_t)engine->def->nctxs * SYN_CANDIDATE_STRIDE;
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 j;

        for (j = 0U; j < ctx->nrules; j++) {
            u32 index = ctx->first_rule + j;
            u32 byte;

            if (index >= engine->def->nrules)
                YEW_BUG("syntax: context rule range exceeds definition");
            for (byte = 0U; byte < SYN_CANDIDATE_BYTES; byte++) {
                if (engine->def->rules[index].aux_match != SYN_AUXM_NONE ||
                    bitset_has(engine->rule_first[index], (u8)byte)) {
                    total++;
                    /* The index is an optional accelerator.  A broad valid
                     * definition must fall back to the filtered rule scan,
                     * not turn its rule/byte cross-product into a fatal
                     * allocation. */
                    if (total >
                        SYN_CANDIDATE_RULE_BYTES_MAX / sizeof(u32))
                        return;
                }
            }
        }
    }
    offsets = yew_xcalloc(noffsets, sizeof(*offsets));
    engine->candidate_rules = yew_xcalloc((size_t)total,
                                           sizeof(*engine->candidate_rules));
    for (i = 0U; i < engine->def->nctxs; i++) {
        const SynCtx *ctx = &engine->def->ctxs[i];
        u32 byte;

        for (byte = 0U; byte < SYN_CANDIDATE_BYTES; byte++) {
            u32 j;

            offsets[(size_t)i * SYN_CANDIDATE_STRIDE + byte] = out;
            for (j = 0U; j < ctx->nrules; j++) {
                u32 index = ctx->first_rule + j;

                if (engine->def->rules[index].aux_match != SYN_AUXM_NONE ||
                    bitset_has(engine->rule_first[index], (u8)byte))
                    engine->candidate_rules[out++] = index;
            }
        }
        offsets[(size_t)i * SYN_CANDIDATE_STRIDE +
                SYN_CANDIDATE_BYTES] = out;
    }
    engine->candidate_offsets = offsets;
}

static u64 state_hash(const SynState *state)
{
    const u8 *p = (const u8 *)state;
    u64 h = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < sizeof(*state); i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void state_validate(const SynState *state)
{
    if (state == NULL)
        YEW_BUG("syntax: NULL state");
    if (state->depth == 0U || state->depth > YEW_SYN_DEPTH_MAX)
        YEW_BUG("syntax: invalid state depth %u", (unsigned)state->depth);
    if (state->ndef == 0U || state->ndef > YEW_SYN_DEF_MAX)
        YEW_BUG("syntax: invalid definition depth %u",
                (unsigned)state->ndef);
}

static SynState syn_state_canon(const SynState *state)
{
    SynState canon;

    state_validate(state);
    canon = *state;
    (void)memset(&canon.f[canon.depth], 0,
                 (YEW_SYN_DEPTH_MAX - canon.depth) * sizeof(canon.f[0]));
    {
        u8 keep = canon.ndef;

        /* A pending guest occupies its future aux slot until the idle
         * loader has made that definition resident. */
        if (((canon.flags & YEW_SYN_F_EMBED_PEND) != 0U ||
             (canon.f[canon.depth - 1U].fl & YEW_SYN_FR_DEFER) != 0U) &&
            keep < YEW_SYN_DEF_MAX)
            keep++;
        (void)memset(&canon.aux[keep], 0,
                     (YEW_SYN_DEF_MAX - keep) * sizeof(canon.aux[0]));
    }
    return canon;
}

static void state_rehash(SynStateTab *tab, u32 cap)
{
    u32 *slots = yew_xcalloc(cap, sizeof(*slots));
    u32 i;

    for (i = 1U; i < tab->len; i++) {
        u32 at = (u32)state_hash(&tab->states[i]) & (cap - 1U);
        while (slots[at] != 0U)
            at = (at + 1U) & (cap - 1U);
        slots[at] = i;
    }
    free(tab->slots);
    tab->slots = slots;
    tab->slots_cap = cap;
}

SynStateTab *yew_syn_state_tab_new(u16 root_ctx)
{
    SynStateTab *tab = yew_xcalloc(1U, sizeof(*tab));
    SynState root;

    tab->cap = 16U;
    tab->states = yew_xcalloc(tab->cap, sizeof(*tab->states));
    tab->len = 2U;
    (void)memset(&root, 0, sizeof(root));
    root.f[0].ctx = root_ctx;
    root.depth = 1U;
    root.ndef = 1U;
    tab->states[YEW_SYN_STATE_ROOT] = root;
    state_rehash(tab, 32U);
    return tab;
}

void yew_syn_state_tab_free(SynStateTab *tab)
{
    if (tab == NULL)
        return;
    free(tab->slots);
    free(tab->states);
    free(tab);
}

u32 yew_syn_state_intern(SynStateTab *tab, const SynState *state)
{
    SynState canon;
    u32 at;

    if (tab == NULL)
        YEW_BUG("syntax: NULL state table");
    canon = syn_state_canon(state);
    at = (u32)state_hash(&canon) & (tab->slots_cap - 1U);
    while (tab->slots[at] != 0U) {
        u32 id = tab->slots[at];
        if (memcmp(&tab->states[id], &canon, sizeof(canon)) == 0)
            return id;
        at = (at + 1U) & (tab->slots_cap - 1U);
    }
    if (tab->len >= YEW_SYN_MAX_STATES) {
        if (!tab->exhausted)
            yew_log(YEW_LOG_WARN,
                    "syntax state table exhausted; reusing root state");
        tab->exhausted = true;
        return YEW_SYN_STATE_ROOT;
    }
    if (tab->len == tab->cap) {
        u32 cap = tab->cap * 2U;
        if (cap > YEW_SYN_MAX_STATES)
            cap = YEW_SYN_MAX_STATES;
        tab->states = yew_xreallocarray(tab->states, cap,
                                        sizeof(*tab->states));
        tab->cap = cap;
    }
    if ((u64)(tab->len + 1U) * 10U >= (u64)tab->slots_cap * 7U) {
        u32 cap = tab->slots_cap * 2U;
        if (cap < tab->slots_cap)
            YEW_BUG("syntax: state hash capacity overflow");
        state_rehash(tab, cap);
        at = (u32)state_hash(&canon) & (tab->slots_cap - 1U);
        while (tab->slots[at] != 0U)
            at = (at + 1U) & (tab->slots_cap - 1U);
    }
    tab->states[tab->len] = canon;
    tab->slots[at] = tab->len;
    return tab->len++;
}

const SynState *yew_syn_state_get(const SynStateTab *tab, u32 id)
{
    if (tab == NULL || id == YEW_SYN_STATE_UNKNOWN || id >= tab->len)
        return NULL;
    return &tab->states[id];
}

u32 yew_syn_state_count(const SynStateTab *tab)
{
    /* Includes the UNKNOWN and ROOT reservations, matching the id domain. */
    return tab == NULL ? 0U : tab->len;
}

bool yew_syn_state_exhausted(const SynStateTab *tab)
{
    return tab != NULL && tab->exhausted;
}

void yew_syn_state_push(SynState *state, u16 ctx)
{
    state_validate(state);
    if (state->depth < YEW_SYN_DEPTH_MAX) {
        SynFrame *frame = &state->f[state->depth];

        frame->ctx = ctx;
        frame->def = YEW_SYN_DEF_OF(state);
        frame->fl = 0U;
        state->depth++;
        return;
    }
    if (state->lost < YEW_SYN_LOST_MAX)
        state->lost++;
    if (state->lost == YEW_SYN_LOST_MAX)
        state->flags |= YEW_SYN_F_DEGRADED;
}

void yew_syn_state_pop(SynState *state, u8 count)
{
    state_validate(state);
    while (count-- != 0U) {
        u8 protected_embed_debt =
            (state->flags & YEW_SYN_F_EMBED_LOST) != 0U ? 2U : 0U;

        if (state->lost > protected_embed_debt) {
            state->lost--;
        } else if (state->depth > 1U) {
            state->depth--;
            (void)memset(&state->f[state->depth], 0,
                         sizeof(state->f[state->depth]));
        }
    }
}

void yew_syn_state_set(SynState *state, u16 ctx)
{
    state_validate(state);
    state->f[state->depth - 1U].ctx = ctx;
}

static void engine_snapshot_lang_names(SynEngine *engine)
{
    const char **names;
    u32 *langs;
    u32 count;
    u32 i;

    for (i = 0U; i < engine->nlang_names; i++)
        free(engine->lang_names[i].name);
    free(engine->lang_names);
    engine->lang_names = NULL;
    engine->nlang_names = 0U;
    count = yew_syn_lang_snapshot(NULL, NULL, 0U);
    if (count == 0U)
        return;
    names = yew_xcalloc(count, sizeof(*names));
    langs = yew_xcalloc(count, sizeof(*langs));
    count = yew_syn_lang_snapshot(names, langs, count);
    engine->lang_names = yew_xcalloc(count, sizeof(*engine->lang_names));
    for (i = 0U; i < count; i++) {
        size_t len = strlen(names[i]);

        engine->lang_names[i].name = yew_xmalloc(len + 1U);
        (void)memcpy(engine->lang_names[i].name, names[i], len + 1U);
        engine->lang_names[i].lang = langs[i];
    }
    engine->nlang_names = count;
    free(langs);
    free(names);
}

static u32 engine_lang_by_name(const SynEngine *engine, const char *name,
                               size_t len)
{
    u32 i;

    for (i = 0U; i < engine->nlang_names; i++) {
        const char *candidate = engine->lang_names[i].name;

        if (strlen(candidate) == len &&
            (len == 0U || memcmp(candidate, name, len) == 0))
            return engine->lang_names[i].lang;
    }
    return YEW_LANG_NONE;
}

static const char *engine_name_by_lang(const SynEngine *engine, u32 lang)
{
    u32 i;

    for (i = 0U; i < engine->nlang_names; i++) {
        if (engine->lang_names[i].lang == lang)
            return engine->lang_names[i].name;
    }
    return "none";
}

SynEngine *yew_syn_engine_new(SynDef *def)
{
    SynEngine *engine = yew_xcalloc(1U, sizeof(*engine));
    u16 root = def == NULL ? 0U : def->root;

    engine->def = def;
    engine_snapshot_lang_names(engine);
    (void)memset(engine->merged_first, 0, sizeof(engine->merged_first));
    engine->defs[0] = (SynResident){
        def == NULL || def->name == NULL ? YEW_LANG_NONE :
            engine_lang_by_name(engine, def->name, strlen(def->name)),
        engine
    };
    engine->ndefs = 1U;
    if (def != NULL && def->nctxs != 0U) {
        u16 i;

        engine->ctx_names = yew_xcalloc(def->nctxs,
                                        sizeof(*engine->ctx_names));
        for (i = 0U; i < def->nctxs; i++)
            engine->ctx_names[i] = yew_syn_ctx_name(def, i);
    }
    engine->states = yew_syn_state_tab_new(root);
    engine->generation = 1U;
    engine->identifier_fast_enabled = true;
    engine->word_literal_fast_enabled = true;
    engine->finite_literal_fast_enabled = true;
    engine->fortran_fast_enabled = true;
    engine->json_key_fast_enabled = true;
    engine->yaml_block_key_fast_enabled = true;
    yew_re_workspace_init(&engine->re_workspace);
    engine_index_aux(engine);
    engine_index_bol(engine);
    engine_index_fortran_bol(engine);
    engine_index_first_line(engine);
    engine_index_wordb(engine);
    engine_index_word_literals(engine);
    engine_index_fortran_words(engine);
    engine_index_finite_literals(engine);
    engine_index_identifier_suffixes(engine);
    engine_index_json_keys(engine);
    engine_index_yaml_block_keys(engine);
    engine_index_first(engine);
    engine_index_ctx_first_nonbol(engine);
    engine_index_embed_end_first(engine);
    engine_index_candidates(engine);
    return engine;
}

void yew_syn_engine_free(SynEngine *engine)
{
    u16 resident;

    if (engine == NULL)
        return;
    for (resident = 1U; resident < engine->ndefs; resident++) {
        SynEngine *runtime = engine->defs[resident].runtime;

        if (runtime != NULL)
            yew_syn_def_unpin(runtime->def);
    }
    yew_syn_state_tab_free(engine->states);
    free(engine->ctx_aux);
    free(engine->rule_bol);
    free(engine->rule_wordb);
    free(engine->rule_word_literal);
    free(engine->rule_identifier_suffix);
    free(engine->rule_json_key);
    free(engine->rule_yaml_block_key);
    free(engine->rule_identifiers);
    engine_free_fortran_words(engine);
    engine_free_word_sets(engine);
    engine_free_finite_sets(engine);
    free(engine->rule_first);
    free(engine->ctx_first_nonbol);
    free(engine->candidate_offsets);
    free(engine->candidate_rules);
    free(engine->ctx_names);
    for (u32 i = 0U; i < engine->nlang_names; i++)
        free(engine->lang_names[i].name);
    free(engine->lang_names);
    yew_re_workspace_free(&engine->re_workspace);
    free(engine);
}

void yew_syn_engine_set_def(SynEngine *engine, SynDef *def)
{
    u16 resident;

    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    for (resident = 1U; resident < engine->ndefs; resident++) {
        SynEngine *runtime = engine->defs[resident].runtime;

        if (runtime != NULL)
            yew_syn_def_unpin(runtime->def);
    }
    yew_syn_state_tab_free(engine->states);
    engine->def = def;
    engine_snapshot_lang_names(engine);
    (void)memset(engine->merged_first, 0, sizeof(engine->merged_first));
    free(engine->ctx_names);
    engine->ctx_names = NULL;
    engine->defs[0] = (SynResident){
        def == NULL || def->name == NULL ? YEW_LANG_NONE :
            engine_lang_by_name(engine, def->name, strlen(def->name)),
        engine
    };
    (void)memset(&engine->defs[1], 0,
                 (YEW_SYN_RESIDENT_MAX - 1U) * sizeof(engine->defs[0]));
    engine->ndefs = 1U;
    if (def != NULL && def->nctxs != 0U) {
        u16 i;

        engine->ctx_names = yew_xcalloc(def->nctxs,
                                        sizeof(*engine->ctx_names));
        for (i = 0U; i < def->nctxs; i++)
            engine->ctx_names[i] = yew_syn_ctx_name(def, i);
    }
    engine->coverage = NULL;
    engine->states = yew_syn_state_tab_new(def == NULL ? 0U : def->root);
    engine_index_aux(engine);
    engine_index_bol(engine);
    engine_index_fortran_bol(engine);
    engine_index_first_line(engine);
    engine_index_wordb(engine);
    engine_index_word_literals(engine);
    engine_index_fortran_words(engine);
    engine_index_finite_literals(engine);
    engine_index_identifier_suffixes(engine);
    engine_index_json_keys(engine);
    engine_index_yaml_block_keys(engine);
    engine_index_first(engine);
    engine_index_ctx_first_nonbol(engine);
    engine_index_embed_end_first(engine);
    engine_index_candidates(engine);
    engine->line_calls = 0U;
    engine->generation++;
    if (engine->generation == 0U)
        engine->generation = 1U;
}

SynStateTab *yew_syn_engine_states(SynEngine *engine)
{
    return engine == NULL ? NULL : engine->states;
}

const SynDef *yew_syn_engine_def(const SynEngine *engine)
{
    return engine == NULL ? NULL : engine->def;
}

const SynDef *yew_syn_engine_def_at(const SynEngine *engine, u8 def_index)
{
    if (engine == NULL || def_index >= engine->ndefs ||
        engine->defs[def_index].runtime == NULL)
        return NULL;
    return engine->defs[def_index].runtime->def;
}

const SynDef *yew_syn_def_resident(SynEngine *engine, u32 lang)
{
    u16 i;

    if (engine == NULL || lang == YEW_LANG_NONE)
        return NULL;
    for (i = 0U; i < engine->ndefs; i++) {
        if (engine->defs[i].lang == lang &&
            engine->defs[i].runtime != NULL)
            return engine->defs[i].runtime->def;
    }
    return NULL;
}

u64 yew_syn_engine_line_calls(const SynEngine *engine)
{
    return engine == NULL ? 0U : engine->line_calls;
}

void yew_syn_engine_reset_counters(SynEngine *engine)
{
    if (engine != NULL)
        engine->line_calls = 0U;
}

void yew_syn_engine_set_identifier_fast_path(SynEngine *engine,
                                             bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->identifier_fast_enabled = enabled;
}

u32 yew_syn_engine_identifier_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->def == NULL)
        return 0U;
    for (i = 0U; i < engine->def->nrules; i++) {
        if ((engine->rule_identifier_suffix != NULL &&
             engine->rule_identifier_suffix[i] != 0U) ||
            (engine->rule_identifiers != NULL &&
             engine->rule_identifiers[i].start_kind !=
                 SYN_IDENT_CLASS_NONE))
            count++;
    }
    return count;
}

void yew_syn_engine_set_word_literal_fast_path(SynEngine *engine,
                                               bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->word_literal_fast_enabled = enabled;
}

u32 yew_syn_engine_word_literal_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->def == NULL ||
        engine->rule_word_literal == NULL)
        return 0U;
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->rule_word_literal[i] != 0U)
            count++;
    }
    return count;
}

void yew_syn_engine_set_finite_literal_fast_path(SynEngine *engine,
                                                 bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->finite_literal_fast_enabled = enabled;
}

u32 yew_syn_engine_finite_literal_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->finite_sets == NULL)
        return 0U;
    for (i = 0U; i < engine->finite_sets_len; i++) {
        if (engine->finite_sets[i].count != 0U)
            count++;
    }
    return count;
}

void yew_syn_engine_set_fortran_fast_path(SynEngine *engine, bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->fortran_fast_enabled = enabled;
}

u32 yew_syn_engine_fortran_fast_rules(const SynEngine *engine)
{
    return engine == NULL ? 0U : engine->fortran_bol_rules;
}

void yew_syn_engine_set_json_key_fast_path(SynEngine *engine, bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->json_key_fast_enabled = enabled;
}

u32 yew_syn_engine_json_key_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->def == NULL ||
        engine->rule_json_key == NULL)
        return 0U;
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->rule_json_key[i] != 0U)
            count++;
    }
    return count;
}

void yew_syn_engine_set_yaml_block_key_fast_path(SynEngine *engine,
                                                 bool enabled)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    engine->yaml_block_key_fast_enabled = enabled;
}

u32 yew_syn_engine_yaml_block_key_fast_rules(const SynEngine *engine)
{
    u32 count = 0U;
    u32 i;

    if (engine == NULL || engine->def == NULL ||
        engine->rule_yaml_block_key == NULL)
        return 0U;
    for (i = 0U; i < engine->def->nrules; i++) {
        if (engine->rule_yaml_block_key[i] != 0U)
            count++;
    }
    return count;
}

bool yew_syn_coverage_init(SynCoverage *coverage, const SynDef *def)
{
    if (coverage == NULL || def == NULL)
        return false;
    (void)memset(coverage, 0, sizeof(*coverage));
    coverage->nctxs = def->nctxs;
    coverage->nrules = def->nrules;
    if (coverage->nctxs != 0U)
        coverage->contexts = yew_xcalloc(coverage->nctxs,
                                         sizeof(*coverage->contexts));
    if (coverage->nrules != 0U)
        coverage->rules = yew_xcalloc(coverage->nrules,
                                      sizeof(*coverage->rules));
    if (coverage->nctxs != 0U)
        coverage->embeds = yew_xcalloc(coverage->nctxs,
                                       sizeof(*coverage->embeds));
    return true;
}

void yew_syn_coverage_clear(SynCoverage *coverage)
{
    if (coverage == NULL)
        return;
    if (coverage->contexts != NULL)
        (void)memset(coverage->contexts, 0,
                     coverage->nctxs * sizeof(*coverage->contexts));
    if (coverage->rules != NULL)
        (void)memset(coverage->rules, 0,
                     coverage->nrules * sizeof(*coverage->rules));
    if (coverage->embeds != NULL)
        (void)memset(coverage->embeds, 0,
                     coverage->nctxs * sizeof(*coverage->embeds));
}

void yew_syn_coverage_free(SynCoverage *coverage)
{
    if (coverage == NULL)
        return;
    free(coverage->contexts);
    free(coverage->rules);
    free(coverage->embeds);
    (void)memset(coverage, 0, sizeof(*coverage));
}

void yew_syn_engine_set_coverage(SynEngine *engine, SynCoverage *coverage)
{
    if (engine == NULL)
        YEW_BUG("syntax: NULL engine");
    if (coverage != NULL &&
        (engine->def == NULL || coverage->nctxs != engine->def->nctxs ||
         coverage->nrules != engine->def->nrules))
        YEW_BUG("syntax: coverage table does not match definition");
    engine->coverage = coverage;
}

static void coverage_context(SynEngine *engine, u16 ctx)
{
    if (engine->coverage != NULL && ctx < engine->coverage->nctxs)
        engine->coverage->contexts[ctx]++;
}

static void coverage_rule(SynEngine *engine, u32 rule)
{
    if (engine->coverage != NULL && rule < engine->coverage->nrules)
        engine->coverage->rules[rule]++;
}

static void coverage_embed(SynEngine *engine, u16 ctx)
{
    if (engine->coverage != NULL && ctx < engine->coverage->nctxs)
        engine->coverage->embeds[ctx]++;
}

static void coverage_frame(SynEngine *master, const SynFrame *frame)
{
    SynEngine *owner;

    (void)checked_ctx(master, frame, &owner);
    coverage_context(owner, frame->ctx);
}

static void coverage_transition(SynEngine *master, const SynState *before,
                                const SynState *after)
{
    u8 i;

    if (before == NULL || after == NULL || after->depth == 0U)
        return;
    if (after->depth > before->depth) {
        for (i = before->depth; i < after->depth; i++)
            coverage_frame(master, &after->f[i]);
    } else if (after->depth != before->depth ||
               after->f[after->depth - 1U].ctx !=
                   before->f[before->depth - 1U].ctx ||
               after->f[after->depth - 1U].def !=
                   before->f[before->depth - 1U].def) {
        coverage_frame(master, &after->f[after->depth - 1U]);
    }
}

static bool bitset_has(const u8 bits[32], u8 byte)
{
    return (bits[byte >> 3U] & (u8)(1U << (byte & 7U))) != 0U;
}

static void emit_span(SynLineOut *out, u32 start, u32 len, u8 attr,
                      u8 flags)
{
    if (out->spans == NULL)
        return;
    while (len != 0U) {
        u16 take = len > UINT16_MAX ? UINT16_MAX : (u16)len;
        if (out->n != 0U) {
            SynSpan *last = &out->spans[out->n - 1U];
            if (last->attr == attr && last->flags == flags &&
                last->start + last->len == start &&
                (u32)last->len + take <= UINT16_MAX) {
                last->len = (u16)(last->len + take);
                start += take;
                len -= take;
                continue;
            }
        }
        if (out->n >= out->cap || out->n >= YEW_SYN_MAX_SPANS) {
            if (out->n != 0U)
                out->spans[out->n - 1U].flags |= YEW_SPAN_TRUNCATED;
            if (out->stop == YEW_SYN_STOP_OK)
                out->stop = YEW_SYN_STOP_SPANS;
            return;
        }
        out->spans[out->n++] = (SynSpan){start, take, attr, flags};
        start += take;
        len -= take;
    }
}

static u32 next_boundary(const u8 *line, u32 len, u32 at)
{
    u32 next = at < len ? at + 1U : at;
    while (next < len && (line[next] & 0xC0U) == 0x80U)
        next++;
    return next;
}

static bool bytes_equal(const u8 *a, size_t an, const char *b, size_t bn)
{
    return an == bn && (an == 0U || memcmp(a, b, an) == 0);
}

static bool aux_match(const SynEngine *engine, const SynState *state,
                      u8 aux_level,
                      const SynRule *rule, const u8 *line, u32 len, u32 at,
                      YewReMatch *match)
{
    static const u8 empty[] = "";
    const char *aux;
    size_t aux_len;
    u32 lo = 0U;

    if (rule->aux_match == SYN_AUXM_INDENT_LT) {
        u64 p = 0U;
        u64 indent = 0U;
        if (at != 0U)
            return false;
        while (p < len) {
            if (line[p] == ' ') {
                indent++;
            } else if (line[p] == '\t') {
                indent = (indent + 8U) & ~UINT64_C(7);
            } else {
                break;
            }
            p++;
        }
        /* Blank lines are content in indentation-delimited constructs such
         * as YAML block scalars; only a nonblank dedent closes the context. */
        if (p == len)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return indent < state->aux[aux_level];
    }
    if (rule->aux_match == SYN_AUXM_LINE_EMPTY) {
        if (at != 0U || len != 0U)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return true;
    }
    if (rule->aux_match == SYN_AUXM_LINE_START) {
        if (at != 0U)
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){0U, 0U};
        return true;
    }
    if (engine->def->aux == NULL || state->aux[aux_level] == 0U)
        return false;
    aux = yew_intern_str(engine->def->aux, state->aux[aux_level]);
    aux_len = yew_intern_len(engine->def->aux, state->aux[aux_level]);
    if (aux == NULL)
        return false;
    match->ngroups = 1U;
    switch ((SynAuxMatch)rule->aux_match) {
    case SYN_AUXM_LINE_EQ:
    case SYN_AUXM_LINE_EQ_WS: {
        const u8 *text;
        if (rule->aux_match == SYN_AUXM_LINE_EQ_WS) {
            while (lo < len && (line[lo] == ' ' || line[lo] == '\t'))
                lo++;
        } else if ((state->flags & YEW_SYN_F_STRIP) != 0U) {
            while (lo < len && line[lo] == '\t')
                lo++;
        }
        text = lo == 0U ? (line == NULL ? empty : line) : line + lo;
        if (at != 0U || !bytes_equal(text, len - lo, aux, aux_len))
            return false;
        match->g[0] = (Span){0U, len};
        return true;
    }
    case SYN_AUXM_LITERAL: {
        const char *pre = yew_intern_str(engine->def->aux, rule->aux_pre);
        const char *post = yew_intern_str(engine->def->aux, rule->aux_post);
        size_t pre_len = yew_intern_len(engine->def->aux, rule->aux_pre);
        size_t post_len = yew_intern_len(engine->def->aux, rule->aux_post);
        u64 total = (u64)pre_len + aux_len + post_len;
        if (total > len - at ||
            (pre_len != 0U && memcmp(line + at, pre, pre_len) != 0) ||
            (aux_len != 0U &&
             memcmp(line + at + pre_len, aux, aux_len) != 0) ||
            (post_len != 0U &&
             memcmp(line + at + pre_len + aux_len, post, post_len) != 0))
            return false;
        match->g[0] = (Span){at, at + total};
        return true;
    }
    case SYN_AUXM_FENCE_CLOSE: {
        u32 p = 0U;
        u32 markers = 0U;
        while (p < len && p < 4U && line[p] == ' ')
            p++;
        if (p > 3U || at != 0U || aux_len == 0U)
            return false;
        while (p + markers < len && line[p + markers] == (u8)aux[0])
            markers++;
        if (markers < aux_len)
            return false;
        while (p + markers < len &&
               (line[p + markers] == ' ' || line[p + markers] == '\t'))
            markers++;
        if (p + markers != len)
            return false;
        match->g[0] = (Span){0U, len};
        return true;
    }
    case SYN_AUXM_INDENT_LT:
    case SYN_AUXM_LINE_EMPTY:
    case SYN_AUXM_LINE_START:
        return false;
    case SYN_AUXM_NONE:
        break;
    }
    return false;
}

static bool ascii_word(u8 byte)
{
    return byte == (u8)'_' || (byte >= (u8)'0' && byte <= (u8)'9') ||
           (byte >= (u8)'A' && byte <= (u8)'Z') ||
           (byte >= (u8)'a' && byte <= (u8)'z');
}

static void word_probe_init(SynWordProbe *probe, const u8 *line, u32 len,
                            u32 at)
{
    u32 p;

    if (at >= len) {
        probe->status = SYN_WORD_PROBE_NO_MATCH;
        return;
    }
    if ((at != 0U && line[at - 1U] >= 0x80U) || line[at] >= 0x80U) {
        probe->status = SYN_WORD_PROBE_FALLBACK;
        return;
    }
    if ((at != 0U && ascii_word(line[at - 1U])) ||
        !ascii_word(line[at])) {
        probe->status = SYN_WORD_PROBE_NO_MATCH;
        return;
    }
    probe->exact_hash = UINT64_C(14695981039346656037);
    probe->folded_hash = UINT64_C(14695981039346656037);
    p = at;
    while (p < len && line[p] < 0x80U && ascii_word(line[p])) {
        probe->exact_hash ^= line[p];
        probe->exact_hash *= UINT64_C(1099511628211);
        probe->folded_hash ^= ascii_fold(line[p]);
        probe->folded_hash *= UINT64_C(1099511628211);
        p++;
    }
    if (p < len && line[p] >= 0x80U) {
        probe->status = SYN_WORD_PROBE_FALLBACK;
        return;
    }
    if (probe->exact_hash == 0U)
        probe->exact_hash = 1U;
    if (probe->folded_hash == 0U)
        probe->folded_hash = 1U;
    probe->hi = p;
    probe->status = SYN_WORD_PROBE_READY;
}

static void word_probe_init_folded(SynWordProbe *probe, const u8 *line,
                                   u32 len, u32 at)
{
    u32 p;

    if (at >= len) {
        probe->status = SYN_WORD_PROBE_NO_MATCH;
        return;
    }
    if ((at != 0U && line[at - 1U] >= 0x80U) || line[at] >= 0x80U) {
        probe->status = SYN_WORD_PROBE_FALLBACK;
        return;
    }
    if ((at != 0U && ascii_word(line[at - 1U])) ||
        !ascii_word(line[at])) {
        probe->status = SYN_WORD_PROBE_NO_MATCH;
        return;
    }
    probe->folded_hash = UINT64_C(14695981039346656037);
    p = at;
    while (p < len && line[p] < 0x80U && ascii_word(line[p])) {
        probe->folded_hash ^= ascii_fold(line[p]);
        probe->folded_hash *= UINT64_C(1099511628211);
        p++;
    }
    if (p < len && line[p] >= 0x80U) {
        probe->status = SYN_WORD_PROBE_FALLBACK;
        return;
    }
    if (probe->folded_hash == 0U)
        probe->folded_hash = 1U;
    probe->hi = p;
    probe->status = SYN_WORD_PROBE_READY;
}

static int word_literal_match(const SynWordSet *set, const YewRe *re,
                              const u8 *line, u32 len, u32 at,
                              SynWordProbe *probe, YewReMatch *match)
{
    u64 hash;
    u32 slot;

    if (set == NULL || set->cap == 0U)
        return -1;
    if (probe->status == SYN_WORD_PROBE_UNSET)
        word_probe_init(probe, line, len, at);
    if (probe->status == SYN_WORD_PROBE_NO_MATCH)
        return 0;
    if (probe->status == SYN_WORD_PROBE_FALLBACK)
        return -1;
    hash = set->fold_ascii ? probe->folded_hash : probe->exact_hash;
    slot = (u32)hash & (set->cap - 1U);
    while (set->slots[slot].hash != 0U) {
        const SynWordSlot *entry = &set->slots[slot];

        if (entry->hash == hash && entry->len == probe->hi - at) {
            u32 i;

            for (i = 0U; i < entry->len; i++) {
                u8 byte = set->fold_ascii ? ascii_fold(line[at + i]) :
                                            line[at + i];

                if (set->bytes[entry->off + i] != byte)
                    break;
            }
            if (i == entry->len) {
                u32 group;

                match->ngroups = re->ngroups;
                for (group = 0U; group < re->ngroups; group++)
                    match->g[group] = (Span){at, probe->hi};
                return 1;
            }
        }
        slot = (slot + 1U) & (set->cap - 1U);
    }
    return 0;
}

static u32 fortran_word_winner(const SynEngine *engine, const u8 *line,
                               u32 at, const SynWordProbe *probe)
{
    u32 slot;

    if (!engine->fortran_fast_enabled ||
        engine->fortran_words_cap == 0U ||
        probe->status != SYN_WORD_PROBE_READY)
        return UINT32_MAX;
    slot = (u32)probe->folded_hash & (engine->fortran_words_cap - 1U);
    while (engine->fortran_words[slot].hash != 0U) {
        const SynFortranWordSlot *entry = &engine->fortran_words[slot];

        if (entry->hash == probe->folded_hash &&
            entry->len == probe->hi - at) {
            const SynWordSet *set = &engine->word_sets[entry->rule];
            u32 i;

            for (i = 0U; i < entry->len; i++) {
                if (set->bytes[entry->off + i] !=
                    ascii_fold(line[at + i]))
                    break;
            }
            if (i == entry->len)
                return entry->rule;
        }
        slot = (slot + 1U) & (engine->fortran_words_cap - 1U);
    }
    return UINT32_MAX;
}

static int identifier_suffix_match(const YewRe *re, u8 suffix,
                                   const u8 *line, u32 len, u32 at,
                                   YewReMatch *match)
{
    u32 p;
    u32 identifier_hi;

    if (at >= len)
        return 0;
    if (line[at] >= 0x80U)
        return -1;
    if (!ascii_identifier_start(line[at]))
        return 0;
    p = at + 1U;
    while (p < len && line[p] < 0x80U &&
           ascii_identifier_continue(line[p]))
        p++;
    identifier_hi = p;
    if (p < len && line[p] >= 0x80U)
        return -1;
    while (p < len && ascii_space(line[p]))
        p++;
    if (p < len && line[p] >= 0x80U)
        return -1;
    if (p >= len || line[p] != suffix)
        return 0;
    match->ngroups = re->ngroups;
    match->g[0] = (Span){at, p + 1U};
    match->g[1] = (Span){at, identifier_hi};
    return 1;
}

static int ascii_identifier_match(const SynIdentifierSpec *spec,
                                  const u8 *line, u32 len, u32 at,
                                  YewReMatch *match)
{
    u32 p = at;
    u32 i;

    if (spec == NULL || spec->start_kind == SYN_IDENT_CLASS_NONE)
        return -1;
    if (spec->boundary_before) {
        bool before_word;
        bool after_word;

        if ((at != 0U && line[at - 1U] >= 0x80U) ||
            (at < len && line[at] >= 0x80U))
            return -1;
        before_word = at != 0U && ascii_word(line[at - 1U]);
        after_word = at < len && ascii_word(line[at]);
        if (before_word == after_word)
            return 0;
    }
    if (spec->nprefix > len - p)
        return 0;
    for (i = 0U; i < spec->nprefix; i++) {
        if (line[p + i] != spec->prefix[i])
            return 0;
    }
    p += spec->nprefix;
    if (p >= len || line[p] >= 0x80U ||
        !identifier_kind_has(spec->start_kind, line[p]))
        return 0;
    p++;
    while (p < len && line[p] < 0x80U &&
           identifier_kind_has(spec->continue_kind, line[p]))
        p++;
    if (spec->boundary_after) {
        if (p < len && line[p] >= 0x80U)
            return -1;
        if (p < len && ascii_word(line[p]))
            return 0;
    }
    match->ngroups = 1U;
    match->g[0] = (Span){at, p};
    return 1;
}

static bool finite_literal_match(const SynFiniteSet *set, const u8 *line,
                                 u32 len, u32 at, YewReMatch *match)
{
    u32 i;

    for (i = 0U; i < set->count; i++) {
        const SynFiniteLit *lit = &set->lits[i];

        if (lit->len > len - at ||
            line[at] != set->bytes[lit->off] ||
            (lit->len > 1U &&
             memcmp(line + at + 1U, set->bytes + lit->off + 1U,
                    lit->len - 1U) != 0U))
            continue;
        match->ngroups = set->ngroups;
        match->g[0] = (Span){at, at + lit->len};
        if (set->ngroups == 2U) {
            if (lit->cap_lo == UINT16_MAX) {
                match->g[1] = (Span){UINT64_MAX, UINT64_MAX};
            } else {
                match->g[1] =
                    (Span){at + lit->cap_lo, at + lit->cap_hi};
            }
        }
        return true;
    }
    return false;
}

static int yaml_block_key_match(const YewRe *re, const u8 *line, u32 len,
                                u32 at, YewReMatch *match)
{
    u32 key_lo;
    u32 colon;
    u32 hi;

    if (at != 0U)
        return 0;
    key_lo = 0U;
    while (key_lo < len && line[key_lo] < 0x80U &&
           ascii_space(line[key_lo]))
        key_lo++;
    if (key_lo < len && line[key_lo] >= 0x80U)
        return -1;
    if (key_lo == len || !ascii_yaml_key_start(line[key_lo]))
        return 0;
    colon = key_lo + 1U;
    while (colon < len && line[colon] < 0x80U &&
           ascii_yaml_key_continue(line[colon]))
        colon++;
    if (colon < len && line[colon] >= 0x80U)
        return -1;
    if (colon == len || line[colon] != (u8)':')
        return 0;
    hi = colon + 1U;
    if (hi < len) {
        if (line[hi] >= 0x80U)
            return -1;
        if (!ascii_space(line[hi]))
            return 0;
        hi++;
    }
    match->ngroups = re->ngroups;
    match->g[0] = (Span){0U, hi};
    match->g[1] = (Span){0U, key_lo};
    match->g[2] = (Span){key_lo, colon};
    match->g[3] = (Span){colon, colon + 1U};
    match->g[4] = (Span){colon + 1U, hi};
    return 1;
}

static bool yaml_block_key_predecessors_cannot_match(const u8 *line,
                                                     u32 len,
                                                     const YewReMatch *match)
{
    u32 key = (u32)match->g[2].lo;
    u32 value = (u32)match->g[3].hi;

    if (key >= len || line[key] == (u8)'%' || line[key] == (u8)'-' ||
        line[key] == (u8)'.')
        return false;
    while (value < len &&
           (line[value] == (u8)' ' || line[value] == (u8)'\t'))
        value++;
    return value == len ||
           (line[value] != (u8)'|' && line[value] != (u8)'>');
}

static int json_key_match(const u8 *line, u32 len, u32 at,
                          YewReMatch *match)
{
    u32 close;
    u32 last_hi = UINT32_MAX;
    u32 last_lo = UINT32_MAX;
    u32 p;

    if (at >= len || line[at] != (u8)'"')
        return 0;
    p = at + 1U;
    for (;;) {
        u8 byte;

        if (p >= len)
            return 0;
        byte = line[p];
        if (byte >= 0x80U)
            return -1;
        if (byte < 0x20U)
            return 0;
        if (byte == (u8)'"') {
            close = p + 1U;
            break;
        }
        if (byte != (u8)'\\') {
            last_lo = p;
            p++;
            last_hi = p;
            continue;
        }
        if (p + 1U >= len)
            return 0;
        byte = line[p + 1U];
        if (byte >= 0x80U)
            return -1;
        if (ascii_json_escape(byte)) {
            last_lo = p;
            p += 2U;
            last_hi = p;
            continue;
        }
        if (byte != (u8)'u' || p + 6U > len)
            return 0;
        if (line[p + 2U] >= 0x80U || line[p + 3U] >= 0x80U ||
            line[p + 4U] >= 0x80U || line[p + 5U] >= 0x80U)
            return -1;
        if (!ascii_hex(line[p + 2U]) || !ascii_hex(line[p + 3U]) ||
            !ascii_hex(line[p + 4U]) || !ascii_hex(line[p + 5U]))
            return 0;
        last_lo = p;
        p += 6U;
        last_hi = p;
    }
    p = close;
    while (p < len && line[p] < 0x80U && ascii_space(line[p]))
        p++;
    if (p < len && line[p] >= 0x80U)
        return -1;
    if (p >= len || line[p] != (u8)':')
        return 0;
    match->ngroups = 3U;
    match->g[0] = (Span){at, p + 1U};
    match->g[1] = (Span){at, close};
    match->g[2] = last_lo == UINT32_MAX ?
        (Span){UINT64_MAX, UINT64_MAX} : (Span){last_lo, last_hi};
    return 1;
}

static bool rule_match(SynEngine *engine, const SynState *state,
                       u8 aux_level,
                       const SynRule *rule, u32 rule_index, const u8 *line,
                       u32 len, u32 at, SynWordProbe *word_probe,
                       YewReMatch *match)
{
    if (rule->aux_match != SYN_AUXM_NONE)
        return aux_match(engine, state, aux_level, rule, line, len, at,
                         match);
    if (rule->re == NULL)
        return false;
    /* min_len is in codepoints.  A remaining byte count smaller than that
     * can never satisfy the rule, even for malformed or multibyte input. */
    if (rule->re->min_len > len - at)
        return false;
    if (engine->yaml_block_key_fast_enabled &&
        engine->rule_yaml_block_key != NULL &&
        engine->rule_yaml_block_key[rule_index] != 0U) {
        int fast = yaml_block_key_match(rule->re, line, len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->json_key_fast_enabled && engine->rule_json_key != NULL &&
        engine->rule_json_key[rule_index] != 0U) {
        int fast = json_key_match(line, len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->identifier_fast_enabled &&
        engine->rule_identifier_suffix != NULL &&
        engine->rule_identifier_suffix[rule_index] != 0U) {
        int fast = identifier_suffix_match(
            rule->re, engine->rule_identifier_suffix[rule_index], line,
            len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->identifier_fast_enabled &&
        engine->rule_identifiers != NULL &&
        engine->rule_identifiers[rule_index].start_kind !=
            SYN_IDENT_CLASS_NONE) {
        int fast = ascii_identifier_match(
            &engine->rule_identifiers[rule_index], line, len, at, match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->fortran_fast_enabled &&
        engine->rule_fortran_word != NULL &&
        engine->rule_fortran_word[rule_index] == 2U) {
        int fast = word_literal_match(&engine->word_sets[rule_index],
                                      rule->re, line, len, at, word_probe,
                                      match);

        if (fast > 0)
            return true;
    }
    if (engine->word_literal_fast_enabled &&
        engine->rule_word_literal != NULL &&
        engine->rule_word_literal[rule_index] != 0U) {
        int fast = word_literal_match(&engine->word_sets[rule_index],
                                      rule->re, line, len, at, word_probe,
                                      match);

        if (fast >= 0)
            return fast != 0;
    }
    if (engine->finite_literal_fast_enabled &&
        engine->finite_sets != NULL &&
        engine->finite_sets[rule_index].count != 0U)
        return finite_literal_match(&engine->finite_sets[rule_index], line,
                                    len, at, match);
    if (rule->re->lit.kind == RE_LIT_WHOLE &&
        rule->re->ngroups == 1U &&
        (rule->re->flags & YEW_RE_ICASE) == 0U) {
        if (rule->re->lit.n > len - at ||
            (rule->re->lit.n != 0U &&
             memcmp(line + at, rule->re->lit.s, rule->re->lit.n) != 0))
            return false;
        match->ngroups = 1U;
        match->g[0] = (Span){at, at + rule->re->lit.n};
        return true;
    }
    if (rule->re->lit.kind != RE_LIT_NONE &&
        (rule->re->flags & YEW_RE_ICASE) == 0U &&
        (rule->re->lit.n > len - at ||
         memcmp(line + at, rule->re->lit.s, rule->re->lit.n) != 0))
        return false;
    return yew_re_match_at_ws(&engine->re_workspace, rule->re,
                              &(YewReInput){NULL, line, len, {0U, len}},
                              BYTEOFF(at), match);
}

static void emit_match(SynLineOut *out, const SynRule *rule,
                       const YewReMatch *match, u32 consume_hi)
{
    u32 p = (u32)match->g[0].lo;
    u32 hi = consume_hi;

    while (p < hi) {
        u8 attr = rule->caps[0] == UINT8_MAX ? rule->attr : rule->caps[0];
        u32 end = hi;
        u32 g;

        for (g = 1U; g < 8U && g < match->ngroups; g++) {
            Span cap = match->g[g];
            if (rule->caps[g] == UINT8_MAX || cap.lo == UINT64_MAX ||
                cap.hi < cap.lo)
                continue;
            if (cap.lo <= p && p < cap.hi) {
                attr = rule->caps[g];
                if (cap.hi < end)
                    end = (u32)cap.hi;
            } else if (p < cap.lo && cap.lo < end) {
                end = (u32)cap.lo;
            }
        }
        if (end <= p)
            end = p + 1U;
        emit_span(out, p, end - p, attr, 0U);
        p = end;
    }
}

static void emit_yaml_block_key(SynLineOut *out, const SynRule *rule,
                                const YewReMatch *match)
{
    u8 base = rule->caps[0] == UINT8_MAX ? rule->attr : rule->caps[0];
    u8 indent = rule->caps[1] == UINT8_MAX ? base : rule->caps[1];
    u8 key = rule->caps[2] == UINT8_MAX ? base : rule->caps[2];
    u8 colon = rule->caps[3] == UINT8_MAX ? base : rule->caps[3];

    emit_span(out, (u32)match->g[1].lo,
              (u32)(match->g[1].hi - match->g[1].lo), indent, 0U);
    emit_span(out, (u32)match->g[2].lo,
              (u32)(match->g[2].hi - match->g[2].lo), key, 0U);
    emit_span(out, (u32)match->g[3].lo,
              (u32)(match->g[3].hi - match->g[3].lo), colon, 0U);
}

static bool set_aux(SynEngine *engine, SynState *state, u8 aux_level,
                    const SynRule *rule,
                    const u8 *line, u32 len, const YewReMatch *match)
{
    Span cap;
    u64 lo;
    u64 hi;

    if ((rule->flags & YEW_SYN_RULE_SET_AUX) == 0U)
        return true;
    if (((rule->flags & YEW_SYN_RULE_AUX_INT) == 0U &&
         engine->def->aux == NULL) || rule->aux_group >= match->ngroups)
        YEW_BUG("syntax: SET_AUX rule has no usable capture/interner");
    cap = match->g[rule->aux_group];
    if (cap.lo == UINT64_MAX || cap.hi == UINT64_MAX)
        return false;
    lo = cap.lo;
    hi = cap.hi;
    if (hi < lo || hi > len || hi - lo > SIZE_MAX)
        YEW_BUG("syntax: invalid aux capture");
    if ((rule->flags & YEW_SYN_RULE_STRIP) != 0U) {
        while (lo < hi && (line[lo] == ' ' || line[lo] == '\t'))
            lo++;
        while (hi > lo && (line[hi - 1U] == ' ' || line[hi - 1U] == '\t'))
            hi--;
    }
    if ((rule->flags & YEW_SYN_RULE_AUX_INT) != 0U) {
        u32 value = 0U;
        u32 add = rule->aux_add;
        u64 p;
        bool decimal = lo < hi;
        for (p = lo; p < hi; p++) {
            if (line[p] < '0' || line[p] > '9') {
                decimal = false;
                break;
            }
            if (value <= (UINT32_MAX - 9U) / 10U)
                value = value * 10U + (u32)(line[p] - '0');
        }
        if (!decimal) {
            value = 0U;
            for (p = lo; p < hi; p++) {
                if (line[p] == ' ')
                    value++;
                else if (line[p] == '\t')
                    value = (value + 8U) & ~7U;
            }
        }
        if (rule->aux_add_group != 0U &&
            rule->aux_add_group < match->ngroups) {
            Span add_cap = match->g[rule->aux_add_group];

            if (add_cap.lo != UINT64_MAX && add_cap.hi == add_cap.lo + 1U &&
                add_cap.hi <= len && line[add_cap.lo] >= '0' &&
                line[add_cap.lo] <= '9')
                add = (u32)(line[add_cap.lo] - '0');
        }
        if (value > UINT32_MAX - add)
            value = UINT32_MAX;
        else
            value += add;
        state->aux[aux_level] = value;
    } else {
        static const char empty[] = "";
        const char *bytes = hi == lo && line == NULL
                                ? empty
                                : (const char *)line + lo;
        state->aux[aux_level] =
            yew_intern(engine->def->aux, bytes, (size_t)(hi - lo));
    }
    return true;
}

static SynEngine *runtime_at(SynEngine *master, u8 def_index)
{
    SynEngine *runtime;

    if (master == NULL || def_index >= master->ndefs ||
        master->defs[def_index].runtime == NULL)
        YEW_BUG("syntax: nonresident definition index %u",
                (unsigned)def_index);
    runtime = master->defs[def_index].runtime;
    if (runtime->def == NULL || runtime->def->ctxs == NULL)
        YEW_BUG("syntax: resident definition has no context table");
    return runtime;
}

static const SynCtx *checked_ctx(SynEngine *master, const SynFrame *frame,
                                 SynEngine **runtime)
{
    SynEngine *active = runtime_at(master, frame->def);

    if (frame->ctx >= active->def->nctxs)
        YEW_BUG("syntax: context %u outside definition %u",
                (unsigned)frame->ctx, (unsigned)frame->def);
    if (runtime != NULL)
        *runtime = active;
    return &active->def->ctxs[frame->ctx];
}

static u16 runtime_ctx_named(const SynEngine *runtime, const char *name,
                             size_t len)
{
    u16 i;

    if (runtime == NULL || runtime->def == NULL)
        return UINT16_MAX;
    for (i = 0U; i < runtime->def->nctxs; i++) {
        const char *candidate = runtime->ctx_names == NULL ? NULL :
                                runtime->ctx_names[i];

        if (candidate != NULL && strlen(candidate) == len &&
            (len == 0U || memcmp(candidate, name, len) == 0))
            return i;
    }
    return UINT16_MAX;
}

static u8 resident_slot(const SynEngine *master, u32 lang)
{
    u16 i;

    for (i = 0U; i < master->ndefs; i++) {
        if (master->defs[i].lang == lang &&
            master->defs[i].runtime != NULL)
            return (u8)i;
    }
    return UINT8_MAX;
}

static void lost_add(SynState *state, u8 count)
{
    while (count-- != 0U && state->lost < YEW_SYN_LOST_MAX)
        state->lost++;
    if (state->lost == YEW_SYN_LOST_MAX)
        state->flags |= YEW_SYN_F_DEGRADED;
}

static bool embed_language(const SynEngine *master, const SynEngine *host,
                           u8 host_slot,
                           const SynEmbed *embed, const u8 *line, u32 len,
                           const YewReMatch *match, u32 *lang)
{
    const char *name = NULL;
    size_t name_len = 0U;

    if (embed->lang_kind == SYN_EMBED_LANG_SELF) {
        *lang = host_slot < master->ndefs ? master->defs[host_slot].lang :
                                            YEW_LANG_NONE;
        return true;
    }
    if (embed->lang_kind == SYN_EMBED_LANG_LITERAL && host->def->aux != NULL) {
        name = yew_intern_str(host->def->aux, embed->lang);
        name_len = yew_intern_len(host->def->aux, embed->lang);
    } else if (embed->lang_kind == SYN_EMBED_LANG_CAPTURE &&
               embed->lang_group < match->ngroups) {
        Span cap = match->g[embed->lang_group];

        if (cap.lo != UINT64_MAX && cap.hi >= cap.lo && cap.hi <= len) {
            name = (const char *)line + cap.lo;
            name_len = (size_t)(cap.hi - cap.lo);
        }
    }
    if (name == NULL || name_len > UINT32_MAX) {
        *lang = YEW_LANG_NONE;
        return false;
    }
    *lang = engine_lang_by_name(master, name, name_len);
    return *lang != YEW_LANG_NONE;
}

static u16 embed_guest_ctx(const SynEngine *host, const SynEngine *guest,
                           const SynEmbed *embed)
{
    const char *name;
    size_t len;

    if (embed->ctx == 0U)
        return guest->def->root;
    if (host->def->aux == NULL)
        return UINT16_MAX;
    name = yew_intern_str(host->def->aux, embed->ctx);
    len = yew_intern_len(host->def->aux, embed->ctx);
    return name == NULL ? UINT16_MAX : runtime_ctx_named(guest, name, len);
}

static bool push_guest(SynEngine *master, SynState *state, u8 slot,
                       const SynEmbed *embed, SynEngine *host)
{
    SynEngine *guest = runtime_at(master, slot);
    u16 ctx = embed_guest_ctx(host, guest, embed);

    if (ctx == UINT16_MAX || state->depth >= YEW_SYN_DEPTH_MAX ||
        state->ndef >= YEW_SYN_DEF_MAX)
        return false;
    state->f[state->depth++] = (SynFrame){ctx, slot, 0U};
    state->aux[state->ndef] = 0U;
    state->ndef++;
    state->flags &= (u8)~YEW_SYN_F_EMBED_PEND;
    return true;
}

static void apply_embed(SynEngine *master, SynEngine *host, SynState *state,
                        const SynRule *rule, const u8 *line, u32 len,
                        const YewReMatch *match, u32 *pending_lang,
                        u32 *refused_lang, const char **unknown_name,
                        size_t *unknown_len)
{
    u8 host_slot = YEW_SYN_DEF_OF(state);
    u32 selected;
    u8 slot = UINT8_MAX;
    bool selected_known;

    selected_known = embed_language(master, host, host_slot, &rule->embed,
                                    line, len, match, &selected);
    if (!selected_known && unknown_name != NULL && unknown_len != NULL) {
        if (rule->embed.lang_kind == SYN_EMBED_LANG_LITERAL &&
            host->def->aux != NULL) {
            *unknown_name = yew_intern_str(host->def->aux,
                                           rule->embed.lang);
            *unknown_len = yew_intern_len(host->def->aux,
                                          rule->embed.lang);
        } else if (rule->embed.lang_kind == SYN_EMBED_LANG_CAPTURE &&
                   rule->embed.lang_group < match->ngroups) {
            Span cap = match->g[rule->embed.lang_group];

            if (cap.lo != UINT64_MAX && cap.hi >= cap.lo && cap.hi <= len) {
                *unknown_name = (const char *)line + cap.lo;
                *unknown_len = (size_t)(cap.hi - cap.lo);
            }
        }
    }
    if (state->depth >= YEW_SYN_DEPTH_MAX) {
        /* There is no slot in which to retain the bridge boundary.  A
         * synthetic lost debt would later consume an unrelated host pop,
         * so leave the stack untouched and expose the refusal flag only. */
        if ((state->flags & YEW_SYN_F_EMBED_LOST) == 0U)
            lost_add(state, 2U);
        state->flags |= YEW_SYN_F_EMBED_LOST | YEW_SYN_F_EMBED_ORPHAN;
        if (selected_known && refused_lang != NULL)
            *refused_lang = selected;
        return;
    }
    state->f[state->depth++] = (SynFrame){
        rule->target, host_slot,
        (u8)(YEW_SYN_FR_BRIDGE |
             ((rule->embed.flags & YEW_SYN_EMBED_DEFER) != 0U ?
              YEW_SYN_FR_DEFER : 0U))
    };
    if (!selected_known)
        return;
    if (rule->embed.lang_kind == SYN_EMBED_LANG_SELF)
        slot = host_slot;
    else
        slot = resident_slot(master, selected);
    if (state->ndef >= YEW_SYN_DEF_MAX ||
        state->depth >= YEW_SYN_DEPTH_MAX) {
        lost_add(state, 2U);
        state->flags |= YEW_SYN_F_EMBED_LOST;
        if (refused_lang != NULL)
            *refused_lang = selected;
        return;
    }
    if (slot == UINT8_MAX) {
        if (master->ndefs >= YEW_SYN_RESIDENT_MAX) {
            lost_add(state, 2U);
            state->flags |= YEW_SYN_F_EMBED_LOST;
            if (refused_lang != NULL)
                *refused_lang = selected;
            return;
        }
        state->aux[state->ndef] = selected;
        state->flags |= YEW_SYN_F_EMBED_PEND;
        if (pending_lang != NULL && *pending_lang == YEW_LANG_NONE)
            *pending_lang = selected;
        return;
    }
    if ((rule->embed.flags & YEW_SYN_EMBED_DEFER) != 0U) {
        state->aux[state->ndef] = (u32)slot + 1U;
        return;
    }
    if (!push_guest(master, state, slot, &rule->embed, host)) {
        lost_add(state, 2U);
        state->flags |= YEW_SYN_F_EMBED_LOST;
        if (refused_lang != NULL)
            *refused_lang = selected;
    }
}

static void apply_op(SynState *state, u8 op, u8 nop, u16 target)
{
    switch ((SynOp)op) {
    case SYN_OP_STAY:
        break;
    case SYN_OP_PUSH:
        yew_syn_state_push(state, target);
        break;
    case SYN_OP_POP:
        yew_syn_state_pop(state, nop == 0U ? 1U : nop);
        break;
    case SYN_OP_SET:
        yew_syn_state_set(state, target);
        break;
    case SYN_OP_EMBED:
        YEW_BUG("syntax: embed operation requires opener metadata");
        break;
    default:
        YEW_BUG("syntax: invalid rule operation %u", (unsigned)op);
    }
}

static void apply_rule_op(SynEngine *master, SynEngine *active,
                          SynState *state, const SynRule *rule,
                          const u8 *line, u32 len,
                          const YewReMatch *match, u32 *pending_lang,
                          u32 *refused_lang, const char **unknown_name,
                          size_t *unknown_len)
{
    u8 depth = state->depth;
    if (rule->op == SYN_OP_EMBED) {
        apply_embed(master, active, state, rule, line, len, match,
                    pending_lang, refused_lang, unknown_name, unknown_len);
    } else if (rule->op == SYN_OP_PUSH && rule->npush != 0U) {
        u8 i;
        if (rule->npush > 4U)
            YEW_BUG("syntax: rule push list exceeds four contexts");
        for (i = 0U; i < rule->npush; i++)
            yew_syn_state_push(state, rule->push[i]);
    } else {
        apply_op(state, rule->op, rule->nop, rule->target);
    }
    /* A refused over-depth push is repaid through lost before the real
     * frame pops.  Its delimiter belongs to the still-live outer frame,
     * so clearing aux during that repayment would corrupt the matcher. */
    if ((rule->flags & YEW_SYN_RULE_CLR_AUX) != 0U &&
        state->depth < depth) {
        YEW_SYN_AUX_OF(state) = 0U;
        state->flags &= (u8)~YEW_SYN_F_STRIP;
    }
}

static void apply_empty_bol(SynEngine *master, SynState *state,
                            const u8 *line, bool instrument,
                            u32 *pending_lang, u32 *refused_lang,
                            const char **unknown_name, size_t *unknown_len)
{
    u32 guard = 0U;
    while (guard++ <= YEW_SYN_DEPTH_MAX) {
        SynEngine *engine;
        u16 ctx_id = state->f[state->depth - 1U].ctx;
        const SynCtx *ctx = checked_ctx(master,
                                        &state->f[state->depth - 1U],
                                        &engine);
        const SynRule *matched = NULL;
        u32 matched_index = UINT32_MAX;
        YewReMatch match;
        SynWordProbe word_probe = {0};
        u32 i;
        if (engine->ctx_aux == NULL || engine->ctx_aux[ctx_id] == 0U)
            return;
        for (i = 0U; i < ctx->nrules; i++) {
            u32 index = ctx->first_rule + i;
            const SynRule *rule;
            if (index >= engine->def->nrules)
                return;
            rule = &engine->def->rules[index];
            if ((state->flags & YEW_SYN_F_PAST_FIRST) != 0U &&
                (rule->flags & YEW_SYN_RULE_FIRST_LINE) != 0U)
                continue;
            if ((rule->value_pred == SYN_VALUE_SET &&
                 (state->flags & YEW_SYN_F_VALUE) == 0U) ||
                (rule->value_pred == SYN_VALUE_CLEAR &&
                 (state->flags & YEW_SYN_F_VALUE) != 0U))
                continue;
            if (rule_match(engine, state, state->ndef - 1U, rule, index,
                           line, 0U, 0U,
                           &word_probe, &match) &&
                match.g[0].hi == 0U) {
                matched = rule;
                matched_index = index;
                break;
            }
        }
        if (matched == NULL)
            return;
        {
            SynState before = *state;

            if (instrument)
                coverage_rule(engine, matched_index);
            if (instrument && matched->op == SYN_OP_EMBED)
                coverage_embed(engine, matched->target);
            (void)set_aux(engine, state, state->ndef - 1U, matched, line,
                          0U, &match);
            apply_rule_op(master, engine, state, matched, line, 0U, &match,
                          pending_lang, refused_lang, unknown_name,
                          unknown_len);
            if (instrument)
                coverage_transition(master, &before, state);
        }
        if ((matched->flags & YEW_SYN_RULE_ZERO_TRANSITION) == 0U ||
            matched->op == SYN_OP_STAY)
            return;
    }
}

static u32 truncated_exit_state(SynEngine *engine, u32 entry_state,
                                const SynState *entry, bool apply_eol)
{
    SynState exit = *entry;

    /* A truncated line deliberately suppresses grammar transitions, but
     * it is still a physical line.  Losing this bit lets line-two-only
     * content satisfy `first_line` rules after an oversized or hostile
     * first line. */
    if (apply_eol) {
        exit.flags &= (u8)~YEW_SYN_F_VALUE;
        if (engine->has_first_line)
            exit.flags |= YEW_SYN_F_PAST_FIRST;
        return yew_syn_state_intern(engine->states, &exit);
    }
    return entry_state;
}

typedef enum SynFortranBolResult {
    SYN_FORTRAN_BOL_FALLBACK,
    SYN_FORTRAN_BOL_NONE,
    SYN_FORTRAN_BOL_MATCH
} SynFortranBolResult;

static void fortran_match_init(YewReMatch *match, u32 ngroups, u32 hi)
{
    u32 i;

    (void)memset(match, 0, sizeof(*match));
    match->ngroups = ngroups;
    for (i = 0U; i < ngroups && i < YEW_RE_MAX_GROUPS; i++)
        match->g[i] = (Span){UINT64_MAX, UINT64_MAX};
    match->g[0] = (Span){0U, hi};
}

static bool fortran_fixed_label(const u8 *line)
{
    bool digit = false;
    u32 i;

    for (i = 0U; i < 5U; i++) {
        if (line[i] >= (u8)'0' && line[i] <= (u8)'9')
            digit = true;
        else if (line[i] != (u8)' ')
            return false;
    }
    return digit;
}

static SynFortranBolResult fortran_fixed_bol(const SynEngine *engine,
                                             const u8 *line, u32 len,
                                             u32 *rule_index,
                                             YewReMatch *match)
{
    const SynCtx *root = &engine->def->ctxs[engine->def->root];
    u32 base = root->first_rule;
    u32 i;

    if (line[0] == (u8)'C' || line[0] == (u8)'c' ||
        line[0] == (u8)'*' || line[0] == (u8)'!') {
        *rule_index = base;
        fortran_match_init(match, 1U, len);
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (line[0] == (u8)'#') {
        *rule_index = base + 1U;
        fortran_match_init(match, 1U, len);
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (len >= 73U) {
        for (i = 0U; i < len; i++) {
            if (line[i] >= 0x80U)
                return SYN_FORTRAN_BOL_FALLBACK;
        }
        *rule_index = base + 2U;
        fortran_match_init(match, 3U, len);
        match->g[1] = (Span){0U, 72U};
        match->g[2] = (Span){72U, len};
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (line[0] == (u8)'\t') {
        if (len >= 2U && line[1] >= (u8)'1' && line[1] <= (u8)'9') {
            *rule_index = base + 3U;
            fortran_match_init(match, 3U, 2U);
            match->g[1] = (Span){0U, 1U};
            match->g[2] = (Span){1U, 2U};
        } else {
            *rule_index = base + 4U;
            fortran_match_init(match, 1U, 1U);
        }
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (len < 6U)
        return SYN_FORTRAN_BOL_NONE;
    for (i = 0U; i < 6U; i++) {
        if (line[i] >= 0x80U)
            return SYN_FORTRAN_BOL_FALLBACK;
    }
    if (fortran_fixed_label(line)) {
        *rule_index = base +
            (line[5] != (u8)' ' && line[5] != (u8)'0' ? 5U : 6U);
        fortran_match_init(match, *rule_index == base + 5U ? 3U : 2U, 6U);
        match->g[1] = (Span){0U, 5U};
        if (*rule_index == base + 5U)
            match->g[2] = (Span){5U, 6U};
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (memcmp(line, "     ", 5U) == 0) {
        *rule_index = base +
            (line[5] != (u8)' ' && line[5] != (u8)'0' ? 7U : 8U);
        fortran_match_init(match, *rule_index == base + 7U ? 3U : 1U, 6U);
        if (*rule_index == base + 7U) {
            match->g[1] = (Span){0U, 5U};
            match->g[2] = (Span){5U, 6U};
        }
        return SYN_FORTRAN_BOL_MATCH;
    }
    return SYN_FORTRAN_BOL_NONE;
}

static SynFortranBolResult fortran_free_bol(const SynEngine *engine,
                                            const u8 *line, u32 len,
                                            u32 *rule_index,
                                            YewReMatch *match)
{
    const SynCtx *root = &engine->def->ctxs[engine->def->root];
    u32 base = root->first_rule;
    u32 p = 0U;
    u32 digits;

    while (p < len && line[p] < 0x80U && ascii_space(line[p]))
        p++;
    if (p < len && line[p] >= 0x80U)
        return SYN_FORTRAN_BOL_FALLBACK;
    if (p < len && line[p] == (u8)'#') {
        *rule_index = base;
        fortran_match_init(match, 1U, len);
        return SYN_FORTRAN_BOL_MATCH;
    }
    if (p < len && line[p] == (u8)'&') {
        *rule_index = base + 1U;
        fortran_match_init(match, 3U, p + 1U);
        match->g[1] = (Span){0U, p};
        match->g[2] = (Span){p, p + 1U};
        return SYN_FORTRAN_BOL_MATCH;
    }
    digits = p;
    while (p < len && p - digits < 5U &&
           line[p] >= (u8)'0' && line[p] <= (u8)'9')
        p++;
    if (p != digits && p < len && line[p] < 0x80U &&
        ascii_space(line[p])) {
        u32 whitespace = p;

        while (p < len && line[p] < 0x80U && ascii_space(line[p]))
            p++;
        if (p < len && line[p] >= 0x80U)
            return SYN_FORTRAN_BOL_FALLBACK;
        *rule_index = base + 2U;
        fortran_match_init(match, 4U, p);
        match->g[1] = (Span){0U, digits};
        match->g[2] = (Span){digits, whitespace};
        match->g[3] = (Span){whitespace, p};
        return SYN_FORTRAN_BOL_MATCH;
    }
    return SYN_FORTRAN_BOL_NONE;
}

static SynFortranBolResult fortran_bol_match(const SynEngine *engine,
                                             const SynState *state,
                                             const u8 *line, u32 len,
                                             u32 *rule_index,
                                             YewReMatch *match)
{
    if (!engine->fortran_fast_enabled ||
        engine->fortran_form == SYN_FORTRAN_NONE || len == 0U ||
        state->f[state->depth - 1U].ctx != engine->def->root)
        return SYN_FORTRAN_BOL_FALLBACK;
    if (engine->fortran_form == SYN_FORTRAN_FIXED)
        return fortran_fixed_bol(engine, line, len, rule_index, match);
    return fortran_free_bol(engine, line, len, rule_index, match);
}

static bool bridge_end_byte_possible(SynEngine *master,
                                     const SynState *state, u32 at, u8 byte)
{
    i32 depth;

    for (depth = (i32)state->depth - 1; depth >= 0; depth--) {
        const SynFrame *frame = &state->f[depth];
        SynEngine *host;
        const SynCtx *ctx;
        const u8 *first;

        if ((frame->fl & YEW_SYN_FR_BRIDGE) == 0U)
            continue;
        ctx = checked_ctx(master, frame, &host);
        if ((ctx->embed.end == SYN_EMBED_END_LINE ||
             ctx->embed.end == SYN_EMBED_END_LINE_CONTINUATION) &&
            at != 0U)
            return false;
        if (ctx->embed.end != SYN_EMBED_END_LINE &&
            ctx->embed.end != SYN_EMBED_END_INLINE &&
            ctx->embed.end != SYN_EMBED_END_INLINE_ROOT &&
            ctx->embed.end != SYN_EMBED_END_LINE_CONTINUATION)
            return false;
        if (ctx->embed.end == SYN_EMBED_END_INLINE_ROOT &&
            depth + 1 < (i32)state->depth &&
            state->depth != (u8)(depth + 2))
            return false;
        first = ctx->embed.end == SYN_EMBED_END_LINE ||
                        ctx->embed.end == SYN_EMBED_END_LINE_CONTINUATION ?
                    host->embed_local_bol_end_first :
                    host->embed_local_end_first;
        return bitset_has(first, byte);
    }
    return false;
}

static bool bridge_end_match(SynEngine *master, const SynState *state,
                             const u8 *line, u32 len, u32 at,
                             SynEngine **host_out, const SynRule **rule_out,
                             u32 *rule_index_out, u8 *bridge_depth_out,
                             u8 *aux_level_out, YewReMatch *match)
{
    i32 depth;

    for (depth = (i32)state->depth - 1; depth >= 0; depth--) {
        const SynFrame *frame = &state->f[depth];
        SynEngine *host;
        const SynCtx *ctx;
        u8 aux_level;
        u32 i;

        if ((frame->fl & YEW_SYN_FR_BRIDGE) == 0U)
            continue;
        ctx = checked_ctx(master, frame, &host);
        if ((ctx->embed.end == SYN_EMBED_END_LINE ||
             ctx->embed.end == SYN_EMBED_END_LINE_CONTINUATION) &&
            at != 0U)
            return false;
        if (ctx->embed.end != SYN_EMBED_END_LINE &&
            ctx->embed.end != SYN_EMBED_END_INLINE &&
            ctx->embed.end != SYN_EMBED_END_INLINE_ROOT &&
            ctx->embed.end != SYN_EMBED_END_LINE_CONTINUATION)
            return false;
        if (ctx->embed.end == SYN_EMBED_END_INLINE_ROOT &&
            depth + 1 < (i32)state->depth &&
            state->depth != (u8)(depth + 2))
            return false;
        aux_level = (u8)(depth + 1 < state->depth ? state->ndef - 2U :
                                                     state->ndef - 1U);
        for (i = 0U; i < ctx->nrules; i++) {
            u32 index = ctx->first_rule + i;
            const SynRule *rule;
            SynWordProbe word_probe = {0};

            if (index >= host->def->nrules)
                YEW_BUG("syntax: bridge rule outside host definition");
            rule = &host->def->rules[index];
            if (rule->end == 0U)
                continue;
            if (at < len && rule->aux_match == SYN_AUXM_NONE &&
                !bitset_has(host->rule_first == NULL ? rule->first :
                            host->rule_first[index], line[at]))
                continue;
            if (rule_match(host, state, aux_level, rule, index, line, len,
                           at, &word_probe, match)) {
                *host_out = host;
                *rule_out = rule;
                *rule_index_out = index;
                *bridge_depth_out = (u8)depth;
                *aux_level_out = aux_level;
                return true;
            }
        }
        return false;
    }
    return false;
}

static bool line_has_continuation(const u8 *line, u32 len)
{
    u32 trailing = 0U;

    while (len > trailing && line[len - trailing - 1U] == (u8)'\\')
        trailing++;
    return (trailing & 1U) != 0U;
}

static void leave_embed(SynState *state, u8 bridge_depth)
{
    bool guest = bridge_depth + 1U < state->depth;

    if (guest) {
        while (state->depth > bridge_depth + 1U) {
            state->depth--;
            (void)memset(&state->f[state->depth], 0,
                         sizeof(state->f[state->depth]));
        }
        state->aux[state->ndef - 1U] = 0U;
        state->ndef--;
    } else if ((state->flags & YEW_SYN_F_EMBED_PEND) != 0U ||
               (state->f[bridge_depth].fl & YEW_SYN_FR_DEFER) != 0U) {
        if (state->ndef < YEW_SYN_DEF_MAX)
            state->aux[state->ndef] = 0U;
    }
    if ((state->flags & YEW_SYN_F_EMBED_LOST) != 0U) {
        state->lost = state->lost > 2U ? (u8)(state->lost - 2U) : 0U;
        state->flags &= (u8)~YEW_SYN_F_EMBED_LOST;
    }
    state->flags &= (u8)~YEW_SYN_F_EMBED_PEND;
}

static const u8 *merged_first_bytes(SynEngine *master,
                                    const SynState *state,
                                    SynEngine *active, const SynCtx *ctx,
                                    bool bol, const u8 **end_first)
{
    i32 depth;
    const SynFrame *bridge_frame = NULL;
    SynEngine *host = NULL;
    const SynCtx *bridge = NULL;
    SynMergedFirst *cached;
    u32 hash;
    u32 byte;
    u32 i;

    if (state->ndef == 1U &&
        (state->f[state->depth - 1U].fl & YEW_SYN_FR_BRIDGE) == 0U) {
        if (end_first != NULL)
            *end_first = NULL;
        return bol || active->ctx_first_nonbol == NULL ? ctx->first :
            active->ctx_first_nonbol[state->f[state->depth - 1U].ctx];
    }

    for (depth = (i32)state->depth - 1; depth >= 0; depth--) {
        const SynFrame *frame = &state->f[depth];

        if ((frame->fl & YEW_SYN_FR_BRIDGE) == 0U)
            continue;
        bridge = checked_ctx(master, frame, &host);
        if (bridge->embed.end == SYN_EMBED_END_INLINE ||
            (bridge->embed.end == SYN_EMBED_END_INLINE_ROOT &&
             (depth + 1 == (i32)state->depth ||
              depth + 2 == (i32)state->depth)))
            bridge_frame = frame;
        else if (bridge->embed.end == SYN_EMBED_END_LINE &&
                 end_first != NULL)
            *end_first = bridge->first;
        break;
    }
    if (bridge_frame == NULL) {
        if (end_first != NULL && (bridge == NULL ||
            bridge->embed.end != SYN_EMBED_END_LINE))
            *end_first = NULL;
        return bol || active->ctx_first_nonbol == NULL ? ctx->first :
            active->ctx_first_nonbol[state->f[state->depth - 1U].ctx];
    }
    hash = (u32)state->f[state->depth - 1U].def * 131U +
           state->f[state->depth - 1U].ctx * 17U +
           (u32)bridge_frame->def * 7U + bridge_frame->ctx * 3U +
           (bol ? 1U : 0U);
    cached = &master->merged_first[hash % YEW_ARRAY_LEN(master->merged_first)];
    if (cached->valid && cached->active_def ==
            state->f[state->depth - 1U].def &&
        cached->active_ctx == state->f[state->depth - 1U].ctx &&
        cached->bridge_def == bridge_frame->def &&
        cached->bridge_ctx == bridge_frame->ctx &&
        cached->bol == (u8)bol)
        goto ready;
    cached->active_def = state->f[state->depth - 1U].def;
    cached->active_ctx = state->f[state->depth - 1U].ctx;
    cached->bridge_def = bridge_frame->def;
    cached->bridge_ctx = bridge_frame->ctx;
    cached->bol = (u8)bol;
    (void)memcpy(cached->bits,
                 bol || active->ctx_first_nonbol == NULL ? ctx->first :
                 active->ctx_first_nonbol[cached->active_ctx],
                 sizeof(cached->bits));
    (void)memset(cached->end_bits, 0, sizeof(cached->end_bits));
    for (i = 0U; i < bridge->nrules; i++) {
        u32 index = bridge->first_rule + i;
        const SynRule *rule = &host->def->rules[index];

        if (rule->end == 0U)
            continue;
        if (rule->aux_match != SYN_AUXM_NONE) {
            (void)memset(cached->bits, 0xff, sizeof(cached->bits));
            (void)memset(cached->end_bits, 0xff,
                         sizeof(cached->end_bits));
            break;
        }
        for (byte = 0U; byte < sizeof(cached->bits); byte++) {
            u8 first = host->rule_first == NULL ? rule->first[byte] :
                                                  host->rule_first[index][byte];
            cached->bits[byte] |= first;
            cached->end_bits[byte] |= first;
        }
    }
    cached->valid = true;
ready:
    if (end_first != NULL)
        *end_first = cached->end_bits;
    return cached->bits;
}

#if defined(YEW_SYN_TEST)
bool yew_syn_engine_test_masks(SynEngine *master, const SynState *state,
                               bool bol, u8 merged[32], u8 bridge[32])
{
    SynEngine *active;
    const SynCtx *ctx;
    const u8 *first;
    u32 byte;

    if (master == NULL || state == NULL || state->depth == 0U ||
        merged == NULL || bridge == NULL)
        return false;
    ctx = checked_ctx(master, &state->f[state->depth - 1U], &active);
    first = merged_first_bytes(master, state, active, ctx, bol, NULL);
    (void)memcpy(merged, first, 32U);
    (void)memset(bridge, 0, 32U);
    for (byte = 0U; byte <= UINT8_MAX; byte++) {
        bool aggregate = bitset_has(master->embed_end_first, (u8)byte) ||
            (bol && bitset_has(master->embed_bol_end_first, (u8)byte));

        if (aggregate && bridge_end_byte_possible(
                             master, state, bol ? 0U : 1U, (u8)byte))
            bridge[byte >> 3U] |= (u8)(1U << (byte & 7U));
    }
    return true;
}

bool yew_syn_engine_test_narrow_mask(SynEngine *master,
                                     const SynState *state, bool bol,
                                     u8 byte)
{
    SynEngine *active;
    const SynCtx *ctx;
    const u8 *first;
    u8 merged[32];
    u8 bridge[32];
    u8 bit = (u8)(1U << (byte & 7U));
    bool present;
    u16 resident;
    size_t i;

    if (!yew_syn_engine_test_masks(master, state, bol, merged, bridge))
        return false;
    present = ((merged[byte >> 3U] | bridge[byte >> 3U]) & bit) != 0U;
    ctx = checked_ctx(master, &state->f[state->depth - 1U], &active);
    first = merged_first_bytes(master, state, active, ctx, bol, NULL);
    for (i = 0U; i < YEW_ARRAY_LEN(master->merged_first); i++) {
        if (first == master->merged_first[i].bits) {
            master->merged_first[i].bits[byte >> 3U] &= (u8)~bit;
            break;
        }
    }
    master->embed_end_first[byte >> 3U] &= (u8)~bit;
    master->embed_bol_end_first[byte >> 3U] &= (u8)~bit;
    for (resident = 0U; resident < master->ndefs; resident++) {
        SynEngine *runtime = master->defs[resident].runtime;

        if (runtime == NULL)
            continue;
        runtime->embed_local_end_first[byte >> 3U] &= (u8)~bit;
        runtime->embed_local_bol_end_first[byte >> 3U] &= (u8)~bit;
    }
    return present;
}
#endif

static void syn_line_run(SynEngine *engine, u32 entry_state,
                         const u8 *line, u32 len, SynLineOut *out,
                         bool apply_eol, bool instrument, SynState *trace,
                         u32 *pending_lang, u32 *refused_lang,
                         const char **unknown_name, size_t *unknown_len)
{
    SynState state;
    const SynState *entry;
    u64 steps = 0U;
    u64 step_cap;
    u32 p = 0U;
    u32 zero_transitions = 0U;

    if (engine == NULL || out == NULL || (line == NULL && len != 0U))
        YEW_BUG("syntax: invalid line arguments");
    engine->line_calls++;
    out->n = 0U;
    out->stop = YEW_SYN_STOP_OK;
    if (pending_lang != NULL)
        *pending_lang = YEW_LANG_NONE;
    if (refused_lang != NULL)
        *refused_lang = YEW_LANG_NONE;
    if (unknown_name != NULL)
        *unknown_name = NULL;
    if (unknown_len != NULL)
        *unknown_len = 0U;
    entry = yew_syn_state_get(engine->states, entry_state);
    if (entry == NULL) {
        entry_state = YEW_SYN_STATE_ROOT;
        entry = yew_syn_state_get(engine->states, entry_state);
    }
    state = *entry;
    if (instrument && state.depth != 0U) {
        SynEngine *active;

        (void)checked_ctx(engine, &state.f[state.depth - 1U], &active);
        coverage_context(active, state.f[state.depth - 1U].ctx);
    }
    if (trace != NULL)
        trace[0] = state;
    if (len > YEW_SYN_LINE_BYTE_CAP) {
        emit_span(out, 0U, len, YEW_ATTR_TEXT, YEW_SPAN_TRUNCATED);
        out->exit_state = truncated_exit_state(engine, entry_state, entry,
                                               apply_eol);
        out->stop = YEW_SYN_STOP_BYTES;
        return;
    }
    if (engine->def == NULL || engine->def->ctxs == NULL) {
        if (trace != NULL) {
            u32 t;
            for (t = 1U; t <= len; t++)
                trace[t] = state;
        }
        emit_span(out, 0U, len, YEW_ATTR_TEXT, 0U);
        out->exit_state = entry_state;
        return;
    }
    step_cap = YEW_SYN_LINE_STEPS(len);
    if (len == 0U) {
        SynEngine *host;
        const SynRule *end_rule;
        u32 end_index;
        u8 bridge_depth;
        u8 host_aux;
        YewReMatch empty_match;

        if (bridge_end_match(engine, &state, line, len, 0U, &host,
                             &end_rule, &end_index, &bridge_depth,
                             &host_aux, &empty_match)) {
            SynState before = state;

            (void)set_aux(host, &state, host_aux, end_rule, line, len,
                          &empty_match);
            leave_embed(&state, bridge_depth);
            apply_rule_op(engine, host, &state, end_rule, line, len,
                          &empty_match, pending_lang, refused_lang,
                          unknown_name, unknown_len);
            if (instrument) {
                coverage_rule(host, end_index);
                coverage_transition(engine, &before, &state);
            }
        } else {
            apply_empty_bol(engine, &state, line, instrument, pending_lang,
                            refused_lang, unknown_name, unknown_len);
        }
        if (trace != NULL)
            trace[0] = state;
    }
    while (p < len) {
        SynEngine *active;
        const SynCtx *ctx = checked_ctx(engine,
                                        &state.f[state.depth - 1U],
                                        &active);
        u8 dflt_attr =
            (state.f[state.depth - 1U].fl & YEW_SYN_FR_BRIDGE) != 0U ?
                ctx->embed.fallback : ctx->dflt_attr;
        const u32 *candidate_offsets = active->candidate_offsets;
        size_t candidate_slot =
            (size_t)state.f[state.depth - 1U].ctx * SYN_CANDIDATE_STRIDE +
            line[p];
        u32 candidate_off = candidate_offsets == NULL ? 0U :
            candidate_offsets[candidate_slot];
        u32 candidate_len = candidate_offsets == NULL ? 0U :
            candidate_offsets[candidate_slot + 1U] - candidate_off;
        const SynRule *matched = NULL;
        u32 matched_index = UINT32_MAX;
        SynState before;
        YewReMatch match;
        SynWordProbe word_probe = {0};
        u32 fortran_word_rule = UINT32_MAX;
        SynFortranBolResult fortran_bol = SYN_FORTRAN_BOL_FALLBACK;
        u32 matched_end;
        u32 ri;

        {
            SynEngine *host;
            const SynRule *end_rule;
            u32 end_index;
            u8 bridge_depth;
            u8 host_aux;

            if ((bitset_has(engine->embed_end_first, line[p]) ||
                 (p == 0U && bitset_has(engine->embed_bol_end_first,
                                        line[p]))) &&
                bridge_end_byte_possible(engine, &state, p, line[p]) &&
                bridge_end_match(engine, &state, line, len, p, &host,
                                 &end_rule, &end_index, &bridge_depth,
                                 &host_aux, &match)) {
                SynState exit_before = state;

                matched_end = (u32)match.g[0].hi;
                emit_match(out, end_rule, &match, matched_end);
                (void)set_aux(host, &state, host_aux, end_rule, line, len,
                              &match);
                leave_embed(&state, bridge_depth);
                apply_rule_op(engine, host, &state, end_rule, line, len,
                              &match, pending_lang, refused_lang,
                              unknown_name, unknown_len);
                if (instrument) {
                    coverage_rule(host, end_index);
                    coverage_transition(engine, &exit_before, &state);
                }
                if (matched_end > p) {
                    if (trace != NULL) {
                        u32 t;

                        for (t = p + 1U; t < matched_end; t++)
                            trace[t] = exit_before;
                        trace[matched_end] = state;
                    }
                    p = matched_end;
                } else {
                    if (trace != NULL)
                        trace[p] = state;
                    if (zero_transitions++ < YEW_SYN_DEPTH_MAX)
                        continue;
                    p = next_boundary(line, len, p);
                }
                continue;
            }
        }

        if (p == 0U) {
            fortran_bol = fortran_bol_match(active, &state, line, len,
                                            &matched_index, &match);
            if (fortran_bol == SYN_FORTRAN_BOL_MATCH)
                matched = &active->def->rules[matched_index];
        }
        if (matched == NULL && p == 0U &&
            active->yaml_block_key_fast_enabled &&
            active->rule_yaml_block_key != NULL) {
            u32 yi;

            for (yi = 0U;
                 yi < (candidate_offsets == NULL ? ctx->nrules :
                                                    candidate_len);
                 yi++) {
                u32 index = candidate_offsets == NULL ?
                    ctx->first_rule + yi :
                    active->candidate_rules[candidate_off + yi];
                int fast;

                if (active->rule_yaml_block_key[index] != 2U)
                    continue;
                fast = yaml_block_key_match(
                    active->def->rules[index].re, line, len, 0U, &match);
                if (fast > 0 &&
                    yaml_block_key_predecessors_cannot_match(line, len,
                                                             &match)) {
                    matched_index = index;
                    matched = &active->def->rules[index];
                }
                break;
            }
        }
        if (active->fortran_fast_enabled &&
            active->fortran_words_cap != 0U &&
            state.f[state.depth - 1U].ctx == active->def->root) {
            word_probe_init_folded(&word_probe, line, len, p);
            if (word_probe.status == SYN_WORD_PROBE_READY)
                fortran_word_rule = fortran_word_winner(
                    active, line, p, &word_probe);
        }
        if (matched == NULL && fortran_word_rule != UINT32_MAX) {
            const SynRule *rule = &active->def->rules[fortran_word_rule];
            u32 group;

            match.ngroups = rule->re->ngroups;
            for (group = 0U; group < match.ngroups; group++)
                match.g[group] = (Span){p, word_probe.hi};
            matched_index = fortran_word_rule;
            matched = rule;
        }
        if (matched == NULL && (active->ctx_aux == NULL ||
            active->ctx_aux[state.f[state.depth - 1U].ctx] == 0U)) {
            const u8 *merged_first = merged_first_bytes(
                engine, &state, active, ctx, p == 0U, NULL);
            u32 q = p + 1U;
            const u8 *next_first = p == 0U ? merged_first_bytes(
                engine, &state, active, ctx, false, NULL) : merged_first;

            if (bitset_has(merged_first, line[p]))
                goto scan_rules;

            while (q < len && !bitset_has(next_first, line[q]))
                q++;
            emit_span(out, p, q - p, dflt_attr, 0U);
            if (trace != NULL) {
                u32 t;
                for (t = p + 1U; t <= q; t++)
                    trace[t] = state;
            }
            p = q;
            continue;
        }
scan_rules:
        for (ri = 0U; matched == NULL &&
             ri < (candidate_offsets == NULL ? ctx->nrules : candidate_len);
             ri++) {
            u32 index = candidate_offsets == NULL ? ctx->first_rule + ri :
                active->candidate_rules[candidate_off + ri];
            const SynRule *rule;
            if (++steps > step_cap || index >= active->def->nrules) {
                emit_span(out, p, len - p, YEW_ATTR_TEXT,
                          YEW_SPAN_TRUNCATED);
                out->exit_state = truncated_exit_state(
                    engine, entry_state, entry, apply_eol);
                out->stop = YEW_SYN_STOP_STEPS;
                return;
            }
            rule = &active->def->rules[index];
            if (active->fortran_fast_enabled &&
                word_probe.status == SYN_WORD_PROBE_READY &&
                active->rule_fortran_word != NULL &&
                active->rule_fortran_word[index] != 0U &&
                (fortran_word_rule != UINT32_MAX ||
                 active->rule_fortran_word[index] == 1U) &&
                index != fortran_word_rule)
                continue;
            if (fortran_bol == SYN_FORTRAN_BOL_NONE &&
                index >= ctx->first_rule &&
                index < ctx->first_rule + active->fortran_bol_rules)
                continue;
            if ((state.flags & YEW_SYN_F_PAST_FIRST) != 0U &&
                (rule->flags & YEW_SYN_RULE_FIRST_LINE) != 0U)
                continue;
            if ((rule->value_pred == SYN_VALUE_SET &&
                 (state.flags & YEW_SYN_F_VALUE) == 0U) ||
                (rule->value_pred == SYN_VALUE_CLEAR &&
                 (state.flags & YEW_SYN_F_VALUE) != 0U))
                continue;
            if (p != 0U && active->rule_bol != NULL &&
                active->rule_bol[index] != 0U)
                continue;
            if (active->rule_wordb != NULL &&
                active->rule_wordb[index] != 0U && line[p] < 0x80U) {
                bool after_word = line[p] == (u8)'_' ||
                                  (line[p] >= (u8)'0' &&
                                   line[p] <= (u8)'9') ||
                                  (line[p] >= (u8)'A' &&
                                   line[p] <= (u8)'Z') ||
                                  (line[p] >= (u8)'a' &&
                                   line[p] <= (u8)'z');
                bool before_word = false;

                if (p != 0U && line[p - 1U] < 0x80U)
                    before_word = line[p - 1U] == (u8)'_' ||
                        (line[p - 1U] >= (u8)'0' &&
                         line[p - 1U] <= (u8)'9') ||
                        (line[p - 1U] >= (u8)'A' &&
                         line[p - 1U] <= (u8)'Z') ||
                        (line[p - 1U] >= (u8)'a' &&
                         line[p - 1U] <= (u8)'z');
                else if (p != 0U)
                    before_word = !after_word;
                if (before_word == after_word)
                    continue;
            }
            if (candidate_offsets == NULL &&
                !bitset_has(active->rule_first == NULL ? rule->first :
                            active->rule_first[index], line[p]) &&
                rule->aux_match == SYN_AUXM_NONE)
                continue;
            if (rule_match(active, &state, state.ndef - 1U, rule, index,
                           line, len, p,
                           &word_probe, &match)) {
                matched = rule;
                matched_index = index;
                break;
            }
        }
        if (matched == NULL) {
            u32 q = next_boundary(line, len, p);
            emit_span(out, p, q - p, dflt_attr, 0U);
            if (trace != NULL) {
                u32 t;
                for (t = p + 1U; t <= q; t++)
                    trace[t] = state;
            }
            p = q;
            continue;
        }
        if (instrument || trace != NULL)
            before = state;
        if (instrument)
            coverage_rule(active, matched_index);
        if (instrument && matched->op == SYN_OP_EMBED)
            coverage_embed(active, matched->target);
        matched_end = (u32)match.g[0].hi;
        if (matched->consume != 0U &&
            matched->consume < match.ngroups &&
            match.g[matched->consume].hi != UINT64_MAX)
            matched_end = (u32)match.g[matched->consume].hi;
        if (matched_end < p || matched_end > match.g[0].hi)
            YEW_BUG("syntax: consume group ends outside whole match");
        if (matched->consume == 3U && match.ngroups == 5U &&
            active->yaml_block_key_fast_enabled &&
            active->rule_yaml_block_key != NULL &&
            active->rule_yaml_block_key[matched_index] != 0U)
            emit_yaml_block_key(out, matched, &match);
        else
            emit_match(out, matched, &match, matched_end);
        {
            bool aux_set = set_aux(active, &state, state.ndef - 1U,
                                   matched, line, len,
                                   &match);
            if ((matched->flags & YEW_SYN_RULE_STRIP) != 0U && aux_set)
                state.flags |= YEW_SYN_F_STRIP;
        }
        if ((matched->flags & YEW_SYN_RULE_SET_VALUE) != 0U)
            state.flags |= YEW_SYN_F_VALUE;
        if ((matched->flags & YEW_SYN_RULE_CLR_VALUE) != 0U)
            state.flags &= (u8)~YEW_SYN_F_VALUE;
        apply_rule_op(engine, active, &state, matched, line, len, &match,
                      pending_lang, refused_lang, unknown_name, unknown_len);
        if (instrument)
            coverage_transition(engine, &before, &state);
        {
            if (matched_end > p) {
                if (trace != NULL) {
                    u32 t;
                    for (t = p + 1U; t < matched_end; t++)
                        trace[t] = before;
                    trace[matched_end] = state;
                }
                p = matched_end;
            } else if ((matched->flags & YEW_SYN_RULE_ZERO_TRANSITION) != 0U &&
                       matched->op != SYN_OP_STAY && p == 0U &&
                       zero_transitions++ < YEW_SYN_DEPTH_MAX) {
                if (trace != NULL)
                    trace[p] = state;
                continue;
            } else {
                const SynCtx *after = checked_ctx(
                    engine, &state.f[state.depth - 1U], NULL);
                u32 q = next_boundary(line, len, p);
                emit_span(out, p, q - p, after->dflt_attr, 0U);
                if (trace != NULL) {
                    u32 t;
                    for (t = p + 1U; t <= q; t++)
                        trace[t] = state;
                }
                p = q;
            }
        }
    }
    if (apply_eol) {
        SynFrame *top = &state.f[state.depth - 1U];

        if ((top->fl & (YEW_SYN_FR_BRIDGE | YEW_SYN_FR_DEFER)) ==
                (YEW_SYN_FR_BRIDGE | YEW_SYN_FR_DEFER) &&
            (state.flags & YEW_SYN_F_EMBED_PEND) == 0U) {
            SynEngine *host;
            const SynCtx *bridge = checked_ctx(engine, top, &host);
            u32 encoded = state.aux[state.ndef];

            top->fl &= (u8)~YEW_SYN_FR_DEFER;
            state.aux[state.ndef] = 0U;
            if (encoded != 0U &&
                !push_guest(engine, &state, (u8)(encoded - 1U),
                            &bridge->embed, host)) {
                lost_add(&state, 2U);
                state.flags |= YEW_SYN_F_EMBED_LOST;
            }
        } else {
            SynEngine *active;
            const SynCtx *ctx = checked_ctx(engine, top, &active);
            SynState before;
            bool guest_was_active =
                (top->fl & YEW_SYN_FR_BRIDGE) == 0U && state.ndef > 1U;

            if (instrument)
                before = state;
            if ((top->fl & YEW_SYN_FR_BRIDGE) != 0U &&
                ctx->at_eol == SYN_OP_POP)
                leave_embed(&state, state.depth - 1U);
            apply_op(&state, ctx->at_eol, ctx->eol_nop, ctx->eol_target);
            if (instrument)
                coverage_transition(engine, &before, &state);
            if (guest_was_active) {
                i32 depth;

                for (depth = (i32)state.depth - 1; depth >= 0; depth--) {
                    SynFrame *bridge_frame = &state.f[depth];
                    SynEngine *host;
                    const SynCtx *bridge;

                    if ((bridge_frame->fl & YEW_SYN_FR_BRIDGE) == 0U)
                        continue;
                    bridge = checked_ctx(engine, bridge_frame, &host);
                    if ((bridge->embed.end == SYN_EMBED_END_LINE ||
                         (bridge->embed.end ==
                              SYN_EMBED_END_LINE_CONTINUATION &&
                          !line_has_continuation(line, len))) &&
                        bridge->at_eol != SYN_OP_STAY) {
                        SynState host_before = state;

                        leave_embed(&state, (u8)depth);
                        apply_op(&state, bridge->at_eol, bridge->eol_nop,
                                 bridge->eol_target);
                        if (instrument)
                            coverage_transition(engine, &host_before, &state);
                    }
                    break;
                }
            }
        }
        state.flags &= (u8)~YEW_SYN_F_VALUE;
        if ((state.flags & YEW_SYN_F_EMBED_ORPHAN) != 0U) {
            state.lost = state.lost > 2U ? (u8)(state.lost - 2U) : 0U;
            state.flags &= (u8)~(YEW_SYN_F_EMBED_LOST |
                                YEW_SYN_F_EMBED_ORPHAN);
        }
        if (engine->has_first_line)
            state.flags |= YEW_SYN_F_PAST_FIRST;
    }
    out->exit_state = memcmp(entry, &state, sizeof(state)) == 0 ?
        entry_state : yew_syn_state_intern(engine->states, &state);
}

void yew_syn_line(SynEngine *engine, u32 entry_state, const u8 *line,
                  u32 len, SynLineOut *out)
{
    syn_line_run(engine, entry_state, line, len, out, true,
                 engine != NULL && engine->coverage != NULL, NULL, NULL,
                 NULL, NULL, NULL);
}

bool yew_syn_stack_trace(SynEngine *engine, u32 entry_state, const u8 *line,
                         u32 len, SynState *trace, size_t trace_cap)
{
    SynLineOut line_out = {NULL, 0U, 0U, YEW_SYN_STATE_UNKNOWN,
                           YEW_SYN_STOP_OK};

    if (engine == NULL || trace == NULL || len > YEW_SYN_LINE_BYTE_CAP ||
        (line == NULL && len != 0U) || trace_cap < (size_t)len + 1U)
        return false;
    syn_line_run(engine, entry_state, line, len, &line_out, false, false,
                 trace, NULL, NULL, NULL, NULL);
    return line_out.stop == YEW_SYN_STOP_OK;
}

bool yew_syn_stack_at(SynEngine *engine, u32 entry_state, const u8 *line,
                      u32 len, u32 p, SynState *out)
{
    SynState *trace;
    bool ok;

    if (engine == NULL || out == NULL || len > YEW_SYN_LINE_BYTE_CAP ||
        (line == NULL && len != 0U))
        return false;
    if (p > len)
        p = len;
    trace = malloc(((size_t)len + 1U) * sizeof(*trace));
    if (trace == NULL)
        return false;
    ok = yew_syn_stack_trace(engine, entry_state, line, len, trace,
                             (size_t)len + 1U);
    if (ok)
        *out = trace[p];
    free(trace);
    return ok;
}

static i64 real_now_us(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * INT64_C(1000000) + ts.tv_nsec / 1000;
}

static i64 syn_now(const SynBuf *syn)
{
    return syn->clock == NULL ? real_now_us(NULL) : syn->clock(syn->clock_ctx);
}

static void vec_reserve(SynU32Vec *vec, size_t need)
{
    size_t cap;
    if (vec->cap >= need)
        return;
    cap = vec->cap == 0U ? (need > 1024U ? need : 8U) : vec->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    vec->data = yew_xreallocarray(vec->data, cap, sizeof(*vec->data));
    vec->cap = cap;
}

static void vec_splice(SynU32Vec *vec, size_t at, size_t removed,
                       size_t inserted)
{
    size_t tail;
    size_t new_len;
    if (at > vec->len)
        at = vec->len;
    if (removed > vec->len - at)
        removed = vec->len - at;
    if (inserted > SIZE_MAX - (vec->len - removed))
        YEW_BUG("syntax: line-state vector overflow");
    new_len = vec->len - removed + inserted;
    vec_reserve(vec, new_len);
    tail = vec->len - at - removed;
    (void)memmove(vec->data + at + inserted, vec->data + at + removed,
                  tail * sizeof(*vec->data));
    if (inserted != 0U)
        (void)memset(vec->data + at, 0, inserted * sizeof(*vec->data));
    vec->len = new_len;
}

static void pending_sync_head(SynBuf *syn)
{
    if (syn->embed_pending_count == 0U) {
        syn->embed_pending = YEW_LANG_NONE;
        syn->embed_pending_line = LINENO(0U);
        return;
    }
    syn->embed_pending = syn->embed_pending_langs[0];
    syn->embed_pending_line = syn->embed_pending_lines[0];
}

static void pending_reset(SynBuf *syn)
{
    syn->embed_pending_count = 0U;
    pending_sync_head(syn);
}

static bool pending_contains(const SynBuf *syn, u32 lang)
{
    u16 i;

    for (i = 0U; i < syn->embed_pending_count; i++) {
        if (syn->embed_pending_langs[i] == lang)
            return true;
    }
    return false;
}

static void pending_add(SynBuf *syn, u32 lang, size_t line)
{
    u16 i;

    for (i = 0U; i < syn->embed_pending_count; i++) {
        if (syn->embed_pending_langs[i] != lang)
            continue;
        if (line < syn->embed_pending_lines[i].v)
            syn->embed_pending_lines[i] = LINENO(line);
        pending_sync_head(syn);
        return;
    }
    if (syn->embed_pending_count >= YEW_SYN_RESIDENT_MAX)
        YEW_BUG("syntax: pending embed queue exceeds resident capacity");
    i = syn->embed_pending_count++;
    syn->embed_pending_langs[i] = lang;
    syn->embed_pending_lines[i] = LINENO(line);
    pending_sync_head(syn);
}

static void pending_remove_head(SynBuf *syn)
{
    if (syn->embed_pending_count != 0U) {
        syn->embed_pending_count--;
        if (syn->embed_pending_count != 0U) {
            (void)memmove(syn->embed_pending_langs,
                          syn->embed_pending_langs + 1U,
                          syn->embed_pending_count *
                              sizeof(*syn->embed_pending_langs));
            (void)memmove(syn->embed_pending_lines,
                          syn->embed_pending_lines + 1U,
                          syn->embed_pending_count *
                              sizeof(*syn->embed_pending_lines));
        }
    }
    pending_sync_head(syn);
}

static void pending_shift_edit(SynBuf *syn, size_t at, size_t removed,
                               size_t inserted, size_t edited_line)
{
    u16 i;

    for (i = 0U; i < syn->embed_pending_count; i++) {
        size_t line = (size_t)syn->embed_pending_lines[i].v;

        if (line < at)
            continue;
        if (line < at + removed)
            line = edited_line;
        else
            line = line - removed + inserted;
        if (syn->entry.len != 0U && line >= syn->entry.len)
            line = syn->entry.len - 1U;
        syn->embed_pending_lines[i] = LINENO(line);
    }
    pending_sync_head(syn);
}

void yew_syn_buf_init(SynBuf *syn)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    (void)memset(syn, 0, sizeof(*syn));
    syn->clock = real_now_us;
}

static void cache_prepare(SynBuf *syn)
{
    SynCache *cache;
    if (syn->private_cache != NULL)
        return;
    cache = yew_xcalloc(1U, sizeof(*cache));
    cache->slab_cap = YEW_SYN_SPAN_CACHE * YEW_SYN_MAX_SPANS;
    cache->slab = yew_xcalloc(cache->slab_cap, sizeof(*cache->slab));
    cache->line_cap = YEW_SYN_LINE_BYTE_CAP + 1U;
    cache->line = yew_xmalloc(cache->line_cap);
    syn->private_cache = cache;
}

static void cache_invalidate(SynBuf *syn)
{
    SynCache *cache = syn->private_cache;
    u32 i;
    if (cache == NULL)
        return;
    cache->hand = 0U;
    cache->slab_hand = 0U;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++)
        cache->slots[i].valid = false;
}

static u32 cache_span_alloc(SynCache *cache, u32 n)
{
    u32 off = cache->slab_hand;
    u32 i;

    if (n > cache->slab_cap)
        YEW_BUG("syntax: span slab request exceeds capacity");
    if (off + n > cache->slab_cap)
        off = 0U;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++) {
        SynCacheEnt *ent = &cache->slots[i];
        if (ent->valid && ent->n != 0U && n != 0U &&
            off < ent->span_off + ent->n && ent->span_off < off + n)
            ent->valid = false;
    }
    cache->slab_hand = off + n;
    if (cache->slab_hand == cache->slab_cap)
        cache->slab_hand = 0U;
    return off;
}

void yew_syn_attach(SynBuf *syn, u32 lang, const TextBuf *tb)
{
    size_t n;
    if (syn == NULL || tb == NULL)
        YEW_BUG("syntax: invalid attach arguments");
    free(syn->entry.data);
    syn->entry = (SynU32Vec){0};
    n = (size_t)yew_textbuf_line_count(tb);
    vec_reserve(&syn->entry, n);
    syn->entry.len = n;
    if (n != 0U)
        (void)memset(syn->entry.data, 0, n * sizeof(*syn->entry.data));
    if (n != 0U)
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    syn->lang = lang;
    syn->settled_to = LINENO(n == 0U ? 0U : 1U);
    syn->wave = LINENO(0U);
    syn->buf_gen = tb->gen;
    syn->settling = n != 0U && lang != YEW_LANG_NONE && syn->engine != NULL;
    if (!syn->settling) {
        syn->settled_to = LINENO(n);
        syn->wave = LINENO(n);
    }
    syn->degraded = false;
    pending_reset(syn);
    syn->embed_refused = YEW_LANG_NONE;
    syn->embed_unknown_logged = false;
    syn->spec_valid = false;
    syn->spec_from = LINENO(0U);
    syn->must_reach = LINENO(syn->settling ? n : 0U);
    syn->engine_gen = syn->engine == NULL ? 0U : syn->engine->generation;
    if (syn->engine != NULL)
        cache_prepare(syn);
    cache_invalidate(syn);
    syn->edit_us = syn_now(syn);
    syn->splice_count = 0U;
    syn->provisional_corrections = 0U;
}

void yew_syn_detach(SynBuf *syn)
{
    SynCache *cache;
    if (syn == NULL)
        return;
    cache = syn->private_cache;
    if (cache != NULL) {
        free(cache->line);
        free(cache->slab);
        free(cache);
    }
    free(syn->entry.data);
    yew_syn_buf_init(syn);
}

void yew_syn_buf_bind(SynBuf *syn, SynEngine *engine)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    syn->engine = engine;
    if (engine != NULL && syn->lang != YEW_LANG_NONE && syn->entry.len != 0U) {
        cache_prepare(syn);
        cache_invalidate(syn);
        syn->engine_gen = engine->generation;
        (void)memset(syn->entry.data, 0,
                     syn->entry.len * sizeof(*syn->entry.data));
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
        syn->wave = LINENO(0U);
        syn->settled_to = LINENO(1U);
        syn->must_reach = LINENO(syn->entry.len);
        syn->settling = true;
    } else {
        syn->engine_gen = engine == NULL ? 0U : engine->generation;
    }
}

void yew_syn_buf_set_clock(SynBuf *syn, SynClockFn clock, void *ctx)
{
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    syn->clock = clock == NULL ? real_now_us : clock;
    syn->clock_ctx = ctx;
}

void yew_syn_edit(SynBuf *syn, LineNo lo, u64 removed, u64 inserted)
{
    size_t at;
    u64 frontier;
    if (syn == NULL)
        YEW_BUG("syntax: NULL buffer state");
    if (lo.v >= syn->entry.len || lo.v >= SIZE_MAX ||
        removed > SIZE_MAX || inserted > SIZE_MAX)
        YEW_BUG("syntax: edit line count exceeds address space");
    at = (size_t)lo.v + 1U;
    if (removed > syn->entry.len - at ||
        inserted > SIZE_MAX - (syn->entry.len - (size_t)removed))
        YEW_BUG("syntax: edit splice is outside the line-state array");
    frontier = syn->must_reach.v;
    if (syn->settling && syn->wave.v < UINT64_MAX &&
        frontier < syn->wave.v + 1U)
        frontier = syn->wave.v + 1U;
    if (frontier > at) {
        if (frontier <= at + removed)
            frontier = at + inserted;
        else
            frontier = frontier - removed + inserted;
    }
    vec_splice(&syn->entry, at, (size_t)removed, (size_t)inserted);
    if (syn->entry.len != 0U)
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    if (syn->settled_to.v > lo.v + 1U)
        syn->settled_to = LINENO(lo.v + 1U);
    if (syn->wave.v > lo.v)
        syn->wave = lo;
    syn->settling = true;
    syn->spec_valid = false;
    pending_shift_edit(syn, at, (size_t)removed, (size_t)inserted,
                       (size_t)lo.v);
    if (frontier < at + inserted)
        frontier = at + inserted;
    if (frontier > syn->entry.len)
        frontier = syn->entry.len;
    syn->must_reach = LINENO(frontier);
    syn->edit_us = syn_now(syn);
    syn->edit_spliced = true;
    syn->splice_count++;
    if (syn->lang == YEW_LANG_NONE || syn->engine == NULL) {
        syn->settling = false;
        syn->settled_to = LINENO(syn->entry.len);
        syn->wave = LINENO(syn->entry.len);
    }
}

static SynCache *cache_get(SynBuf *syn)
{
    if (syn->private_cache == NULL)
        YEW_BUG("syntax: span cache was not prepared before drawing");
    return syn->private_cache;
}

static u32 line_content_len(const TextBuf *tb, LineNo line);

static u8 *line_copy(SynBuf *syn, const TextBuf *tb, LineNo line, u32 *len)
{
    SynCache *cache = cache_get(syn);
    Span span = yew_textbuf_line_span(tb, line);
    u64 need = line_content_len(tb, line);
    TextIter it;
    u64 copied = 0U;

    if (need > YEW_SYN_LINE_BYTE_CAP + 1U)
        need = YEW_SYN_LINE_BYTE_CAP + 1U;
    if (need != 0U && yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
        while (copied < need) {
            const u8 *bytes;
            u64 n;
            u64 take;
            if (!yew_textiter_chunk(&it, tb, &bytes, &n))
                YEW_BUG("syntax: failed to read line bytes");
            take = n < need - copied ? n : need - copied;
            (void)memcpy(cache->line + copied, bytes, (size_t)take);
            copied += take;
            if (copied < need && !yew_textiter_advance(&it, tb))
                YEW_BUG("syntax: truncated line iterator");
        }
    }
    if (copied != need)
        YEW_BUG("syntax: incomplete line copy");
    *len = (u32)need;
    return cache->line;
}

static bool textbuf_byte(const TextBuf *tb, u64 off, u8 *byte)
{
    TextIter it;
    const u8 *bytes;
    u64 n;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &n) || n == 0U)
        return false;
    *byte = bytes[0];
    return true;
}

static u32 line_content_len(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    u64 len = span.hi - span.lo;
    u8 byte;
    if (len != 0U && textbuf_byte(tb, span.hi - 1U, &byte) && byte == '\n')
        len--;
    if (len != 0U && textbuf_byte(tb, span.lo + len - 1U, &byte) &&
        byte == '\r')
        len--;
    return len > UINT32_MAX ? UINT32_MAX : (u32)len;
}

static void reconcile_generation(SynBuf *syn, const TextBuf *tb)
{
    size_t n = (size_t)yew_textbuf_line_count(tb);
    if (syn->buf_gen == tb->gen && syn->entry.len == n)
        return;
    if (syn->edit_spliced && syn->entry.len == n) {
        syn->buf_gen = tb->gen;
        syn->edit_spliced = false;
        return;
    }
    if (syn->entry.len != n) {
        vec_reserve(&syn->entry, n);
        syn->entry.len = n;
        if (n != 0U)
            (void)memset(syn->entry.data, 0, n * sizeof(*syn->entry.data));
        if (n != 0U)
            syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    }
    syn->wave = LINENO(0U);
    syn->settled_to = LINENO(n == 0U ? 0U : 1U);
    syn->settling = n != 0U;
    syn->buf_gen = tb->gen;
    syn->edit_spliced = false;
    syn->must_reach = LINENO(n);
    cache_invalidate(syn);
}

static void reconcile_engine(SynBuf *syn)
{
    if (syn->engine == NULL || syn->engine_gen == syn->engine->generation)
        return;
    if (syn->entry.len != 0U) {
        (void)memset(syn->entry.data, 0,
                     syn->entry.len * sizeof(*syn->entry.data));
        syn->entry.data[0] = YEW_SYN_STATE_ROOT;
    }
    syn->wave = LINENO(0U);
    syn->settled_to = LINENO(syn->entry.len == 0U ? 0U : 1U);
    syn->must_reach = LINENO(syn->entry.len);
    syn->settling = syn->lang != YEW_LANG_NONE && syn->entry.len != 0U;
    syn->spec_valid = false;
    syn->engine_gen = syn->engine->generation;
    cache_invalidate(syn);
}

static bool resident_install(SynEngine *master, u32 lang,
                             SynEngine *runtime)
{
    u32 byte;

    if (master == NULL || runtime == NULL || runtime->def == NULL ||
        lang == YEW_LANG_NONE)
        return false;
    if (resident_slot(master, lang) != UINT8_MAX)
        return true;
    if (master->ndefs >= YEW_SYN_RESIDENT_MAX)
        return false;
    yew_syn_def_pin(runtime->def);
    master->defs[master->ndefs++] = (SynResident){lang, runtime};
    for (byte = 0U; byte < sizeof(master->embed_end_first); byte++) {
        master->embed_end_first[byte] |=
            runtime->embed_local_end_first[byte];
        master->embed_bol_end_first[byte] |=
            runtime->embed_local_bol_end_first[byte];
    }
    return true;
}

#if defined(YEW_SYN_TEST)
bool yew_syn_engine_test_install_resident(SynEngine *master, u32 lang,
                                          SynEngine *runtime)
{
    return resident_install(master, lang, runtime);
}
#endif

static void syn_pending_request(SynBuf *syn, u32 lang, size_t line)
{
    if (lang == YEW_LANG_NONE ||
        yew_syn_def_resident(syn->engine, lang) != NULL)
        return;
    pending_add(syn, lang, line);
}

bool yew_syn_embed_pump(SynBuf *syn, SynEngine *engine, i64 budget_us)
{
    size_t i;
    u32 lang = YEW_LANG_NONE;
    SynEngine *runtime;
    bool queued = false;

    if (syn == NULL || engine == NULL ||
        budget_us < YEW_SYN_EMBED_LOAD_BUDGET_US)
        return false;
    if (syn->embed_pending_count != 0U) {
        lang = syn->embed_pending_langs[0];
        i = syn->embed_pending_lines[0].v;
        queued = true;
    }
    if (!queued) {
        for (i = 0U; lang == YEW_LANG_NONE && i < syn->entry.len; i++) {
            const SynState *state = yew_syn_state_get(
                engine->states, syn->entry.data[i]);

            if (state != NULL &&
                (state->flags & YEW_SYN_F_EMBED_PEND) != 0U &&
                state->ndef < YEW_SYN_DEF_MAX) {
                lang = state->aux[state->ndef];
                if (lang != YEW_LANG_NONE &&
                    resident_slot(engine, lang) == UINT8_MAX)
                    break;
                lang = YEW_LANG_NONE;
            }
        }
    }
    if (lang == YEW_LANG_NONE || engine->ndefs >= YEW_SYN_RESIDENT_MAX)
        return false;
    runtime = yew_syn_engine_for(lang);
    if (!resident_install(engine, lang, runtime))
        return false;

    if (queued) {
        pending_remove_head(syn);
    }

    /* The pending state is an entry state, so its opener is on the prior
     * line for deferred embeds and may be there for an immediate embed.
     * Replaying one line of headroom is cheap and covers both cases. */
    if (!queued && i != 0U)
        i--;
    if (i < syn->entry.len) {
        size_t clear_from = i + 1U;
        size_t tail = syn->entry.len - clear_from;

        if (tail != 0U)
            (void)memset(syn->entry.data + clear_from, 0,
                         tail * sizeof(*syn->entry.data));
        syn->wave = LINENO(i);
        syn->settled_to = LINENO(i + 1U);
        syn->must_reach = LINENO(syn->entry.len);
        syn->settling = true;
        syn->spec_valid = false;
        cache_invalidate(syn);
    }
    return true;
}

void yew_syn_settle(SynBuf *syn, const TextBuf *tb, LineNo view_lo,
                    LineNo view_hi, i64 budget_us, SynSettleReport *report)
{
    SynSettleReport local;
    i64 started;
    i64 elapsed = 0;
    u64 nlines;
    u64 i;
    u64 clock_every;
    i64 deadline_us;
    SynSpan spans[YEW_SYN_MAX_SPANS];

    if (syn == NULL || tb == NULL)
        YEW_BUG("syntax: invalid settle arguments");
    if (report == NULL)
        report = &local;
    (void)memset(report, 0, sizeof(*report));
    report->damage_lo = LINENO(UINT64_MAX);
    started = syn_now(syn);
    /* Injected deterministic clocks count observations, so preserve their
     * established 256-line quantum.  The real monotonic clock samples
     * more often to keep a 1 ms frame from overshooting its deadline. */
    clock_every = syn->clock == real_now_us ? YEW_SYN_CLOCK_EVERY :
                  YEW_SYN_INJECTED_CLOCK_EVERY;
    deadline_us = budget_us;
    if (syn->clock == real_now_us &&
        deadline_us > YEW_SYN_CLOCK_HEADROOM_US)
        deadline_us -= YEW_SYN_CLOCK_HEADROOM_US;
    reconcile_engine(syn);
    reconcile_generation(syn, tb);
    if (budget_us >= YEW_SYN_IDLE_BUDGET_US)
        (void)yew_syn_embed_pump(syn, syn->engine,
                                 YEW_SYN_EMBED_LOAD_BUDGET_US);
    nlines = syn->entry.len;
    if (syn->engine == NULL || syn->lang == YEW_LANG_NONE || nlines == 0U) {
        syn->settling = false;
        syn->wave = LINENO(nlines);
        syn->settled_to = LINENO(nlines);
        report->fixpoint = true;
        report->damage_lo = LINENO(0U);
        report->damage_hi = LINENO(0U);
        return;
    }
    i = syn->wave.v;
    if (i >= nlines)
        i = nlines - 1U;
    for (; i < nlines; i++) {
        SynLineOut out = {spans, 0U, YEW_SYN_MAX_SPANS, 0U,
                          YEW_SYN_STOP_OK};
        u32 pending_lang;
        u32 refused_lang;
        const char *unknown_name;
        size_t unknown_len;
        u32 len;
        const u8 *bytes;
        u32 held;
        u32 next;
        u32 entry = syn->entry.data[i];
        if (entry == YEW_SYN_STATE_UNKNOWN)
            entry = YEW_SYN_STATE_ROOT;
        bytes = line_copy(syn, tb, LINENO(i), &len);
        syn_line_run(syn->engine, entry, bytes, len, &out, true,
                     syn->engine->coverage != NULL, NULL, &pending_lang,
                     &refused_lang, &unknown_name, &unknown_len);
        syn_pending_request(syn, pending_lang, i);
        if (refused_lang != YEW_LANG_NONE) {
            syn->embed_refused = refused_lang;
            syn->degraded = true;
        }
        if (unknown_name != NULL && !syn->embed_unknown_logged) {
            int shown = unknown_len > 120U ? 120 : (int)unknown_len;

            yew_log(YEW_LOG_WARN,
                    "syntax embed language '%.*s' is unavailable; using fallback",
                    shown, unknown_name);
            syn->embed_unknown_logged = true;
        }
        report->lines++;
        {
            const SynState *exit =
                yew_syn_state_get(yew_syn_engine_states(syn->engine),
                                  out.exit_state);
            if (out.stop != YEW_SYN_STOP_OK ||
                yew_syn_state_exhausted(
                    yew_syn_engine_states(syn->engine)) ||
                (exit != NULL &&
                 (exit->flags & YEW_SYN_F_DEGRADED) != 0U)) {
                if (!syn->degraded)
                    yew_log(YEW_LOG_WARN,
                            "syntax highlighting degraded for buffer");
                syn->degraded = true;
            }
        }
        if (report->damage_lo.v == UINT64_MAX)
            report->damage_lo = LINENO(i);
        report->damage_hi = LINENO(i + 1U);
        if (i >= view_lo.v && i < view_hi.v)
            report->hit_view = true;
        next = out.exit_state;
        if (i + 1U >= nlines) {
            syn->wave = LINENO(nlines);
            syn->settled_to = LINENO(nlines);
            syn->must_reach = LINENO(0U);
            syn->settling = false;
            syn->spec_valid = false;
            report->fixpoint = true;
            break;
        }
        held = syn->entry.data[i + 1U];
        if (held != YEW_SYN_STATE_UNKNOWN && held == next &&
            i + 1U >= syn->must_reach.v) {
            syn->wave = LINENO(nlines);
            syn->settled_to = LINENO(nlines);
            syn->must_reach = LINENO(0U);
            syn->settling = false;
            if (syn->spec_valid && i + 1U >= syn->spec_from.v) {
                syn->provisional_corrections++;
                report->provisional = true;
            }
            syn->spec_valid = false;
            report->fixpoint = true;
            break;
        }
        syn->entry.data[i + 1U] = next;
        if (syn->spec_valid && i + 1U >= syn->spec_from.v) {
            syn->provisional_corrections++;
            report->provisional = true;
            syn->spec_valid = false;
        }
        syn->settled_to = LINENO(i + 2U);
        syn->wave = LINENO(i + 1U);
        if (i + 1U >= view_lo.v && i + 1U < view_hi.v) {
            report->hit_view = true;
            report->damage_hi = LINENO(i + 2U);
        }
        if ((i & (clock_every - 1U)) == 0U && budget_us > 0) {
            elapsed = syn_now(syn) - started;
        }
        if (elapsed >= deadline_us && budget_us > 0) {
            syn->settling = true;
            if (syn->wave.v < view_lo.v) {
                syn->spec_from = view_lo;
                syn->spec_valid = true;
                report->provisional = true;
            }
            break;
        }
    }
    if (report->fixpoint) {
        i64 ended = syn_now(syn);
        elapsed = ended >= started ? ended - started : 0;
    }
    report->us = (u64)(elapsed > 0 ? elapsed : 0);
    if (report->damage_lo.v == UINT64_MAX) {
        report->damage_lo = LINENO(0U);
        report->damage_hi = LINENO(0U);
    }
    syn->buf_gen = tb->gen;
}

static SynCacheEnt *cache_find(SynCache *cache, u64 line, u64 gen, u32 state)
{
    u32 i;
    for (i = 0U; i < YEW_SYN_SPAN_CACHE; i++) {
        SynCacheEnt *ent = &cache->slots[i];
        if (ent->valid && ent->line == line && ent->gen == gen &&
            ent->entry_state == state)
            return ent;
    }
    return NULL;
}

static void note_span_stop(SynBuf *syn, u8 stop)
{
    if (stop == YEW_SYN_STOP_OK)
        return;
    if (!syn->degraded)
        yew_log(YEW_LOG_WARN, "syntax highlighting degraded for buffer");
    syn->degraded = true;
}

void yew_syn_spans(SynBuf *syn, const TextBuf *tb, LineNo line,
                   SynLineOut *out)
{
    SynCache *cache;
    SynCacheEnt *ent;
    u32 state;
    u32 copy;
    u32 len;
    const u8 *bytes;

    if (syn == NULL || tb == NULL || out == NULL || line.v >= syn->entry.len)
        YEW_BUG("syntax: invalid span request");
    reconcile_engine(syn);
    out->n = 0U;
    out->stop = YEW_SYN_STOP_OK;
    state = syn->entry.data[line.v];
    if (state == YEW_SYN_STATE_UNKNOWN)
        state = YEW_SYN_STATE_ROOT;
    if (syn->engine == NULL || syn->lang == YEW_LANG_NONE) {
        len = line_content_len(tb, line);
        emit_span(out, 0U, len, YEW_ATTR_TEXT, 0U);
        out->exit_state = state;
        return;
    }
    cache = cache_get(syn);
    ent = cache_find(cache, line.v, tb->gen, state);
    if (ent == NULL) {
        ent = &cache->slots[cache->hand++ % YEW_SYN_SPAN_CACHE];
        ent->valid = false;
        bytes = line_copy(syn, tb, line, &len);
        {
            SynLineOut evaluated = {cache->scratch, 0U, YEW_SYN_MAX_SPANS, 0U,
                                    YEW_SYN_STOP_OK};
            yew_syn_line(syn->engine, state, bytes, len, &evaluated);
            ent->span_off = cache_span_alloc(cache, evaluated.n);
            if (evaluated.n != 0U)
                (void)memcpy(cache->slab + ent->span_off, evaluated.spans,
                             evaluated.n * sizeof(*cache->slab));
            ent->n = evaluated.n;
            ent->exit_state = evaluated.exit_state;
            ent->stop = evaluated.stop;
        }
        ent->line = line.v;
        ent->gen = tb->gen;
        ent->entry_state = state;
        ent->valid = true;
    }
    copy = ent->n < out->cap ? ent->n : out->cap;
    if (copy != 0U)
        (void)memcpy(out->spans, cache->slab + ent->span_off,
                     copy * sizeof(*out->spans));
    out->n = copy;
    out->exit_state = ent->exit_state;
    out->stop = copy == ent->n ? ent->stop : YEW_SYN_STOP_SPANS;
    if (copy != ent->n && copy != 0U)
        out->spans[copy - 1U].flags |= YEW_SPAN_TRUNCATED;
    note_span_stop(syn, out->stop);
}

bool yew_syn_status_visible(const SynBuf *syn)
{
    if (syn == NULL)
        return false;
    if (syn->degraded)
        return true;
    return syn->settling && syn_now(syn) - syn->edit_us >=
                                (i64)YEW_SYN_SETTLING_MS * 1000;
}

void yew_syn_status(const SynBuf *syn, u64 line_count, char *dst, size_t cap)
{
    const char *root_name = "none";
    const char *active_name = "none";
    const char *refused_name = "none";
    u32 states = 0U;
    u32 pending = 0U;
    u8 ndefs = 0U;
    u8 depth = 0U;
    size_t i;

    if (dst == NULL || cap == 0U)
        return;
    if (syn == NULL) {
        (void)snprintf(dst, cap, "syntax unavailable");
        return;
    }
    if (syn->engine != NULL) {
        const SynDef *root = yew_syn_engine_def_at(syn->engine, 0U);
        const SynState *active = NULL;
        size_t at = syn->wave.v < syn->entry.len ? (size_t)syn->wave.v :
                    syn->entry.len == 0U ? 0U : syn->entry.len - 1U;

        if (root != NULL && root->name != NULL)
            root_name = root->name;
        states = yew_syn_state_count(syn->engine->states);
        pending = syn->embed_pending_count;
        if (syn->embed_refused != YEW_LANG_NONE)
            refused_name = engine_name_by_lang(syn->engine,
                                               syn->embed_refused);
        if (syn->entry.len != 0U)
            active = yew_syn_state_get(syn->engine->states,
                                       syn->entry.data[at]);
        if (active != NULL) {
            const SynDef *def = yew_syn_engine_def_at(
                syn->engine, YEW_SYN_DEF_OF(active));

            depth = active->depth;
            ndefs = active->ndef;
            if (def != NULL && def->name != NULL)
                active_name = def->name;
        }
        for (i = 0U; i < syn->entry.len; i++) {
            const SynState *state = yew_syn_state_get(
                syn->engine->states, syn->entry.data[i]);

            if (state != NULL &&
                (state->flags & YEW_SYN_F_EMBED_PEND) != 0U &&
                state->ndef < YEW_SYN_DEF_MAX) {
                u32 lang = state->aux[state->ndef];
                size_t prior;
                bool seen = pending_contains(syn, lang);

                for (prior = 0U; !seen && prior < i; prior++) {
                    const SynState *earlier = yew_syn_state_get(
                        syn->engine->states, syn->entry.data[prior]);

                    seen = earlier != NULL &&
                        (earlier->flags & YEW_SYN_F_EMBED_PEND) != 0U &&
                        earlier->ndef < YEW_SYN_DEF_MAX &&
                        earlier->aux[earlier->ndef] == lang;
                }
                if (!seen)
                    pending++;
            }
        }
    }
    (void)snprintf(dst, cap,
                   "settled %llu/%llu, wave %llu, root=%s active=%s "
                   "defs=%u/%u depth=%u/%u states=%u degraded=%s "
                   "embed_pending=%u embed_refused=%s",
                   (unsigned long long)syn->settled_to.v,
                   (unsigned long long)line_count,
                   (unsigned long long)syn->wave.v,
                   root_name, active_name, (unsigned)ndefs,
                   (unsigned)YEW_SYN_DEF_MAX, (unsigned)depth,
                   (unsigned)YEW_SYN_DEPTH_MAX, (unsigned)states,
                   syn->degraded ? "yes" : "no", (unsigned)pending,
                   refused_name);
}
