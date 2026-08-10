#include "edit/keymap.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "util/base.h"
#include "util/sort.h"

enum {
    KEY_MODS = YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SHIFT | YEW_MOD_SUPER
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
    {"left", YEW_KEY_LEFT},       {"right", YEW_KEY_RIGHT},
    {"up", YEW_KEY_UP},           {"down", YEW_KEY_DOWN},
    {"esc", YEW_KEY_ESCAPE},      {"cr", YEW_KEY_ENTER},
    {"tab", YEW_KEY_TAB},         {"bs", YEW_KEY_BACKSPACE},
    {"del", YEW_KEY_DELETE},      {"space", (u32)' '},
    {"home", YEW_KEY_HOME},       {"end", YEW_KEY_END},
    {"pgup", YEW_KEY_PAGE_UP},    {"pgdn", YEW_KEY_PAGE_DOWN},
    {"ins", YEW_KEY_INSERT},      {"f1", YEW_KEY_F1},
    {"f2", YEW_KEY_F2},           {"f3", YEW_KEY_F3},
    {"f4", YEW_KEY_F4},           {"f5", YEW_KEY_F5},
    {"f6", YEW_KEY_F6},           {"f7", YEW_KEY_F7},
    {"f8", YEW_KEY_F8},           {"f9", YEW_KEY_F9},
    {"f10", YEW_KEY_F10},         {"f11", YEW_KEY_F11},
    {"f12", YEW_KEY_F12},         {"lt", (u32)'<'}
};

static char *keymap_strdup(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *copy = yew_xmalloc(n);

    (void)memcpy(copy, s, n);
    return copy;
}

static bool code_control(u32 code)
{
    return code < 32U || code == 127U;
}

KeyId yew_keyid(Key key)
{
    u32 code = key.code;
    u16 mods = (u16)(key.mods & KEY_MODS);
    KeyId id;

    if (key.ev == YEW_KEY_RELEASE) {
        id.v = 0U;
        return id;
    }
    if (key.ntext != 0U) {
        u32 text_code;
        size_t used = yew_utf8_decode(key.text, key.ntext, &text_code);

        if (used == key.ntext && !yew_utf8_is_escape(text_code))
            code = text_code;
    }
    if (code >= 1U && code <= 26U) {
        code = (u32)'a' + code - 1U;
        mods = (u16)(mods | YEW_MOD_CTRL);
    }
    if ((mods & YEW_MOD_CTRL) != 0U && code >= (u32)'A' &&
        code <= (u32)'Z')
        code += (u32)'a' - (u32)'A';
    if (key.ntext != 0U && !code_control(code))
        mods = (u16)(mods & (u16)~YEW_MOD_SHIFT);
    id.v = ((u64)code << 16) | (u64)mods;
    return id;
}

static bool key_name_parse(const char *s, size_t n, u32 *code)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(key_names); i++) {
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

    for (i = 0U; i < YEW_ARRAY_LEN(key_names); i++) {
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
    } prefixes[] = {{'C', YEW_MOD_CTRL}, {'A', YEW_MOD_ALT},
                    {'S', YEW_MOD_SHIFT}, {'M', YEW_MOD_SUPER}};
    size_t pos = 0U;
    size_t prefix;
    u16 mods = 0U;
    u32 code;
    bool named = false;

    for (prefix = 0U; prefix < YEW_ARRAY_LEN(prefixes); prefix++) {
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
        named = true;
    } else {
        size_t used = yew_utf8_decode((const u8 *)s + pos, n - pos, &code);

        if (used != n - pos || yew_utf8_is_escape(code) || code < 33U ||
            code == 127U || code == (u32)'<')
            return false;
    }
    if (!named && code < YEW_KEY_BASE && (mods & YEW_MOD_SHIFT) != 0U) {
        if (code >= (u32)'a' && code <= (u32)'z')
            code -= (u32)'a' - (u32)'A';
        mods = (u16)(mods & (u16)~YEW_MOD_SHIFT);
    }
    if ((mods & YEW_MOD_CTRL) != 0U && code >= (u32)'A' &&
        code <= (u32)'Z')
        code += (u32)'a' - (u32)'A';
    out->v = ((u64)code << 16) | (u64)mods;
    return true;
}

u32 yew_key_parse_seq(const char *s, KeyId *out, u32 max)
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

