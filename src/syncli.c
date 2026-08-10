#define _POSIX_C_SOURCE 200809L

#include "syncli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/diag.h"
#include "syn/defs.h"
#include "syn/engine.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

static const char syn_usage[] =
    "Usage:\n"
    "  yew syn list\n"
    "  yew syn compile [--all | FILE]\n"
    "  yew syn check FILE [--strict]\n"
    "  yew syn dump FILE --tables\n"
    "  yew syn dump FILE --spans INPUT\n"
    "  yew syn cache clear|path\n"
    "\n"
    "Exit: 0 valid, 1 invalid definition or arguments, 3 I/O error,\n"
    "4 internal error.  --strict promotes warnings to errors.\n";

static void stderr_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                        const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)fputs(rendered, stderr);
}

static int usage_error(const char *msg)
{
    if (msg != NULL)
        (void)fprintf(stderr, "yew syn: error: %s\n", msg);
    (void)fputs(syn_usage, stderr);
    return YEW_EXIT_ERR;
}

static bool output(const Bytebuf *out)
{
    return out->len == 0U || fwrite(out->data, 1U, out->len, stdout) == out->len;
}

static char *slurp(const char *path, size_t *len, Arena *arena)
{
    FILE *fp;
    Bytebuf bytes;
    char *copy;

    errno = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(errno));
        return NULL;
    }
    bytebuf_init(&bytes);
    for (;;) {
        u8 chunk[65536];
        size_t got = fread(chunk, 1U, sizeof(chunk), fp);

        if (got != 0U)
            bytebuf_append(&bytes, chunk, got);
        if (got != sizeof(chunk))
            break;
    }
    if (ferror(fp) != 0) {
        int saved = errno;

        (void)fclose(fp);
        bytebuf_free(&bytes);
        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(saved == 0 ? EIO : saved));
        return NULL;
    }
    if (fclose(fp) != 0) {
        bytebuf_free(&bytes);
        (void)fprintf(stderr, "yew syn: cannot close %s: %s\n", path,
                      strerror(errno));
        return NULL;
    }
    copy = arena_alloc(arena, bytes.len + 1U, 1U);
    if (bytes.len != 0U)
        (void)memcpy(copy, bytes.data, bytes.len);
    copy[bytes.len] = '\0';
    *len = bytes.len;
    bytebuf_free(&bytes);
    return copy;
}

static const SynLangDesc *lang_at(u32 ordinal)
{
    u32 id;
    u32 seen = 0U;

    for (id = 1U; id <= UINT16_MAX; id++) {
        const SynLangDesc *desc = yew_syn_lang_desc(id);

        if (desc == NULL)
            continue;
        if (seen == ordinal)
            return desc;
        seen++;
    }
    return NULL;
}

static int list_defs(bool bypass)
{
    Bytebuf out;
    const char *no_cache = getenv("YEW_NO_SYN_CACHE");
    u32 count = yew_syn_lang_count();
    u32 i;

    bytebuf_init(&out);
    for (i = 0U; i < count; i++) {
        const SynLangDesc *desc = lang_at(i);
        char *cache;
        const char *state;
        u32 j;

        if (desc == NULL) {
            bytebuf_free(&out);
            (void)fputs("yew syn: internal language table gap\n", stderr);
            return YEW_EXIT_BUG;
        }
        cache = yew_syn_cache_path(desc->name);
        state = bypass || (no_cache != NULL && strcmp(no_cache, "1") == 0) ?
                "bypassed" :
                cache != NULL && access(cache, F_OK) == 0 ? "warm" : "cold";
        bytebuf_printf(&out, "%s\t", desc->name);
        for (j = 0U; j < desc->nextensions; j++) {
            if (j != 0U)
                bytebuf_push_u8(&out, (u8)',');
            bytebuf_append(&out, desc->extensions[j],
                           strlen(desc->extensions[j]));
        }
        bytebuf_printf(&out, "\t%s\t%s\n", desc->source, state);
        free(cache);
    }
    if (!output(&out)) {
        bytebuf_free(&out);
        return YEW_EXIT_IO;
    }
    bytebuf_free(&out);
    return YEW_EXIT_OK;
}

