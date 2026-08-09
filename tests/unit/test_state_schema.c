/*
 * Sprint 25 §3: the v1 schema.
 *
 * Three properties, each guarding a silent failure:
 *
 * ID REMAPPING — a tab whose group record went missing must become
 * UNGROUPED, never attached to whatever id happens to exist now.  The
 * wrong answer files someone's document into an unrelated group and
 * looks entirely normal.
 *
 * PRE-ORDER AGREEMENT — `panes` names windows by index and `wins` lists
 * them.  Two traversals that disagree by one restore every pane holding
 * its neighbour's cursor.  Nothing about that looks broken until you
 * compare against what you had before you quit.
 *
 * PERMILLE FIXPOINT — ratios travel as integers because there are no
 * floats in the format.  If the conversion is not a fixpoint,
 * save->restore->save drifts a pane border a cell at a time.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"

/* ---------------------------------------------------------------- */
/* The id map                                                       */
/* ---------------------------------------------------------------- */

/*
 * The contract's own fixture: file ids 7, 9, 12 restore to live ids
 * 1, 2, 3 with membership and ordinals intact.
 */
void test_state_schema_remaps_group_ids(void)
{
    IdMapVec m;

    sag_idmap_init(&m);
    sag_idmap_put(&m, 7U, 1U);
    sag_idmap_put(&m, 9U, 2U);
    sag_idmap_put(&m, 12U, 3U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 7U), 1U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 9U), 2U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 12U), 3U);
    /* Ungrouped stays ungrouped without a lookup. */
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 0U), 0U);
    /*
     * A file id with no record resolves to 0 — UNGROUPED — and
     * emphatically not to a live id that happens to exist.  Note 1, 2
     * and 3 are all live here, so a naive "reuse the number"
     * implementation would return a real group for every one of these.
     */
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 1U), 0U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 2U), 0U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 3U), 0U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 99U), 0U);
    sag_idmap_free(&m);
}

void test_state_schema_id_map_grows(void)
{
    IdMapVec m;
    u32 i;

    sag_idmap_init(&m);
    for (i = 1U; i <= 200U; i++)
        sag_idmap_put(&m, i, i * 10U);
    for (i = 1U; i <= 200U; i++)
        SAG_ASSERT_EQ_U64(sag_idmap_get(&m, i), i * 10U);
    SAG_ASSERT_EQ_U64(sag_idmap_get(&m, 201U), 0U);
    sag_idmap_free(&m);
}

/* ---------------------------------------------------------------- */
/* Scalars                                                          */
/* ---------------------------------------------------------------- */

/* All 999 legal permille values survive the round trip exactly. */
void test_state_schema_permille_is_a_fixpoint(void)
{
    i64 p;

    for (p = SAG_STATE_RATIO_MIN; p <= SAG_STATE_RATIO_MAX; p++) {
        float r = sag_permille_to_ratio(p);

        SAG_ASSERT_EQ_I64(sag_ratio_to_permille(r), p);
    }
    /* Out of range clamps rather than producing a ratio that would
     * make a pane zero cells wide. */
    SAG_ASSERT_EQ_I64(sag_ratio_to_permille(0.0f), SAG_STATE_RATIO_MIN);
    SAG_ASSERT_EQ_I64(sag_ratio_to_permille(1.0f), SAG_STATE_RATIO_MAX);
    SAG_ASSERT_EQ_I64(sag_ratio_to_permille(-5.0f), SAG_STATE_RATIO_MIN);
    SAG_ASSERT(sag_permille_to_ratio(-1) > 0.0f);
    SAG_ASSERT(sag_permille_to_ratio(5000) < 1.0f);
}

/* `goal: -1` is SAG_GCOL_EOL, because UINT64_MAX does not fit i64 and
 * would force every reader into an unsigned special case. */
void test_state_schema_goal_column_round_trips(void)
{
    SAG_ASSERT_EQ_I64(sag_goal_to_i64(SAG_GCOL_EOL), -1);
    SAG_ASSERT_EQ_U64(sag_goal_from_i64(-1), SAG_GCOL_EOL);
    SAG_ASSERT_EQ_I64(sag_goal_to_i64(0U), 0);
    SAG_ASSERT_EQ_U64(sag_goal_from_i64(0), 0U);
    SAG_ASSERT_EQ_I64(sag_goal_to_i64(14U), 14);
    SAG_ASSERT_EQ_U64(sag_goal_from_i64(14), 14U);
    /* Any other unrepresentable value degrades to EOL rather than
     * wrapping into a negative column. */
    SAG_ASSERT_EQ_I64(sag_goal_to_i64(0x8000000000000000ULL), -1);
}

/* ---------------------------------------------------------------- */
/* Emitting a live editor                                           */
/* ---------------------------------------------------------------- */

