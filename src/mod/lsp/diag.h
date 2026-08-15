#ifndef YEW_MOD_LSP_DIAG_H
#define YEW_MOD_LSP_DIAG_H

#include "mod/lsp/json.h"
#include "text/coords.h"
#include "text/mark.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;
typedef struct Win Win;

typedef enum {
    YEW_DIAG_ERROR = 1,
    YEW_DIAG_WARN,
    YEW_DIAG_INFO,
    YEW_DIAG_HINT
} DiagSev;

enum {
    YEW_DIAGT_UNNECESSARY = 1U << 0,
    YEW_DIAGT_DEPRECATED = 1U << 1
};

typedef struct Diagnostic {
    MarkId lo;
    MarkId hi;
    Span cache;
    u64 cache_gen;
    u32 line;
    u8 sev;
    u8 tags;
    u32 server;
    u32 identity;
    char *code;
    char *source;
    char *message;
} Diagnostic;

VEC_DECL(DiagnosticVec, Diagnostic);

typedef struct DiagStore {
    DiagnosticVec d;
    Arena arena;
    u32 n[5];
    u32 gen;
    u32 next_identity;
    i64 version;
    bool stale;
} DiagStore;

DiagStore *yew_diag_store_new(void);
void yew_diag_store_free(Buffer *b);

void yew_diag_replace(Ed *ed, Buffer *b, u32 server, const JsonValue *arr,
                      i64 version);
u32 yew_diag_at_line(const Buffer *b, LineNo line, const Diagnostic **out,
                     u32 max);
const Diagnostic *yew_diag_at_point(const Buffer *b, ByteOff point);
Span yew_diag_span(Buffer *b, Diagnostic *d);
u32 yew_diag_list(Ed *ed, PickItem *out, u32 max);

const char *yew_diag_glyph(u8 sev, size_t *len);
const char *yew_diag_role(u8 sev);
u16 yew_diag_attrs(u8 sev, u8 tags, bool truecolor);
MsgSev yew_diag_msg_sev(u8 sev);

/* UI seams used by the LSP commands and the ordinary draw pass. */
void yew_diag_refresh_view(Ed *ed, Win *w);
void yew_diag_draw_rows(Ed *ed, Win *w, u16 lo, u16 hi);
void yew_diag_cursor_hint(Ed *ed, Win *w);
void yew_diag_picker_open(Ed *ed);
bool yew_diag_jump(Ed *ed, Win *w, bool forward);

#endif