static SynDef *load_def(const char *path, Arena *arena, DiagCtx *dc,
                        int *status)
{
    struct stat st;
    SynDef *def;

    errno = 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(path, R_OK) != 0) {
        int saved = errno == 0 ? EIO : errno;

        (void)fprintf(stderr, "yew syn: cannot read %s: %s\n", path,
                      strerror(saved));
        *status = YEW_EXIT_IO;
        return NULL;
    }
    fl_diag_init(dc, arena);
    fl_diag_set_sink(dc, stderr_sink, NULL);
    def = yew_syn_def_load(arena, dc, path);
    *status = def == NULL ? YEW_EXIT_ERR : YEW_EXIT_OK;
    return def;
}

static char *runtime_source(const SynLangDesc *desc)
{
    const char *root = getenv("YEW_RUNTIME_DIR");
    const char *relative = desc->source;
    char *path;
    size_t n;

    if (strncmp(relative, "runtime/", 8U) == 0)
        relative += 8U;
    if (root != NULL && root[0] != '\0') {
        n = strlen(root) + 1U + strlen(relative);
        path = yew_xmalloc(n + 1U);
        (void)snprintf(path, n + 1U, "%s/%s", root, relative);
        if (access(path, R_OK) == 0)
            return path;
        free(path);
    }
    n = strlen(YEW_RUNTIME_DIR_DEFAULT) + 1U + strlen(relative);
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/%s", YEW_RUNTIME_DIR_DEFAULT,
                   relative);
    if (access(path, R_OK) == 0)
        return path;
    free(path);
    path = yew_xmalloc(strlen(desc->source) + 1U);
    (void)memcpy(path, desc->source, strlen(desc->source) + 1U);
    return path;
}

static int compile_one(const char *path)
{
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    int status;

    arena_init(&arena);
    def = load_def(path, &arena, &dc, &status);
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&arena);
    return status;
}

static int compile_all(void)
{
    u32 count = yew_syn_lang_count();
    u32 i;

    for (i = 0U; i < count; i++) {
        const SynLangDesc *desc = lang_at(i);
        char *path;
        int status;

        if (desc == NULL)
            return YEW_EXIT_BUG;
        path = runtime_source(desc);
        status = compile_one(path);
        free(path);
        if (status != YEW_EXIT_OK)
            return status;
    }
    return YEW_EXIT_OK;
}

static int check_one(const char *path, bool strict)
{
    Arena arena;
    DiagCtx dc;
    char *source;
    size_t len = 0U;
    u32 file_id;
    u32 errors = 0U;
    u32 warnings = 0U;
    SynDef *def;
    int status;

    arena_init(&arena);
    source = slurp(path, &len, &arena);
    if (source == NULL) {
        arena_free_all(&arena);
        return YEW_EXIT_IO;
    }
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, stderr_sink, NULL);
    file_id = fl_diag_add_file(&dc, path, source, len);
    def = yew_syn_def_compile(&arena, &dc, (const u8 *)source, len, file_id,
                              &errors, &warnings);
    status = errors != 0U || (strict && warnings != 0U) ? YEW_EXIT_ERR :
             YEW_EXIT_OK;
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&arena);
    return status;
}

static const char *op_name(u8 op)
{
    switch ((SynOp)op) {
    case SYN_OP_PUSH: return "push";
    case SYN_OP_POP: return "pop";
    case SYN_OP_SET: return "set";
    default: return "stay";
    }
}

static const char *aux_match_name(u8 match)
{
    switch ((SynAuxMatch)match) {
    case SYN_AUXM_LINE_EQ: return "line_eq";
    case SYN_AUXM_LITERAL: return "literal";
    case SYN_AUXM_FENCE_CLOSE: return "fence_close";
    case SYN_AUXM_INDENT_LT: return "indent_lt";
    default: return "none";
    }
}

