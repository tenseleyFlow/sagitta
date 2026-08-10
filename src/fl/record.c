#include "fl/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/vm.h"
#include "ui/message.h"

static bool named_register(u8 name)
{
    return (name >= (u8)'a' && name <= (u8)'z') ||
           (name >= (u8)'A' && name <= (u8)'Z');
}

static u8 lower_register(u8 name)
{
    return name >= (u8)'A' && name <= (u8)'Z'
               ? (u8)(name - (u8)'A' + (u8)'a')
               : name;
}

static u32 txn_depth(const Ed *ed)
{
    FlVm *vm = sag_fl_vm((Ed *)ed);

    return vm == NULL ? 0U : vm->txn.depth;
}

void sag_record_init(Rec *rec)
{
    if (rec == NULL)
        return;
    (void)memset(rec, 0, sizeof(*rec));
    bytebuf_init(&rec->blob);
    rec->import_ed = true;
}

void sag_record_free(Rec *rec)
{
    if (rec == NULL)
        return;
    RecEventVec_free(&rec->ev);
    bytebuf_free(&rec->blob);
    (void)memset(rec, 0, sizeof(*rec));
}

bool sag_record_start(Ed *ed, u8 reg)
{
    Rec *rec;

    if (ed == NULL || !named_register(reg))
        return false;
    if (txn_depth(ed) != 0U) {
        sag_msg(ed, SAG_MSG_ERROR,
                "cannot start recording inside an edit transaction");
        return false;
    }
    rec = &ed->rec;
    if (rec->active) {
        sag_msg(ed, SAG_MSG_WARN, "already recording @%c", (int)rec->reg);
        return false;
    }
    rec->ev.len = 0U;
    rec->blob.len = 0U;
    rec->reg = lower_register(reg);
    rec->append = reg >= (u8)'A' && reg <= (u8)'Z';
    rec->import_ed = true;
    if (rec->append) {
        const RegVal *current = sag_reg_get(&ed->regs, rec->reg);

        if (current != NULL && current->bytes.len != 0U)
            rec->import_ed = false;
    }
    rec->mode_at_start = (u8)ed->mode;
    rec->txn_depth_at_start = txn_depth(ed);
    rec->in_prompt = false;
    rec->active = true;
    sag_msg(ed, SAG_MSG_INFO, "recording @%c", (int)reg);
    return true;
}

bool sag_record_active(const Ed *ed)
{
    return ed != NULL && ed->rec.active;
}

void sag_record_tap(CmdId id, const CmdCtx *cx)
{
    const CmdDesc *desc;
    RecEvent event;
    Rec *rec;
    const void *sarg;
    u32 sarg_len;
    u8 resolved_reg;

    if (cx == NULL || cx->ed == NULL || cx->source == SAG_SRC_REPLAY)
        return;
    rec = &cx->ed->rec;
    if (!rec->active)
        return;
    desc = sag_cmd_desc(id);
    sarg = cx->sarg;
    sarg_len = cx->sarg_len;
    if (desc != NULL && strcmp(desc->name, "ed.macro.replay_last") == 0) {
        /*
         * replay_last is resolved state, not a stable operation.  Capture
         * the register it means now so stopping this recording (which
         * changes last_reg) cannot turn the emitted macro into self-replay.
         */
        if (!named_register(rec->last_reg))
            return;
        resolved_reg = lower_register(rec->last_reg);
        id = sag_cmd_lookup("ed.macro.replay",
                            (u32)(sizeof("ed.macro.replay") - 1U));
        desc = sag_cmd_desc(id);
        if (id.v == 0U || desc == NULL)
            SAG_BUG("macro replay command is missing from the registry");
        sarg = &resolved_reg;
        sarg_len = 1U;
    }
    if (cx->source != SAG_SRC_CMDLINE &&
        ((desc != NULL && (desc->flags & SAG_CMD_PROMPTS) != 0U) ||
         (desc != NULL && strcmp(desc->name, "ed.mode.enter") == 0 &&
          cx->sarg_len == 1U && cx->sarg != NULL && cx->sarg[0] == 'E'))) {
        rec->in_prompt = true;
        return;
    }
    if (rec->in_prompt && cx->source != SAG_SRC_CMDLINE)
        return;
    if (cx->source == SAG_SRC_CMDLINE)
        rec->in_prompt = false;
    if (rec->blob.len > UINT32_MAX ||
        sarg_len > UINT32_MAX - (u32)rec->blob.len)
        SAG_BUG("macro recorder argument storage overflow");
    event = (RecEvent){id, cx->count, cx->count_given, cx->iarg,
                       (u32)rec->blob.len, sarg_len,
                       (u8)cx->ed->mode, (u8)cx->source};
    if (sarg_len != 0U)
        bytebuf_append(&rec->blob, sarg, sarg_len);
    RecEventVec_push(&rec->ev, event);
}

