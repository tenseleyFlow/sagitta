#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "mod/plug/manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/data.h"
#include "fl/flhook.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/intern.h"

static const char *const CAP_NAMES[YEW_CAP__N] = {
    "fs", "shell", "net", "clipboard"
};

static const char *const MANIFEST_KEYS[] = {
    "name", "version", "api", "entry", "capabilities", "events",
    "description", "dependencies"
};

typedef struct ManifestRead {
    Arena *arena;
    DiagCtx *dc;
    FlSpan span;
    const char *root;
    const char *basename;
    PlugManifest mf;
    bool seen_name;
    bool seen_version;
    bool seen_api;
    bool seen_entry;
    bool seen_caps;
    bool seen_events;
    bool failed;
} ManifestRead;

const char *yew_cap_name(YewCap cap)
{
    return (u32)cap < (u32)YEW_CAP__N ? CAP_NAMES[cap] : NULL;
}

bool yew_cap_parse(const char *name, size_t len, YewCap *out)
{
    u32 i;

    if (name == NULL)
        return false;
    for (i = 0U; i < (u32)YEW_CAP__N; i++) {
        size_t n = strlen(CAP_NAMES[i]);

        if (len == n && memcmp(name, CAP_NAMES[i], n) == 0) {
            if (out != NULL)
                *out = (YewCap)i;
            return true;
        }
    }
    return false;
}

bool yew_plug_event_valid(const char *name, size_t len)
{
    u32 event;

    if (name == NULL)
        return false;
    if ((len == sizeof("plug.enable") - 1U &&
         memcmp(name, "plug.enable", len) == 0) ||
        (len == sizeof("plug.disable") - 1U &&
         memcmp(name, "plug.disable", len) == 0))
        return true;
    if (!fl_event_parse(name, (u32)len, &event))
        return false;
    /* cursor.move remains a runtime hook for compatibility, but it is not
     * part of Sprint 54's frozen plugin manifest contract. */
    return event != (u32)FL_EV_CURSOR_MOVE;
}

static void manifest_error(ManifestRead *r, const char *fmt,
                           const char *arg)
{
    if (arg == NULL)
        fl_diag_emit(r->dc, FL_DIAG_ERROR, r->span, "%s", fmt);
    else
        fl_diag_emit(r->dc, FL_DIAG_ERROR, r->span, fmt, arg);
    r->failed = true;
}

static bool value_str(FlValue v, const char **text, size_t *len)
{
    const FlStr *s;

    if (v.t != (u8)FL_STR)
        return false;
    s = (const FlStr *)v.as.o;
    if (memchr(s->b, '\0', s->len) != NULL)
        return false;
    if (text != NULL)
        *text = s->b;
    if (len != NULL)
        *len = s->len;
    return true;
}

static const char *copy_str(Arena *a, const char *text, size_t len)
{
    return arena_strndup(a, text, len);
}

static bool key_eq(const char *key, size_t len, const char *want)
{
    size_t n = strlen(want);

    return len == n && memcmp(key, want, n) == 0;
}

static bool one_edit_away(const char *got, size_t gn,
                          const char *want, size_t wn)
{
    size_t i = 0U;
    size_t j = 0U;
    u32 edits = 0U;

    if (gn > wn + 1U || wn > gn + 1U)
        return false;
    while (i < gn && j < wn) {
        if (got[i] == want[j]) {
            i++;
            j++;
            continue;
        }
        if (++edits > 1U)
            return false;
        if (gn > wn)
            i++;
        else if (wn > gn)
            j++;
        else {
            i++;
            j++;
        }
    }
    if (i < gn || j < wn)
        edits++;
    return edits == 1U;
}

static const char *suggest_key(const char *key, size_t len)
{
    size_t i;

    for (i = 0U; i + 1U < YEW_ARRAY_LEN(MANIFEST_KEYS); i++)
        if (one_edit_away(key, len, MANIFEST_KEYS[i],
                          strlen(MANIFEST_KEYS[i])))
            return MANIFEST_KEYS[i];
    return NULL;
}