static const char *unit_name(u8 flags)
{
    if ((flags & YEW_SYN_CTX_UNIT_ATOM) != 0U)
        return "atom";
    if ((flags & YEW_SYN_CTX_UNIT_SPAN) != 0U)
        return "span";
    return "none";
}

static const char *ctx_name(const SynDef *def, u16 id)
{
    const char *name = yew_syn_ctx_name(def, id);

    return name == NULL ? "<unknown>" : name;
}

static void quote(Bytebuf *out, const char *s)
{
    const u8 *p = (const u8 *)(s == NULL ? "" : s);

    bytebuf_push_u8(out, (u8)'"');
    while (*p != 0U) {
        switch (*p) {
        case '\\': bytebuf_append(out, "\\\\", 2U); break;
        case '"': bytebuf_append(out, "\\\"", 2U); break;
        case '\n': bytebuf_append(out, "\\n", 2U); break;
        case '\r': bytebuf_append(out, "\\r", 2U); break;
        case '\t': bytebuf_append(out, "\\t", 2U); break;
        default:
            if (*p < 0x20U || *p == 0x7fU)
                bytebuf_printf(out, "\\x%02x", (unsigned)*p);
            else
                bytebuf_push_u8(out, *p);
            break;
        }
        p++;
    }
    bytebuf_push_u8(out, (u8)'"');
}

static void dump_first(Bytebuf *out, const u8 first[32])
{
    u32 i;

    bytebuf_append(out, " first=", 7U);
    for (i = 0U; i < 32U; i++)
        bytebuf_printf(out, "%02x", (unsigned)first[i]);
}

static void dump_rule(Bytebuf *out, const SynDef *def, u32 index)
{
    const SynRule *rule = &def->rules[index];
    u32 i;

    bytebuf_printf(out, "  rule %u match=", (unsigned)index);
    quote(out, yew_syn_rule_pattern(def, index));
    bytebuf_printf(out, " attr=%s op=%s", yew_syn_attr_name(rule->attr),
                   op_name(rule->op));
    if (rule->op == SYN_OP_PUSH) {
        bytebuf_append(out, " target=", 8U);
        if (rule->npush != 0U) {
            for (i = 0U; i < rule->npush; i++) {
                if (i != 0U)
                    bytebuf_push_u8(out, (u8)',');
                bytebuf_append(out, ctx_name(def, rule->push[i]),
                               strlen(ctx_name(def, rule->push[i])));
            }
        } else {
            bytebuf_append(out, ctx_name(def, rule->target),
                           strlen(ctx_name(def, rule->target)));
        }
    } else if (rule->op == SYN_OP_SET) {
        bytebuf_printf(out, " target=%s", ctx_name(def, rule->target));
    } else if (rule->op == SYN_OP_POP) {
        bytebuf_printf(out, " count=%u", (unsigned)rule->nop);
    }
    bytebuf_printf(out, " consume=%u aux=%s", (unsigned)rule->consume,
                   aux_match_name(rule->aux_match));
    if (rule->flags != 0U)
        bytebuf_printf(out, " flags=0x%02x", (unsigned)rule->flags);
    for (i = 0U; i < YEW_ARRAY_LEN(rule->caps); i++) {
        if (rule->caps[i] != UINT8_MAX)
            bytebuf_printf(out, " capture.%u=%s", (unsigned)i,
                           yew_syn_attr_name(rule->caps[i]));
    }
    if (rule->aux_pre != 0U) {
        bytebuf_append(out, " aux_pre=", 9U);
        quote(out, yew_intern_str(def->aux, rule->aux_pre));
    }
    if (rule->aux_post != 0U) {
        bytebuf_append(out, " aux_post=", 10U);
        quote(out, yew_intern_str(def->aux, rule->aux_post));
    }
    dump_first(out, rule->first);
    bytebuf_push_u8(out, (u8)'\n');
}