void sag_record_key(Ed *ed, Key key)
{
    (void)key;
    if (ed == NULL || !ed->rec.active)
        return;
    /* Prompt keystrokes are deliberately not retained.  The resolved
     * command tap records the committed command exactly once. */
    ed->rec.in_prompt = ed->prompt != SAG_PROMPT_NONE || ed->cmdline.active;
}

static void emit_escaped(Bytebuf *out, const u8 *s, u32 len)
{
    u32 i;

    bytebuf_push_u8(out, (u8)'"');
    for (i = 0U; i < len; i++) {
        u8 ch = s[i];

        switch (ch) {
        case '"': bytebuf_append(out, "\\\"", 2U); break;
        case '\\': bytebuf_append(out, "\\\\", 2U); break;
        case '\n': bytebuf_append(out, "\\n", 2U); break;
        case '\r': bytebuf_append(out, "\\r", 2U); break;
        case '\t': bytebuf_append(out, "\\t", 2U); break;
        case '\0': bytebuf_append(out, "\\0", 2U); break;
        default:
            if (ch < 0x20U || ch == 0x7fU)
                bytebuf_printf(out, "\\x%02x", (unsigned)ch);
            else
                bytebuf_push_u8(out, ch);
            break;
        }
    }
    bytebuf_push_u8(out, (u8)'"');
}

static bool event_name(const RecEvent *event, const char *name)
{
    const CmdDesc *desc = sag_cmd_desc(event->cmd);

    return desc != NULL && strcmp(desc->name, name) == 0;
}

static const u8 *event_sarg(const Rec *rec, const RecEvent *event)
{
    static const u8 empty[] = "";

    if ((size_t)event->sarg_at > rec->blob.len ||
        (size_t)event->sarg_len > rec->blob.len - event->sarg_at)
        return empty;
    return rec->blob.data + event->sarg_at;
}

static u32 event_sarg_len(const Rec *rec, const RecEvent *event)
{
    if ((size_t)event->sarg_at > rec->blob.len ||
        (size_t)event->sarg_len > rec->blob.len - event->sarg_at)
        return 0U;
    return event->sarg_len;
}

static bool sarg_is(const Rec *rec, const RecEvent *event, char ch)
{
    const u8 *s = event_sarg(rec, event);

    return event_sarg_len(rec, event) == 1U && s[0] == (u8)ch;
}

static bool special_motion(const Rec *rec, const RecEvent *event)
{
    const CmdDesc *desc = sag_cmd_desc(event->cmd);

    if (desc == NULL || desc->word == NULL)
        return false;
    if (event_name(event, "ed.edit.insert.text"))
        return true;
    if (event->iarg != 0)
        return false;
    if (event_name(event, "ed.mode.enter"))
        return sarg_is(rec, event, 'L') || sarg_is(rec, event, 'W') ||
               sarg_is(rec, event, 'B') || sarg_is(rec, event, 'C') ||
               sarg_is(rec, event, 'H');
    return event_sarg_len(rec, event) == 0U;
}

