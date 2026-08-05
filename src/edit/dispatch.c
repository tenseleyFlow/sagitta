#include "edit/dispatch.h"

#include "ui/tabs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/keys_default.h"
#include "util/buf.h"
#include "util/log.h"

static void dispatch_reset_chord(Ed *ed)
{
    ed->chord.n = 0U;
    ed->chord.layer = -1;
    ed->chord.count = 0U;
    ed->chord.count_given = false;
    ed->chord.deadline = 0;
}

static void dispatch_reset_capture(Ed *ed)
{
    ed->capture_cmd = SAG_CMD_NONE;
    ed->capture_count = 0U;
    ed->capture_count_given = false;
}

static void dispatch_set_message(Ed *ed, const char *message)
{
    size_t len = strlen(message);

    if (len >= sizeof(ed->dispatch_message))
        len = sizeof(ed->dispatch_message) - 1U;
    memcpy(ed->dispatch_message, message, len);
    ed->dispatch_message[len] = '\0';
}

static void dispatch_set_seq_message(Ed *ed, const char *prefix)
{
    Bytebuf text;
    size_t prefix_len = strlen(prefix);
    size_t len;

    bytebuf_init(&text);
    bytebuf_append(&text, prefix, prefix_len);
    sag_key_format_seq(ed->chord.seq, ed->chord.n, &text);
    len = text.len;
    if (len >= sizeof(ed->dispatch_message))
        len = sizeof(ed->dispatch_message) - 1U;
    if (len != 0U)
        memcpy(ed->dispatch_message, text.data, len);
    ed->dispatch_message[len] = '\0';
    bytebuf_free(&text);
}

static u32 dispatch_timeout_from_env(void)
{
    const char *value = getenv("SAG_CHORD_TIMEOUT_MS");
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || *value == '\0')
        return SAG_CHORD_TIMEOUT_DEFAULT_MS;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL ||
        parsed > UINT32_MAX)
        return SAG_CHORD_TIMEOUT_DEFAULT_MS;
    return (u32)parsed;
}

static void dispatch_arm(Ed *ed, i64 now_ms)
{
    i64 timeout = (i64)ed->chord_timeout_ms;

    if (now_ms > INT64_MAX - timeout)
        ed->chord.deadline = INT64_MAX;
    else
        ed->chord.deadline = now_ms + timeout;
}

static KeyMatch dispatch_current_match(const Ed *ed,
                                       const Binding **binding)
{
    const Keymap *owner;

    if (ed->chord.layer < 0 || (u32)ed->chord.layer >= ed->keys.n)
        return SAG_MATCH_NONE;
    owner = ed->keys.l[ed->chord.layer];
    return sag_keymap_lookup(owner, ed->chord.seq, ed->chord.n, NULL,
                             binding);
}

static void dispatch_fire(Ed *ed, const Binding *selected)
{
    Binding binding = *selected;
    const CmdDesc *desc = sag_cmd_desc(binding.cmd);
    CmdCtx cx;

    if (desc != NULL && (desc->flags & SAG_CMD_CAPTURES_TEXT) != 0U &&
        binding.sarg == NULL) {
        ed->capture_cmd = binding.cmd;
        ed->capture_count = ed->chord.count_given ? ed->chord.count : 1U;
        ed->capture_count_given = ed->chord.count_given;
        dispatch_reset_chord(ed);
        dispatch_set_message(ed, "argument: ");
        return;
    }

    cx = (CmdCtx){0};
    cx.ed = ed;
    cx.win = ed->cmdline.active ? sag_cmdline_target(ed) : ed->win;
    cx.count = ed->chord.count_given ? ed->chord.count : 1U;
    cx.count_given = ed->chord.count_given;
    cx.iarg = binding.iarg;
    cx.sarg = binding.sarg;
    cx.sarg_len = binding.sarg == NULL ? 0U : (u32)strlen(binding.sarg);
    cx.source = SAG_SRC_KEY;
    dispatch_reset_chord(ed);
    ed->last_cmd = binding.cmd;
    ed->last_status = sag_ed_invoke(ed, binding.cmd, &cx);
    ed->dispatch_count++;
}

