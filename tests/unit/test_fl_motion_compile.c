#include "harness.h"

#include <string.h>

#include "fl/compile.h"
#include "fl/diag.h"
#include "fl/gc.h"
#include "fl/parse.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct MotionCompileFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} MotionCompileFix;

static void motion_compile_open(MotionCompileFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_vm_init(&f->vm, &f->arena, &f->in, &f->dc);
}

static void motion_compile_close(MotionCompileFix *f)
{
    fl_vm_free(&f->vm);
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

static FlMotionProg *compile_motion(MotionCompileFix *f, const char *src)
{
    FlOrigin origin = {(u8)FL_ORIGIN_CLI, 0U, 0U};
    FlProgram p;
    FlFn *fn;
    u32 i;

    (void)fl_diag_add_file(&f->dc, "motion.fl", src, strlen(src));
    p = fl_parse(&f->arena, &f->dc, &f->in, src, strlen(src), 0U);
    YEW_ASSERT(!p.had_error);
    fn = fl_compile(&f->vm, &f->dc, &p, 0U, origin);
    YEW_ASSERT(fn != NULL);
    if (fn == NULL)
        return NULL;
    for (i = 0U; i < fn->ch.nconsts; i++) {
        if (fn->ch.consts[i].t == (u8)FL_MOTION_PROG)
            return (FlMotionProg *)fn->ch.consts[i].as.o;
    }
    YEW_ASSERT(false);
    return NULL;
}

void test_fl_motion_compile_flattens_nested_highlight_extents(void)
{
    MotionCompileFix f;
    FlMotionProg *p;

    motion_compile_open(&f);
    p = compile_motion(&f, "@[ H(> H(< v) ^) ]\n");
    YEW_ASSERT(p != NULL);
    if (p != NULL) {
        YEW_ASSERT_EQ_U64(p->n, 6U);
        YEW_ASSERT_EQ_U64(p->op[0].kind, FL_MK_HIGHLIGHT);
        YEW_ASSERT_EQ_U64(p->op[0].arg, 5U);
        YEW_ASSERT_EQ_U64(p->op[2].kind, FL_MK_HIGHLIGHT);
        YEW_ASSERT_EQ_U64(p->op[2].arg, 2U);
        YEW_ASSERT_EQ_U64(p->op[5].ch, (u8)'^');
    }
    motion_compile_close(&f);
}

void test_fl_motion_compile_preserves_explicit_one_count(void)
{
    MotionCompileFix f;
    FlMotionProg *p;

    motion_compile_open(&f);
    p = compile_motion(&f, "@[ > 1> av 1av H(1<) ]\n");
    YEW_ASSERT(p != NULL);
    if (p != NULL) {
        YEW_ASSERT_EQ_U64(p->n, 6U);
        YEW_ASSERT_EQ_U64(p->op[0].count, 1U);
        YEW_ASSERT((p->op[0].flags & FL_MOTION_F_COUNT_GIVEN) == 0U);
        YEW_ASSERT((p->op[1].flags & FL_MOTION_F_COUNT_GIVEN) != 0U);
        YEW_ASSERT((p->op[2].flags & FL_MOTION_F_ALT) != 0U);
        YEW_ASSERT((p->op[2].flags & FL_MOTION_F_COUNT_GIVEN) == 0U);
        YEW_ASSERT((p->op[3].flags & FL_MOTION_F_ALT) != 0U);
        YEW_ASSERT((p->op[3].flags & FL_MOTION_F_COUNT_GIVEN) != 0U);
        YEW_ASSERT((p->op[4].flags & FL_MOTION_F_COUNT_GIVEN) == 0U);
        YEW_ASSERT((p->op[5].flags & FL_MOTION_F_COUNT_GIVEN) != 0U);
    }
    motion_compile_close(&f);
}