static bool valid_name(const char *name, size_t len)
{
    size_t i;

    if (len == 0U || len > 32U)
        return false;
    for (i = 0U; i < len; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
            return false;
    return true;
}

static bool semver_num(const char **at, bool more)
{
    const char *p = *at;

    if (*p < '0' || *p > '9')
        return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9')
        return false;
    while (*p >= '0' && *p <= '9')
        p++;
    if (more) {
        if (*p != '.')
            return false;
        p++;
    }
    *at = p;
    return true;
}

static bool semver_identifiers(const char **at, bool numeric_zero_rule)
{
    const char *p = *at;

    for (;;) {
        const char *start = p;
        bool numeric = true;

        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'A' && *p <= 'Z') ||
               (*p >= 'a' && *p <= 'z') || *p == '-') {
            if (*p < '0' || *p > '9')
                numeric = false;
            p++;
        }
        if (p == start || (numeric_zero_rule && numeric && *start == '0' &&
                           p - start > 1))
            return false;
        if (*p != '.')
            break;
        p++;
    }
    *at = p;
    return true;
}

static bool valid_semver(const char *version)
{
    const char *p = version;

    if (!semver_num(&p, true) || !semver_num(&p, true) ||
        !semver_num(&p, false))
        return false;
    if (*p == '-') {
        p++;
        if (!semver_identifiers(&p, true))
            return false;
    }
    if (*p == '+') {
        p++;
        if (!semver_identifiers(&p, false))
            return false;
    }
    return *p == '\0';
}

static bool path_join(char *out, size_t outsz, const char *a, const char *b)
{
    int n = snprintf(out, outsz, "%s/%s", a, b);

    return n > 0 && (size_t)n < outsz;
}

static bool path_inside(const char *root, const char *path)
{
    size_t n = strlen(root);

    if (strcmp(root, "/") == 0)
        return path[0] == '/';
    return strncmp(root, path, n) == 0 && path[n] == '/';
}

static bool validate_entry(ManifestRead *r, const char *entry, size_t len)
{
    char joined[PATH_MAX];
    char *resolved;

    if (len == 0U || entry[0] == '/') {
        manifest_error(r, "plugin entry escapes directory: %s", entry);
        return false;
    }
    if (!path_join(joined, sizeof(joined), r->root, entry)) {
        manifest_error(r, "plugin entry path is too long: %s", entry);
        return false;
    }
    resolved = realpath(joined, NULL);
    if (resolved == NULL) {
        manifest_error(r, "cannot resolve plugin entry: %s", joined);
        return false;
    }
    if (!path_inside(r->root, resolved)) {
        manifest_error(r, "plugin entry escapes directory: %s", resolved);
        free(resolved);
        return false;
    }
    free(resolved);
    return true;
}

static bool validate_caps(ManifestRead *r, FlValue value)
{
    const FlList *list;
    u32 i;

    if (value.t != (u8)FL_LIST) {
        manifest_error(r, "manifest capabilities must be a list", NULL);
        return false;
    }
    list = (const FlList *)value.as.o;
    for (i = 0U; i < list->n; i++) {
        const char *name;
        size_t len;
        YewCap cap;

        if (!value_str(list->v[i], &name, &len)) {
            manifest_error(r, "manifest capability must be a string", NULL);
            continue;
        }
        if (!yew_cap_parse(name, len, &cap)) {
            char *copy = arena_strndup(r->arena, name, len);

            manifest_error(r, "unknown plugin capability: %s", copy);
            continue;
        }
        r->mf.caps_wanted |= 1U << (u32)cap;
    }
    return !r->failed;
}