static bool dispatch_take_count(Ed *ed, KeyId key)
{
    u32 code = (u32)(key.v >> 16U);
    u32 mods = (u32)(key.v & 0xffffU);
    u32 digit;

    if (ed->mode >= SAG_MODE__N || !sag_modes[ed->mode].takes_count ||
        ed->chord.n != 0U || mods != 0U || code < (u32)'0' ||
        code > (u32)'9')
        return false;
    digit = code - (u32)'0';
    if (!ed->chord.count_given && digit == 0U)
        return false;
    ed->chord.count_given = true;
    if (ed->chord.count > (SAG_COUNT_MAX - digit) / 10U) {
        ed->chord.count = SAG_COUNT_MAX;
        dispatch_set_message(ed, "count limited to 99999");
    } else {
        ed->chord.count = ed->chord.count * 10U + digit;
    }
    return true;
}

void sag_dispatch_init(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        SAG_BUG("dispatch init: NULL editor");
    for (i = 0U; i < SAG_MODE__N; i++)
        (void)memset(&ed->mode_keys[i], 0, sizeof(ed->mode_keys[i]));
    (void)memset(&ed->user_keys, 0, sizeof(ed->user_keys));
    (void)memset(&ed->keys, 0, sizeof(ed->keys));
    (void)memset(&ed->chord, 0, sizeof(ed->chord));
    ed->last_cmd = SAG_CMD_NONE;
    ed->dispatch_count = 0U;
    ed->dispatch_message[0] = '\0';
    sag_cmd_init();
    ed->mode = SAG_MODE_L;
    ed->prev_unit = SAG_MODE_L;
    ed->chord.layer = -1;
    ed->chord_timeout_ms = dispatch_timeout_from_env();
    ed->last_status = SAG_CMD_OK;
    dispatch_reset_capture(ed);
    sag_keys_default_install(ed);
}

void sag_dispatch_free(Ed *ed)
{
    u32 i;

    for (i = 0U; i < SAG_MODE__N; i++)
        sag_keymap_free(&ed->mode_keys[i]);
    sag_keymap_free(&ed->user_keys);
    (void)memset(&ed->keys, 0, sizeof(ed->keys));
    dispatch_reset_chord(ed);
    dispatch_reset_capture(ed);
    ed->dispatch_message[0] = '\0';
}