static void ss_fixture(Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/* Emits `ed` and parses the result back, NUL-terminated for strstr. */
static FlLit *ss_emit_parse(Ed *ed, Arena *a, Bytebuf *out)
{
    FlParseErr err;
    FlLit *lit;

    out->len = 0U;
    sag_state_emit(ed, out);
    lit = sag_fl_parse(a, out->data, out->len, &err);
    if (lit == NULL)
        (void)fprintf(stderr, "parse failed at %u:%u: %s\n", err.line,
                      err.col, err.msg == NULL ? "?" : err.msg);
    bytebuf_push_u8(out, 0U);
    out->len--;
    return lit;
}

/* The document has the frozen shape, and its own parser accepts it. */
void test_state_schema_emits_a_parseable_v1_document(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;

    ss_fixture(&ed);
    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(lit, "version"), 0), 1);
    SAG_ASSERT_NOT_NULL(sag_fl_get(lit, "workspace"));
    SAG_ASSERT_NOT_NULL(sag_fl_get(lit, "options"));
    SAG_ASSERT_NOT_NULL(sag_fl_get(lit, "groups"));
    SAG_ASSERT_NOT_NULL(sag_fl_get(lit, "tabs"));
    SAG_ASSERT_NOT_NULL(sag_fl_get(lit, "files"));
    /* One tab, one window, one cursor. */
    SAG_ASSERT_EQ_U64(sag_fl_len(sag_fl_get(lit, "tabs")), 1U);
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/*
 * GROUPS BEFORE TABS — a property of the schema, not of the writer.
 *
 * The restore attaches each tab as it finishes that tab's record, so a
 * document with tabs first would need a fixup pass over ids that have
 * already been renumbered.
 */
void test_state_schema_writes_groups_before_tabs(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    const char *doc;
    u32 i;
    int groups_at = -1;
    int tabs_at = -1;

    ss_fixture(&ed);
    arena_init(&a);
    bytebuf_init(&out);
    {
        FlLit *lit = ss_emit_parse(&ed, &a, &out);

        SAG_ASSERT_NOT_NULL(lit);
        /* Key ORDER in the emitted map, not just presence. */
        for (i = 0U; i < lit->len; i++) {
            if (strncmp(lit->keys[i], "groups", lit->keylens[i]) == 0 &&
                lit->keylens[i] == 6U)
                groups_at = (int)i;
            if (strncmp(lit->keys[i], "tabs", lit->keylens[i]) == 0 &&
                lit->keylens[i] == 4U)
                tabs_at = (int)i;
        }
    }
    SAG_ASSERT(groups_at >= 0);
    SAG_ASSERT(tabs_at >= 0);
    SAG_ASSERT(groups_at < tabs_at);
    /* And in the bytes, which is what a reimplementation sees. */
    doc = (const char *)out.data;
    SAG_ASSERT(strstr(doc, "groups:") < strstr(doc, "tabs:"));
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/* Groups round-trip with their label, origin and resume path. */
void test_state_schema_emits_groups_with_their_fields(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;
    const FlLit *groups;
    const FlLit *g0;
    u64 n = 0U;
    u32 gid;

    ss_fixture(&ed);
    SAG_ASSERT(sag_tab_open(&ed, "/tmp/sag-ss-a.txt") >= 0);
    SAG_ASSERT(sag_tab_open(&ed, "/tmp/sag-ss-b.txt") >= 0);
    gid = sag_group_create(&ed, "/tmp", "src/");
    sag_group_add_member(&ed, gid, 1);
    sag_group_add_member(&ed, gid, 2);

    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    groups = sag_fl_get(lit, "groups");
    SAG_ASSERT_EQ_U64(sag_fl_len(groups), 1U);
    g0 = sag_fl_at(groups, 0U);
    SAG_ASSERT_EQ_U64((u64)sag_fl_int_or(sag_fl_get(g0, "id"), 0), gid);
    SAG_ASSERT_EQ_STR(sag_fl_str_or(sag_fl_get(g0, "label"), "", &n),
                      "src/");
    SAG_ASSERT_EQ_STR(sag_fl_str_or(sag_fl_get(g0, "dir_path"), "", &n),
                      "/tmp");
    /* Members carry their group and ordinal on the TAB record. */
    {
        const FlLit *tabs = sag_fl_get(lit, "tabs");
        const FlLit *t1 = sag_fl_at(tabs, 1U);

        SAG_ASSERT_EQ_U64((u64)sag_fl_int_or(sag_fl_get(t1, "group"), 0),
                          gid);
        SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(t1, "group_ordinal"),
                                        0), 1);
    }
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/*
 * THE §4 PITFALL, on a real split tree.
 *
 * Every `win` index named anywhere in `panes` must be a valid index
 * into `wins`, and the count must match the leaf count exactly.  An
 * off-by-one here is the bug that restores each pane with its
 * neighbour's cursor.
 */