void yew_key_format_seq(const KeyId *seq, u32 n, Bytebuf *out)
{
    static const struct {
        u16 bit;
        const char *prefix;
    } prefixes[] = {{YEW_MOD_CTRL, "C-"}, {YEW_MOD_ALT, "A-"},
                    {YEW_MOD_SHIFT, "S-"}, {YEW_MOD_SUPER, "M-"}};
    u32 i;

    for (i = 0U; i < n; i++) {
        u16 mods = (u16)(seq[i].v & 0xFFFFU);
        u32 code = (u32)(seq[i].v >> 16);
        const char *name;
        size_t j;

        if (i != 0U)
            bytebuf_push_u8(out, (u8)' ');
        for (j = 0U; j < YEW_ARRAY_LEN(prefixes); j++) {
            if ((mods & prefixes[j].bit) != 0U)
                bytebuf_append(out, prefixes[j].prefix, 2U);
        }
        name = key_name_format(code);
        if (name) {
            bytebuf_push_u8(out, (u8)'<');
            bytebuf_append(out, name, strlen(name));
            bytebuf_push_u8(out, (u8)'>');
        } else {
            u8 encoded[YEW_UTF8_MAX];
            size_t encoded_n = yew_utf8_encode(code, encoded);

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
    case YEW_ARITY_NONE:
        return row->iarg == 0 && row->sarg == NULL;
    case YEW_ARITY_INT:
    case YEW_ARITY_OPT_INT:
        return row->sarg == NULL;
    case YEW_ARITY_STR:
        return row->iarg == 0 &&
               (row->sarg != NULL ||
                (desc->flags & YEW_CMD_CAPTURES_TEXT) != 0U);
    case YEW_ARITY_OPT_STR:
        return row->iarg == 0;
    }
    return false;
}

static YewKeymapError validate_row(const BindRow *row, KeyId *seq, u32 *out_n)
{
    const CmdDesc *desc;
    CmdId cmd;
    u32 n;
    u32 i;

    if (row == NULL || row->seq == NULL || row->cmd == NULL)
        return YEW_KEYMAP_ERR_SEQUENCE;
    n = yew_key_parse_seq(row->seq, seq, YEW_CHORD_MAX + 1U);
    if (n == 0U)
        return YEW_KEYMAP_ERR_SEQUENCE;
    if (n > YEW_CHORD_MAX)
        return YEW_KEYMAP_ERR_TOO_LONG;
    cmd = yew_cmd_lookup(row->cmd, (u32)strlen(row->cmd));
    desc = yew_cmd_desc(cmd);
    if (cmd.v == 0U || desc == NULL)
        return YEW_KEYMAP_ERR_COMMAND;
    if (!binding_arity_ok(desc, row))
        return YEW_KEYMAP_ERR_ARITY;
    for (i = 0U; i < n; i++)
        if ((u32)(seq[i].v >> 16U) == YEW_KEY_ESCAPE && i + 1U < n)
            return YEW_KEYMAP_ERR_ESCAPE_PREFIX;
    *out_n = n;
    return YEW_KEYMAP_ERR_NONE;
}

YewKeymapError yew_keymap_validate_row(const BindRow *row)
{
    KeyId seq[YEW_CHORD_MAX + 1U];
    u32 n = 0U;

    return validate_row(row, seq, &n);
}

static bool add_row(Keymap *out, ScratchNodeVec *scratch,
                    const BindRow *row, YewKeymapError *error)
{
    KeyId seq[YEW_CHORD_MAX + 1U];
    u32 n = 0U;
    CmdId cmd;
    u32 node = 0U;
    u32 i;
    Binding binding;

    *error = validate_row(row, seq, &n);
    if (*error != YEW_KEYMAP_ERR_NONE)
        return false;
    cmd = yew_cmd_lookup(row->cmd, (u32)strlen(row->cmd));
    for (i = 0U; i < n; i++)
        node = scratch_child(scratch, node, seq[i].v);
    if (scratch->data[node].bind != 0U) {
        *error = YEW_KEYMAP_ERR_DUPLICATE;
        return false;
    }
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
        yew_sort_stable(src->edges.data, src->edges.len, sizeof(ScratchEdge),
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

const char *yew_keymap_error_string(YewKeymapError error)
{
    switch (error) {
    case YEW_KEYMAP_ERR_SEQUENCE: return "invalid key sequence";
    case YEW_KEYMAP_ERR_COMMAND: return "unknown command";
    case YEW_KEYMAP_ERR_DUPLICATE: return "duplicate key sequence";
    case YEW_KEYMAP_ERR_ARITY: return "command arguments do not match its arity";
    case YEW_KEYMAP_ERR_ESCAPE_PREFIX: return "<esc> may not be a chord prefix";
    case YEW_KEYMAP_ERR_TOO_LONG: return "key sequence exceeds eight keys";
    case YEW_KEYMAP_ERR_NONE: return "no keymap error";
    }
    return "unknown keymap error";
}

bool yew_keymap_build_diag(Keymap *km, const char *name,
                           const BindRow *rows, u32 n,
                           YewKeymapDiag *diag)
{
    Keymap built = {0};
    ScratchNodeVec scratch = {0};
    ScratchNode root = {0};
    Binding reserved = {0};
    u32 i;
    bool ok = false;

    if (diag != NULL)
        *diag = (YewKeymapDiag){0U, YEW_KEYMAP_ERR_NONE};

    if (!km || !name || (n != 0U && !rows))
        return false;
    built.name = name;
    BindingVec_push(&built.binds, reserved);
    ScratchNodeVec_push(&scratch, root);
    for (i = 0U; i < n; i++) {
        YewKeymapError error = YEW_KEYMAP_ERR_NONE;

        if (!rows[i].seq || !rows[i].cmd)
            error = YEW_KEYMAP_ERR_SEQUENCE;
        else if (!add_row(&built, &scratch, &rows[i], &error)) {
            /* `error` is set by every failing validation. */
        }
        if (error != YEW_KEYMAP_ERR_NONE) {
            if (diag != NULL)
                *diag = (YewKeymapDiag){i, error};
            goto done;
        }
    }
    if (!flatten(&built, &scratch))
        goto done;
    yew_keymap_free(km);
    *km = built;
    (void)memset(&built, 0, sizeof(built));
    ok = true;
done:
    scratch_free(&scratch);
    yew_keymap_free(&built);
    return ok;
}

bool yew_keymap_build(Keymap *km, const char *name,
                      const BindRow *rows, u32 n)
{
    return yew_keymap_build_diag(km, name, rows, n, NULL);
}

void yew_keymap_free(Keymap *km)
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

bool yew_keymap_step(const Keymap *km, u32 node, KeyId key, u32 *child)
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

KeyMatch yew_keymap_node_match(const Keymap *km, u32 node,
                               const Binding **binding)
{
    const KeyNode *found;
    bool full;
    bool prefix;

    if (binding)
        *binding = NULL;
    if (!km || node >= km->nodes.len)
        return YEW_MATCH_NONE;
    found = &km->nodes.data[node];
    full = found->bind != 0U;
    prefix = found->edge_n != 0U;
    if (full && binding)
        *binding = &km->binds.data[found->bind];
    if (full && prefix)
        return YEW_MATCH_FULL_PREFIX;
    if (full)
        return YEW_MATCH_FULL;
    if (prefix)
        return YEW_MATCH_PREFIX;
    return YEW_MATCH_NONE;
}

KeyMatch yew_keymap_lookup(const Keymap *km, const KeyId *seq, u32 n,
                           u32 *node, const Binding **binding)
{
    u32 at = 0U;
    u32 i;

    if (node)
        *node = 0U;
    if (binding)
        *binding = NULL;
    if (!km || !seq || n == 0U)
        return YEW_MATCH_NONE;
    for (i = 0U; i < n; i++) {
        if (!yew_keymap_step(km, at, seq[i], &at))
            return YEW_MATCH_NONE;
    }
    if (node)
        *node = at;
    return yew_keymap_node_match(km, at, binding);
}

KeyMatch yew_keystack_lookup(const KeyStack *stack, const KeyId *seq, u32 n,
                             i32 *layer, u32 *node,
                             const Binding **binding)
{
    u32 i;

    if (layer)
        *layer = -1;
    if (!stack)
        return YEW_MATCH_NONE;
    for (i = stack->n; i != 0U; i--) {
        KeyMatch match = yew_keymap_lookup(stack->l[i - 1U], seq, n, node,
                                           binding);

        if (match != YEW_MATCH_NONE) {
            if (layer)
                *layer = (i32)(i - 1U);
            return match;
        }
    }
    return YEW_MATCH_NONE;
}

u32 yew_keymap_binding_count(const Keymap *km)
{
    return km && km->binds.len != 0U ? (u32)(km->binds.len - 1U) : 0U;
}

const Binding *yew_keymap_binding_at(const Keymap *km, u32 index)
{
    if (!km || (size_t)index + 1U >= km->binds.len)
        return NULL;
    return &km->binds.data[(size_t)index + 1U];
}

static bool visit_node(const Keymap *km, u32 node, KeyId *seq, u32 depth,
                       YewKeymapVisitFn visit, void *ctx)
{
    const KeyNode *at = &km->nodes.data[node];
    u32 i;

    if (at->bind != 0U && !visit(seq, depth, &km->binds.data[at->bind], ctx))
        return false;
    for (i = 0U; i < at->edge_n; i++) {
        const KeyEdge *edge = &km->edges.data[at->edge_lo + i];

        if (depth >= YEW_CHORD_MAX)
            return false;
        seq[depth].v = edge->key;
        if (!visit_node(km, edge->child, seq, depth + 1U, visit, ctx))
            return false;
    }
    return true;
}

bool yew_keymap_visit(const Keymap *km, YewKeymapVisitFn visit, void *ctx)
{
    KeyId seq[YEW_CHORD_MAX];

    if (!km || !visit || km->nodes.len == 0U)
        return false;
    return visit_node(km, 0U, seq, 0U, visit, ctx);
}
