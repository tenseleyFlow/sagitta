#ifndef YEW_EDIT_KEYMAP_H
#define YEW_EDIT_KEYMAP_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "term/input.h"
#include "util/buf.h"
#include "util/vec.h"

#define YEW_CHORD_MAX 8U
#define YEW_LAYER_MAX 32U

typedef struct {
    u64 v;
} KeyId;

typedef struct {
    CmdId cmd;
    i64 iarg;
    const char *sarg;
} Binding;

typedef struct {
    u64 key;
    u32 child;
} KeyEdge;

typedef struct {
    u32 edge_lo;
    u32 edge_n;
    u32 bind;
} KeyNode;

VEC_DECL(KeyNodeVec, KeyNode);
VEC_DECL(KeyEdgeVec, KeyEdge);
VEC_DECL(BindingVec, Binding);

typedef struct Keymap {
    KeyNodeVec nodes;
    KeyEdgeVec edges;
    BindingVec binds;
    const char *name;
} Keymap;

typedef struct BindRow {
    const char *seq;
    const char *cmd;
    i64 iarg;
    const char *sarg;
} BindRow;

typedef enum YewKeymapError {
    YEW_KEYMAP_ERR_NONE = 0,
    YEW_KEYMAP_ERR_SEQUENCE,
    YEW_KEYMAP_ERR_COMMAND,
    YEW_KEYMAP_ERR_DUPLICATE,
    YEW_KEYMAP_ERR_ARITY,
    YEW_KEYMAP_ERR_ESCAPE_PREFIX,
    YEW_KEYMAP_ERR_TOO_LONG
} YewKeymapError;

typedef struct YewKeymapDiag {
    u32 row;
    YewKeymapError error;
} YewKeymapDiag;

typedef struct KeyStack {
    const Keymap *l[YEW_LAYER_MAX];
    u32 n;
} KeyStack;

typedef enum {
    YEW_MATCH_NONE,
    YEW_MATCH_PREFIX,
    YEW_MATCH_FULL,
    YEW_MATCH_FULL_PREFIX
} KeyMatch;

typedef bool (*YewKeymapVisitFn)(const KeyId *seq, u32 n,
                                 const Binding *binding, void *ctx);

KeyId yew_keyid(Key key);
u32 yew_key_parse_seq(const char *s, KeyId *out, u32 max);
void yew_key_format_seq(const KeyId *seq, u32 n, Bytebuf *out);

bool yew_keymap_build(Keymap *km, const char *name,
                      const BindRow *rows, u32 n);
bool yew_keymap_build_diag(Keymap *km, const char *name,
                           const BindRow *rows, u32 n,
                           YewKeymapDiag *diag);
const char *yew_keymap_error_string(YewKeymapError error);
YewKeymapError yew_keymap_validate_row(const BindRow *row);
void yew_keymap_free(Keymap *km);

bool yew_keymap_step(const Keymap *km, u32 node, KeyId key, u32 *child);
KeyMatch yew_keymap_node_match(const Keymap *km, u32 node,
                               const Binding **binding);
KeyMatch yew_keymap_lookup(const Keymap *km, const KeyId *seq, u32 n,
                           u32 *node, const Binding **binding);

/* Selects the highest layer having any match.  Subsequent chord keys should
 * use yew_keymap_step() on that returned owner, never merge lower layers. */
KeyMatch yew_keystack_lookup(const KeyStack *stack, const KeyId *seq, u32 n,
                             i32 *layer, u32 *node,
                             const Binding **binding);

u32 yew_keymap_binding_count(const Keymap *km);
const Binding *yew_keymap_binding_at(const Keymap *km, u32 index);
bool yew_keymap_visit(const Keymap *km, YewKeymapVisitFn visit, void *ctx);

#endif
