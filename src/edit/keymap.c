#include "edit/keymap.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "util/base.h"
#include "util/sort.h"

enum {
    KEY_MODS = SAG_MOD_CTRL | SAG_MOD_ALT | SAG_MOD_SHIFT | SAG_MOD_SUPER
};

typedef struct {
    u64 key;
    u32 child;
} ScratchEdge;

VEC_DECL(ScratchEdgeVec, ScratchEdge);

typedef struct {
    ScratchEdgeVec edges;
    u32 bind;
} ScratchNode;

VEC_DECL(ScratchNodeVec, ScratchNode);

typedef struct {
    const char *name;
    u32 code;
} KeyName;

static const KeyName key_names[] = {
    {"left", SAG_KEY_LEFT},       {"right", SAG_KEY_RIGHT},
    {"up", SAG_KEY_UP},           {"down", SAG_KEY_DOWN},
    {"esc", SAG_KEY_ESCAPE},      {"cr", SAG_KEY_ENTER},
    {"tab", SAG_KEY_TAB},         {"bs", SAG_KEY_BACKSPACE},
    {"del", SAG_KEY_DELETE},      {"space", (u32)' '},
    {"home", SAG_KEY_HOME},       {"end", SAG_KEY_END},
    {"pgup", SAG_KEY_PAGE_UP},    {"pgdn", SAG_KEY_PAGE_DOWN},
    {"ins", SAG_KEY_INSERT},      {"f1", SAG_KEY_F1},
    {"f2", SAG_KEY_F2},           {"f3", SAG_KEY_F3},
    {"f4", SAG_KEY_F4},           {"f5", SAG_KEY_F5},
    {"f6", SAG_KEY_F6},           {"f7", SAG_KEY_F7},
    {"f8", SAG_KEY_F8},           {"f9", SAG_KEY_F9},
    {"f10", SAG_KEY_F10},         {"f11", SAG_KEY_F11},
    {"f12", SAG_KEY_F12},         {"lt", (u32)'<'}
};

static char *keymap_strdup(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = sag_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

static bool text_control(const Key *key)
{
    return key->code < 32U || key->code == 127U;
}

KeyId sag_keyid(Key key)
{
    u32 code = key.code;
    u16 mods = (u16)(key.mods & KEY_MODS);
    KeyId id;

    if (key.ev == SAG_KEY_RELEASE) {
        id.v = 0U;
        return id;
    }
    if (code >= 1U && code <= 26U) {
        code = (u32)'a' + code - 1U;
        mods = (u16)(mods | SAG_MOD_CTRL);
    }
    if ((mods & SAG_MOD_CTRL) != 0U && code >= (u32)'A' &&
        code <= (u32)'Z')
        code += (u32)'a' - (u32)'A';
    if (key.ntext != 0U && !text_control(&key))
        mods = (u16)(mods & (u16)~SAG_MOD_SHIFT);
    id.v = ((u64)code << 16) | (u64)mods;
    return id;
}

static bool key_name_parse(const char *s, size_t n, u32 *code)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(key_names); i++) {
        size_t name_n = strlen(key_names[i].name);

        if (name_n == n && memcmp(key_names[i].name, s, n) == 0) {
            *code = key_names[i].code;
            return true;
        }
    }
    return false;
}

static const char *key_name_format(u32 code)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(key_names); i++) {
        if (key_names[i].code == code)
            return key_names[i].name;
    }
    return NULL;
}

