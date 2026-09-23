#define _POSIX_C_SOURCE 200809L

#include "ui/cmdhist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/secret.h"
#include "util/xdg.h"

struct CmdHist {
    char **entries;
    size_t len;
    size_t cap;
    char *path;
    bool memory;
};

static char *hist_dup_n(const char *text, size_t len)
{
    char *copy = yew_xmalloc(len + 1U);

    if (len != 0U)
        (void)memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static char *hist_dup(const char *text)
{
    return hist_dup_n(text, strlen(text));
}

static void hist_reserve(CmdHist *h, size_t need)
{
    size_t cap;

    if (h->cap >= need)
        return;
    cap = h->cap == 0U ? 16U : h->cap;
    while (cap < need)
        cap *= 2U;
    h->entries = yew_xreallocarray(h->entries, cap, sizeof(*h->entries));
    h->cap = cap;
}

static void hist_remove(CmdHist *h, size_t index)
{
    yew_xfree(h->entries[index]);
    if (index + 1U < h->len) {
        (void)memmove(&h->entries[index], &h->entries[index + 1U],
                      (h->len - index - 1U) * sizeof(*h->entries));
    }
    h->len--;
}

static bool hist_store(CmdHist *h, const char *line, size_t len)
{
    size_t i;
    char *copy;

    if (len == 0U || line[0] == ' ')
        return false;
    if (len > YEW_HIST_LINE_MAX)
        len = YEW_HIST_LINE_MAX;
    copy = hist_dup_n(line, len);
    for (i = 0U; i < h->len; i++) {
        if (strcmp(h->entries[i], copy) == 0) {
            if (i + 1U == h->len) {
                yew_xfree(copy);
                return false;
            }
            hist_remove(h, i);
            break;
        }
    }
    hist_reserve(h, h->len + 1U);
    h->entries[h->len++] = copy;
    if (h->len > YEW_HIST_MAX)
        hist_remove(h, 0U);
    return true;
}

static bool hist_kind_valid(const char *kind)
{
    const unsigned char *p = (const unsigned char *)kind;

    if (kind == NULL || kind[0] == '\0')
        return false;
    for (; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

static char *hist_path(const char *kind)
{
    char *state;
    char *dir;
    size_t size;
    char *path;

    if (!hist_kind_valid(kind))
        return NULL;
    state = yew_xdg_state_dir();
    if (state == NULL)
        return NULL;
    size = strlen(state) + strlen("/history") + 1U;
    dir = yew_xmalloc(size);
    (void)snprintf(dir, size, "%s/history", state);
    yew_xfree(state);
    if (!yew_mkdirs(dir, 0700U)) {
        yew_xfree(dir);
        return NULL;
    }
    size = strlen(dir) + 1U + strlen(kind) + 1U;
    path = yew_xmalloc(size);
    (void)snprintf(path, size, "%s/%s", dir, kind);
    yew_xfree(dir);
    return path;
}

static void hist_escape(Bytebuf *out, const char *line)
{
    static const char hex[] = "0123456789ABCDEF";
    const u8 *p = (const u8 *)line;
    size_t i;
    size_t len = strlen(line);

    if (len > YEW_HIST_LINE_MAX)
        len = YEW_HIST_LINE_MAX;
    for (i = 0U; i < len; i++) {
        u8 byte = p[i];

        if (byte == (u8)'\\') {
            bytebuf_append(out, "\\\\", 2U);
        } else if (byte == (u8)'\n') {
            bytebuf_append(out, "\\n", 2U);
        } else if (byte == (u8)'\t') {
            bytebuf_append(out, "\\t", 2U);
        } else if (byte < 0x20U || byte == 0x7fU) {
            char escaped[4] = {'\\', 'x', hex[byte >> 4U],
                               hex[byte & 0x0fU]};
            bytebuf_append(out, escaped, sizeof(escaped));
        } else {
            bytebuf_push_u8(out, byte);
        }
    }
}

static int hex_value(u8 byte)
{
    if (byte >= (u8)'0' && byte <= (u8)'9')
        return (int)(byte - (u8)'0');
    if (byte >= (u8)'a' && byte <= (u8)'f')
        return 10 + (int)(byte - (u8)'a');
    if (byte >= (u8)'A' && byte <= (u8)'F')
        return 10 + (int)(byte - (u8)'A');
    return -1;
}

static char *hist_unescape(const u8 *line, size_t len)
{
    Bytebuf out;
    size_t i;
    char *text;

    bytebuf_init(&out);
    for (i = 0U; i < len; i++) {
        u8 byte = line[i];

        if (byte != (u8)'\\') {
            if (byte == 0U)
                goto corrupt;
            bytebuf_push_u8(&out, byte);
            continue;
        }
        if (++i >= len)
            goto corrupt;
        byte = line[i];
        if (byte == (u8)'\\')
            bytebuf_push_u8(&out, (u8)'\\');
        else if (byte == (u8)'n')
            bytebuf_push_u8(&out, (u8)'\n');
        else if (byte == (u8)'t')
            bytebuf_push_u8(&out, (u8)'\t');
        else if (byte == (u8)'x') {
            int hi;
            int lo;

            if (i + 2U >= len)
                goto corrupt;
            hi = hex_value(line[i + 1U]);
            lo = hex_value(line[i + 2U]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
                goto corrupt;
            bytebuf_push_u8(&out, (u8)((u32)hi * 16U + (u32)lo));
            i += 2U;
        } else {
            goto corrupt;
        }
        if (out.len > YEW_HIST_LINE_MAX)
            out.len = YEW_HIST_LINE_MAX;
    }
    bytebuf_push_u8(&out, 0U);
    text = (char *)out.data;
    return text;

corrupt:
    bytebuf_free(&out);
    return NULL;
}

static void hist_parse_line(CmdHist *h, const Bytebuf *line)
{
    char *decoded = hist_unescape(line->data, line->len);

    if (decoded == NULL) {
        yew_log(YEW_LOG_WARN, "dropping corrupt history entry in %s",
                h->path == NULL ? "memory history" : h->path);
        return;
    }
    (void)hist_store(h, decoded, strlen(decoded));
    yew_xfree(decoded);
}

static bool hist_read(CmdHist *h)
{
    enum { ENCODED_MAX = YEW_HIST_LINE_MAX * 4 };
    u8 block[4096];
    Bytebuf line;
    bool overlong = false;
    int fd;

    if (h->path == NULL)
        return false;
    fd = open(h->path, O_RDONLY
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
    );
    if (fd < 0) {
        int error = errno;

        if (error != ENOENT)
            yew_log(YEW_LOG_WARN, "cannot read command history %s: %s",
                    h->path, strerror(error));
        return error == ENOENT;
    }
    bytebuf_init(&line);
    for (;;) {
        ssize_t got = read(fd, block, sizeof(block));
        size_t i;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            yew_log(YEW_LOG_WARN, "cannot read command history %s: %s",
                    h->path, strerror(errno));
            bytebuf_free(&line);
            (void)close(fd);
            return false;
        }
        if (got == 0)
            break;
        for (i = 0U; i < (size_t)got; i++) {
            if (block[i] == (u8)'\n') {
                if (overlong) {
                    yew_log(YEW_LOG_WARN,
                            "dropping overlong history entry in %s", h->path);
                } else {
                    hist_parse_line(h, &line);
                }
                line.len = 0U;
                overlong = false;
            } else if (!overlong) {
                if (line.len == ENCODED_MAX) {
                    overlong = true;
                    line.len = 0U;
                } else {
                    bytebuf_push_u8(&line, block[i]);
                }
            }
        }
    }
    if (line.len != 0U || overlong) {
        yew_log(YEW_LOG_WARN, "dropping incomplete history entry in %s",
                h->path);
    }
    bytebuf_free(&line);
    (void)close(fd);
    return true;
}

static int hist_lock(const char *path)
{
    struct flock lock = {0};
    size_t size = strlen(path) + strlen(".lock") + 1U;
    char *lock_path = yew_xmalloc(size);
    int fd;

    (void)snprintf(lock_path, size, "%s.lock", path);
    fd = open(lock_path, O_RDWR | O_CREAT
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif
              , 0600);
    yew_xfree(lock_path);
    if (fd < 0)
        return -1;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            (void)close(fd);
            return -1;
        }
    }
    return fd;
}

static void hist_clear(CmdHist *h)
{
    while (h->len != 0U)
        hist_remove(h, h->len - 1U);
}

CmdHist *yew_hist_open(const char *kind)
{
    CmdHist *h = yew_xcalloc(1U, sizeof(*h));

    h->path = hist_path(kind);
    h->memory = h->path == NULL;
    if (h->memory) {
        yew_log(YEW_LOG_WARN,
                "command history unavailable; using in-memory history");
    } else {
        (void)hist_read(h);
    }
    return h;
}

/* <ws_dir>/history/<kind>, created 0700 like every other state dir. */
static char *ws_hist_path(const char *kind, const char *ws_dir)
{
    char *dir;
    char *path;
    size_t size;

    if (!hist_kind_valid(kind) || ws_dir == NULL || ws_dir[0] == '\0')
        return NULL;
    size = strlen(ws_dir) + strlen("/history") + 1U;
    dir = yew_xmalloc(size);
    (void)snprintf(dir, size, "%s/history", ws_dir);
    if (!yew_mkdirs(dir, 0700U)) {
        yew_xfree(dir);
        return NULL;
    }
    size = strlen(dir) + 1U + strlen(kind) + 1U;
    path = yew_xmalloc(size);
    (void)snprintf(path, size, "%s/%s", dir, kind);
    yew_xfree(dir);
    return path;
}

CmdHist *yew_hist_open_scoped(const char *kind, const char *ws_dir,
                              bool workspace_scope)
{
    CmdHist *h = yew_xcalloc(1U, sizeof(*h));
    char *global = hist_path(kind);
    char *local = ws_hist_path(kind, ws_dir);

    /*
     * Loaded in this order on purpose: global first, workspace second.
     * hist_store moves a duplicate to the END, so a command run in both
     * places comes back once, dated by its most local use — which is
     * what "the most local are newest" means under s18's newest-last
     * convention.
     */
    if (global != NULL) {
        h->path = global;
        (void)hist_read(h);
    }
    if (local != NULL) {
        h->path = local;
        (void)hist_read(h);
    }
    /*
     * And now the only thing that decides where WRITES go.  The merged
     * list in memory is never written anywhere as a unit: yew_hist_add
     * appends one line here, and yew_hist_flush re-reads THIS file and
     * compacts it, so the merge cannot leak from one scope to the
     * other.
     */
    if (workspace_scope && local != NULL) {
        h->path = local;
        yew_xfree(global);
    } else if (!workspace_scope && global != NULL) {
        h->path = global;
        yew_xfree(local);
    } else {
        /*
         * The requested scope has no file — a stateless session asking
         * for workspace scope, or an unusable state home asking for
         * global.  Fall back to whichever exists rather than to an
         * in-memory history: losing every command the user types this
         * session is a worse answer to "I could not make one directory"
         * than writing to the other one.
         */
        h->path = local != NULL ? local : global;
        if (h->path == local)
            yew_xfree(global);
        else
            yew_xfree(local);
    }
    h->memory = h->path == NULL;
    if (h->memory) {
        yew_log(YEW_LOG_WARN,
                "command history unavailable; using in-memory history");
    }
    return h;
}

const char *yew_hist_path(const CmdHist *h)
{
    return h == NULL ? NULL : h->path;
}

CmdHist *yew_hist_open_memory(void)
{
    CmdHist *h = yew_xcalloc(1U, sizeof(*h));

    h->memory = true;
    return h;
}

void yew_hist_close(CmdHist *h)
{
    if (h == NULL)
        return;
    hist_clear(h);
    yew_xfree(h->entries);
    yew_xfree(h->path);
    yew_xfree(h);
}

void yew_hist_add(CmdHist *h, const char *line)
{
    Bytebuf encoded;
    int lock_fd;
    int fd;
    ssize_t wrote;

    if (h == NULL || line == NULL || line[0] == '\0' || line[0] == ' ')
        return;
    if (!hist_store(h, line, strlen(line)) || h->memory)
        return;
    bytebuf_init(&encoded);
    hist_escape(&encoded, line);
    bytebuf_push_u8(&encoded, (u8)'\n');
    lock_fd = hist_lock(h->path);
    fd = open(h->path, O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
                            | O_CLOEXEC
#endif
              , 0600);
    if (fd < 0) {
        yew_log(YEW_LOG_WARN, "cannot append command history %s: %s",
                h->path, strerror(errno));
    } else {
        do {
            wrote = write(fd, encoded.data, encoded.len);
        } while (wrote < 0 && errno == EINTR);
        if (wrote < 0 || (size_t)wrote != encoded.len) {
            yew_log(YEW_LOG_WARN, "short append to command history %s",
                    h->path);
        }
        (void)close(fd);
    }
    if (lock_fd >= 0)
        (void)close(lock_fd);
    bytebuf_free(&encoded);
}

void yew_hist_flush(CmdHist *h)
{
    CmdHist merged = {0};
    Bytebuf encoded;
    int lock_fd;
    size_t i;

    if (h == NULL || h->memory)
        return;
    lock_fd = hist_lock(h->path);
    if (lock_fd < 0) {
        yew_log(YEW_LOG_WARN, "cannot lock command history %s: %s", h->path,
                strerror(errno));
        return;
    }
    merged.path = h->path;
    if (!hist_read(&merged)) {
        hist_clear(&merged);
        yew_xfree(merged.entries);
        (void)close(lock_fd);
        return;
    }
    bytebuf_init(&encoded);
    for (i = 0U; i < merged.len; i++) {
        hist_escape(&encoded, merged.entries[i]);
        bytebuf_push_u8(&encoded, (u8)'\n');
    }
    if (yew_file_write_atomic(h->path, encoded.data, encoded.len, 0600) !=
        YEW_SAVE_OK) {
        yew_log(YEW_LOG_WARN, "cannot compact command history %s: %s",
                h->path, strerror(errno));
    } else {
        hist_clear(h);
        yew_xfree(h->entries);
        h->entries = merged.entries;
        h->len = merged.len;
        h->cap = merged.cap;
        merged.entries = NULL;
        merged.len = 0U;
        merged.cap = 0U;
    }
    bytebuf_free(&encoded);
    hist_clear(&merged);
    yew_xfree(merged.entries);
    (void)close(lock_fd);
}

size_t yew_hist_len(const CmdHist *h)
{
    return h == NULL ? 0U : h->len;
}

const char *yew_hist_at(const CmdHist *h, size_t index)
{
    return h == NULL || index >= h->len ? NULL : h->entries[index];
}

bool yew_hist_is_memory(const CmdHist *h)
{
    return h == NULL || h->memory;
}

void yew_hist_cur_reset(HistCur *c, const char *draft)
{
    if (c == NULL)
        return;
    yew_xfree(c->stem);
    yew_xfree(c->draft);
    c->idx = -1;
    c->stem = NULL;
    c->draft = hist_dup(draft == NULL ? "" : draft);
}

void yew_hist_cur_dispose(HistCur *c)
{
    if (c == NULL)
        return;
    yew_xfree(c->stem);
    yew_xfree(c->draft);
    *c = (HistCur){.idx = -1};
}

static bool hist_has_prefix(const char *entry, const char *stem)
{
    return strncmp(entry, stem, strlen(stem)) == 0;
}

const char *yew_hist_prev(CmdHist *h, HistCur *c)
{
    i32 i;

    if (h == NULL || c == NULL)
        return NULL;
    if (c->draft == NULL)
        c->draft = hist_dup("");
    if (c->stem == NULL)
        c->stem = hist_dup(c->draft);
    i = c->idx < 0 ? (i32)h->len - 1 : c->idx - 1;
    for (; i >= 0; i--) {
        if (hist_has_prefix(h->entries[(size_t)i], c->stem)) {
            c->idx = i;
            return h->entries[(size_t)i];
        }
    }
    return NULL;
}

const char *yew_hist_next(CmdHist *h, HistCur *c)
{
    size_t i;

    if (h == NULL || c == NULL || c->idx < 0)
        return NULL;
    for (i = (size_t)c->idx + 1U; i < h->len; i++) {
        if (hist_has_prefix(h->entries[i], c->stem == NULL ? "" : c->stem)) {
            c->idx = (i32)i;
            return h->entries[i];
        }
    }
    c->idx = -1;
    return c->draft == NULL ? "" : c->draft;
}

/* ================================================================ */
/* Sprint 57.26 §3: history suggestions                              */
/* ================================================================ */

static u32 hist_shell_opens;

u32 yew_hist_test_shell_opens(void)
{
    return hist_shell_opens;
}

void yew_hist_test_reset_shell_opens(void)
{
    hist_shell_opens = 0U;
}

void yew_hist_suggest_init(YewHistSuggest *s)
{
    if (s != NULL)
        (void)memset(s, 0, sizeof(*s));
}

void yew_hist_suggest_free(YewHistSuggest *s)
{
    u32 i;

    if (s == NULL)
        return;
    for (i = 0U; i < s->n; i++)
        yew_xfree(s->v[i]);
    yew_xfree(s->v);
    yew_xfree(s->lens);
    yew_xfree(s->slots);
    (void)memset(s, 0, sizeof(*s));
}

static u32 suggest_hash(const char *text, size_t len)
{
    u32 h = 2166136261U;
    size_t i;

    for (i = 0U; i < len; i++) {
        h ^= (u32)(u8)text[i];
        h *= 16777619U;
    }
    return h;
}

static bool is_blank_byte(char c)
{
    return c == ' ' || c == '\t';
}

static bool name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool name_char(char c)
{
    return name_start(c) || (c >= '0' && c <= '9');
}

bool yew_hist_suggest_refused(const char *text, size_t len)
{
    size_t i = 0U;

    for (i = 0U; i < len; i++) {
        u8 c = (u8)text[i];

        /* A newline is a multi-line command; any control byte is one a
         * ghost cannot draw. */
        if (c < 0x20U || c == 0x7FU)
            return true;
    }
    /*
     * `NAME=value`, anywhere on the line: `export API_TOKEN=…`,
     * `env AWS_SECRET_ACCESS_KEY=… cmd`, `a=1;DB_PASSWORD=…`.  Any name
     * that starts at a word boundary counts, so `--token=…` is refused
     * too -- a secret on screen is the harm, whatever spelled it.
     */
    i = 0U;
    while (i < len) {
        size_t start;

        if (!name_start(text[i]) || (i != 0U && name_char(text[i - 1U]))) {
            i++;
            continue;
        }
        start = i;
        while (i < len && name_char(text[i]))
            i++;
        if (i < len && text[i] == '=') {
            char name[128];
            size_t n = i - start;

            if (n >= sizeof(name))
                n = sizeof(name) - 1U;
            (void)memcpy(name, text + start, n);
            name[n] = '\0';
            if (yew_secret_name(name))
                return true;
        }
    }
    return false;
}

static bool suggest_has(const YewHistSuggest *s, const char *text,
                        size_t len, u32 h)
{
    u32 mask;
    u32 at;

    if (s->n_slots == 0U)
        return false;
    mask = s->n_slots - 1U;
    for (at = h & mask; s->slots[at] != 0U; at = (at + 1U) & mask) {
        u32 k = s->slots[at] - 1U;

        if (s->lens[k] == len && memcmp(s->v[k], text, len) == 0)
            return true;
    }
    return false;
}

static void suggest_slot(YewHistSuggest *s, u32 index, u32 h)
{
    u32 mask = s->n_slots - 1U;
    u32 at;

    for (at = h & mask; s->slots[at] != 0U; at = (at + 1U) & mask)
        ;
    s->slots[at] = index + 1U;
}

static void suggest_grow(YewHistSuggest *s)
{
    u32 i;

    if (s->n == s->cap) {
        s->cap = s->cap == 0U ? 256U : s->cap * 2U;
        s->v = yew_xreallocarray(s->v, s->cap, sizeof(*s->v));
        s->lens = yew_xreallocarray(s->lens, s->cap, sizeof(*s->lens));
    }
    /* Keep the set at most half full. */
    if ((s->n + 1U) * 2U > s->n_slots) {
        u32 slots = s->n_slots == 0U ? 512U : s->n_slots * 2U;

        yew_xfree(s->slots);
        s->slots = yew_xcalloc(slots, sizeof(*s->slots));
        s->n_slots = slots;
        for (i = 0U; i < s->n; i++)
            suggest_slot(s, i, suggest_hash(s->v[i], s->lens[i]));
    }
}

bool yew_hist_suggest_add(YewHistSuggest *s, const char *text, size_t len)
{
    u32 h;

    if (s == NULL || text == NULL)
        return false;
    while (len != 0U && is_blank_byte(text[0])) {
        text++;
        len--;
    }
    if (len == 0U || len > YEW_HIST_LINE_MAX || s->n >= YEW_HIST_SUGGEST_MAX)
        return false;
    if (yew_hist_suggest_refused(text, len))
        return false;
    h = suggest_hash(text, len);
    if (suggest_has(s, text, len, h))
        return false;
    suggest_grow(s);
    s->v[s->n] = hist_dup_n(text, len);
    s->lens[s->n] = (u32)len;
    suggest_slot(s, s->n, h);
    s->n++;
    return true;
}

const char *yew_hist_suggest_match(const YewHistSuggest *s, const char *body,
                                   size_t len, size_t *rest_len)
{
    u32 i;

    if (rest_len != NULL)
        *rest_len = 0U;
    if (s == NULL || body == NULL)
        return NULL;
    while (len != 0U && is_blank_byte(body[0])) {
        body++;
        len--;
    }
    if (len == 0U)
        return NULL;
    /* Newest first: the first hit is the answer.  One prefix compare
     * per entry, nothing else, on every keystroke. */
    for (i = 0U; i < s->n; i++) {
        if (s->lens[i] > len && memcmp(s->v[i], body, len) == 0) {
            if (rest_len != NULL)
                *rest_len = s->lens[i] - len;
            return s->v[i] + len;
        }
    }
    return NULL;
}

/* Entries collected oldest first, then offered newest first. */
typedef struct HistEntries {
    char **v;
    size_t *lens;
    size_t n;
    size_t cap;
} HistEntries;

static void entries_push(HistEntries *e, char *text, size_t len)
{
    if (e->n == e->cap) {
        e->cap = e->cap == 0U ? 64U : e->cap * 2U;
        e->v = yew_xreallocarray(e->v, e->cap, sizeof(*e->v));
        e->lens = yew_xreallocarray(e->lens, e->cap, sizeof(*e->lens));
    }
    e->v[e->n] = text;
    e->lens[e->n] = len;
    e->n++;
}

static void entries_offer(YewHistSuggest *s, HistEntries *e)
{
    size_t i;

    for (i = e->n; i > 0U; i--) {
        (void)yew_hist_suggest_add(s, e->v[i - 1U], e->lens[i - 1U]);
        yew_xfree(e->v[i - 1U]);
    }
    yew_xfree(e->v);
    yew_xfree(e->lens);
    (void)memset(e, 0, sizeof(*e));
}

/* The next line of [data, data+len) from `*at`: its length, and `*at`
 * moved past its newline.  False at the end. */
static bool next_line(const char *data, size_t len, size_t *at,
                      const char **line, size_t *n)
{
    const char *nl;

    if (*at >= len)
        return false;
    *line = data + *at;
    nl = memchr(*line, '\n', len - *at);
    *n = nl == NULL ? len - *at : (size_t)(nl - *line);
    *at += *n + 1U;
    return true;
}

void yew_hist_parse_fish(YewHistSuggest *s, const char *data, size_t len)
{
    static const char tag[] = "- cmd: ";
    HistEntries e = {0};
    const char *line;
    size_t n;
    size_t at = 0U;

    if (s == NULL || data == NULL)
        return;
    while (next_line(data, len, &at, &line, &n)) {
        char *text;
        size_t i;
        size_t out = 0U;

        if (n < sizeof(tag) - 1U || memcmp(line, tag, sizeof(tag) - 1U) != 0)
            continue;
        line += sizeof(tag) - 1U;
        n -= sizeof(tag) - 1U;
        text = yew_xmalloc(n + 1U);
        /* fish's own escaping of the value: `\\` and `\n`, nothing
         * else (any other backslash is literal). */
        for (i = 0U; i < n; i++) {
            if (line[i] == '\\' && i + 1U < n && line[i + 1U] == '\\') {
                text[out++] = '\\';
                i++;
            } else if (line[i] == '\\' && i + 1U < n &&
                       line[i + 1U] == 'n') {
                text[out++] = '\n';
                i++;
            } else {
                text[out++] = line[i];
            }
        }
        text[out] = '\0';
        entries_push(&e, text, out);
    }
    entries_offer(s, &e);
}

size_t yew_hist_unmetafy(char *bytes, size_t len)
{
    size_t in;
    size_t out = 0U;

    if (bytes == NULL)
        return 0U;
    for (in = 0U; in < len; in++) {
        if ((u8)bytes[in] == 0x83U && in + 1U < len) {
            bytes[out++] = (char)((u8)bytes[in + 1U] ^ 0x20U);
            in++;
        } else {
            bytes[out++] = bytes[in];
        }
    }
    return out;
}

/* `: 1790129293:0;` -- EXTENDED_HISTORY's start and elapsed time. */
static size_t zsh_prefix_len(const char *line, size_t n)
{
    size_t i = 2U;

    if (n < 2U || line[0] != ':' || line[1] != ' ')
        return 0U;
    if (i >= n || line[i] < '0' || line[i] > '9')
        return 0U;
    while (i < n && line[i] >= '0' && line[i] <= '9')
        i++;
    if (i >= n || line[i] != ':')
        return 0U;
    i++;
    while (i < n && line[i] >= '0' && line[i] <= '9')
        i++;
    if (i >= n || line[i] != ';')
        return 0U;
    return i + 1U;
}

void yew_hist_parse_zsh(YewHistSuggest *s, const char *data, size_t len)
{
    HistEntries e = {0};
    Bytebuf entry;
    char *plain;
    const char *line;
    size_t n;
    size_t at = 0U;
    bool open = false;

    if (s == NULL || data == NULL)
        return;
    /* Before anything else reads a byte: metafied text read as text is
     * mojibake, and a suggestion taken from it names a file that does not
     * exist (invariant 2). */
    plain = hist_dup_n(data, len);
    len = yew_hist_unmetafy(plain, len);
    bytebuf_init(&entry);
    while (next_line(plain, len, &at, &line, &n)) {
        bool more;

        if (!open) {
            size_t skip = zsh_prefix_len(line, n);

            line += skip;
            n -= skip;
            entry.len = 0U;
        } else {
            bytebuf_push_u8(&entry, (u8)'\n');
        }
        /* A line ending in a backslash continues: the command was
         * multi-line, and stays one entry (which is then refused). */
        more = n != 0U && line[n - 1U] == '\\';
        bytebuf_append(&entry, line, more ? n - 1U : n);
        open = more;
        if (!open)
            entries_push(&e, hist_dup_n((const char *)entry.data, entry.len),
                         entry.len);
    }
    if (open)
        entries_push(&e, hist_dup_n((const char *)entry.data, entry.len),
                     entry.len);
    bytebuf_free(&entry);
    yew_xfree(plain);
    entries_offer(s, &e);
}

void yew_hist_parse_bash(YewHistSuggest *s, const char *data, size_t len)
{
    HistEntries e = {0};
    const char *line;
    size_t n;
    size_t at = 0U;

    if (s == NULL || data == NULL)
        return;
    while (next_line(data, len, &at, &line, &n)) {
        size_t i = 1U;

        /* HISTTIMEFORMAT's `#<seconds>` lines. */
        if (n > 1U && line[0] == '#') {
            while (i < n && line[i] >= '0' && line[i] <= '9')
                i++;
            if (i == n)
                continue;
        }
        if (n != 0U)
            entries_push(&e, hist_dup_n(line, n), n);
    }
    entries_offer(s, &e);
}

/* The whole file, or its last YEW_HIST_SHELL_READ_MAX bytes from a line
 * start.  Heap-owned; NULL when it is not a readable regular file. */
static char *shell_file_read(const char *path, size_t *len)
{
    struct stat st;
    Bytebuf b;
    char chunk[16384];
    char *out;
    int fd;
    size_t skip = 0U;

    *len = 0U;
    if (path == NULL)
        return NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return NULL;
    hist_shell_opens++;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return NULL;
    }
    if (st.st_size > (off_t)YEW_HIST_SHELL_READ_MAX) {
        if (lseek(fd, st.st_size - (off_t)YEW_HIST_SHELL_READ_MAX,
                  SEEK_SET) < 0) {
            (void)close(fd);
            return NULL;
        }
        skip = 1U; /* the first, partial line is not an entry */
    }
    bytebuf_init(&b);
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (b.len + (size_t)n > (size_t)YEW_HIST_SHELL_READ_MAX)
            n = (ssize_t)((size_t)YEW_HIST_SHELL_READ_MAX - b.len);
        bytebuf_append(&b, chunk, (size_t)n);
        if (b.len >= (size_t)YEW_HIST_SHELL_READ_MAX)
            break;
    }
    (void)close(fd);
    if (skip != 0U) {
        const u8 *nl = b.len == 0U ? NULL : memchr(b.data, '\n', b.len);

        skip = nl == NULL ? b.len : (size_t)(nl - b.data) + 1U;
    }
    out = hist_dup_n(b.len == 0U ? "" : (const char *)b.data + skip,
                     b.len - skip);
    *len = b.len - skip;
    bytebuf_free(&b);
    return out;
}