static bool unit_relative(const RecEvent *event)
{
    return event_name(event, "ed.move.unit.next") ||
           event_name(event, "ed.move.unit.prev") ||
           event_name(event, "ed.move.unit.up") ||
           event_name(event, "ed.move.unit.down") ||
           event_name(event, "ed.move.unit.next_alt") ||
           event_name(event, "ed.move.unit.prev_alt") ||
           event_name(event, "ed.move.unit.up_alt") ||
           event_name(event, "ed.move.unit.down_alt") ||
           event_name(event, "ed.edit.delete.unit");
}

static const char *fixed_token(const Rec *rec, const RecEvent *event)
{
    if (event_name(event, "ed.mode.enter")) {
        if (sarg_is(rec, event, 'L')) return "l";
        if (sarg_is(rec, event, 'W')) return "w";
        if (sarg_is(rec, event, 'B')) return "b";
        if (sarg_is(rec, event, 'C')) return "c";
        if (sarg_is(rec, event, 'H')) return "H(";
    }
    if (event_name(event, "ed.move.unit.next")) return ">";
    if (event_name(event, "ed.move.unit.prev")) return "<";
    if (event_name(event, "ed.move.unit.up")) return "^";
    if (event_name(event, "ed.move.unit.down")) return "v";
    if (event_name(event, "ed.move.unit.next_alt")) return "a>";
    if (event_name(event, "ed.move.unit.prev_alt")) return "a<";
    if (event_name(event, "ed.move.unit.up_alt")) return "a^";
    if (event_name(event, "ed.move.unit.down_alt")) return "av";
    if (event_name(event, "ed.edit.delete.unit")) return "del";
    if (event_name(event, "ed.mode.escape")) return "esc";
    return NULL;
}

static void emit_count(Bytebuf *out, const RecEvent *event, u32 folded)
{
    if (event->count_given)
        bytebuf_printf(out, "%u", (unsigned)(event->count == 0U
                                                  ? 1U : event->count));
    else if (folded > 1U)
        bytebuf_printf(out, "%u", (unsigned)folded);
}

static bool same_foldable(const RecEvent *a, const RecEvent *b)
{
    const CmdDesc *desc = sag_cmd_desc(a->cmd);

    return desc != NULL && (desc->flags & SAG_CMD_REPEATABLE) != 0U &&
           !a->count_given && !b->count_given && a->cmd.v == b->cmd.v &&
           a->iarg == b->iarg && a->sarg_len == 0U && b->sarg_len == 0U;
}

static size_t emit_motion_event(const Rec *rec, size_t at, size_t end,
                                Bytebuf *out, u32 *highlight_depth)
{
    const RecEvent *event = &rec->ev.data[at];
    const CmdDesc *desc = sag_cmd_desc(event->cmd);
    const char *token = fixed_token(rec, event);
    u32 folded = 1U;

    if (event_name(event, "ed.edit.insert.text")) {
        emit_count(out, event, 1U);
        bytebuf_push_u8(out, (u8)'i');
        bytebuf_push_u8(out, (u8)'"');
        if (event->count_given) {
            const u8 *s = event_sarg(rec, event);
            u32 len = event_sarg_len(rec, event);
            u32 j;

            for (j = 0U; j < len; j++) {
                u8 ch = s[j];
                if (ch == '"' || ch == '\\') {
                    bytebuf_push_u8(out, (u8)'\\');
                    bytebuf_push_u8(out, ch);
                } else if (ch == '\n') bytebuf_append(out, "\\n", 2U);
                else if (ch == '\r') bytebuf_append(out, "\\r", 2U);
                else if (ch == '\t') bytebuf_append(out, "\\t", 2U);
                else if (ch == '\0') bytebuf_append(out, "\\0", 2U);
                else if (ch < 0x20U || ch == 0x7fU)
                    bytebuf_printf(out, "\\x%02x", (unsigned)ch);
                else bytebuf_push_u8(out, ch);
            }
            bytebuf_push_u8(out, (u8)'"');
            return at + 1U;
        }
        while (at < end && event_name(&rec->ev.data[at],
                                      "ed.edit.insert.text") &&
               !rec->ev.data[at].count_given) {
            const RecEvent *part = &rec->ev.data[at];
            const u8 *s = event_sarg(rec, part);
            u32 i;

            for (i = 0U; i < event_sarg_len(rec, part); i++) {
                u8 ch = s[i];
                if (ch == '"' || ch == '\\') {
                    bytebuf_push_u8(out, (u8)'\\');
                    bytebuf_push_u8(out, ch);
                } else if (ch == '\n') bytebuf_append(out, "\\n", 2U);
                else if (ch == '\r') bytebuf_append(out, "\\r", 2U);
                else if (ch == '\t') bytebuf_append(out, "\\t", 2U);
                else if (ch == '\0') bytebuf_append(out, "\\0", 2U);
                else if (ch < 0x20U || ch == 0x7fU)
                    bytebuf_printf(out, "\\x%02x", (unsigned)ch);
                else bytebuf_push_u8(out, ch);
            }
            at++;
        }
        bytebuf_push_u8(out, (u8)'"');
        return at;
    }
    while (at + folded < end &&
           same_foldable(event, &rec->ev.data[at + folded]))
        folded++;
    emit_count(out, event, folded);
    if (event_name(event, "ed.mode.enter") &&
        sarg_is(rec, event, 'H')) {
        bytebuf_append(out, "H(", 2U);
        (*highlight_depth)++;
    } else if (event_name(event, "ed.mode.escape") &&
               *highlight_depth != 0U) {
        bytebuf_push_u8(out, (u8)')');
        (*highlight_depth)--;
    } else if (token != NULL)
        bytebuf_append(out, token, strlen(token));
    else if (desc != NULL && desc->word != NULL)
        bytebuf_append(out, desc->word, strlen(desc->word));
    else
        SAG_BUG("recordable command has no motion word");
    return at + folded;
}

