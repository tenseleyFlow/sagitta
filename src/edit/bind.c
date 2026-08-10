#include "edit/bind.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "fl/flhook.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/origin.h"
#include "fl/trace.h"
#include "fl/vm.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/log.h"

typedef struct SagBindRow {
    char *seq;
    char *cmd;
    char *sarg;
    FlValue fn;
    i64 iarg;
    u32 origin;
    u32 ledger_id;
    Mode mode;
    bool active;
} SagBindRow;

typedef struct SagBindings {
    SagBindRow *v;
    u32 n;
    u32 cap;
    u32 free_hint;
    u32 active_hi;
    u32 batch_depth;
    u32 rebuilds;
    bool pending;
    char error[256];
} SagBindings;

static char *bind_strdup(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = sag_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

static void bind_error(Ed *ed, const char *fmt, ...)
{
    va_list ap;

    if (ed == NULL || ed->bindings == NULL)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(ed->bindings->error, sizeof(ed->bindings->error), fmt,
                    ap);
    va_end(ap);
}

static SagBindRow *row_by_ledger(Ed *ed, u32 ledger_id)
{
    FlRegistration *reg;
    SagBindings *binds;
    u32 row;

    if (ed == NULL || (binds = ed->bindings) == NULL || ledger_id == 0U ||
        ledger_id > ed->hooks.ledger.n)
        return NULL;
    reg = &ed->hooks.ledger.v[ledger_id - 1U];
    if (!reg->active || reg->kind != (u8)REG_BIND || reg->handle == 0U)
        return NULL;
    row = reg->handle - 1U;
    if (row >= binds->n || !binds->v[row].active ||
        binds->v[row].ledger_id != ledger_id)
        return NULL;
    return &binds->v[row];
}

CmdStatus sag_bind_closure_cmd(CmdCtx *cx)
{
    SagBindRow *row;
    FlVm *vm;
    FlValue ignored = FL_NIL_V;
    Bytebuf trace;

    if (cx == NULL || cx->ed == NULL || cx->iarg <= 0 ||
        (u64)cx->iarg > UINT32_MAX)
        return SAG_CMD_ERR_ARG;
    row = row_by_ledger(cx->ed, (u32)cx->iarg);
    vm = sag_fl_vm(cx->ed);
    if (row == NULL || vm == NULL ||
        (row->fn.t != (u8)FL_CLOSURE && row->fn.t != (u8)FL_NATIVE))
        return SAG_CMD_ERR_STATE;
    if (fl_call(vm, row->fn, NULL, 0U, &ignored))
        return SAG_CMD_OK;

    bytebuf_init(&trace);
    fl_trace_render(vm, vm->err, &trace);
    sag_log(SAG_LOG_ERROR, "bound Fletch closure failed: %.*s",
            (int)trace.len, trace.data == NULL ? "" :
            (const char *)trace.data);
    sag_msg(cx->ed, SAG_MSG_ERROR, "bound Fletch closure failed");
    bytebuf_free(&trace);
    return SAG_CMD_ERR_STATE;
}

static CmdId closure_command_id(void)
{
    CmdId id = sag_cmd_lookup("ed.fl.closure", 13U);

    if (id.v == 0U)
        SAG_BUG("ed.fl.closure command is not registered");
    return id;
}

static void bind_mark(FlVm *vm, void *ctx)
{
    Ed *ed = (Ed *)ctx;
    SagBindings *binds;
    u32 i;

    if (vm == NULL || ed == NULL || (binds = ed->bindings) == NULL)
        return;
    for (i = 0U; i < binds->n; i++) {
        if (binds->v[i].active)
            fl_gc_mark_value(vm, binds->v[i].fn);
    }
}

static SagBindRow *find_sequence(Ed *ed, Mode mode, const char *seq)
{
    SagBindings *binds = ed->bindings;
    u32 i;

    for (i = binds->active_hi; i != 0U; i--) {
        SagBindRow *row = &binds->v[i - 1U];

        if (row->active && row->mode == mode && strcmp(row->seq, seq) == 0)
            return row;
    }
    return NULL;
}

static u64 sequence_hash(const char *seq)
{
    const unsigned char *p = (const unsigned char *)seq;
    u64 hash = UINT64_C(14695981039346656037);

    while (*p != '\0') {
        hash ^= (u64)*p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool sequence_set_add(const char **set, size_t cap, const char *seq)
{
    size_t at = (size_t)sequence_hash(seq) & (cap - 1U);

    while (set[at] != NULL) {
        if (strcmp(set[at], seq) == 0)
            return false;
        at = (at + 1U) & (cap - 1U);
    }
    set[at] = seq;
    return true;
}

static bool rows_for_mode(Ed *ed, Mode mode, BindRow **out, u32 *out_n)
{
    SagBindings *binds = ed->bindings;
    const char **seen;
    bool *keep;
    BindRow *rows;
    u32 n = 0U;
    size_t cap = 16U;
    u64 want = (u64)binds->active_hi * 2U + 2U;
    u32 i;
    u32 at = 0U;

    while ((u64)cap < want) {
        if (cap > SIZE_MAX / 2U)
            SAG_BUG("binding shadow set exceeds address space");
        cap *= 2U;
    }
    seen = sag_xcalloc(cap, sizeof(*seen));
    keep = binds->active_hi == 0U ? NULL :
           sag_xcalloc(binds->active_hi, sizeof(*keep));
    for (i = binds->active_hi; i != 0U; i--) {
        SagBindRow *row = &binds->v[i - 1U];

        if (row->active && row->mode == mode &&
            sequence_set_add(seen, cap, row->seq)) {
            keep[i - 1U] = true;
            n++;
        }
    }
    rows = n == 0U ? NULL : sag_xcalloc(n, sizeof(*rows));
    for (i = 0U; i < binds->active_hi; i++) {
        SagBindRow *row = &binds->v[i];

        if (!keep[i])
            continue;
        rows[at++] = (BindRow){row->seq, row->cmd, row->iarg, row->sarg};
    }
    *out = rows;
    *out_n = at;
    free(keep);
    free(seen);
    return true;
}

static bool validate_candidate(Ed *ed, const SagBindRow *extra)
{
    BindRow row;
    SagKeymapError error;

    row = (BindRow){extra->seq, extra->cmd, extra->iarg, extra->sarg};
    error = sag_keymap_validate_row(&row);
    if (error != SAG_KEYMAP_ERR_NONE)
        bind_error(ed, "%s", sag_keymap_error_string(error));
    return error == SAG_KEYMAP_ERR_NONE;
}

void sag_bind_init(Ed *ed)
{
    FlVm *vm;
    u32 i;

    if (ed == NULL)
        SAG_BUG("bind init: NULL editor");
    if (ed->bindings != NULL)
        return;
    ed->bindings = sag_xcalloc(1U, sizeof(*ed->bindings));
    (void)closure_command_id();
    for (i = 0U; i < (u32)SAG_MODE__N; i++) {
        if (!sag_keymap_build(&ed->bind_keys[i], "config", NULL, 0U))
            SAG_BUG("cannot build empty config keymap");
    }
    ed->keys.n = 3U;
    ed->keys.l[2] = &ed->bind_keys[ed->mode];
    vm = sag_fl_vm(ed);
    if (vm != NULL)
        fl_gc_root_provider(vm, bind_mark, ed);
}

void sag_bind_free(Ed *ed)
{
    SagBindings *binds;
    u32 i;

    if (ed == NULL || (binds = ed->bindings) == NULL)
        return;
    for (i = 0U; i < binds->n; i++) {
        free(binds->v[i].seq);
        free(binds->v[i].cmd);
        free(binds->v[i].sarg);
    }
    free(binds->v);
    free(binds);
    ed->bindings = NULL;
}

u32 sag_bind_add(Ed *ed, u32 origin, Mode mode, const char *seq,
                 CmdId cmd, i64 iarg, const char *sarg, FlValue fn)
{
    SagBindings *binds;
    SagBindRow candidate = {0};
    SagBindRow *row;
    const CmdDesc *desc;
    u32 slot;
    u32 want;

    if (ed == NULL || (binds = ed->bindings) == NULL ||
        mode >= SAG_MODE__N || seq == NULL) {
        bind_error(ed, "invalid bind arguments");
        return 0U;
    }
    binds->error[0] = '\0';
    row = find_sequence(ed, mode, seq);
    if (row != NULL && row->origin == origin) {
        bind_error(ed, "duplicate key sequence");
        return 0U;
    }
    if (fn.t == (u8)FL_CLOSURE || fn.t == (u8)FL_NATIVE) {
        cmd = closure_command_id();
        iarg = 1;
    } else if (fn.t != (u8)FL_NIL) {
        bind_error(ed, "binding target is not callable");
        return 0U;
    }
    desc = sag_cmd_desc(cmd);
    if (desc == NULL) {
        bind_error(ed, "unknown command");
        return 0U;
    }
    candidate.seq = (char *)seq;
    candidate.cmd = (char *)desc->name;
    candidate.sarg = (char *)sarg;
    candidate.iarg = iarg;
    candidate.fn = fn;
    candidate.mode = mode;
    if (!validate_candidate(ed, &candidate))
        return 0U;

    for (slot = binds->free_hint; slot < binds->n; slot++)
        if (!binds->v[slot].active)
            break;
    if (slot == binds->n && binds->n == binds->cap) {
        want = binds->cap == 0U ? 16U : binds->cap * 2U;
        binds->v = sag_xreallocarray(binds->v, want, sizeof(*binds->v));
        binds->cap = want;
    }
    row = &binds->v[slot];
    if (slot < binds->n) {
        free(row->seq);
        free(row->cmd);
        free(row->sarg);
    }
    *row = candidate;
    row->seq = bind_strdup(seq);
    row->cmd = bind_strdup(desc->name);
    row->sarg = sarg == NULL ? NULL : bind_strdup(sarg);
    row->origin = origin;
    row->active = true;
    row->ledger_id = fl_reg_add(&ed->hooks.ledger, origin, REG_BIND,
                                slot + 1U);
    if (fn.t == (u8)FL_CLOSURE || fn.t == (u8)FL_NATIVE)
        row->iarg = (i64)row->ledger_id;
    if (slot == binds->n)
        binds->n++;
    if (slot + 1U > binds->active_hi)
        binds->active_hi = slot + 1U;
    binds->free_hint = slot + 1U;
    while (binds->free_hint < binds->n &&
           binds->v[binds->free_hint].active)
        binds->free_hint++;
    binds->pending = true;
    if (binds->batch_depth == 0U)
        sag_bind_rebuild(ed);
    return row->ledger_id;
}

bool sag_bind_remove(Ed *ed, u32 ledger_id)
{
    SagBindRow *row = row_by_ledger(ed, ledger_id);
    SagBindings *binds;
    u32 slot;

    if (row == NULL)
        return false;
    binds = ed->bindings;
    slot = (u32)(row - binds->v);
    row->active = false;
    row->fn = FL_NIL_V;
    (void)fl_reg_remove(&ed->hooks.ledger, ledger_id);
    if (slot < binds->free_hint)
        binds->free_hint = slot;
    while (binds->active_hi != 0U &&
           !binds->v[binds->active_hi - 1U].active)
        binds->active_hi--;
    binds->pending = true;
    if (binds->batch_depth == 0U)
        sag_bind_rebuild(ed);
    return true;
}

void sag_bind_rebuild(Ed *ed)
{
    SagBindings *binds;
    u32 mode;

    if (ed == NULL || (binds = ed->bindings) == NULL || !binds->pending)
        return;
    for (mode = 0U; mode < (u32)SAG_MODE__N; mode++) {
        BindRow *rows = NULL;
        u32 n = 0U;

        (void)rows_for_mode(ed, (Mode)mode, &rows, &n);
        if (!sag_keymap_build(&ed->bind_keys[mode], "config", rows, n))
            SAG_BUG("validated config keymap no longer builds");
        free(rows);
    }
    binds->pending = false;
    binds->rebuilds++;
    ed->keys.n = 3U;
    ed->keys.l[2] = &ed->bind_keys[ed->mode];
}

void sag_bind_batch_begin(Ed *ed)
{
    if (ed == NULL || ed->bindings == NULL)
        return;
    if (ed->bindings->batch_depth == UINT32_MAX)
        SAG_BUG("bind batch depth overflow");
    ed->bindings->batch_depth++;
}

void sag_bind_batch_end(Ed *ed)
{
    if (ed == NULL || ed->bindings == NULL ||
        ed->bindings->batch_depth == 0U)
        return;
    ed->bindings->batch_depth--;
    if (ed->bindings->batch_depth == 0U)
        sag_bind_rebuild(ed);
}

const char *sag_bind_error(const Ed *ed)
{
    return ed == NULL || ed->bindings == NULL ? "bind subsystem unavailable" :
           ed->bindings->error;
}

u32 sag_bind_active_count(const Ed *ed)
{
    u32 n = 0U;
    u32 i;

    if (ed == NULL || ed->bindings == NULL)
        return 0U;
    for (i = 0U; i < ed->bindings->n; i++)
        if (ed->bindings->v[i].active)
            n++;
    return n;
}

u32 sag_bind_rebuild_count(const Ed *ed)
{
    return ed == NULL || ed->bindings == NULL ? 0U :
           ed->bindings->rebuilds;
}

typedef struct MapListCtx {
    Bytebuf out;
} MapListCtx;

static bool map_list_row(const KeyId *seq, u32 n,
                         const Binding *binding, void *ctx)
{
    MapListCtx *list = ctx;
    const CmdDesc *desc = sag_cmd_desc(binding->cmd);
    char number[32];

    sag_key_format_seq(seq, n, &list->out);
    bytebuf_append(&list->out, "  ", 2U);
    bytebuf_append(&list->out, desc == NULL ? "<unknown>" : desc->name,
                   strlen(desc == NULL ? "<unknown>" : desc->name));
    if (binding->iarg != 0) {
        int used = snprintf(number, sizeof(number), " %lld",
                            (long long)binding->iarg);

        if (used > 0)
            bytebuf_append(&list->out, number, (size_t)used);
    }
    if (binding->sarg != NULL) {
        bytebuf_push_u8(&list->out, (u8)' ');
        bytebuf_append(&list->out, binding->sarg, strlen(binding->sarg));
    }
    bytebuf_push_u8(&list->out, (u8)'\n');
    return true;
}

CmdStatus sag_bind_cmd_map(CmdCtx *cx)
{
    MapListCtx list;

    if (cx == NULL || cx->ed == NULL || cx->ed->mode >= SAG_MODE__N)
        return SAG_CMD_ERR_ARG;
    bytebuf_init(&list.out);
    (void)sag_keymap_visit(&cx->ed->bind_keys[cx->ed->mode],
                           map_list_row, &list);
    if (list.out.len == 0U)
        sag_msg(cx->ed, SAG_MSG_INFO, "no configured bindings for mode %s",
                sag_modes[cx->ed->mode].name);
    else
        sag_msg(cx->ed, SAG_MSG_INFO, "%.*s", (int)list.out.len,
                (const char *)list.out.data);
    bytebuf_free(&list.out);
    return SAG_CMD_OK;
}

static bool get_string(FlVm *vm, FlValue v, const char *what,
                       const FlStr **out)
{
    if (v.t != (u8)FL_STR)
        return fl_raise(vm, "type", "%s must be a string", what);
    *out = (const FlStr *)v.as.o;
    if (memchr((*out)->b, '\0', (*out)->len) != NULL)
        return fl_raise(vm, "type", "%s may not contain NUL", what);
    return true;
}

static bool mode_parse(const FlStr *s, Mode *out)
{
    u32 i;

    for (i = 0U; i < (u32)SAG_MODE__N; i++) {
        if (s->len == strlen(sag_modes[i].name) &&
            memcmp(s->b, sag_modes[i].name, s->len) == 0) {
            *out = (Mode)i;
            return true;
        }
    }
    return false;
}

static char *string_copy(const FlStr *s)
{
    char *copy = sag_xmalloc((size_t)s->len + 1U);

    (void)memcpy(copy, s->b, s->len);
    copy[s->len] = '\0';
    return copy;
}

static bool key_is(const FlStr *s, const char *name)
{
    size_t n = strlen(name);

    return s->len == n && memcmp(s->b, name, n) == 0;
}

static bool bind_args(FlVm *vm, FlValue value, i64 *iarg, char **sarg)
{
    FlMap *map;
    FlValue key;
    FlValue got;
    u32 cursor = 0U;

    if (value.t != (u8)FL_MAP)
        return fl_raise(vm, "type", "bind arguments must be a map");
    map = (FlMap *)value.as.o;
    while (fl_map_iter(map, &cursor, &key, &got)) {
        const FlStr *name;

        if (key.t != (u8)FL_STR)
            return fl_raise(vm, "type", "bind argument names must be strings");
        name = (const FlStr *)key.as.o;
        if (key_is(name, "iarg") || key_is(name, "count")) {
            if (got.t != (u8)FL_INT)
                return fl_raise(vm, "type", "bind iarg must be an integer");
            *iarg = got.as.i;
        } else if (key_is(name, "sarg")) {
            const FlStr *s;

            if (!get_string(vm, got, "bind sarg", &s))
                return false;
            free(*sarg);
            *sarg = string_copy(s);
        } else {
            return fl_raise(vm, "name", "unknown bind argument '%.*s'",
                            (int)name->len, name->b);
        }
    }
    return true;
}

static bool callable_takes_no_args(FlValue fn)
{
    if (fn.t == (u8)FL_CLOSURE)
        return ((const FlClosure *)fn.as.o)->fn->arity == 0U;
    if (fn.t == (u8)FL_NATIVE) {
        const FlNative *native = (const FlNative *)fn.as.o;

        return native->min_ar == 0U;
    }
    return false;
}

static bool add_modes(FlVm *vm, const FlStr *mode_name, const char *seq,
                      CmdId cmd, i64 iarg, const char *sarg, FlValue fn,
                      u32 origin, FlValue *out)
{
    Ed *ed = vm->ed;
    u32 ids[3] = {0U, 0U, 0U};
    Mode modes[3];
    u32 nmode = 1U;
    u32 i;

    if (mode_name->len == 1U && mode_name->b[0] == '*') {
        modes[0] = SAG_MODE_L;
        modes[1] = SAG_MODE_W;
        modes[2] = SAG_MODE_B;
        nmode = 3U;
    } else if (!mode_parse(mode_name, &modes[0])) {
        return fl_raise(vm, "name", "unknown editor mode '%.*s'",
                        (int)mode_name->len, mode_name->b);
    }
    sag_bind_batch_begin(ed);
    for (i = 0U; i < nmode; i++) {
        ids[i] = sag_bind_add(ed, origin, modes[i], seq, cmd, iarg, sarg,
                              fn);
        if (ids[i] == 0U) {
            while (i != 0U)
                (void)sag_bind_remove(ed, ids[--i]);
            sag_bind_batch_end(ed);
            return fl_raise(vm, "user", "bind: %s", sag_bind_error(ed));
        }
    }
    sag_bind_batch_end(ed);
    *out = FL_INT_V((i64)ids[0]);
    return true;
}

bool fl_bind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    const FlStr *mode;
    const FlStr *seq_value;
    const FlStr *command = NULL;
    char *seq;
    char *name = NULL;
    char *sarg = NULL;
    CmdId cmd = SAG_CMD_NONE;
    FlValue fn = FL_NIL_V;
    i64 iarg = 0;
    u32 origin;
    bool ok;

    if (vm == NULL || vm->ed == NULL)
        return vm == NULL ? false :
               fl_raise(vm, "handle", "bind: no editor is attached");
    if ((nargs != 3U && nargs != 4U) ||
        !get_string(vm, args[0], "bind mode", &mode) ||
        !get_string(vm, args[1], "bind sequence", &seq_value))
        return false;
    if (args[2].t == (u8)FL_STR) {
        if (!get_string(vm, args[2], "bind command", &command))
            return false;
        name = string_copy(command);
        cmd = sag_cmd_lookup(name, command->len);
        if (cmd.v == 0U) {
            ok = fl_raise(vm, "name", "unknown command '%s'", name);
            free(name);
            return ok;
        }
    } else if (args[2].t == (u8)FL_CLOSURE ||
               args[2].t == (u8)FL_NATIVE) {
        fn = args[2];
        if (!callable_takes_no_args(fn))
            return fl_raise(vm, "arity", "bound closure must take no arguments");
        if (nargs == 4U)
            return fl_raise(vm, "type", "closure binds take no argument map");
    } else {
        return fl_raise(vm, "type", "bind target must be a command string or function");
    }
    if (nargs == 4U && !bind_args(vm, args[3], &iarg, &sarg)) {
        free(name);
        free(sarg);
        return false;
    }
    origin = fl_origin_of_frame(vm);
    if (origin == FL_ORIGIN_ID_NONE) {
        free(name);
        free(sarg);
        return fl_raise(vm, "handle", "bind: callback has no editor origin");
    }
    seq = string_copy(seq_value);
    ok = add_modes(vm, mode, seq, cmd, iarg, sarg, fn, origin, out);
    free(seq);
    free(name);
    free(sarg);
    return ok;
}

static bool remove_modes(FlVm *vm, const FlStr *mode_name, const char *seq,
                         u32 origin)
{
    Ed *ed = vm->ed;
    Mode modes[3];
    SagBindRow *rows[3];
    u32 nmode = 1U;
    u32 i;

    if (mode_name->len == 1U && mode_name->b[0] == '*') {
        modes[0] = SAG_MODE_L;
        modes[1] = SAG_MODE_W;
        modes[2] = SAG_MODE_B;
        nmode = 3U;
    } else if (!mode_parse(mode_name, &modes[0])) {
        return fl_raise(vm, "name", "unknown editor mode '%.*s'",
                        (int)mode_name->len, mode_name->b);
    }
    for (i = 0U; i < nmode; i++) {
        rows[i] = find_sequence(ed, modes[i], seq);
        if (rows[i] == NULL)
            return fl_raise(vm, "name", "no binding for '%s' in mode %s",
                            seq, sag_modes[modes[i]].name);
        if (rows[i]->origin != origin)
            return fl_raise(vm, "user", "cannot unbind '%s': owned by %s",
                            seq, fl_origin_label(ed, rows[i]->origin));
    }
    sag_bind_batch_begin(ed);
    for (i = 0U; i < nmode; i++)
        (void)sag_bind_remove(ed, rows[i]->ledger_id);
    sag_bind_batch_end(ed);
    return true;
}

bool fl_unbind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    const FlStr *mode;
    const FlStr *seq_value;
    char *seq;
    u32 origin;
    bool ok;

    if (vm == NULL || vm->ed == NULL)
        return vm == NULL ? false :
               fl_raise(vm, "handle", "unbind: no editor is attached");
    if (nargs != 2U || !get_string(vm, args[0], "unbind mode", &mode) ||
        !get_string(vm, args[1], "unbind sequence", &seq_value))
        return false;
    origin = fl_origin_of_frame(vm);
    if (origin == FL_ORIGIN_ID_NONE)
        return fl_raise(vm, "handle", "unbind: callback has no editor origin");
    seq = string_copy(seq_value);
    ok = remove_modes(vm, mode, seq, origin);
    free(seq);
    if (ok)
        *out = FL_NIL_V;
    return ok;
}
