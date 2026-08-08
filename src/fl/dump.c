/*
 * Sprint 29 deliverable 5: the AST dumper.
 *
 * Deterministic s-expressions and nothing else -- no pointers, no
 * addresses, no sizes that vary with the allocator.  This is the
 * parser's only output until Sprint 30, so it is the substrate for
 * every golden in the sprint AND the input to the determinism lane,
 * which dumps the same file twice and compares bytes.
 */
#include "fl/parse.h"

#include <string.h>

#include "fl/lex.h"

static void dump_node(Bytebuf *out, const FlNode *n, const Interner *in);

static void dump_str(Bytebuf *out, const Interner *in, u32 id)
{
    const char *s = sag_intern_str(in, id);
    size_t i;
    size_t len;

    if (s == NULL) {
        bytebuf_append(out, "\"\"", 2U);
        return;
    }
    len = strlen(s);
    /* Escaped on the way out so a string containing a quote, a newline
     * or a paren cannot forge s-expression structure in a golden. */
    bytebuf_push_u8(out, (u8)'"');
    for (i = 0U; i < len; i++) {
        u8 c = (u8)s[i];

        switch (c) {
        case '"':  bytebuf_append(out, "\\\"", 2U); break;
        case '\\': bytebuf_append(out, "\\\\", 2U); break;
        case '\n': bytebuf_append(out, "\\n", 2U); break;
        case '\t': bytebuf_append(out, "\\t", 2U); break;
        case '\r': bytebuf_append(out, "\\r", 2U); break;
        default:
            if (c < 0x20U)
                bytebuf_printf(out, "\\x%02X", (unsigned)c);
            else
                bytebuf_push_u8(out, c);
            break;
        }
    }
    bytebuf_push_u8(out, (u8)'"');
}

static void dump_kids(Bytebuf *out, FlNode *const *v, u32 n,
                      const Interner *in)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        bytebuf_push_u8(out, (u8)' ');
        dump_node(out, v[i], in);
    }
}

static void dump_opt(Bytebuf *out, const FlNode *n, const Interner *in)
{
    bytebuf_push_u8(out, (u8)' ');
    if (n == NULL)
        bytebuf_append(out, "nil", 3U);
    else
        dump_node(out, n, in);
}

static const char *motion_kind_word(u8 k)
{
    switch ((FlMotionKind)k) {
    case FL_MK_UNIT:      return "unit";
    case FL_MK_ARROW:     return "arrow";
    case FL_MK_HIGHLIGHT: return "highlight";
    case FL_MK_INSERT:    return "ins";
    case FL_MK_DEL:       return "del";
    case FL_MK_ESC:       return "esc";
    default:              return "word";
    }
}

static void dump_motion(Bytebuf *out, const FlNode *n, const Interner *in)
{
    bytebuf_printf(out, "(motion %u %s", (unsigned)n->as.motion.count,
                   motion_kind_word(n->as.motion.mkind));
    switch ((FlMotionKind)n->as.motion.mkind) {
    case FL_MK_UNIT:
        bytebuf_printf(out, " %c", (char)n->as.motion.ch);
        break;
    case FL_MK_ARROW:
        /* The alt flag is a separate word rather than a prefix on the
         * character, so a golden diff shows which of the two changed. */
        bytebuf_printf(out, " %c", (char)n->as.motion.ch);
        if (n->as.motion.alt)
            bytebuf_append(out, " alt", 4U);
        break;
    case FL_MK_INSERT:
    case FL_MK_WORD:
        bytebuf_push_u8(out, (u8)' ');
        dump_str(out, in, n->as.motion.payload);
        break;
    case FL_MK_HIGHLIGHT:
        dump_kids(out, n->as.motion.inner, n->as.motion.ninner, in);
        break;
    default:
        break;
    }
    bytebuf_push_u8(out, (u8)')');
}

