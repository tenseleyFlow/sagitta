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
    char *copy = sag_xmalloc(len + 1U);

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
    h->entries = sag_xreallocarray(h->entries, cap, sizeof(*h->entries));
    h->cap = cap;
}

static void hist_remove(CmdHist *h, size_t index)
{
    free(h->entries[index]);
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
    if (len > SAG_HIST_LINE_MAX)
        len = SAG_HIST_LINE_MAX;
    copy = hist_dup_n(line, len);
    for (i = 0U; i < h->len; i++) {
        if (strcmp(h->entries[i], copy) == 0) {
            if (i + 1U == h->len) {
                free(copy);
                return false;
            }
            hist_remove(h, i);
            break;
        }
    }
    hist_reserve(h, h->len + 1U);
    h->entries[h->len++] = copy;
    if (h->len > SAG_HIST_MAX)
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
    state = sag_xdg_state_dir();
    if (state == NULL)
        return NULL;
    size = strlen(state) + strlen("/history") + 1U;
    dir = sag_xmalloc(size);
    (void)snprintf(dir, size, "%s/history", state);
    free(state);
    if (!sag_mkdirs(dir, 0700U)) {
        free(dir);
        return NULL;
    }
    size = strlen(dir) + 1U + strlen(kind) + 1U;
    path = sag_xmalloc(size);
    (void)snprintf(path, size, "%s/%s", dir, kind);
    free(dir);
    return path;
}

static void hist_escape(Bytebuf *out, const char *line)
{
    static const char hex[] = "0123456789ABCDEF";
    const u8 *p = (const u8 *)line;
    size_t i;
    size_t len = strlen(line);

    if (len > SAG_HIST_LINE_MAX)
        len = SAG_HIST_LINE_MAX;
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
        if (out.len > SAG_HIST_LINE_MAX)
            out.len = SAG_HIST_LINE_MAX;
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
        sag_log(SAG_LOG_WARN, "dropping corrupt history entry in %s",
                h->path == NULL ? "memory history" : h->path);
        return;
    }
    (void)hist_store(h, decoded, strlen(decoded));
    free(decoded);
}

static bool hist_read(CmdHist *h)
{
    enum { ENCODED_MAX = SAG_HIST_LINE_MAX * 4 };
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
            sag_log(SAG_LOG_WARN, "cannot read command history %s: %s",
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
            sag_log(SAG_LOG_WARN, "cannot read command history %s: %s",
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
                    sag_log(SAG_LOG_WARN,
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
        sag_log(SAG_LOG_WARN, "dropping incomplete history entry in %s",
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
    char *lock_path = sag_xmalloc(size);
    int fd;

    (void)snprintf(lock_path, size, "%s.lock", path);
    fd = open(lock_path, O_RDWR | O_CREAT
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif
              , 0600);
    free(lock_path);
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

CmdHist *sag_hist_open(const char *kind)
{
    CmdHist *h = sag_xcalloc(1U, sizeof(*h));

    h->path = hist_path(kind);
    h->memory = h->path == NULL;
    if (h->memory) {
        sag_log(SAG_LOG_WARN,
                "command history unavailable; using in-memory history");
    } else {
        (void)hist_read(h);
    }
    return h;
}

CmdHist *sag_hist_open_memory(void)
{
    CmdHist *h = sag_xcalloc(1U, sizeof(*h));

    h->memory = true;
    return h;
}

void sag_hist_close(CmdHist *h)
{
    if (h == NULL)
        return;
    hist_clear(h);
    free(h->entries);
    free(h->path);
    free(h);
}

void sag_hist_add(CmdHist *h, const char *line)
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
        sag_log(SAG_LOG_WARN, "cannot append command history %s: %s",
                h->path, strerror(errno));
    } else {
        do {
            wrote = write(fd, encoded.data, encoded.len);
        } while (wrote < 0 && errno == EINTR);
        if (wrote < 0 || (size_t)wrote != encoded.len) {
            sag_log(SAG_LOG_WARN, "short append to command history %s",
                    h->path);
        }
        (void)close(fd);
    }
    if (lock_fd >= 0)
        (void)close(lock_fd);
    bytebuf_free(&encoded);
}

void sag_hist_flush(CmdHist *h)
{
    CmdHist merged = {0};
    Bytebuf encoded;
    int lock_fd;
    size_t i;

    if (h == NULL || h->memory)
        return;
    lock_fd = hist_lock(h->path);
    if (lock_fd < 0) {
        sag_log(SAG_LOG_WARN, "cannot lock command history %s: %s", h->path,
                strerror(errno));
        return;
    }
    merged.path = h->path;
    if (!hist_read(&merged)) {
        hist_clear(&merged);
        free(merged.entries);
        (void)close(lock_fd);
        return;
    }
    bytebuf_init(&encoded);
    for (i = 0U; i < merged.len; i++) {
        hist_escape(&encoded, merged.entries[i]);
        bytebuf_push_u8(&encoded, (u8)'\n');
    }
    if (sag_file_write_atomic(h->path, encoded.data, encoded.len, 0600) !=
        SAG_SAVE_OK) {
        sag_log(SAG_LOG_WARN, "cannot compact command history %s: %s",
                h->path, strerror(errno));
    } else {
        hist_clear(h);
        free(h->entries);
        h->entries = merged.entries;
        h->len = merged.len;
        h->cap = merged.cap;
        merged.entries = NULL;
        merged.len = 0U;
        merged.cap = 0U;
    }
    bytebuf_free(&encoded);
    hist_clear(&merged);
    free(merged.entries);
    (void)close(lock_fd);
}

size_t sag_hist_len(const CmdHist *h)
{
    return h == NULL ? 0U : h->len;
}

const char *sag_hist_at(const CmdHist *h, size_t index)
{
    return h == NULL || index >= h->len ? NULL : h->entries[index];
}

bool sag_hist_is_memory(const CmdHist *h)
{
    return h == NULL || h->memory;
}

void sag_hist_cur_reset(HistCur *c, const char *draft)
{
    if (c == NULL)
        return;
    free(c->stem);
    free(c->draft);
    c->idx = -1;
    c->stem = NULL;
    c->draft = hist_dup(draft == NULL ? "" : draft);
}

void sag_hist_cur_dispose(HistCur *c)
{
    if (c == NULL)
        return;
    free(c->stem);
    free(c->draft);
    *c = (HistCur){.idx = -1};
}

static bool hist_has_prefix(const char *entry, const char *stem)
{
    return strncmp(entry, stem, strlen(stem)) == 0;
}

const char *sag_hist_prev(CmdHist *h, HistCur *c)
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

const char *sag_hist_next(CmdHist *h, HistCur *c)
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
