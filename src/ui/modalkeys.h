#ifndef YEW_UI_MODALKEYS_H
#define YEW_UI_MODALKEYS_H

#include "edit/keymap.h"
#include "term/input.h"

typedef struct Ed Ed;

typedef struct ModalOpener {
    KeyId seq[YEW_CHORD_MAX];
    u8 n;
} ModalOpener;

typedef enum ModalKind {
    YEW_MODAL_NONE,
    YEW_MODAL_INPUT,
    YEW_MODAL_PICKER,
    YEW_MODAL_GROUP,
    YEW_MODAL_CONTEXT,
    YEW_MODAL_PROMPT,
    YEW_MODAL_TAB,
    YEW_MODAL_WORKSPACE,
    YEW_MODAL_RENAME,
    YEW_MODAL__N
} ModalKind;

typedef struct ModalKeys {
    ModalOpener origin;
    ModalOpener owner[YEW_MODAL__N];
    Key pending[YEW_CHORD_MAX];
    u8 n;
    ModalKind pending_owner;
    i64 deadline;
    u64 generation;
} ModalKeys;

typedef void (*ModalKeyHandler)(Ed *ed, Key key, i64 now_ms);

/* Call at every new dialog lifetime, even when no shortcut opened it. */
void yew_modal_keys_open(Ed *ed, ModalKind kind);
void yew_modal_keys_route(Ed *ed, Key key, i64 now_ms, ModalKeyHandler handle);
void yew_modal_keys_tick(Ed *ed, i64 now_ms, ModalKeyHandler handle);
/* False if replay changed ownership; a following paste must not leak. */
bool yew_modal_keys_flush(Ed *ed, i64 now_ms, ModalKeyHandler handle);
i64 yew_modal_keys_deadline(const Ed *ed);

#endif