static int dump_tables(const SynDef *def)
{
    Bytebuf out;
    u16 i;

    bytebuf_init(&out);
    bytebuf_printf(&out, "language %s\nroot %s\n", def->name,
                   ctx_name(def, def->root));
    for (i = 0U; i < def->nctxs; i++) {
        const SynCtx *ctx = &def->ctxs[i];
        u32 j;

        bytebuf_printf(&out, "context %s default=%s at_eol=%s unit=%s",
                       ctx_name(def, i), yew_syn_attr_name(ctx->dflt_attr),
                       op_name(ctx->at_eol), unit_name(ctx->flags));
        if (ctx->at_eol == SYN_OP_SET)
            bytebuf_printf(&out, " target=%s",
                           ctx_name(def, ctx->eol_target));
        else if (ctx->at_eol == SYN_OP_POP)
            bytebuf_printf(&out, " count=%u", (unsigned)ctx->eol_nop);
        dump_first(&out, ctx->first);
        bytebuf_push_u8(&out, (u8)'\n');
        for (j = 0U; j < ctx->nrules; j++)
            dump_rule(&out, def, ctx->first_rule + j);
    }
    if (!output(&out)) {
        bytebuf_free(&out);
        return YEW_EXIT_IO;
    }
    bytebuf_free(&out);
    return YEW_EXIT_OK;
}

static const char *state_context(SynEngine *engine, u32 state)
{
    const SynState *value = yew_syn_state_get(yew_syn_engine_states(engine),
                                              state);
    const SynDef *def = yew_syn_engine_def(engine);

    if (value == NULL || value->depth == 0U)
        return "<unknown>";
    return ctx_name(def, value->ctx[value->depth - 1U]);
}

static int dump_spans(const SynDef *def, const char *path)
{
    Arena input_arena;
    char *input;
    size_t input_len = 0U;
    size_t lo = 0U;
    u32 line_no = 1U;
    u32 entry = YEW_SYN_STATE_ROOT;
    SynEngine *engine;
    Bytebuf text;
    int status = YEW_EXIT_OK;

    arena_init(&input_arena);
    input = slurp(path, &input_len, &input_arena);
    if (input == NULL) {
        arena_free_all(&input_arena);
        return YEW_EXIT_IO;
    }
    engine = yew_syn_engine_new((SynDef *)def);
    bytebuf_init(&text);
    while (lo <= input_len) {
        size_t hi = lo;
        size_t line_len;
        SynSpan spans[YEW_SYN_MAX_SPANS];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
        u32 i;

        while (hi < input_len && input[hi] != '\n')
            hi++;
        line_len = hi - lo;
        if (line_len != 0U && input[lo + line_len - 1U] == '\r')
            line_len--;
        if (line_len > UINT32_MAX) {
            (void)fprintf(stderr, "yew syn: input line %u is too long\n",
                          (unsigned)line_no);
            status = YEW_EXIT_IO;
            break;
        }
        yew_syn_line(engine, entry, (const u8 *)input + lo, (u32)line_len,
                     &out);
        bytebuf_printf(&text, "line %u entry=%s exit=%s\n",
                       (unsigned)line_no, state_context(engine, entry),
                       state_context(engine, out.exit_state));
        for (i = 0U; i < out.n; i++) {
            SynState at;
            const char *context = state_context(engine, entry);

            if (yew_syn_stack_at(engine, entry, (const u8 *)input + lo,
                                 (u32)line_len, spans[i].start, &at) &&
                at.depth != 0U)
                context = ctx_name(def, at.ctx[at.depth - 1U]);
            bytebuf_printf(&text, "  %u:%u-%u attr=%s context=%s",
                           (unsigned)line_no, (unsigned)spans[i].start,
                           (unsigned)(spans[i].start + spans[i].len),
                           yew_syn_attr_name(spans[i].attr), context);
            if ((spans[i].flags & YEW_SPAN_TRUNCATED) != 0U)
                bytebuf_append(&text, " truncated", 10U);
            bytebuf_push_u8(&text, (u8)'\n');
        }
        entry = out.exit_state;
        if (hi == input_len)
            break;
        lo = hi + 1U;
        line_no++;
    }
    if (status == YEW_EXIT_OK && !output(&text))
        status = YEW_EXIT_IO;
    bytebuf_free(&text);
    yew_syn_engine_free(engine);
    arena_free_all(&input_arena);
    return status;
}