static char *env_path(const char *root, const char *suffix)
{
    size_t nr;
    size_t ns;
    char *out;

    if (root == NULL || root[0] != '/')
        return NULL;
    nr = strlen(root);
    ns = strlen(suffix);
    out = yew_xmalloc(nr + ns + 1U);
    (void)memcpy(out, root, nr);
    (void)memcpy(out + nr, suffix, ns + 1U);
    return out;
}

static void read_one(YewHistSuggest *s, char *path,
                     void (*parse)(YewHistSuggest *, const char *, size_t))
{
    size_t len;
    char *data = shell_file_read(path, &len);

    if (data != NULL)
        parse(s, data, len);
    yew_xfree(data);
    yew_xfree(path);
}

void yew_hist_suggest_read_shells(YewHistSuggest *s)
{
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *histfile = getenv("HISTFILE");
    const char *base = NULL;
    bool zsh_file = false;

    if (s == NULL)
        return;
    if (histfile != NULL && histfile[0] == '/') {
        base = strrchr(histfile, '/') + 1;
        zsh_file = strstr(base, "zsh") != NULL;
    } else {
        histfile = NULL;
    }
    read_one(s,
             data_home != NULL && data_home[0] == '/'
                 ? env_path(data_home, "/fish/fish_history")
                 : env_path(home, "/.local/share/fish/fish_history"),
             yew_hist_parse_fish);
    read_one(s,
             histfile != NULL && zsh_file ? env_path(histfile, "")
                                          : env_path(home, "/.zsh_history"),
             yew_hist_parse_zsh);
    read_one(s,
             histfile != NULL && !zsh_file ? env_path(histfile, "")
                                           : env_path(home, "/.bash_history"),
             yew_hist_parse_bash);
}