void sag_dispatch_key(Ed *ed, Key key, i64 now_ms)
{
    const Binding *binding = NULL;
    KeyMatch match;
    KeyId id;
    u32 node = 0U;

    if (key.ev == SAG_KEY_RELEASE)
        return;
    /*
     * A confirm run owns the keyboard while it is asking.  It sits
     * ahead of the keymap because y/n/a/q/l are ordinary bindings the
     * rest of the time, and answering "replace this one?" must not also
     * run whatever `a` is bound to.
     */
    /*
     * The dirty-close question owns the keyboard while it is up: `w`
     * and `d` are ordinary bindings the rest of the time, and answering
     * "save changes?" must not also run whatever `d` is bound to.
     */
    if (ed->tab_prompt.active) {
        u8 answer = key.code == SAG_KEY_ESCAPE
                    ? 0x1BU
                    : (key.ntext == 1U ? key.text[0] : 0U);

        if (sag_tab_prompt_key(ed, answer))
            return;
    }
    if (ed->confirm.active) {
        u8 answer = key.code == SAG_KEY_ESCAPE
                    ? 0x1BU
                    : (key.ntext == 1U ? key.text[0] : 0U);

        if (sag_search_confirm_key(ed, answer))
            return;
    }
    if (ed->capture_cmd.v != 0U) {
        CmdCtx cx = {0};
        CmdId cmd = ed->capture_cmd;

        if (key.code == SAG_KEY_ESCAPE) {
            dispatch_reset_capture(ed);
            ed->dispatch_message[0] = '\0';
            return;
        }
        cx.ed = ed;
        cx.win = ed->cmdline.active ? sag_cmdline_target(ed) : ed->win;
        cx.count = ed->capture_count;
        cx.count_given = ed->capture_count_given;
        cx.sarg = (const char *)key.text;
        cx.sarg_len = key.ntext;
        cx.source = SAG_SRC_KEY;
        dispatch_reset_capture(ed);
        if (key.ntext == 0U) {
            dispatch_set_message(ed, "argument needs text");
            ed->last_cmd = cmd;
            ed->last_status = SAG_CMD_ERR_ARG;
            ed->dispatch_count++;
            return;
        }
        ed->dispatch_message[0] = '\0';
        ed->last_cmd = cmd;
        ed->last_status = sag_ed_invoke(ed, cmd, &cx);
        ed->dispatch_count++;
        return;
    }
    id = sag_keyid(key);
    if ((u32)(id.v >> 16U) == SAG_KEY_ESCAPE &&
        (ed->chord.n != 0U || ed->chord.count_given)) {
        dispatch_reset_chord(ed);
        return;
    }
    if (dispatch_take_count(ed, id))
        return;

    if (ed->chord.n == 0U) {
        KeyId seq[1] = {id};
        i32 layer = -1;

        match = sag_keystack_lookup(&ed->keys, seq, 1U, &layer, &node,
                                    &binding);
        if (match == SAG_MATCH_NONE) {
            ed->chord.seq[0] = id;
            ed->chord.n = 1U;
            dispatch_set_seq_message(ed, "unbound: ");
            dispatch_reset_chord(ed);
            return;
        }
        ed->chord.seq[0] = id;
        ed->chord.n = 1U;
        ed->chord.layer = layer;
        if (match == SAG_MATCH_FULL) {
            dispatch_fire(ed, binding);
            return;
        }
        dispatch_arm(ed, now_ms);
        return;
    }

    if (ed->chord.n >= SAG_CHORD_MAX) {
        dispatch_set_seq_message(ed, "unbound sequence: ");
        dispatch_reset_chord(ed);
        return;
    }
    ed->chord.seq[ed->chord.n++] = id;
    match = dispatch_current_match(ed, &binding);
    if (match == SAG_MATCH_FULL) {
        dispatch_fire(ed, binding);
        return;
    }
    if (match == SAG_MATCH_PREFIX || match == SAG_MATCH_FULL_PREFIX) {
        dispatch_arm(ed, now_ms);
        return;
    }

    ed->chord.n--;
    match = dispatch_current_match(ed, &binding);
    if (match == SAG_MATCH_FULL || match == SAG_MATCH_FULL_PREFIX) {
        dispatch_fire(ed, binding);
        sag_dispatch_key(ed, key, now_ms);
        return;
    }
    ed->chord.seq[ed->chord.n++] = id;
    dispatch_set_seq_message(ed, "unbound sequence: ");
    dispatch_reset_chord(ed);
}

void sag_dispatch_tick(Ed *ed, i64 now_ms)
{
    const Binding *binding = NULL;
    KeyMatch match;

    if (ed->chord.n == 0U || ed->chord.deadline == 0 ||
        now_ms < ed->chord.deadline)
        return;
    match = dispatch_current_match(ed, &binding);
    if (match == SAG_MATCH_FULL || match == SAG_MATCH_FULL_PREFIX)
        dispatch_fire(ed, binding);
    else {
        dispatch_set_seq_message(ed, "unbound sequence: ");
        dispatch_reset_chord(ed);
    }
}

i64 sag_dispatch_deadline(const Ed *ed)
{
    return ed->chord.n == 0U || ed->chord.deadline == 0
        ? -1 : ed->chord.deadline;
}

const Chord *sag_dispatch_pending(const Ed *ed)
{
    return &ed->chord;
}

const char *sag_dispatch_owner(const Ed *ed)
{
    if (ed->chord.layer < 0 || (u32)ed->chord.layer >= ed->keys.n)
        return NULL;
    return ed->keys.l[ed->chord.layer]->name;
}

const char *sag_dispatch_message(const Ed *ed)
{
    return ed->dispatch_message;
}

void sag_dispatch_clear_message(Ed *ed)
{
    ed->dispatch_message[0] = '\0';
}

void sag_dispatch_set_mode(Ed *ed, Mode mode)
{
    if (mode >= SAG_MODE__N)
        SAG_BUG("invalid editor mode %u", (u32)mode);
    dispatch_reset_chord(ed);
    dispatch_reset_capture(ed);
    ed->mode = mode;
    ed->keys.l[0] = &ed->mode_keys[mode];
}
