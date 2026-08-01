#define _POSIX_C_SOURCE 200809L

#include "term/input.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "unicode/utf8.h"
#include "util/base.h"
#include "util/log.h"

enum {
    IN_GROUND,
    IN_ESC,
    IN_CSI,
    IN_SS3,
    IN_STRING,
    IN_STRING_ESC,
    IN_X10,
    IN_PASTE,
    IN_STRING_DROP,
    IN_STRING_DROP_ESC,
    IN_CSI_DROP
};

enum {
    LOG_UNKNOWN = 1,
    LOG_MALFORMED = 2,
    LOG_STRING_CAP = 4,
    LOG_BUFFER_CAP = 8,
    LOG_UTF8 = 16,
    LOG_MOUSE = 32,
    LOG_OSC52 = 64
};

static const u8 paste_end[] = "\x1b[201~";
static const char input_enable[] =
    "\x1b[?2004h"
    "\x1b[?1002h"
    "\x1b[?1006h"
    "\x1b[?1004h";
static const char input_disable[] =
    "\x1b[<u"
    "\x1b[?2004l"
    "\x1b[?1002l"
    "\x1b[?1006l"
    "\x1b[?1004l";
static const char kitty_enable[] = "\x1b[>21u";

/* Leave a little capacity headroom for the six-byte paste sentinel. */
#define SAG_PASTE_EMIT 4080U

static void input_append(Bytebuf *buf, const u8 *data, size_t len)
{
    size_t need = buf->len + len;

    if (need > buf->cap) {
        buf->data = sag_xrealloc(buf->data, need);
        buf->cap = need;
    }
    (void)memcpy(buf->data + buf->len, data, len);
    buf->len = need;
}