static void dump_node(Bytebuf *out, const FlNode *n, const Interner *in)
{
    if (n == NULL) {
        bytebuf_append(out, "nil", 3U);
        return;
    }
    switch ((FlAstKind)n->kind) {
    case FL_A_LET:
        bytebuf_append(out, "(let ", 5U);
        dump_str(out, in, n->as.let.name);
        dump_opt(out, n->as.let.init, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_ASSIGN:
        bytebuf_append(out, "(assign", 7U);
        dump_opt(out, n->as.assign.tgt, in);
        dump_opt(out, n->as.assign.val, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_FN:
    case FL_A_FN_EXPR: {
        u32 i;

        bytebuf_append(out, n->kind == (u8)FL_A_FN ? "(fn " : "(fn-expr ",
                       n->kind == (u8)FL_A_FN ? 4U : 9U);
        if (n->kind == (u8)FL_A_FN) {
            dump_str(out, in, n->as.fn.name);
            bytebuf_push_u8(out, (u8)' ');
        }
        bytebuf_append(out, "(params", 7U);
        for (i = 0U; i < n->as.fn.nparams; i++) {
            bytebuf_push_u8(out, (u8)' ');
            dump_str(out, in, n->as.fn.params[i]);
        }
        bytebuf_push_u8(out, (u8)')');
        dump_opt(out, n->as.fn.body, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    }
    case FL_A_MACRO:
        bytebuf_append(out, "(macro ", 7U);
        dump_str(out, in, n->as.macro.name);
        dump_opt(out, n->as.macro.body, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_IMPORT:
        bytebuf_append(out, "(import ", 8U);
        if (n->as.import.is_string) {
            dump_str(out, in, n->as.import.path);
            bytebuf_append(out, " as ", 4U);
        }
        dump_str(out, in, n->as.import.name);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_IF:
        bytebuf_append(out, "(if", 3U);
        dump_opt(out, n->as.ifs.cond, in);
        dump_opt(out, n->as.ifs.then, in);
        dump_opt(out, n->as.ifs.els, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_WHILE:
        bytebuf_append(out, "(while", 6U);
        dump_opt(out, n->as.whiles.cond, in);
        dump_opt(out, n->as.whiles.body, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_FOR:
        bytebuf_append(out, "(for ", 5U);
        dump_str(out, in, n->as.fors.var);
        if (n->as.fors.var2 != 0U) {
            bytebuf_push_u8(out, (u8)' ');
            dump_str(out, in, n->as.fors.var2);
        }
        dump_opt(out, n->as.fors.iter, in);
        dump_opt(out, n->as.fors.body, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_RETURN:
        bytebuf_append(out, "(return", 7U);
        dump_opt(out, n->as.ret.value, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_BREAK:    bytebuf_append(out, "(break)", 7U); return;
    case FL_A_CONTINUE: bytebuf_append(out, "(continue)", 10U); return;
    case FL_A_EDIT:
        bytebuf_append(out, "(edit", 5U);
        dump_opt(out, n->as.edit.body, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_TRY:
        bytebuf_append(out, "(try", 4U);
        dump_opt(out, n->as.trys.body, in);
        bytebuf_append(out, " catch ", 7U);
        dump_str(out, in, n->as.trys.var);
        dump_opt(out, n->as.trys.handler, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_EXPR_STMT:
        dump_node(out, n->as.expr_stmt.expr, in);
        return;
    case FL_A_BLOCK:
        bytebuf_append(out, "(block", 6U);
        dump_kids(out, n->as.list.items, n->as.list.n, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_BINOP:
        bytebuf_printf(out, "(%s", fl_tok_spelling((FlTokKind)n->as.bin.op));
        dump_opt(out, n->as.bin.l, in);
        dump_opt(out, n->as.bin.r, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_UNOP:
        bytebuf_printf(out, "(u%s", fl_tok_spelling((FlTokKind)n->as.un.op));
        dump_opt(out, n->as.un.operand, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_CALL:
        bytebuf_append(out, "(call", 5U);
        dump_opt(out, n->as.call.callee, in);
        dump_kids(out, n->as.call.args, n->as.call.nargs, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_INDEX:
        bytebuf_append(out, "(index", 6U);
        dump_opt(out, n->as.index.obj, in);
        dump_opt(out, n->as.index.idx, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_FIELD:
        bytebuf_append(out, "(field", 6U);
        dump_opt(out, n->as.field.obj, in);
        bytebuf_push_u8(out, (u8)' ');
        dump_str(out, in, n->as.field.name);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_IDENT:
        bytebuf_append(out, "(id ", 4U);
        dump_str(out, in, n->as.ident.name);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_LIT:
        switch ((FlLitKind)n->as.lit.lit) {
        case FL_L_NIL:  bytebuf_append(out, "(lit nil)", 9U); return;
        case FL_L_BOOL:
            bytebuf_printf(out, "(lit bool %s)",
                           n->as.lit.v.b ? "true" : "false");
            return;
        case FL_L_INT:
            bytebuf_printf(out, "(lit int %lld)",
                           (long long)n->as.lit.v.i);
            return;
        case FL_L_FLOAT:
            /*
             * %.17g round-trips an f64 exactly and prints the same
             * digits on gcc and clang, which the determinism lane
             * compares byte for byte.
             */
            bytebuf_printf(out, "(lit float %.17g)", n->as.lit.v.f);
            return;
        default:
            bytebuf_append(out, "(lit str ", 9U);
            dump_str(out, in, n->as.lit.v.str_id);
            bytebuf_push_u8(out, (u8)')');
            return;
        }
    case FL_A_LIST:
        bytebuf_append(out, "(list", 5U);
        dump_kids(out, n->as.list.items, n->as.list.n, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_MAP: {
        u32 i;

        bytebuf_append(out, "(map", 4U);
        for (i = 0U; i < n->as.map.n; i++) {
            bytebuf_append(out, " (", 2U);
            dump_node(out, n->as.map.keys[i], in);
            bytebuf_push_u8(out, (u8)' ');
            dump_node(out, n->as.map.vals[i], in);
            bytebuf_push_u8(out, (u8)')');
        }
        bytebuf_push_u8(out, (u8)')');
        return;
    }
    case FL_A_MOTION_BLOCK:
        bytebuf_append(out, "(motion-block", 13U);
        dump_kids(out, n->as.list.items, n->as.list.n, in);
        bytebuf_push_u8(out, (u8)')');
        return;
    case FL_A_MOTION:
        dump_motion(out, n, in);
        return;
    default:
        bytebuf_append(out, "(?)", 3U);
        return;
    }
}

void fl_ast_dump_node(Bytebuf *out, const FlNode *n, const Interner *in)
{
    dump_node(out, n, in);
}

void fl_ast_dump(Bytebuf *out, const FlProgram *p, const Interner *in)
{
    u32 i;

    if (out == NULL || p == NULL)
        return;
    for (i = 0U; i < p->n; i++) {
        dump_node(out, p->stmts[i], in);
        bytebuf_push_u8(out, (u8)'\n');
    }
}
