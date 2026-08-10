#ifndef YEW_TEXT_REGISTER_H
#define YEW_TEXT_REGISTER_H

#include <stdbool.h>

#include "text/edit.h"
#include "util/buf.h"
#include "util/vec.h"

#define YEW_KILL_RING_MAX 256U
#define YEW_KILL_RING_DEPTH_DEFAULT 32U
#define YEW_KILL_RING_BYTES_DEFAULT (UINT64_C(8) * 1024U * 1024U)

typedef enum {
    YEW_REG_CHARWISE = 0,
    YEW_REG_LINEWISE = 1,
    YEW_REG_BLOCKWISE = 2
} RegType;

VEC_DECL(YewRegRowVec, Span);

typedef struct RegVal {
    u8 type;
    bool ragged;
    u32 width;
    Bytebuf bytes;
    YewRegRowVec rows;
    i64 t_wall;
} RegVal;

typedef struct RegInfo {
    u8 type;
    bool ragged;
    u32 width;
    u32 rows;
    u64 bytes;
    i64 t_wall;
} RegInfo;

typedef enum {
    YEW_CLIP_SYNC_OFF = 0,
    YEW_CLIP_SYNC_YANK,
    YEW_CLIP_SYNC_ALL,
    YEW_CLIP_SYNC_UNNAMED
} YewClipboardSync;

VEC_DECL(YewRegPasteSpanVec, Span);

typedef struct Registers {
    RegVal named[26];
    RegVal unnamed;
    RegVal numbered[10];
    RegVal small_del;
    RegVal last_insert;
    RegVal search;
    RegVal cmdline;
    RegVal file;
    RegVal alt_file;
    RegVal system;
    RegVal ring[YEW_KILL_RING_MAX];
    u32 ring_head;
    u32 ring_len;
    u32 ring_depth;
    u64 ring_bytes;
    u64 ring_bytes_max;
    u64 clip_read_max;
    u8 clipboard_sync;
    const UndoTree *bound_undo;
    const FileMeta *bound_meta;

    YewRegPasteSpanVec paste_spans;
    const TextBuf *paste_owner;
    Cursor paste_origin;
    u32 paste_win_id;
    u32 paste_ring_index;
    u32 paste_tabw;
    bool paste_before;
    bool paste_live;
} Registers;

void yew_regval_init(RegVal *v);
void yew_regval_free(RegVal *v);
void yew_regval_copy(RegVal *dst, const RegVal *src);
void yew_regval_from_span(RegVal *out, const TextBuf *tb, Span range,
                          RegType type, const FileMeta *meta);

void yew_reg_init(Registers *r);
void yew_reg_free(Registers *r);
void yew_reg_bind_context(Registers *r, const UndoTree *undo,
                          const FileMeta *meta);
RegVal *yew_reg_get(Registers *r, u8 name);
void yew_reg_set(Registers *r, u8 name, const RegVal *v);
void yew_reg_set_macro(Registers *r, u8 name, const RegVal *v, bool append);
void yew_reg_set_cmdline(Registers *r, const u8 *bytes, size_t len);
void yew_reg_set_search(Registers *r, const u8 *bytes, size_t len);
void yew_reg_append(Registers *r, u8 name, const RegVal *v);
void yew_reg_yank(Registers *r, u8 explicit_name, const RegVal *v);
void yew_reg_delete(Registers *r, u8 explicit_name, const RegVal *v);

bool yew_reg_paste(Registers *r, EditCtx *ec, u8 name, bool before,
                   u32 tabw);
void yew_reg_ring_push(Registers *r, const RegVal *v);
void yew_reg_ring_set_depth(Registers *r, u32 depth);
bool yew_reg_ring_cycle(Registers *r, EditCtx *ec, i32 delta);
u32 yew_reg_ring_list(const Registers *r, RegInfo *out, u32 max);

#endif