static bool needs_annotation(const Rec *rec)
{
    size_t i;

    for (i = 0U; i < rec->ev.len; i++)
        if (unit_relative(&rec->ev.data[i]))
            return true;
    return false;
}

static void emit_motion_segment(const Rec *rec, size_t lo, size_t hi,
                                Bytebuf *out, u32 segment,
                                bool annotate)
{
    size_t i = lo;
    u32 highlight_depth = 0U;

    bytebuf_printf(out, "  macro rec_%c", (int)rec->reg);
    if (segment != 0U)
        bytebuf_printf(out, "_%u", (unsigned)segment);
    bytebuf_append(out, " = @[", 5U);
    if (annotate) {
        const char *unit = rec->mode_at_start == (u8)SAG_MODE_W ? "w" :
                           rec->mode_at_start == (u8)SAG_MODE_B ? "b" : "l";
        bytebuf_push_u8(out, (u8)' ');
        bytebuf_append(out, unit, 1U);
    }
    while (i < hi) {
        bytebuf_push_u8(out, (u8)' ');
        i = emit_motion_event(rec, i, hi, out, &highlight_depth);
    }
    while (highlight_depth-- != 0U)
        bytebuf_push_u8(out, (u8)')');
    bytebuf_append(out, " ];\n  rec_", sizeof(" ];\n  rec_") - 1U);
    bytebuf_push_u8(out, rec->reg);
    if (segment != 0U)
        bytebuf_printf(out, "_%u", (unsigned)segment);
    bytebuf_append(out, "();\n", 4U);
}

static void emit_run(const Rec *rec, const RecEvent *event, Bytebuf *out)
{
    const CmdDesc *desc = sag_cmd_desc(event->cmd);
    bool need_comma = false;

    /* Fuzzed/corrupt event ids still emit a parseable no-op program. */
    bytebuf_append(out, "  ed.run(", 9U);
    emit_escaped(out, (const u8 *)(desc == NULL ? "ed.nop" : desc->name),
                 (u32)strlen(desc == NULL ? "ed.nop" : desc->name));
    bytebuf_append(out, ", {", 3U);
    if (event->count_given) {
        bytebuf_printf(out, " count: %u",
                       (unsigned)(event->count == 0U ? 1U : event->count));
        need_comma = true;
    }
    if (event->iarg != 0) {
        bytebuf_printf(out, "%s iarg: %lld", need_comma ? "," : "",
                       (long long)event->iarg);
        need_comma = true;
    }
    if (event_sarg_len(rec, event) != 0U) {
        if (need_comma)
            bytebuf_append(out, ", sarg: ", sizeof(", sarg: ") - 1U);
        else
            bytebuf_append(out, " sarg: ", sizeof(" sarg: ") - 1U);
        emit_escaped(out, event_sarg(rec, event),
                     event_sarg_len(rec, event));
    }
    bytebuf_append(out, " });\n", 5U);
}