static bool parse_atom(const char *s, size_t n, KeyId *out)
{
    static const struct {
        char letter;
        u16 bit;
    } prefixes[] = {{'C', SAG_MOD_CTRL}, {'A', SAG_MOD_ALT},
                    {'S', SAG_MOD_SHIFT}, {'M', SAG_MOD_SUPER}};
    size_t pos = 0U;
    size_t prefix;
    u16 mods = 0U;
    u32 code;

    for (prefix = 0U; prefix < SAG_ARRAY_LEN(prefixes); prefix++) {
        if (pos + 2U <= n && s[pos] == prefixes[prefix].letter &&
            s[pos + 1U] == '-') {
            mods = (u16)(mods | prefixes[prefix].bit);
            pos += 2U;
        }
    }
    if (pos == n)
        return false;
    /* A modifier which appears out of canonical order is not a key atom. */
    if (pos + 2U <= n && s[pos + 1U] == '-' &&
        (s[pos] == 'C' || s[pos] == 'A' || s[pos] == 'S' || s[pos] == 'M'))
        return false;
    if (s[pos] == '<') {
        if (n - pos < 3U || s[n - 1U] != '>' ||
            !key_name_parse(s + pos + 1U, n - pos - 2U, &code))
            return false;
    } else {
        size_t used = sag_utf8_decode((const u8 *)s + pos, n - pos, &code);

        if (used != n - pos || sag_utf8_is_escape(code) || code < 33U ||
            code == 127U || code == (u32)'<')
            return false;
    }
    out->v = ((u64)code << 16) | (u64)mods;
    return true;
}

u32 sag_key_parse_seq(const char *s, KeyId *out, u32 max)
{
    const char *p = s;
    u32 n = 0U;

    if (!s || !out || max == 0U || *s == '\0')
        return 0U;
    while (*p != '\0') {
        const char *start;

        while (*p == ' ')
            p++;
        if (*p == '\0')
            return 0U;
        start = p;
        while (*p != '\0' && *p != ' ')
            p++;
        if (n == max || !parse_atom(start, (size_t)(p - start), &out[n]))
            return 0U;
        n++;
    }
    return n;
}

void sag_key_format_seq(const KeyId *seq, u32 n, Bytebuf *out)
{
    static const struct {
        u16 bit;
        const char *prefix;
    } prefixes[] = {{SAG_MOD_CTRL, "C-"}, {SAG_MOD_ALT, "A-"},
                    {SAG_MOD_SHIFT, "S-"}, {SAG_MOD_SUPER, "M-"}};
    u32 i;

    for (i = 0U; i < n; i++) {
        u16 mods = (u16)(seq[i].v & 0xFFFFU);
        u32 code = (u32)(seq[i].v >> 16);
        const char *name;
        size_t j;

        if (i != 0U)
            bytebuf_push_u8(out, (u8)' ');
        for (j = 0U; j < SAG_ARRAY_LEN(prefixes); j++) {
            if ((mods & prefixes[j].bit) != 0U)
                bytebuf_append(out, prefixes[j].prefix, 2U);
        }
        name = key_name_format(code);
        if (name) {
            bytebuf_push_u8(out, (u8)'<');
            bytebuf_append(out, name, strlen(name));
            bytebuf_push_u8(out, (u8)'>');
        } else {
            u8 encoded[SAG_UTF8_MAX];
            size_t encoded_n = sag_utf8_encode(code, encoded);

            if (encoded_n != 0U)
                bytebuf_append(out, encoded, encoded_n);
        }
    }
}

static int scratch_edge_cmp(const void *a, const void *b, void *ctx)
{
    const ScratchEdge *ea = a;
    const ScratchEdge *eb = b;

    (void)ctx;
    return ea->key < eb->key ? -1 : ea->key > eb->key;
}

static u32 scratch_child(ScratchNodeVec *nodes, u32 parent, u64 key)
{
    size_t i;

    for (i = 0U; i < nodes->data[parent].edges.len; i++) {
        if (nodes->data[parent].edges.data[i].key == key)
            return nodes->data[parent].edges.data[i].child;
    }
    {
        ScratchNode node = {0};
        ScratchEdge edge;

        ScratchNodeVec_push(nodes, node);
        edge.key = key;
        edge.child = (u32)(nodes->len - 1U);
        ScratchEdgeVec_push(&nodes->data[parent].edges, edge);
        return edge.child;
    }
}

static void scratch_free(ScratchNodeVec *nodes)
{
    size_t i;

    for (i = 0U; i < nodes->len; i++)
        ScratchEdgeVec_free(&nodes->data[i].edges);
    ScratchNodeVec_free(nodes);
}