static int dump_def(const char *definition, const char *mode,
                    const char *input)
{
    Arena arena;
    DiagCtx dc;
    SynDef *def;
    int status;

    arena_init(&arena);
    def = load_def(definition, &arena, &dc, &status);
    if (def != NULL) {
        status = strcmp(mode, "--tables") == 0 ? dump_tables(def) :
                 dump_spans(def, input);
        yew_syn_def_dispose(def);
    }
    arena_free_all(&arena);
    return status;
}

int yew_syn_main(int argc, char **argv, bool clean)
{
    int at = 1;

    if (at < argc && strcmp(argv[at], "--clean") == 0) {
        clean = true;
        at++;
    }
    yew_syn_cache_set_bypass(clean);
    yew_syn_discovery_set_bypass(clean);
    if (at >= argc || strcmp(argv[at], "--help") == 0) {
        (void)fputs(syn_usage, at >= argc ? stderr : stdout);
        return at >= argc ? YEW_EXIT_ERR : YEW_EXIT_OK;
    }
    if (strcmp(argv[at], "list") == 0)
        return at + 1 == argc ? list_defs(clean) :
               usage_error("list takes no arguments");
    if (strcmp(argv[at], "compile") == 0) {
        if (at + 2 != argc)
            return usage_error("compile requires --all or FILE");
        return strcmp(argv[at + 1], "--all") == 0 ? compile_all() :
               compile_one(argv[at + 1]);
    }
    if (strcmp(argv[at], "check") == 0) {
        bool strict = false;
        const char *path = NULL;
        int i;

        for (i = at + 1; i < argc; i++) {
            if (strcmp(argv[i], "--strict") == 0 && !strict)
                strict = true;
            else if (path == NULL)
                path = argv[i];
            else
                return usage_error("check requires one FILE");
        }
        return path == NULL ? usage_error("check requires FILE") :
               check_one(path, strict);
    }
    if (strcmp(argv[at], "dump") == 0) {
        if (at + 3 == argc && strcmp(argv[at + 2], "--tables") == 0)
            return dump_def(argv[at + 1], argv[at + 2], NULL);
        if (at + 4 == argc && strcmp(argv[at + 2], "--spans") == 0)
            return dump_def(argv[at + 1], argv[at + 2], argv[at + 3]);
        return usage_error("dump requires FILE --tables or FILE --spans INPUT");
    }
    if (strcmp(argv[at], "cache") == 0) {
        if (at + 2 != argc)
            return usage_error("cache requires clear or path");
        if (strcmp(argv[at + 1], "clear") == 0) {
            if (yew_syn_cache_clear())
                return YEW_EXIT_OK;
            else {
                (void)fputs("yew syn: cannot clear syntax cache\n", stderr);
                return YEW_EXIT_IO;
            }
        }
        if (strcmp(argv[at + 1], "path") == 0) {
            char *path = yew_syn_cache_dir();

            if (path == NULL) {
                (void)fputs("yew syn: cannot resolve syntax cache path\n",
                            stderr);
                return YEW_EXIT_IO;
            }
            (void)printf("%s\n", path);
            free(path);
            return ferror(stdout) == 0 ? YEW_EXIT_OK : YEW_EXIT_IO;
        }
        return usage_error("cache requires clear or path");
    }
    return usage_error("unknown command");
}