void sag_record_emit(const Rec *rec, const Ed *ed, Bytebuf *out)
{
    size_t i = 0U;
    u32 segment = 0U;
    bool annotate;
    (void)ed;

    if (rec == NULL || out == NULL)
        return;
    bytebuf_append(out, "# sagitta-macro: 1\n", 19U);
    bytebuf_printf(out, "# recorded-with: sagitta %s\n", SAG_VERSION);
    bytebuf_append(out, "# keymap: default\n", 18U);
    if (rec->import_ed)
        bytebuf_append(out, "import ed\n", sizeof("import ed\n") - 1U);
    bytebuf_append(out, "(fn() {\n", 8U);
    annotate = needs_annotation(rec);
    while (i < rec->ev.len) {
        size_t lo;

        if (!special_motion(rec, &rec->ev.data[i])) {
            emit_run(rec, &rec->ev.data[i], out);
            i++;
            continue;
        }
        lo = i;
        while (i < rec->ev.len && special_motion(rec, &rec->ev.data[i]))
            i++;
        emit_motion_segment(rec, lo, i, out, segment, annotate);
        annotate = false;
        segment++;
    }
    bytebuf_append(out, "})();\n", 6U);
}

CmdStatus sag_record_stop(Ed *ed)
{
    Bytebuf source;
    CmdStatus status;
    u32 nevents;

    if (ed == NULL || !ed->rec.active)
        return SAG_CMD_ERR_STATE;
    if (txn_depth(ed) != 0U) {
        sag_msg(ed, SAG_MSG_ERROR,
                "cannot stop recording inside an edit transaction");
        return SAG_CMD_ERR_STATE;
    }
    ed->rec.active = false;
    ed->rec.in_prompt = false;
    bytebuf_init(&source);
    sag_record_emit(&ed->rec, ed, &source);
    status = sag_flapi_reg_write(ed, ed->rec.reg, source.data,
                                 (u32)source.len, ed->rec.append);
    nevents = (u32)ed->rec.ev.len;
    if (status == SAG_CMD_OK) {
        ed->rec.last_reg = ed->rec.reg;
        sag_msg(ed, SAG_MSG_INFO, "recorded @%c (%u events, %u bytes)",
                (int)ed->rec.reg, (unsigned)nevents,
                (unsigned)source.len);
    }
    bytebuf_free(&source);
    return status;
}

static bool replay_one(Ed *ed, FlFn *fn)
{
    FlVm *vm = sag_fl_vm(ed);
    bool split_run;
    bool ok;

    if (vm == NULL)
        return false;
    split_run = vm->txn.entry_active &&
                fl_runtime_cmd_source(vm) != SAG_SRC_REPLAY;
    if (split_run) {
        if (vm->txn.depth != 0U) {
            sag_msg(ed, SAG_MSG_ERROR,
                    "cannot replay a macro inside an edit transaction");
            return false;
        }
        if (!vm->host->edit_begin(vm))
            return false;
    }
    ok = fl_call_chunk(ed->fl, fn, SAG_SRC_REPLAY);
    if (split_run && !vm->host->edit_end(vm, ok))
        ok = false;
    return ok;
}

u32 sag_record_status(const Ed *ed, RecStatus *out)
{
    if (ed == NULL || out == NULL)
        return 0U;
    out->active = ed->rec.active;
    out->reg = ed->rec.reg;
    out->nevents = (u32)ed->rec.ev.len;
    out->last_reg = ed->rec.last_reg;
    return 1U;
}