static void collect_win_ids(const FlLit *panes, int *seen, int *n)
{
    const FlLit *w = sag_fl_get(panes, "win");

    if (w != NULL) {
        seen[(*n)++] = (int)sag_fl_int_or(w, -1);
        return;
    }
    collect_win_ids(sag_fl_get(panes, "a"), seen, n);
    collect_win_ids(sag_fl_get(panes, "b"), seen, n);
}

void test_state_schema_pane_win_indices_match_the_wins_list(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;
    const FlLit *tab0;
    int seen[SAG_PANE_MAX_LEAVES * 2];
    int n = 0;
    int i;
    u32 nwins;

    ss_fixture(&ed);
    /*
     * Three leaves: a horizontal split, then a vertical one inside it.
     *
     * The split mutates the old leaf into the split node IN PLACE and
     * returns the new leaf, so focus has to follow the return value —
     * splitting `ed.focus` twice would ask a split node to split.
     */
    {
        Pane *leaf = sag_pane_split(&ed, ed.focus, SAG_SPLIT_H);

        SAG_ASSERT_NOT_NULL(leaf);
        ed.focus = leaf;
        sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
        leaf = sag_pane_split(&ed, ed.focus, SAG_SPLIT_V);
        SAG_ASSERT_NOT_NULL(leaf);
        ed.focus = leaf;
        sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    }
    /* The tab's tree is the live one. */
    sag_tab_at(&ed, 0)->root = ed.pane_root;
    sag_tab_at(&ed, 0)->focus = ed.focus;

    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    tab0 = sag_fl_at(sag_fl_get(lit, "tabs"), 0U);
    nwins = sag_fl_len(sag_fl_get(tab0, "wins"));
    SAG_ASSERT_EQ_U64(nwins, 3U);

    collect_win_ids(sag_fl_get(tab0, "panes"), seen, &n);
    /* One `win` per leaf, and exactly as many leaves as windows. */
    SAG_ASSERT_EQ_I64(n, (int)nwins);
    for (i = 0; i < n; i++) {
        int j;

        SAG_ASSERT(seen[i] >= 0);
        SAG_ASSERT(seen[i] < (int)nwins);
        /* No index used twice: two panes sharing a window record is
         * the same corruption seen from the other side. */
        for (j = 0; j < i; j++)
            SAG_ASSERT(seen[i] != seen[j]);
    }
    /* Pre-order: the leftmost leaf is window 0. */
    SAG_ASSERT_EQ_I64(seen[0], 0);
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/* A split records its direction and a permille ratio — never a float. */
void test_state_schema_splits_carry_permille_not_floats(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;
    const FlLit *panes;
    u64 n = 0U;
    i64 permille;

    ss_fixture(&ed);
    {
        Pane *leaf = sag_pane_split(&ed, ed.focus, SAG_SPLIT_H);

        SAG_ASSERT_NOT_NULL(leaf);
        ed.focus = leaf;
    }
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    sag_tab_at(&ed, 0)->root = ed.pane_root;
    sag_tab_at(&ed, 0)->focus = ed.focus;

    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    panes = sag_fl_get(sag_fl_at(sag_fl_get(lit, "tabs"), 0U), "panes");
    SAG_ASSERT_EQ_STR(sag_fl_str_or(sag_fl_get(panes, "split"), "", &n),
                      "h");
    permille = sag_fl_int_or(sag_fl_get(panes, "ratio_permille"), 0);
    SAG_ASSERT(permille >= SAG_STATE_RATIO_MIN);
    SAG_ASSERT(permille <= SAG_STATE_RATIO_MAX);
    /* No decimal point anywhere in the document — a locale that emits
     * `0,5` cannot exist if nothing emits a fraction. */
    SAG_ASSERT(strstr((const char *)out.data, ".") == NULL ||
               strstr((const char *)out.data, "0.5") == NULL);
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/*
 * `deferred` is written for every non-active tab that holds no buffer,
 * and never for the active one — it is an instruction to the restore,
 * not a record of residency.
 */
void test_state_schema_deferred_is_an_instruction(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;
    const FlLit *tabs;

    ss_fixture(&ed);
    SAG_ASSERT(sag_tab_open(&ed, "/tmp/sag-ss-defer.txt") >= 0);
    /* Tab 1 was opened but never viewed, so it holds no buffer. */
    SAG_ASSERT(!sag_tab_is_resident(&ed, 1));
    SAG_ASSERT_EQ_I64(ed.tabs.active, 0);

    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    tabs = sag_fl_get(lit, "tabs");
    SAG_ASSERT(!sag_fl_bool_or(sag_fl_get(sag_fl_at(tabs, 0U), "deferred"),
                               true));
    SAG_ASSERT(sag_fl_bool_or(sag_fl_get(sag_fl_at(tabs, 1U), "deferred"),
                              false));
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/* Undo is a REFERENCE: the sidecar's name and version, never payload
 * bytes (DoD 11). */
void test_state_schema_undo_is_a_reference_only(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlLit *lit;
    const FlLit *files;

    ss_fixture(&ed);
    SAG_ASSERT_EQ_I64(sag_ed_open(&ed, "/tmp/sag-ss-undo.txt"),
                      SAG_LOAD_ENOENT);
    arena_init(&a);
    bytebuf_init(&out);
    lit = ss_emit_parse(&ed, &a, &out);
    SAG_ASSERT_NOT_NULL(lit);
    files = sag_fl_get(lit, "files");
    if (sag_fl_len(files) > 0U) {
        const FlLit *undo = sag_fl_get(sag_fl_at(files, 0U), "undo");
        u64 n = 0U;
        const char *name = sag_fl_str_or(sag_fl_get(undo, "file"), "", &n);

        SAG_ASSERT_NOT_NULL(strstr(name, ".sagu"));
        SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(undo, "version"), 0),
                          1);
    }
    /* The only mention of .sagu is the reference — no payload rode
     * along. */
    SAG_ASSERT(strstr((const char *)out.data, "\\x") == NULL);
    bytebuf_free(&out);
    arena_free_all(&a);
    sag_ed_free(&ed);
}

/*
 * `saved_at` is a wall clock (state_emit.c), so two emissions that
 * straddle a second boundary differ in that one field and in no other.
 * Pinning it to 0 is what test_state_corpus.c already does for the
 * frozen corpus, for the same reason and with the same trade: the
 * emitter stays honest in production rather than growing a
 * fake-the-clock hook, and the test asserts the property that actually
 * holds.
 *
 * Without this the test is a coin flip weighted by how long the two
 * calls take -- which is why it survived for sprints on a fast machine
 * and then failed in the valgrind lane, where everything runs ~30x
 * slower and the window between the two emits is wide enough to cross
 * a tick.
 */
static void ss_pin_saved_at(Bytebuf *doc)
{
    static const char key[] = "    saved_at: ";
    char *at;
    char *end;
    size_t head;

    bytebuf_push_u8(doc, 0U);
    doc->len--;
    at = strstr((char *)doc->data, key);
    if (at == NULL)
        return;
    end = at + sizeof(key) - 1U;
    while (*end != '\0' && *end != ',')
        end++;
    head = (size_t)(at - (char *)doc->data) + sizeof(key) - 1U;
    (void)memmove(doc->data + head + 1U, (u8 *)end,
                  doc->len - (size_t)(end - (char *)doc->data));
    doc->data[head] = (u8)'0';
    doc->len -= (size_t)(end - (char *)doc->data) - head - 1U;
}

/* Emitting the same editor twice is byte-identical, and the document
 * re-emits from its parse unchanged (the save->restore->save fixpoint,
 * at the document level). */
void test_state_schema_emission_is_deterministic(void)
{
    Ed ed;
    Arena a;
    Bytebuf one;
    Bytebuf two;
    Bytebuf again;
    FlLit *lit;
    FlEmit e;

    ss_fixture(&ed);
    SAG_ASSERT(sag_tab_open(&ed, "/tmp/sag-ss-det.txt") >= 0);
    arena_init(&a);
    bytebuf_init(&one);
    bytebuf_init(&two);
    bytebuf_init(&again);

    sag_state_emit(&ed, &one);
    sag_state_emit(&ed, &two);
    ss_pin_saved_at(&one);
    ss_pin_saved_at(&two);
    SAG_ASSERT_EQ_U64(one.len, two.len);
    SAG_ASSERT_EQ_I64(memcmp(one.data, two.data, one.len), 0);

    /* parse -> emit lands on the same bytes. */
    {
        FlParseErr err;

        lit = sag_fl_parse(&a, one.data, one.len, &err);
        SAG_ASSERT_NOT_NULL(lit);
    }
    sag_fl_emit_init(&e, &again);
    sag_fl_emit_lit(&e, NULL, lit);
    sag_fl_emit_done(&e);
    SAG_ASSERT_EQ_U64(again.len, one.len);
    SAG_ASSERT_EQ_I64(memcmp(again.data, one.data, one.len), 0);

    bytebuf_free(&one);
    bytebuf_free(&two);
    bytebuf_free(&again);
    arena_free_all(&a);
    sag_ed_free(&ed);
}