static void write_blob(int fd, const void *data, size_t len)
{
    const u8 *p = data;

    while (len != 0U) {
        ssize_t n = write(fd, p, len);

        if (n > 0) {
            p += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

static void note_drop(In *in, u8 shape, const char *message)
{
    in->dropped++;
    if ((in->log_mask & shape) == 0U) {
        in->log_mask = (u8)(in->log_mask | shape);
        sag_log(SAG_LOG_DEBUG, "input: %s (further instances suppressed)",
                message);
    }
}

static bool string_is_osc52(const In *in)
{
    static const u8 prefix[] = "\x1b]52;";
    size_t len = in->scan - in->seq_start;

    return len >= sizeof(prefix) - 1U &&
           memcmp(in->buf.data + in->seq_start, prefix,
                  sizeof(prefix) - 1U) == 0;
}

static void note_string_drop(In *in, u8 shape, const char *message)
{
    if (!string_is_osc52(in)) {
        note_drop(in, shape, message);
        return;
    }
    in->dropped++;
    if ((in->log_mask & LOG_OSC52) == 0U) {
        in->log_mask = (u8)(in->log_mask | LOG_OSC52);
        sag_log(SAG_LOG_WARN,
                "input: unsolicited OSC 52 reply discarded "
                "(further instances suppressed)");
    }
}

static void clear_key(Key *out)
{
    (void)memset(out, 0, sizeof(*out));
}

static void emit_named(Key *out, u32 code, u16 mods)
{
    out->kind = SAG_EV_KEY;
    out->code = code;
    out->mods = mods;
    out->ev = SAG_KEY_PRESS;
}

static void emit_scalar(Key *out, u32 cp, u16 mods, const u8 *text,
                        size_t text_len)
{
    emit_named(out, cp, mods);
    if (text_len > sizeof(out->text))
        text_len = sizeof(out->text);
    if (text_len != 0U)
        (void)memcpy(out->text, text, text_len);
    out->ntext = (u8)text_len;
}

static void compact(In *in)
{
    size_t left;
    size_t consumed;

    if (in->state != IN_GROUND && in->state != IN_PASTE)
        return;
    if ((in->state == IN_GROUND && in->rd <= SAG_IN_PASTE_CHUNK) ||
        (in->state == IN_PASTE && in->rd < SAG_PASTE_EMIT))
        return;
    consumed = in->rd;
    left = in->buf.len - consumed;
    if (left != 0U)
        (void)memmove(in->buf.data, in->buf.data + consumed, left);
    in->buf.len = left;
    in->rd = 0U;
    if (in->state == IN_PASTE)
        in->scan -= consumed;
}

static void reset_params(In *in)
{
    (void)memset(in->par, 0, sizeof(in->par));
    (void)memset(in->sub, 0, sizeof(in->sub));
    (void)memset(in->xpar, 0, sizeof(in->xpar));
    (void)memset(in->xpresent, 0, sizeof(in->xpresent));
    (void)memset(in->xnsub, 0, sizeof(in->xnsub));
    in->npar = 1U;
    in->xnsub[0] = 1U;
    in->priv = 0U;
    in->inter = 0U;
}

static bool params_empty(const In *in)
{
    return in->npar == 1U && !in->xpresent[0][0] &&
           in->xnsub[0] == 1U;
}

static bool params_plain(const In *in)
{
    size_t i;

    for (i = 0U; i < in->npar; i++) {
        if (in->xnsub[i] != 1U)
            return false;
    }
    return true;
}

static bool is_scalar(u32 cp)
{
    return cp <= 0x10FFFFU && !(cp >= 0xD800U && cp <= 0xDFFFU);
}

static bool modifier(const In *in, size_t index, u16 *mods)
{
    u32 m = index >= in->npar || !in->xpresent[index][0]
                ? 1U : in->xpar[index][0];

    if (m == 0U || m > 256U)
        return false;
    *mods = (u16)(m - 1U);
    return true;
}

static u32 tilde_code(u32 n)
{
    switch (n) {
    case 1: case 7: return SAG_KEY_HOME;
    case 2: return SAG_KEY_INSERT;
    case 3: return SAG_KEY_DELETE;
    case 4: case 8: return SAG_KEY_END;
    case 5: return SAG_KEY_PAGE_UP;
    case 6: return SAG_KEY_PAGE_DOWN;
    case 11: case 12: case 13: case 14: case 15:
        return SAG_KEY_F1 + n - 11U;
    case 17: case 18: case 19: case 20: case 21:
        return SAG_KEY_F6 + n - 17U;
    case 23: case 24: return SAG_KEY_F11 + n - 23U;
    case 25: case 26: return SAG_KEY_F13 + n - 25U;
    case 28: case 29: return SAG_KEY_F15 + n - 28U;
    case 31: case 32: case 33: case 34:
        return SAG_KEY_F17 + n - 31U;
    default: return 0U;
    }
}

static u32 kitty_code(u32 cp)
{
    if (cp == 27U)
        return SAG_KEY_ESCAPE;
    if (cp == 13U)
        return SAG_KEY_ENTER;
    if (cp == 9U)
        return SAG_KEY_TAB;
    if (cp == 127U)
        return SAG_KEY_BACKSPACE;
    if (cp >= 57376U && cp <= 57398U)
        return SAG_KEY_F13 + cp - 57376U;
    if (cp >= 57399U && cp <= 57408U)
        return SAG_KEY_KP_0 + cp - 57399U;
    if (cp >= 57409U && cp <= 57416U)
        return SAG_KEY_KP_DECIMAL + cp - 57409U;
    if (cp >= 57417U && cp <= 57427U)
        return SAG_KEY_KP_LEFT + cp - 57417U;
    if (cp >= 57441U && cp <= 57452U)
        return SAG_KEY_LEFT_SHIFT + cp - 57441U;
    switch (cp) {
    case 57358: return SAG_KEY_CAPS_LOCK;
    case 57359: return SAG_KEY_SCROLL_LOCK;
    case 57360: return SAG_KEY_NUM_LOCK;
    case 57361: return SAG_KEY_PRINT_SCREEN;
    case 57362: return SAG_KEY_PAUSE;
    case 57363: return SAG_KEY_MENU;
    default: break;
    }
    if (cp >= 57344U && cp <= 63743U)
        return 0U;
    if (is_scalar(cp))
        return cp;
    return 0U;
}

static bool append_text_cp(Key *out, u32 cp)
{
    u8 encoded[SAG_UTF8_MAX];
    size_t n = sag_utf8_encode(cp, encoded);

    if (n == 0U || out->ntext >= 15U || n > 15U - out->ntext)
        return false;
    (void)memcpy(out->text + out->ntext, encoded, n);
    out->ntext = (u8)(out->ntext + n);
    return true;
}

static bool dispatch_kitty(In *in, Key *out)
{
    u32 primary;
    u32 code;
    u16 mods;
    u32 event = SAG_KEY_PRESS;
    size_t i;

    if (in->priv != 0U || in->inter != 0U || in->npar > 3U ||
        in->xnsub[0] > 3U || in->xnsub[1] > 2U)
        return false;
    if (!in->xpresent[0][0])
        return false;
    primary = in->xnsub[0] >= 3U && in->xpresent[0][2]
                  ? in->xpar[0][2] : in->xpar[0][0];
    code = kitty_code(primary);
    if (code == 0U || !modifier(in, 1U, &mods))
        return false;
    if (in->npar >= 2U && in->xnsub[1] >= 2U && in->xpresent[1][1])
        event = in->xpar[1][1];
    if (event < SAG_KEY_PRESS || event > SAG_KEY_RELEASE)
        return false;
    for (i = 0U; i < in->xnsub[0]; i++) {
        if (in->xpresent[0][i] && kitty_code(in->xpar[0][i]) == 0U)
            return false;
    }
    if (in->npar >= 3U) {
        for (i = 0U; i < in->xnsub[2]; i++) {
            u32 text_cp = in->xpar[2][i];

            if (in->xpresent[2][i] && !is_scalar(text_cp))
                return false;
        }
    }
    emit_named(out, code, mods);
    out->ev = (u8)event;

    if (in->npar >= 3U) {
        for (i = 0U; i < in->xnsub[2]; i++) {
            if (!in->xpresent[2][i])
                continue;
            if (!append_text_cp(out, in->xpar[2][i]))
                break;
        }
    } else if ((mods & (SAG_MOD_CTRL | SAG_MOD_ALT | SAG_MOD_SUPER |
                        SAG_MOD_HYPER | SAG_MOD_META)) == 0U &&
               code < SAG_KEY_BASE) {
        u32 produced = in->xnsub[0] >= 2U && in->xpresent[0][1]
                           ? in->xpar[0][1] : in->xpar[0][0];
        (void)append_text_cp(out, produced);
    }
    return true;
}

static bool decode_mouse(Key *out, u32 cb, u32 cx, u32 cy, u8 final)
{
    u32 base;

    if (cx == 0U || cy == 0U || cx > 65536U || cy > 65536U)
        return false;
    out->mods = (u16)(((cb & 4U) ? SAG_MOD_SHIFT : 0U) |
                      ((cb & 8U) ? SAG_MOD_ALT : 0U) |
                      ((cb & 16U) ? SAG_MOD_CTRL : 0U));
    base = cb & ~(4U | 8U | 16U);
    out->kind = SAG_EV_MOUSE;
    out->code = 0U;
    out->col = (u16)(cx - 1U);
    out->row = (u16)(cy - 1U);
    out->ev = SAG_KEY_PRESS;

    if (final == (u8)'m' && base <= 2U) {
        out->button = (u8)(SAG_MB_LEFT + base);
        out->ev = SAG_KEY_RELEASE;
    } else if (final == (u8)'M' && base <= 2U) {
        out->button = (u8)(SAG_MB_LEFT + base);
    } else if (final == (u8)'M' && base >= 32U && base <= 34U) {
        out->button = (u8)(SAG_MB_LEFT + base - 32U);
        out->ev = SAG_KEY_REPEAT;
    } else if (final == (u8)'M' && base >= 64U && base <= 67U) {
        out->button = (u8)(SAG_MB_WHEEL_UP + base - 64U);
    } else if (final == (u8)'M' && base >= 128U && base <= 129U) {
        out->button = (u8)(SAG_MB_BACK + base - 128U);
    } else {
        clear_key(out);
        return false;
    }
    return true;
}

static bool simple_csi(In *in, u8 final, Key *out)
{
    u32 code = 0U;
    u16 mods;

    if (in->priv != 0U || in->inter != 0U || in->npar > 2U ||
        !params_plain(in) ||
        (!params_empty(in) && in->xpar[0][0] != 1U) ||
        !modifier(in, 1U, &mods))
        return false;
    switch (final) {
    case 'A': code = SAG_KEY_UP; break;
    case 'B': code = SAG_KEY_DOWN; break;
    case 'C': code = SAG_KEY_RIGHT; break;
    case 'D': code = SAG_KEY_LEFT; break;
    case 'E': code = SAG_KEY_BEGIN; break;
    case 'F': code = SAG_KEY_END; break;
    case 'H': code = SAG_KEY_HOME; break;
    case 'P': code = SAG_KEY_F1; break;
    case 'Q': code = SAG_KEY_F2; break;
    case 'R': code = SAG_KEY_F3; break;
    case 'S': code = SAG_KEY_F4; break;
    default: return false;
    }
    emit_named(out, code, mods);
    return true;
}

static bool dispatch_csi(In *in, u8 final, Key *out)
{
    u16 mods;
    u32 code;

    if (final == (u8)'u')
        return dispatch_kitty(in, out);
    if (final == (u8)'R' && in->priv == 0U && in->npar == 2U &&
        in->xpresent[0][0] && in->xpar[0][0] != 1U) {
        return false;
    }
    if (simple_csi(in, final, out))
        return true;
    if (final == (u8)'Z' && in->priv == 0U && in->inter == 0U &&
        params_empty(in)) {
        emit_named(out, SAG_KEY_TAB, SAG_MOD_SHIFT);
        return true;
    }
    if ((final == (u8)'I' || final == (u8)'O') && in->priv == 0U &&
        in->inter == 0U && params_empty(in)) {
        out->kind = SAG_EV_FOCUS;
        out->code = final == (u8)'I' ? SAG_KEY_FOCUS_IN : SAG_KEY_FOCUS_OUT;
        out->ev = SAG_KEY_PRESS;
        return true;
    }
    if (final == (u8)'~' && in->priv == 0U && in->inter == 0U &&
        in->npar <= 2U && params_plain(in) && modifier(in, 1U, &mods)) {
        if (in->xpresent[0][0] && in->xpar[0][0] == 200U &&
            in->npar == 1U) {
            in->state = IN_PASTE;
            in->in_paste = true;
            in->rd = in->scan - 1U;
            out->kind = SAG_EV_PASTE_BEGIN;
            return true;
        }
        if (in->xpresent[0][0] && in->xpar[0][0] == 201U &&
            in->npar == 1U)
            return false;
        code = in->xpresent[0][0] ? tilde_code(in->xpar[0][0]) : 0U;
        if (code != 0U) {
            emit_named(out, code, mods);
            return true;
        }
    }
    if ((final == (u8)'M' || final == (u8)'m') && in->priv == (u8)'<' &&
        in->inter == 0U && in->npar == 3U && params_plain(in) &&
        in->xpresent[0][0] &&
        in->xpresent[1][0] && in->xpresent[2][0] &&
        decode_mouse(out, in->xpar[0][0], in->xpar[1][0],
                     in->xpar[2][0], final))
        return true;
    return false;
}

static bool dispatch_ss3(u8 final, Key *out)
{
    u32 code;

    switch (final) {
    case 'A': code = SAG_KEY_UP; break;
    case 'B': code = SAG_KEY_DOWN; break;
    case 'C': code = SAG_KEY_RIGHT; break;
    case 'D': code = SAG_KEY_LEFT; break;
    case 'E': code = SAG_KEY_BEGIN; break;
    case 'F': code = SAG_KEY_END; break;
    case 'H': code = SAG_KEY_HOME; break;
    case 'P': code = SAG_KEY_F1; break;
    case 'Q': code = SAG_KEY_F2; break;
    case 'R': code = SAG_KEY_F3; break;
    case 'S': code = SAG_KEY_F4; break;
    case 'M': code = SAG_KEY_KP_ENTER; break;
    case 'X': code = SAG_KEY_KP_EQUAL; break;
    case 'j': code = SAG_KEY_KP_MULTIPLY; break;
    case 'k': code = SAG_KEY_KP_ADD; break;
    case 'l': code = SAG_KEY_KP_SEPARATOR; break;
    case 'm': code = SAG_KEY_KP_SUBTRACT; break;
    case 'n': code = SAG_KEY_KP_DECIMAL; break;
    case 'o': code = SAG_KEY_KP_DIVIDE; break;
    case 'p': case 'q': case 'r': case 's': case 't':
    case 'u': case 'v': case 'w': case 'x': case 'y':
        code = SAG_KEY_KP_0 + final - (u8)'p';
        break;
    default: return false;
    }
    emit_named(out, code, 0U);
    return true;
}

static void arm_deadline(In *in, i64 now_ms)
{
    if (!in->caps.kitty_kbd && in->deadline == 0)
        in->deadline = now_ms > INT64_MAX - SAG_ESC_TIMEOUT_MS
                           ? INT64_MAX : now_ms + SAG_ESC_TIMEOUT_MS;
}

static bool deadline_expired(const In *in, i64 now_ms)
{
    return in->eof || (in->deadline != 0 && now_ms >= in->deadline);
}

static bool expire_escape(In *in, Key *out)
{
    size_t after_escape = in->seq_start + 1U;

    if (after_escape > in->buf.len)
        after_escape = in->buf.len;
    in->rd = after_escape;
    in->scan = in->rd;
    in->state = IN_GROUND;
    in->deadline = 0;
    in->pending_mods = 0U;
    emit_named(out, SAG_KEY_ESCAPE, 0U);
    return true;
}

static bool parse_ground(In *in, Key *out)
{
    u8 b = in->buf.data[in->rd];
    u16 mods = in->pending_mods;

    if (b == 0x1BU && mods == 0U) {
        in->seq_start = in->rd;
        in->scan = in->rd + 1U;
        in->state = IN_ESC;
        return false;
    }
    if (b >= 0x80U) {
        size_t need;
        size_t have = in->buf.len - in->rd;
        u32 cp;
        size_t used;

        if (b >= 0xC2U && b <= 0xDFU)
            need = 2U;
        else if (b >= 0xE0U && b <= 0xEFU)
            need = 3U;
        else if (b >= 0xF0U && b <= 0xF4U)
            need = 4U;
        else
            need = 1U;
        if (have < need && !in->eof) {
            in->utf8_state = (u8)need;
            in->utf8_cp = 0U;
            return false;
        }
        used = sag_utf8_decode(in->buf.data + in->rd, have, &cp);
        in->rd += used;
        in->utf8_state = 0U;
        in->utf8_cp = cp;
        in->pending_mods = 0U;
        if (sag_utf8_is_escape(cp)) {
            note_drop(in, LOG_UTF8, "invalid UTF-8 byte");
            return true;
        }
        emit_scalar(out, cp, mods, in->buf.data + in->rd - used, used);
        return true;
    }

    in->rd++;
    in->pending_mods = 0U;
    if (b == 0U) {
        emit_named(out, (u32)' ', (u16)(mods | SAG_MOD_CTRL));
    } else if (b >= 1U && b <= 7U) {
        emit_named(out, (u32)('a' + b - 1U), (u16)(mods | SAG_MOD_CTRL));
    } else if (b == 8U) {
        emit_named(out, SAG_KEY_BACKSPACE, (u16)(mods | SAG_MOD_CTRL));
    } else if (b == 9U) {
        emit_named(out, SAG_KEY_TAB, mods);
    } else if (b == 10U) {
        emit_named(out, (u32)'j', (u16)(mods | SAG_MOD_CTRL));
    } else if (b >= 11U && b <= 12U) {
        emit_named(out, (u32)('a' + b - 1U), (u16)(mods | SAG_MOD_CTRL));
    } else if (b == 13U) {
        emit_named(out, SAG_KEY_ENTER, mods);
    } else if (b >= 14U && b <= 26U) {
        emit_named(out, (u32)('a' + b - 1U), (u16)(mods | SAG_MOD_CTRL));
    } else if (b >= 28U && b <= 31U) {
        emit_named(out, (u32)('\\' + b - 28U),
                   (u16)(mods | SAG_MOD_CTRL));
    } else if (b == 127U) {
        emit_named(out, SAG_KEY_BACKSPACE, mods);
    } else {
        emit_scalar(out, b, mods, &b, 1U);
    }
    return true;
}

static bool paste_next(In *in, Key *out)
{
    const size_t marker_len = sizeof(paste_end) - 1U;
    size_t available = in->buf.len - in->scan;
    size_t search_len = available;
    size_t i;
    size_t emit_len;

    if (search_len > SAG_PASTE_EMIT + marker_len)
        search_len = SAG_PASTE_EMIT + marker_len;
    for (i = 0U; i + marker_len <= search_len; i++) {
        if (memcmp(in->buf.data + in->scan + i, paste_end, marker_len) == 0)
            break;
    }
    if (i + marker_len <= search_len) {
        if (i != 0U) {
            emit_len = i < SAG_PASTE_EMIT ? i : SAG_PASTE_EMIT;
            bytebuf_append(&in->paste, in->buf.data + in->scan, emit_len);
            in->scan += emit_len;
            in->rd = in->scan;
            out->kind = SAG_EV_PASTE_DATA;
            return true;
        }
        in->scan += marker_len;
        in->rd = in->scan;
        in->state = IN_GROUND;
        in->in_paste = false;
        out->kind = SAG_EV_PASTE_END;
        return true;
    }
    if (in->eof) {
        if (available != 0U) {
            emit_len = available < SAG_PASTE_EMIT
                           ? available : SAG_PASTE_EMIT;
            bytebuf_append(&in->paste, in->buf.data + in->scan, emit_len);
            in->scan += emit_len;
            in->rd = in->scan;
            out->kind = SAG_EV_PASTE_DATA;
            return true;
        }
        in->state = IN_GROUND;
        in->in_paste = false;
        in->rd = in->scan;
        out->kind = SAG_EV_PASTE_END;
        return true;
    }
    if (available < SAG_PASTE_EMIT + marker_len)
        return false;
    emit_len = available - marker_len;
    if (emit_len > SAG_PASTE_EMIT)
        emit_len = SAG_PASTE_EMIT;
    bytebuf_append(&in->paste, in->buf.data + in->scan, emit_len);
    in->scan += emit_len;
    in->rd = in->scan;
    out->kind = SAG_EV_PASTE_DATA;
    return true;
}

void sag_input_init(In *in, const TtyCaps *caps)
{
    (void)memset(in, 0, sizeof(*in));
    bytebuf_init(&in->buf);
    bytebuf_init(&in->paste);
    in->paste.data = sag_xmalloc(SAG_PASTE_EMIT);
    in->paste.cap = SAG_PASTE_EMIT;
    if (caps != NULL)
        in->caps = *caps;
}

void sag_input_free(In *in)
{
    if (in == NULL)
        return;
    bytebuf_free(&in->buf);
    bytebuf_free(&in->paste);
    (void)memset(in, 0, sizeof(*in));
}

void sag_input_seed(In *in, const Bytebuf *pending)
{
    if (in == NULL || pending == NULL)
        return;
    sag_input_feed(in, pending->data, pending->len);
}

void sag_input_feed(In *in, const u8 *b, size_t n)
{
    size_t retained;
    size_t room;

    if (in == NULL || b == NULL || n == 0U)
        return;
    compact(in);
    retained = in->buf.len - in->rd;
    room = retained < SAG_IN_MAX_BUFFER ? SAG_IN_MAX_BUFFER - retained : 0U;
    if (n > room) {
        note_drop(in, LOG_BUFFER_CAP, "input buffer cap exceeded");
        n = room;
    }
    if (n != 0U) {
        if (!in->caps.kitty_kbd && in->deadline != 0 &&
            (in->state == IN_ESC || in->state == IN_CSI ||
             in->state == IN_SS3))
            in->deadline = 0;
        input_append(&in->buf, b, n);
    }
}

bool sag_input_next(In *in, i64 now_ms, Key *out)
{
    size_t before;

    if (in == NULL || out == NULL)
        return false;
    clear_key(out);
    in->paste.len = 0U;
    compact(in);
    before = in->rd;

    for (;;) {
        u8 b;

        if (in->state == IN_PASTE)
            return paste_next(in, out);

        bool scanning = in->state == IN_ESC || in->state == IN_CSI ||
                        in->state == IN_SS3 || in->state == IN_STRING ||
                        in->state == IN_STRING_ESC;

        if ((in->state == IN_ESC || in->state == IN_CSI ||
             in->state == IN_SS3) && deadline_expired(in, now_ms))
            return expire_escape(in, out);

        if (in->state == IN_STRING_DROP ||
            in->state == IN_STRING_DROP_ESC) {
            while (in->rd < in->buf.len) {
                b = in->buf.data[in->rd++];
                if (in->state == IN_STRING_DROP_ESC && b == (u8)'\\') {
                    in->state = IN_GROUND;
                    break;
                }
                if (b == 7U) {
                    in->state = IN_GROUND;
                    break;
                }
                in->state = b == 0x1BU ? IN_STRING_DROP_ESC
                                       : IN_STRING_DROP;
            }
            if (in->eof)
                in->state = IN_GROUND;
            if (in->state != IN_GROUND)
                return false;
            continue;
        }

        if (in->state == IN_CSI_DROP) {
            while (in->rd < in->buf.len) {
                b = in->buf.data[in->rd++];
                if (b >= 0x40U && b <= 0x7EU) {
                    in->state = IN_GROUND;
                    break;
                }
            }
            if (in->eof && in->rd == in->buf.len)
                in->state = IN_GROUND;
            if (in->state != IN_GROUND)
                return false;
            continue;
        }

        if (in->state == IN_X10) {
            u32 cb;
            u32 cx;
            u32 cy;

            if (in->buf.len - in->rd < 3U) {
                if (!in->eof)
                    return false;
                in->rd = in->buf.len;
                in->state = IN_GROUND;
                note_drop(in, LOG_MOUSE, "truncated X10 mouse report");
                return true;
            }
            b = in->buf.data[in->rd];
            if (b < 32U || in->buf.data[in->rd + 1U] < 32U ||
                in->buf.data[in->rd + 2U] < 32U) {
                in->rd += 3U;
                in->state = IN_GROUND;
                note_drop(in, LOG_MOUSE, "malformed X10 mouse report");
                return true;
            }
            cb = (u32)b - 32U;
            cx = (u32)in->buf.data[in->rd + 1U] - 32U;
            cy = (u32)in->buf.data[in->rd + 2U] - 32U;
            in->rd += 3U;
            in->state = IN_GROUND;
            if (!decode_mouse(out, cb, cx, cy, (u8)'M')) {
                note_drop(in, LOG_MOUSE, "unsupported X10 mouse report");
                return true;
            }
            return true;
        }

        if ((scanning ? in->scan : in->rd) == in->buf.len) {
            if (in->state == IN_STRING || in->state == IN_STRING_ESC) {
                if (in->eof) {
                    in->rd = in->scan;
                    in->state = IN_GROUND;
                    note_string_drop(in, LOG_UNKNOWN,
                                     "unterminated terminal string sequence");
                    return false;
                }
                return false;
            }
            if (in->state != IN_GROUND) {
                arm_deadline(in, now_ms);
                if (deadline_expired(in, now_ms))
                    return expire_escape(in, out);
            }
            return false;
        }

        b = in->buf.data[scanning ? in->scan : in->rd];
        if (in->state == IN_GROUND) {
            bool emitted = parse_ground(in, out);

            if (emitted)
                return true;
            if (in->state == IN_ESC)
                continue;
            return false;
        }
        if (in->state == IN_ESC) {
            in->scan++;
            if (b == (u8)'[') {
                in->state = IN_CSI;
                reset_params(in);
            } else if (b == (u8)'O') {
                in->state = IN_SS3;
            } else if (b == (u8)']' || b == (u8)'P' || b == (u8)'_' ||
                       b == (u8)'^' || b == (u8)'X') {
                in->state = IN_STRING;
                in->string_len = 0U;
                in->deadline = 0;
            } else if (b == 0x1BU) {
                emit_named(out, SAG_KEY_ESCAPE, 0U);
                in->rd = in->scan - 1U;
                in->seq_start = in->rd;
                in->scan = in->rd + 1U;
                in->deadline = 0;
                if (in->scan == in->buf.len)
                    arm_deadline(in, now_ms);
                return true;
            } else {
                in->state = IN_GROUND;
                in->deadline = 0;
                in->rd = in->scan - 1U;
                in->pending_mods = SAG_MOD_ALT;
            }
            continue;
        }
        if (in->state == IN_SS3) {
            if (b < 0x40U || b > 0x7EU) {
                in->scan++;
                in->rd = in->scan;
                in->state = IN_GROUND;
                in->deadline = 0;
                note_drop(in, LOG_MALFORMED, "malformed SS3 sequence");
                return true;
            }
            in->scan++;
            in->rd = in->scan;
            in->state = IN_GROUND;
            in->deadline = 0;
            if (!dispatch_ss3(b, out)) {
                note_drop(in, LOG_UNKNOWN, "unknown SS3 sequence");
                return true;
            }
            return true;
        }
        if (in->state == IN_STRING || in->state == IN_STRING_ESC) {
            in->scan++;
            if (in->state == IN_STRING_ESC && b == (u8)'\\') {
                in->rd = in->scan;
                in->state = IN_GROUND;
                in->deadline = 0;
                note_string_drop(in, LOG_UNKNOWN,
                                 "terminal string sequence");
                continue;
            }
            if (b == 7U) {
                in->rd = in->scan;
                in->state = IN_GROUND;
                in->deadline = 0;
                note_string_drop(in, LOG_UNKNOWN,
                                 "terminal string sequence");
                continue;
            }
            in->string_len++;
            in->state = b == 0x1BU ? IN_STRING_ESC : IN_STRING;
            if (in->string_len > SAG_IN_STRING_MAX) {
                in->rd = in->scan;
                in->state = b == 0x1BU ? IN_STRING_DROP_ESC
                                       : IN_STRING_DROP;
                in->deadline = 0;
                note_string_drop(in, LOG_STRING_CAP,
                                 "terminal string cap exceeded");
                return true;
            }
            continue;
        }
        if (in->state == IN_CSI) {
            size_t pi = in->npar - 1U;
            size_t si = in->xnsub[pi] - 1U;

            if (b >= (u8)'0' && b <= (u8)'9' && in->inter == 0U) {
                u32 digit = (u32)(b - (u8)'0');
                u32 value = in->xpar[pi][si];

                in->scan++;
                value = value > (UINT32_MAX - digit) / 10U
                            ? UINT32_MAX : value * 10U + digit;
                in->xpar[pi][si] = value;
                in->xpresent[pi][si] = true;
                if (si == 0U)
                    in->par[pi] = value > 65535U ? 65535U : (u16)value;
                continue;
            }
            if (b == (u8)';' && in->inter == 0U && in->npar < 16U) {
                in->scan++;
                in->npar++;
                in->xnsub[in->npar - 1U] = 1U;
                continue;
            }
            if (b == (u8)':' && in->inter == 0U &&
                in->xnsub[pi] < 16U) {
                in->scan++;
                in->xnsub[pi]++;
                in->sub[pi] = (u8)(in->xnsub[pi] - 1U);
                continue;
            }
            if ((b == (u8)'?' || b == (u8)'>' || b == (u8)'<' ||
                 b == (u8)'=') && pi == 0U && si == 0U &&
                !in->xpresent[0][0] && in->priv == 0U) {
                in->scan++;
                in->priv = b;
                continue;
            }
            if (b >= 0x20U && b <= 0x2FU) {
                in->scan++;
                in->inter = b;
                continue;
            }
            if (b >= 0x40U && b <= 0x7EU) {
                bool emitted;

                in->scan++;
                in->rd = in->scan;
                if (b == (u8)'M' && in->priv == 0U && in->inter == 0U &&
                    params_empty(in)) {
                    in->state = IN_X10;
                    in->deadline = 0;
                    continue;
                }
                in->state = IN_GROUND;
                in->deadline = 0;
                emitted = dispatch_csi(in, b, out);
                if (!emitted) {
                    note_drop(in, LOG_UNKNOWN, "unknown CSI sequence");
                    return true;
                }
                return true;
            }
            in->scan++;
            in->rd = in->scan;
            in->state = IN_CSI_DROP;
            in->deadline = 0;
            note_drop(in, LOG_MALFORMED, "malformed CSI sequence");
            return true;
        }
        if (in->rd == before)
            return false;
    }
}

i64 sag_input_deadline(const In *in, i64 now_ms)
{
    i64 left;

    if (in == NULL || in->deadline == 0 || in->caps.kitty_kbd)
        return -1;
    left = in->deadline - now_ms;
    return left > 0 ? left : 0;
}

void sag_input_eof(In *in)
{
    if (in != NULL)
        in->eof = true;
}

const u8 *sag_input_paste_chunk(const In *in, size_t *n)
{
    if (n != NULL)
        *n = in == NULL ? 0U : in->paste.len;
    return in == NULL ? NULL : in->paste.data;
}

void sag_input_enable(int wfd, const TtyCaps *caps)
{
    write_blob(wfd, input_enable, sizeof(input_enable) - 1U);
    if (caps != NULL && caps->kitty_kbd)
        write_blob(wfd, kitty_enable, sizeof(kitty_enable) - 1U);
}

void sag_input_disable(int wfd)
{
    write_blob(wfd, input_disable, sizeof(input_disable) - 1U);
}