static bool binding_arity_ok(const CmdDesc *desc, const BindRow *row)
{
    switch ((CmdArity)desc->arity) {
    case SAG_ARITY_NONE:
        return row->iarg == 0 && row->sarg == NULL;
    case SAG_ARITY_INT:
    case SAG_ARITY_OPT_INT:
        return row->sarg == NULL;
    case SAG_ARITY_STR:
        return row->iarg == 0 && row->sarg != NULL;
    case SAG_ARITY_OPT_STR:
        return row->iarg == 0;
    }
    return false;
}

static bool add_row(Keymap *out, ScratchNodeVec *scratch,
                    const BindRow *row)
{
    KeyId seq[SAG_CHORD_MAX];
    u32 n = sag_key_parse_seq(row->seq, seq, SAG_CHORD_MAX);
    CmdId cmd;
    const CmdDesc *desc;
    u32 node = 0U;
    u32 i;
    Binding binding;

    if (n == 0U)
        return false;
    cmd = sag_cmd_lookup(row->cmd, (u32)strlen(row->cmd));
    desc = sag_cmd_desc(cmd);
    if (cmd.v == 0U || !desc || !binding_arity_ok(desc, row))
        return false;
    for (i = 0U; i < n; i++) {
        if ((u32)(seq[i].v >> 16) == SAG_KEY_ESCAPE && i + 1U < n)
            return false;
        node = scratch_child(scratch, node, seq[i].v);
    }
    if (scratch->data[node].bind != 0U)
        return false;
    binding.cmd = cmd;
    binding.iarg = row->iarg;
    binding.sarg = row->sarg ? keymap_strdup(row->sarg) : NULL;
    BindingVec_push(&out->binds, binding);
    scratch->data[node].bind = (u32)(out->binds.len - 1U);
    return true;
}

static bool flatten(Keymap *out, ScratchNodeVec *scratch)
{
    size_t i;
    KeyNode zero = {0};

    if (scratch->len > UINT32_MAX)
        return false;
    KeyNodeVec_reserve(&out->nodes, scratch->len);
    for (i = 0U; i < scratch->len; i++)
        KeyNodeVec_push(&out->nodes, zero);
    for (i = 0U; i < scratch->len; i++) {
        ScratchNode *src = &scratch->data[i];
        KeyNode *dst = &out->nodes.data[i];
        size_t j;

        if (out->edges.len > UINT32_MAX || src->edges.len > UINT32_MAX)
            return false;
        sag_sort_stable(src->edges.data, src->edges.len, sizeof(ScratchEdge),
                        scratch_edge_cmp, NULL);
        dst->edge_lo = (u32)out->edges.len;
        dst->edge_n = (u32)src->edges.len;
        dst->bind = src->bind;
        for (j = 0U; j < src->edges.len; j++) {
            KeyEdge edge;

            edge.key = src->edges.data[j].key;
            edge.child = src->edges.data[j].child;
            KeyEdgeVec_push(&out->edges, edge);
        }
    }
    return true;
}

bool sag_keymap_build(Keymap *km, const char *name,
                      const BindRow *rows, u32 n)
{
    Keymap built = {0};
    ScratchNodeVec scratch = {0};
    ScratchNode root = {0};
    Binding reserved = {0};
    u32 i;
    bool ok = false;

    if (!km || !name || (n != 0U && !rows))
        return false;
    built.name = name;
    BindingVec_push(&built.binds, reserved);
    ScratchNodeVec_push(&scratch, root);
    for (i = 0U; i < n; i++) {
        if (!rows[i].seq || !rows[i].cmd || !add_row(&built, &scratch,
                                                      &rows[i]))
            goto done;
    }
    if (!flatten(&built, &scratch))
        goto done;
    sag_keymap_free(km);
    *km = built;
    (void)memset(&built, 0, sizeof(built));
    ok = true;
done:
    scratch_free(&scratch);
    sag_keymap_free(&built);
    return ok;
}

void sag_keymap_free(Keymap *km)
{
    size_t i;

    if (!km)
        return;
    for (i = 1U; i < km->binds.len; i++)
        free((void *)km->binds.data[i].sarg);
    KeyNodeVec_free(&km->nodes);
    KeyEdgeVec_free(&km->edges);
    BindingVec_free(&km->binds);
    km->name = NULL;
}