static bool validate_events(ManifestRead *r, FlValue value)
{
    const FlList *list;
    u32 i;

    if (value.t != (u8)FL_LIST) {
        manifest_error(r, "manifest events must be a list", NULL);
        return false;
    }
    list = (const FlList *)value.as.o;
    r->mf.nevents = list->n;
    if (list->n != 0U) {
        r->mf.events = arena_alloc(r->arena,
                                   (size_t)list->n * sizeof(*r->mf.events),
                                   _Alignof(u32));
        r->mf.event_names = arena_alloc(
            r->arena, (size_t)list->n * sizeof(*r->mf.event_names),
            _Alignof(const char *));
        (void)memset(r->mf.events, 0,
                     (size_t)list->n * sizeof(*r->mf.events));
    }
    for (i = 0U; i < list->n; i++) {
        const char *name;
        size_t len;

        if (!value_str(list->v[i], &name, &len)) {
            manifest_error(r, "manifest event must be a string", NULL);
            continue;
        }
        r->mf.event_names[i] = copy_str(r->arena, name, len);
        if (!yew_plug_event_valid(name, len))
            manifest_error(r, "unknown plugin event: %s",
                           r->mf.event_names[i]);
    }
    return !r->failed;
}

static void validate_field(ManifestRead *r, const char *key, size_t key_len,
                           FlValue value)
{
    const char *text;
    size_t len;

    if (key_eq(key, key_len, "dependencies")) {
        manifest_error(r, "plugins have no dependencies at 1.0", NULL);
    } else if (key_eq(key, key_len, "name")) {
        r->seen_name = true;
        if (!value_str(value, &text, &len) || !valid_name(text, len)) {
            manifest_error(r, "manifest name must match [a-z0-9-]{1,32}",
                           NULL);
        } else {
            r->mf.name_text = copy_str(r->arena, text, len);
        }
    } else if (key_eq(key, key_len, "version")) {
        r->seen_version = true;
        if (!value_str(value, &text, &len)) {
            manifest_error(r, "manifest version must be a semver string",
                           NULL);
        } else {
            r->mf.version = copy_str(r->arena, text, len);
            if (!valid_semver(r->mf.version))
                manifest_error(r, "manifest version must be semver: %s",
                               r->mf.version);
        }
    } else if (key_eq(key, key_len, "api")) {
        r->seen_api = true;
        if (value.t != (u8)FL_INT || value.as.i < 0 ||
            (u64)value.as.i > (u64)UINT32_MAX) {
            manifest_error(r, "manifest api must be a nonnegative integer",
                           NULL);
        } else {
            r->mf.api = (u32)value.as.i;
            if (r->mf.api > YEW_PLUG_API_MAJOR) {
                fl_diag_emit(r->dc, FL_DIAG_ERROR, r->span,
                             "requires plugin API %u; this yew speaks 1",
                             r->mf.api);
                r->failed = true;
            }
        }
    } else if (key_eq(key, key_len, "entry")) {
        r->seen_entry = true;
        if (!value_str(value, &text, &len)) {
            manifest_error(r, "manifest entry must be a relative path",
                           NULL);
        } else {
            r->mf.entry = copy_str(r->arena, text, len);
            (void)validate_entry(r, r->mf.entry, len);
        }
    } else if (key_eq(key, key_len, "capabilities")) {
        r->seen_caps = true;
        (void)validate_caps(r, value);
    } else if (key_eq(key, key_len, "events")) {
        r->seen_events = true;
        (void)validate_events(r, value);
    } else if (key_eq(key, key_len, "description")) {
        if (!value_str(value, &text, &len) || memchr(text, '\n', len) != NULL ||
            memchr(text, '\r', len) != NULL) {
            manifest_error(r, "manifest description must be one line", NULL);
        } else {
            r->mf.desc = copy_str(r->arena, text, len);
        }
    } else {
        const char *suggest = suggest_key(key, key_len);
        char *copy = arena_strndup(r->arena, key, key_len);

        if (suggest != NULL) {
            fl_diag_emit(r->dc, FL_DIAG_ERROR, r->span,
                         "unknown manifest key '%s'; did you mean '%s'?",
                         copy, suggest);
            r->failed = true;
        } else {
            manifest_error(r, "unknown manifest key: %s", copy);
        }
    }
}

