#ifndef YEW_MOD_LSP_SYNC_H
#define YEW_MOD_LSP_SYNC_H

#include <stdbool.h>

#include "mod/lsp/jsonrpc.h"
#include "text/edit.h"
#include "util/arena.h"
#include "util/vec.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct LspServer LspServer;

enum {
    YEW_LSP_PENDING_MAX = 512,
    YEW_POSENC_UTF8 = 1,
    YEW_POSENC_UTF16 = 2
};

typedef struct LspChange {
    i64 sl, sc, el, ec;
    const u8 *text;
    u32 len;
} LspChange;

VEC_DECL(Vec_LspChange, LspChange);

typedef struct LspDoc {
    LspServer *server;
    u32 buf_id;
    char *uri;
    i64 version;
    u64 sent_gen;
    bool open;
    bool full_sync;
    bool insert_waiting;
    Vec_LspChange pending;
    Arena changes;
} LspDoc;

typedef struct LspGen {
    u32 buf_id;
    i64 version;
    u64 tb_gen;
} LspGen;

void yew_lsp_doc_init(LspDoc *doc, u32 buf_id, const char *uri);
void yew_lsp_doc_free(LspDoc *doc);

bool yew_lsp_doc_open(RpcConn *rpc, LspDoc *doc, const Buffer *buffer,
                      const char *language_id);
void yew_lsp_doc_note_edit(LspDoc *doc, u8 pos_enc, u8 sync_kind,
                           const TextBuf *tb, u8 kind, ByteOff at, u64 len);
/* The pre-edit notification has no insert payload.  This paired post hook
 * copies the inserted range immediately after mutation, before another edit
 * can change it.  Deletes are complete in the pre hook. */
void yew_lsp_doc_note_edit_post(LspDoc *doc, u8 kind, const TextBuf *tb,
                                ByteOff at, u64 len);
bool yew_lsp_doc_flush(RpcConn *rpc, LspDoc *doc, u8 sync_kind,
                       const TextBuf *tb);
void yew_lsp_doc_save(RpcConn *rpc, const LspDoc *doc, const TextBuf *tb,
                      bool include_text);
void yew_lsp_doc_close(RpcConn *rpc, LspDoc *doc);

LspGen yew_lsp_gen(const LspDoc *doc, const TextBuf *tb);
bool yew_lsp_gen_matches(const LspGen *gen, const LspDoc *doc,
                         const TextBuf *tb);

/* Editor integration.  All are no-ops when the buffer has no ready server. */
void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_lsp_note_edit_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_lsp_sync_flush(Ed *ed);
void yew_lsp_sync_save(Ed *ed, Buffer *buffer);
void yew_lsp_sync_close(Ed *ed, Buffer *buffer);
bool yew_lsp_gen_current(const Ed *ed, const LspGen *gen);

#endif
