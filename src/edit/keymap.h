#ifndef SAG_EDIT_KEYMAP_H
#define SAG_EDIT_KEYMAP_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "term/input.h"
#include "util/buf.h"
#include "util/vec.h"

#define SAG_CHORD_MAX 8U
#define SAG_LAYER_MAX 32U

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

typedef struct KeyStack {
    const Keymap *l[SAG_LAYER_MAX];
    u32 n;
} KeyStack;

typedef enum {
    SAG_MATCH_NONE,
    SAG_MATCH_PREFIX,
    SAG_MATCH_FULL,
    SAG_MATCH_FULL_PREFIX
} KeyMatch;

typedef bool (*SagKeymapVisitFn)(const KeyId *seq, u32 n,
                                 const Binding *binding, void *ctx);

KeyId sag_keyid(Key key);
u32 sag_key_parse_seq(const char *s, KeyId *out, u32 max);
void sag_key_format_seq(const KeyId *seq, u32 n, Bytebuf *out);

bool sag_keymap_build(Keymap *km, const char *name,
                      const BindRow *rows, u32 n);
void sag_keymap_free(Keymap *km);

bool sag_keymap_step(const Keymap *km, u32 node, KeyId key, u32 *child);
KeyMatch sag_keymap_node_match(const Keymap *km, u32 node,
                               const Binding **binding);
KeyMatch sag_keymap_lookup(const Keymap *km, const KeyId *seq, u32 n,
                           u32 *node, const Binding **binding);

/* Selects the highest layer having any match.  Subsequent chord keys should
 * use sag_keymap_step() on that returned owner, never merge lower layers. */
KeyMatch sag_keystack_lookup(const KeyStack *stack, const KeyId *seq, u32 n,
                             i32 *layer, u32 *node,
                             const Binding **binding);

u32 sag_keymap_binding_count(const Keymap *km);
const Binding *sag_keymap_binding_at(const Keymap *km, u32 index);
bool sag_keymap_visit(const Keymap *km, SagKeymapVisitFn visit, void *ctx);

#endif