CmdStatus sag_macro_replay(Ed *ed, u8 reg, u32 count)
{
    const RegVal *value;
    FlFn *fn;
    u32 i;

    if (ed == NULL || !named_register(reg) || count == 0U)
        return SAG_CMD_ERR_ARG;
    reg = lower_register(reg);
    value = sag_reg_get(&ed->regs, reg);
    if (value == NULL || value->bytes.len == 0U) {
        sag_msg(ed, SAG_MSG_ERROR, "macro @%c is empty", (int)reg);
        return SAG_CMD_ERR_ARG;
    }
    fn = fl_macro_compile_cached(ed->fl, reg, value->bytes.data,
                                 value->bytes.len);
    if (fn == NULL)
        return SAG_CMD_ERR_ARG;
    for (i = 0U; i < count; i++)
        if (!replay_one(ed, fn))
            return SAG_CMD_ERR_STATE;
    ed->rec.last_reg = reg;
    return SAG_CMD_OK;
}

u32 sag_macro_list(const Ed *ed, RegInfo *out, u32 max)
{
    u32 found = 0U;
    u8 reg;

    if (ed == NULL)
        return 0U;
    for (reg = (u8)'a'; reg <= (u8)'z'; reg++) {
        const RegVal *value = sag_reg_get((Registers *)&ed->regs, reg);

        if (value == NULL || value->bytes.len == 0U)
            continue;
        if (out != NULL && found < max) {
            out[found].type = value->type;
            out[found].ragged = value->ragged;
            out[found].width = value->width;
            out[found].rows = (u32)value->rows.len;
            out[found].bytes = value->bytes.len;
            out[found].t_wall = value->t_wall;
        }
        found++;
    }
    return found;
}

static bool command_register(const CmdCtx *cx, u8 *reg)
{
    if (cx->sarg == NULL || cx->sarg_len != 1U ||
        !named_register((u8)cx->sarg[0]))
        return false;
    *reg = (u8)cx->sarg[0];
    return true;
}

CmdStatus sag_record_cmd_record(CmdCtx *cx)
{
    u8 reg;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->ed->rec.active && cx->sarg_len == 0U)
        return sag_record_stop(cx->ed);
    if (!command_register(cx, &reg)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "record requires register a-z/A-Z");
        return SAG_CMD_ERR_ARG;
    }
    return sag_record_start(cx->ed, reg) ? SAG_CMD_OK : SAG_CMD_ERR_STATE;
}

CmdStatus sag_record_cmd_stop(CmdCtx *cx)
{
    return cx == NULL || cx->ed == NULL ? SAG_CMD_ERR_ARG
                                        : sag_record_stop(cx->ed);
}

CmdStatus sag_record_cmd_replay(CmdCtx *cx)
{
    u8 reg;

    if (cx == NULL || cx->ed == NULL || !command_register(cx, &reg))
        return SAG_CMD_ERR_ARG;
    /* The registry's repeat loop owns count expansion. */
    return sag_macro_replay(cx->ed, reg, 1U);
}

CmdStatus sag_record_cmd_replay_last(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->ed->rec.last_reg == 0U) {
        if (cx != NULL && cx->ed != NULL)
            sag_msg(cx->ed, SAG_MSG_ERROR, "no previous macro replay");
        return SAG_CMD_ERR_STATE;
    }
    return sag_macro_replay(cx->ed, cx->ed->rec.last_reg, 1U);
}

CmdStatus sag_record_cmd_list(CmdCtx *cx)
{
    u32 count;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    count = sag_macro_list(cx->ed, NULL, 0U);
    sag_msg(cx->ed, SAG_MSG_INFO, "%u macro register%s", (unsigned)count,
            count == 1U ? "" : "s");
    return SAG_CMD_OK;
}

CmdStatus sag_record_cmd_repeat(CmdCtx *cx)
{
    if (cx != NULL && cx->ed != NULL)
        sag_msg(cx->ed, SAG_MSG_ERROR,
                "ed.repeat requires retained resolved arguments; Sprint 35 "
                "does not retain them outside recordings");
    return SAG_CMD_ERR_STATE;
}