bool sag_keymap_step(const Keymap *km, u32 node, KeyId key, u32 *child)
{
    u32 lo;
    u32 hi;

    if (!km || node >= km->nodes.len)
        return false;
    lo = km->nodes.data[node].edge_lo;
    hi = lo + km->nodes.data[node].edge_n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        u64 found = km->edges.data[mid].key;

        if (found < key.v)
            lo = mid + 1U;
        else if (found > key.v)
            hi = mid;
        else {
            if (child)
                *child = km->edges.data[mid].child;
            return true;
        }
    }
    return false;
}

KeyMatch sag_keymap_node_match(const Keymap *km, u32 node,
                               const Binding **binding)
{
    const KeyNode *found;
    bool full;
    bool prefix;

    if (binding)
        *binding = NULL;
    if (!km || node >= km->nodes.len)
        return SAG_MATCH_NONE;
    found = &km->nodes.data[node];
    full = found->bind != 0U;
    prefix = found->edge_n != 0U;
    if (full && binding)
        *binding = &km->binds.data[found->bind];
    if (full && prefix)
        return SAG_MATCH_FULL_PREFIX;
    if (full)
        return SAG_MATCH_FULL;
    if (prefix)
        return SAG_MATCH_PREFIX;
    return SAG_MATCH_NONE;
}

KeyMatch sag_keymap_lookup(const Keymap *km, const KeyId *seq, u32 n,
                           u32 *node, const Binding **binding)
{
    u32 at = 0U;
    u32 i;

    if (node)
        *node = 0U;
    if (binding)
        *binding = NULL;
    if (!km || !seq || n == 0U)
        return SAG_MATCH_NONE;
    for (i = 0U; i < n; i++) {
        if (!sag_keymap_step(km, at, seq[i], &at))
            return SAG_MATCH_NONE;
    }
    if (node)
        *node = at;
    return sag_keymap_node_match(km, at, binding);
}

KeyMatch sag_keystack_lookup(const KeyStack *stack, const KeyId *seq, u32 n,
                             i32 *layer, u32 *node,
                             const Binding **binding)
{
    u32 i;

    if (layer)
        *layer = -1;
    if (!stack)
        return SAG_MATCH_NONE;
    for (i = stack->n; i != 0U; i--) {
        KeyMatch match = sag_keymap_lookup(stack->l[i - 1U], seq, n, node,
                                           binding);

        if (match != SAG_MATCH_NONE) {
            if (layer)
                *layer = (i32)(i - 1U);
            return match;
        }
    }
    return SAG_MATCH_NONE;
}

u32 sag_keymap_binding_count(const Keymap *km)
{
    return km && km->binds.len != 0U ? (u32)(km->binds.len - 1U) : 0U;
}

const Binding *sag_keymap_binding_at(const Keymap *km, u32 index)
{
    if (!km || (size_t)index + 1U >= km->binds.len)
        return NULL;
    return &km->binds.data[(size_t)index + 1U];
}

static bool visit_node(const Keymap *km, u32 node, KeyId *seq, u32 depth,
                       SagKeymapVisitFn visit, void *ctx)
{
    const KeyNode *at = &km->nodes.data[node];
    u32 i;

    if (at->bind != 0U && !visit(seq, depth, &km->binds.data[at->bind], ctx))
        return false;
    for (i = 0U; i < at->edge_n; i++) {
        const KeyEdge *edge = &km->edges.data[at->edge_lo + i];

        if (depth >= SAG_CHORD_MAX)
            return false;
        seq[depth].v = edge->key;
        if (!visit_node(km, edge->child, seq, depth + 1U, visit, ctx))
            return false;
    }
    return true;
}

bool sag_keymap_visit(const Keymap *km, SagKeymapVisitFn visit, void *ctx)
{
    KeyId seq[SAG_CHORD_MAX];

    if (!km || !visit || km->nodes.len == 0U)
        return false;
    return visit_node(km, 0U, seq, 0U, visit, ctx);
}
