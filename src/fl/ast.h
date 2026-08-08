#ifndef SAG_FL_AST_H
#define SAG_FL_AST_H

/*
 * Sprint 29 deliverable 2: the Fletch AST.
 *
 * Arena-allocated and destructor-free: a program's nodes all die with
 * the arena the parser was handed, so nothing here owns anything and
 * there is no traversal that has to get freeing right.
 *
 * CHILD ARRAYS ARE ALLOCATED AT THEIR FINAL SIZE -- count first, then
 * fill -- and never grown.  A parent holds interior pointers into those
 * arrays, so a realloc would leave every sibling reference dangling.
 * The same discipline Cgfried's s17 pinned, for the same reason.
 */

#include <stdbool.h>

#include "fl/diag.h"
#include "util/base.h"

typedef enum FlAstKind {
    /* statements */
    FL_A_LET, FL_A_ASSIGN, FL_A_FN, FL_A_MACRO, FL_A_IMPORT,
    FL_A_IF, FL_A_WHILE, FL_A_FOR, FL_A_RETURN, FL_A_BREAK,
    FL_A_CONTINUE, FL_A_EDIT, FL_A_TRY, FL_A_EXPR_STMT, FL_A_BLOCK,
    /* expressions */
    FL_A_BINOP, FL_A_UNOP, FL_A_CALL, FL_A_INDEX, FL_A_FIELD,
    FL_A_IDENT, FL_A_LIT, FL_A_LIST, FL_A_MAP, FL_A_FN_EXPR,
    /* motion */
    FL_A_MOTION_BLOCK, FL_A_MOTION,
    FL_A_KIND__N
} FlAstKind;

/* The tag on FL_A_LIT.  `nil`, `true` and `false` are literals in the
 * grammar (§2's `literal`), so they are values here rather than three
 * more node kinds. */
typedef enum FlLitKind {
    FL_L_NIL, FL_L_BOOL, FL_L_INT, FL_L_FLOAT, FL_L_STR
} FlLitKind;

/* What an FL_A_MOTION word is.  Mirrors the lexer's motion tokens; the
 * count lives on the node rather than as a separate child because §2's
 * `motion = [ COUNT ] motion_word` binds one count to one word. */
typedef enum FlMotionKind {
    FL_MK_UNIT, FL_MK_ARROW, FL_MK_HIGHLIGHT, FL_MK_INSERT,
    FL_MK_DEL, FL_MK_ESC, FL_MK_WORD
} FlMotionKind;

typedef struct FlNode FlNode;

struct FlNode {
    u8 kind;      /* FlAstKind */
    FlSpan sp;
    union {
        /* let x [= init] */
        struct { FlNode *init; u32 name; } let;
        /* target = value */
        struct { FlNode *tgt; FlNode *val; } assign;
        /*
         * Pointer members lead in every arm so the 4-byte fields pack
         * into the tail: ordering `name` first would pad the struct out
         * past the 48-byte budget asserted below.
         */
        struct { u32 *params; FlNode *body; u32 name; u32 nparams; } fn;
        /* macro NAME = motion_block */
        struct { FlNode *body; u32 name; } macro;
        /* import NAME | import "path" as NAME */
        struct { u32 path; u32 name; bool is_string; } import;
        struct { FlNode *cond; FlNode *then; FlNode *els; } ifs;
        struct { FlNode *cond; FlNode *body; } whiles;
        struct { FlNode *iter; FlNode *body; u32 var; u32 var2; } fors;
        struct { FlNode *value; } ret;
        struct { FlNode *body; } edit;
        struct { FlNode *body; FlNode *handler; u32 var; } trys;
        struct { FlNode *expr; } expr_stmt;
        /* BLOCK, LIST, MOTION_BLOCK and CALL's arguments all share the
         * shape: a right-sized array and its length. */
        struct { FlNode **items; u32 n; } list;
        struct { FlNode **keys; FlNode **vals; u32 n; } map;
        struct { FlNode *l; FlNode *r; u8 op; } bin;   /* op: FlTokKind */
        struct { FlNode *operand; u8 op; } un;
        struct { FlNode *callee; FlNode **args; u32 nargs; } call;
        struct { FlNode *obj; FlNode *idx; } index;
        struct { FlNode *obj; u32 name; } field;
        struct { u32 name; } ident;
        struct {
            union { i64 i; double f; u32 str_id; bool b; } v;
            u8 lit;                                     /* FlLitKind */
        } lit;
        struct {
            /* FL_MK_HIGHLIGHT hangs its contained motions off `inner`;
             * every other kind leaves it NULL. */
            FlNode **inner;
            u32 ninner;
            u32 count;
            u32 payload;      /* intern id: INSERT text or CMDWORD name */
            u8 mkind;         /* FlMotionKind */
            u8 ch;            /* unit/arrow character */
            bool alt;
        } motion;
    } as;
};

/*
 * DoD 4.  The budget is not decoration: a 10 000-line program is
 * hundreds of thousands of nodes, and every 8 bytes added here is
 * another megabyte of arena and another pass over cache that the
 * compiler in Sprint 30 pays for on every walk.
 */
_Static_assert(sizeof(FlNode) <= 48, "FlNode must stay within 48 bytes");

typedef struct FlProgram {
    FlNode **stmts;
    u32 n;
    /*
     * EOF arrived mid-construct and NOTHING was wrong with what came
     * before -- the s32 REPL's "issue a continuation prompt" signal.
     *
     * Never set alongside an error: a genuine syntax error inside an
     * open bracket must read as an error, or the REPL waits forever for
     * a closing token that will not fix it.
     */
    bool incomplete;
    bool had_error;
} FlProgram;

#endif /* SAG_FL_AST_H */
