#include "ui/modalkeys.h"

#include "edit/ed.h"
#include "mod/lsp/lsp.h"
#include "ui/ctxmenu.h"
#include "ui/grouppicker.h"
#include "ui/picker.h"

static ModalKind active(const Ed *ed)
{
    if (ed->cmdline.active && ed->cmdline.modal)
        return YEW_MODAL_INPUT;
    if (yew_ctx_active())
        return YEW_MODAL_CONTEXT;
    if (ed->prompt != YEW_PROMPT_NONE)
        return YEW_MODAL_PROMPT;
    if (yew_gp_active())
        return YEW_MODAL_GROUP;
    if (yew_picker_active(ed))
        return YEW_MODAL_PICKER;
    if (yew_lsp_rename_confirm_active(ed))
        return YEW_MODAL_RENAME;
    if (ed->cmdline.active && ed->cmdline.kind == YEW_PROMPT_INPUT)
        return YEW_MODAL_INPUT;
    if (ed->tab_prompt.active)
        return YEW_MODAL_TAB;
    if (ed->ws_prompt.active)
        return YEW_MODAL_WORKSPACE;
    return YEW_MODAL_NONE;
}

static void clear_pending(ModalKeys *keys)
{
    keys->n = 0U;
    keys->deadline = 0;
    keys->pending_owner = YEW_MODAL_NONE;
}

void yew_modal_keys_open(Ed *ed, ModalKind kind)
{
    if (ed == NULL || kind <= YEW_MODAL_NONE || kind >= YEW_MODAL__N)
        return;
    ed->modal_keys.owner[kind] = ed->modal_keys.origin;
    ed->modal_keys.generation++;
    clear_pending(&ed->modal_keys);
}

/* Carry the opener across synchronous picker/input callbacks. Dispatch
 * overrides it when an actual new shortcut, rather than Enter, runs. */
static void deliver(Ed *ed, Key key, i64 now_ms, ModalKeyHandler handle)
{
    ModalOpener saved = ed->modal_keys.origin;
    ModalKind kind = active(ed);

    ed->modal_keys.origin = ed->modal_keys.owner[kind];
    handle(ed, key, now_ms);
    ed->modal_keys.origin = saved;
}

static void replay(Ed *ed, i64 now_ms, ModalKeyHandler handle)
{
    Key held[YEW_CHORD_MAX];
    ModalKeys *keys = &ed->modal_keys;
    ModalKind owner = keys->pending_owner;
    u64 generation = keys->generation;
    u8 n = keys->n;
    u8 i;

    for (i = 0U; i < n; i++)
        held[i] = keys->pending[i];
    clear_pending(keys);
    for (i = 0U; i < n && active(ed) == owner &&
                 keys->generation == generation; i++)
        deliver(ed, held[i], now_ms, handle);
}

void yew_modal_keys_route(Ed *ed, Key key, i64 now_ms, ModalKeyHandler handle)
{
    ModalKeys *keys = &ed->modal_keys;
    ModalKind kind = active(ed);
    ModalOpener opener = keys->owner[kind];
    KeyId id;
    u64 generation = keys->generation;

    if (keys->n != 0U && keys->pending_owner != kind)
        clear_pending(keys);
    if (keys->n != 0U && now_ms >= keys->deadline) {
        replay(ed, now_ms, handle);
        if (active(ed) != kind || keys->generation != generation)
            return;
    }
    if (kind == YEW_MODAL_NONE || opener.n == 0U) {
        deliver(ed, key, now_ms, handle);
        return;
    }
    if (key.ev == YEW_KEY_RELEASE)
        return;
    id = yew_keyid(key);
    if (key.ev != YEW_KEY_PRESS) {
        /* Holding the opening key must not immediately close its dialog. */
        if (id.v != opener.seq[0].v && keys->n == 0U)
            deliver(ed, key, now_ms, handle);
        return;
    }
    if (id.v != opener.seq[keys->n].v) {
        replay(ed, now_ms, handle);
        if (active(ed) != kind || keys->generation != generation)
            return;
        if (id.v != opener.seq[0].v) {
            deliver(ed, key, now_ms, handle);
            return;
        }
    }
    keys->pending[keys->n++] = key;
    keys->pending_owner = kind;
    if (keys->n == opener.n) {
        Key cancel = {0};

        clear_pending(keys);
        cancel.kind = YEW_EV_KEY;
        cancel.ev = YEW_KEY_PRESS;
        cancel.code = YEW_KEY_ESCAPE;
        deliver(ed, cancel, now_ms, handle);
    } else {
        keys->deadline = now_ms > INT64_MAX - ed->chord_timeout_ms ?
                             INT64_MAX : now_ms + ed->chord_timeout_ms;
    }
}

void yew_modal_keys_tick(Ed *ed, i64 now_ms, ModalKeyHandler handle)
{
    if (ed->modal_keys.n != 0U && now_ms >= ed->modal_keys.deadline)
        replay(ed, now_ms, handle);
}

bool yew_modal_keys_flush(Ed *ed, i64 now_ms, ModalKeyHandler handle)
{
    ModalKind before = active(ed);
    u64 generation = ed->modal_keys.generation;

    if (ed->modal_keys.n != 0U)
        replay(ed, now_ms, handle);
    return before == active(ed) && generation == ed->modal_keys.generation;
}

i64 yew_modal_keys_deadline(const Ed *ed)
{
    return ed->modal_keys.n == 0U ? -1 : ed->modal_keys.deadline;
}