static bool validate_map(ManifestRead *r, FlValue root)
{
    const FlMap *map;
    u32 cursor = 0U;
    FlValue key;
    FlValue value;

    if (root.t != (u8)FL_MAP) {
        manifest_error(r, "plugin.fl must contain one map literal", NULL);
        return false;
    }
    map = (const FlMap *)root.as.o;
    while (fl_map_iter(map, &cursor, &key, &value)) {
        const char *text;
        size_t len;

        if (!value_str(key, &text, &len)) {
            manifest_error(r, "manifest key must be a string", NULL);
            continue;
        }
        validate_field(r, text, len, value);
    }
    if (!r->seen_name || !r->seen_version || !r->seen_api ||
        !r->seen_entry || !r->seen_caps || !r->seen_events)
        manifest_error(r, "manifest is missing a required key", NULL);
    if (r->mf.name_text != NULL &&
        strcmp(r->mf.name_text, r->basename) != 0) {
        fl_diag_emit(r->dc, FL_DIAG_ERROR, r->span,
                     "plugin name '%s' does not match directory '%s'",
                     r->mf.name_text, r->basename);
        r->failed = true;
    }
    return !r->failed;
}

static bool read_source(Arena *a, const char *path, char **out, size_t *len,
                        DiagCtx *dc)
{
    struct stat st;
    char *bytes;
    size_t off = 0U;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "cannot read %s: %s", path, strerror(errno));
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (u64)st.st_size > (u64)FL_DATA_MAX_BYTES) {
        (void)close(fd);
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "invalid plugin manifest file: %s", path);
        return false;
    }
    bytes = arena_alloc(a, (size_t)st.st_size + 1U, _Alignof(char));
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + off, (size_t)st.st_size - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            (void)close(fd);
            fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                         "cannot read plugin manifest: %s", path);
            return false;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "cannot close plugin manifest: %s", path);
        return false;
    }
    bytes[off] = '\0';
    *out = bytes;
    *len = off;
    return true;
}

bool yew_plug_manifest_read(Arena *a, const char *dir,
                            PlugManifest *out, DiagCtx *dc)
{
    ManifestRead read = {0};
    Interner in;
    FlVm vm;
    FlValue root;
    char manifest_path[PATH_MAX];
    char *source;
    char *canonical;
    const char *slash;
    size_t len;
    u32 errors_before;

    if (out != NULL)
        (void)memset(out, 0, sizeof(*out));
    if (a == NULL || dir == NULL || out == NULL || dc == NULL)
        return false;
    canonical = realpath(dir, NULL);
    if (canonical == NULL) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "cannot resolve plugin directory %s: %s", dir,
                     strerror(errno));
        return false;
    }
    if (!path_join(manifest_path, sizeof(manifest_path), canonical,
                   "plugin.fl") ||
        /* DiagCtx keeps a borrowed source pointer for later caret rendering.
         * Parsed manifest values live in `a`, but the registered source must
         * live with the diagnostic context even when callers release their
         * short-lived manifest arena before emitting a later error. */
        !read_source(dc->arena, manifest_path, &source, &len, dc)) {
        free(canonical);
        return false;
    }
    read.arena = a;
    read.dc = dc;
    read.root = arena_strdup(a, canonical);
    free(canonical);
    slash = strrchr(read.root, '/');
    read.basename = slash == NULL ? read.root : slash + 1U;
    read.span.file_id = fl_diag_add_file(dc, manifest_path, source, len);
    read.span.line = 1U;
    read.span.col = 1U;
    read.span.len = 1U;
    errors_before = fl_diag_errors(dc);
    interner_init(&in, a);
    (void)fl_vm_init(&vm, a, &in, dc);
    root = fl_data_read(&vm, source, len, dc);
    if (fl_diag_errors(dc) == errors_before)
        (void)validate_map(&read, root);
    fl_vm_free(&vm);
    interner_free(&in);
    if (read.failed || fl_diag_errors(dc) != errors_before)
        return false;
    read.mf.dir = read.root;
    *out = read.mf;
    return true;
}
