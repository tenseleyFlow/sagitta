#define _POSIX_C_SOURCE 200809L

#include "fl/macrolib.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/bind.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "fl/flhook.h"
#include "fl/parse.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/origin.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/xdg.h"

enum { MACROLIB_MAX_BYTES = 8U * 1024U * 1024U };

typedef struct MacroFile {
    char *path;
    char *stem;
    char *source;
    size_t source_len;
    YewMacroHeader header;
    FlValue exports;
    bool rejected;
} MacroFile;

typedef struct MacroEntry {
    char *name;
    char *alias;
    char *binding;
    u32 file;
    FlValue callable;
    u8 arity;
    bool replayable;
} MacroEntry;

struct MacroLib {
    MacroFile *files;
    u32 nfiles;
    u32 file_cap;
    MacroEntry *entries;
    u32 nentries;
    u32 entry_cap;
    char *dir;
    FlValue root;
    u32 *ledger_ids;
    u32 nledger;
    u32 ledger_cap;
    bool enabled;
    bool scanning;
};

typedef struct NameVec {
    char **v;
    u32 n;
    u32 cap;
} NameVec;

static void registrations_remove(Ed *ed, MacroLib *lib);

static void header_field(YewMacroText *field, const char *s, size_t n)
{
    while (n != 0U && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n != 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' ||
                       s[n - 1U] == '\r'))
        n--;
    field->s = s;
    field->len = n > UINT32_MAX ? UINT32_MAX : (u32)n;
    field->present = true;
}

static bool header_key(const char *line, size_t n, const char *key,
                       const char **value, size_t *value_len)
{
    size_t kn = strlen(key);

    if (n < kn || memcmp(line, key, kn) != 0)
        return false;
    *value = line + kn;
    *value_len = n - kn;
    return true;
}

YewMacroHeaderStatus yew_macro_header_parse(const char *source, size_t len,
                                             YewMacroHeader *out)
{
    size_t at = 0U;

    if (out == NULL)
        return YEW_MACRO_HEADER_UNSUPPORTED;
    (void)memset(out, 0, sizeof(*out));
    if (source == NULL)
        return len == 0U ? YEW_MACRO_HEADER_OK :
                           YEW_MACRO_HEADER_UNSUPPORTED;
    while (at < len) {
        size_t end = at;
        const char *line;
        size_t n;
        const char *value;
        size_t value_len;

        while (end < len && source[end] != '\n')
            end++;
        line = source + at;
        n = end - at;
        while (n != 0U && (*line == ' ' || *line == '\t')) {
            line++;
            n--;
        }
        if (n == 0U) {
            at = end < len ? end + 1U : end;
            continue;
        }
        if (*line != '#')
            break;
        line++;
        n--;
        while (n != 0U && (*line == ' ' || *line == '\t')) {
            line++;
            n--;
        }
        if (header_key(line, n, "yew-macro:", &value, &value_len)) {
            YewMacroText schema = {0};

            header_field(&schema, value, value_len);
            out->has_schema = true;
            if (schema.len == 1U && schema.s[0] >= '0' &&
                schema.s[0] <= '9')
                out->schema = (u32)(schema.s[0] - '0');
            else
                out->schema = UINT32_MAX;
        } else if (header_key(line, n, "recorded-with:", &value,
                              &value_len)) {
            header_field(&out->recorded_with, value, value_len);
        } else if (header_key(line, n, "keymap:", &value, &value_len)) {
            header_field(&out->keymap, value, value_len);
        } else if (header_key(line, n, "recorded:", &value, &value_len)) {
            header_field(&out->recorded, value, value_len);
        } else if (header_key(line, n, "describe:", &value, &value_len)) {
            header_field(&out->describe, value, value_len);
        }
        at = end < len ? end + 1U : end;
    }
    return out->has_schema && out->schema != 1U ?
           YEW_MACRO_HEADER_UNSUPPORTED : YEW_MACRO_HEADER_OK;
}

static char *dup_n(const char *s, size_t n)
{
    char *copy = yew_xmalloc(n + 1U);

    if (n != 0U)
        (void)memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static char *path_join(const char *dir, const char *name)
{
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    bool slash = dn != 0U && dir[dn - 1U] != '/';
    char *path;

    if (dn > SIZE_MAX - nn - (slash ? 2U : 1U))
        return NULL;
    path = yew_xmalloc(dn + nn + (slash ? 2U : 1U));
    (void)memcpy(path, dir, dn);
    if (slash)
        path[dn++] = '/';
    (void)memcpy(path + dn, name, nn + 1U);
    return path;
}

static char *default_dir(void)
{
    char *cfg = yew_xdg_config_dir();
    char *dir;

    if (cfg == NULL)
        return dup_n("", 0U);
    dir = path_join(cfg, "macros");
    free(cfg);
    return dir == NULL ? dup_n("", 0U) : dir;
}

static char *effective_dir(Ed *ed)
{
    OptVal value;

    if (yew_opt_get(ed, NULL, NULL, "macro.dir", 9U, &value) &&
        value.type == (u8)YEW_OPT_STR && value.as.str.len != 0U)
        return dup_n(value.as.str.s, value.as.str.len);
    return default_dir();
}

static void file_free(MacroFile *file)
{
    free(file->path);
    free(file->stem);
    free(file->source);
    (void)memset(file, 0, sizeof(*file));
}

static void entry_free(MacroEntry *entry)
{
    free(entry->name);
    free(entry->alias);
    free(entry->binding);
    (void)memset(entry, 0, sizeof(*entry));
}

static void contents_free(MacroLib *lib)
{
    u32 i;

    for (i = 0U; i < lib->nentries; i++)
        entry_free(&lib->entries[i]);
    for (i = 0U; i < lib->nfiles; i++)
        file_free(&lib->files[i]);
    free(lib->entries);
    free(lib->files);
    free(lib->dir);
    free(lib->ledger_ids);
    lib->entries = NULL;
    lib->files = NULL;
    lib->dir = NULL;
    lib->ledger_ids = NULL;
    lib->nledger = 0U;
    lib->ledger_cap = 0U;
    lib->nentries = 0U;
    lib->entry_cap = 0U;
    lib->nfiles = 0U;
    lib->file_cap = 0U;
}

MacroLib *yew_macrolib_new(Ed *ed)
{
    MacroLib *lib;
    FlVm *vm = yew_fl_vm(ed);

    if (vm == NULL)
        return NULL;
    lib = yew_xcalloc(1U, sizeof(*lib));
    lib->root = FL_NIL_V;
    lib->enabled = true;
    fl_gc_host_root_add(vm, &lib->root);
    return lib;
}

void yew_macrolib_free(Ed *ed, MacroLib *lib)
{
    FlVm *vm;

    if (lib == NULL)
        return;
    if (ed != NULL)
        registrations_remove(ed, lib);
    vm = yew_fl_vm(ed);
    if (vm != NULL)
        fl_gc_host_root_remove(vm, &lib->root);
    contents_free(lib);
    free(lib);
}

static int name_cmp(const void *a, const void *b, void *ctx)
{
    const char *const *aa = a;
    const char *const *bb = b;

    (void)ctx;
    return strcmp(*aa, *bb);
}

static bool is_fl_name(const char *name)
{
    size_t n = strlen(name);

    return n > 3U && strcmp(name + n - 3U, ".fl") == 0;
}

static void names_free(NameVec *names)
{
    u32 i;

    for (i = 0U; i < names->n; i++)
        free(names->v[i]);
    free(names->v);
    (void)memset(names, 0, sizeof(*names));
}

static bool names_read(const char *dir, NameVec *names)
{
    DIR *dp;
    struct dirent *de;

    (void)memset(names, 0, sizeof(*names));
    dp = opendir(dir);
    if (dp == NULL)
        return errno == ENOENT;
    while ((de = readdir(dp)) != NULL) {
        if (!is_fl_name(de->d_name))
            continue;
        if (names->n == names->cap) {
            names->cap = names->cap == 0U ? 16U : names->cap * 2U;
            names->v = yew_xreallocarray(names->v, names->cap,
                                          sizeof(*names->v));
        }
        names->v[names->n++] = dup_n(de->d_name, strlen(de->d_name));
    }
    if (closedir(dp) != 0) {
        names_free(names);
        return false;
    }
    yew_sort_stable(names->v, names->n, sizeof(*names->v), name_cmp, NULL);
    return true;
}

static bool read_file(const char *path, char **out, size_t *out_len)
{
    Bytebuf bytes;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;
    bytebuf_init(&bytes);
    for (;;) {
        char block[16384];
        ssize_t got = read(fd, block, sizeof(block));

        if (got > 0) {
            bytebuf_append(&bytes, block, (size_t)got);
            if (bytes.len > (size_t)MACROLIB_MAX_BYTES) {
                (void)close(fd);
                bytebuf_free(&bytes);
                errno = EFBIG;
                return false;
            }
        } else if (got == 0) {
            break;
        } else if (errno != EINTR) {
            (void)close(fd);
            bytebuf_free(&bytes);
            return false;
        }
    }
    if (close(fd) != 0) {
        bytebuf_free(&bytes);
        return false;
    }
    *out = dup_n((const char *)bytes.data, bytes.len);
    *out_len = bytes.len;
    bytebuf_free(&bytes);
    return true;
}

static void report(DiagCtx *dc, u32 file_id, FlDiagLevel level,
                   const char *fmt, ...)
{
    va_list ap;

    if (dc != NULL) {
        va_start(ap, fmt);
        fl_diag_vemit(dc, level, (FlSpan){file_id, 1U, 1U, 1U}, fmt, ap);
        va_end(ap);
    } else {
        char text[512];

        va_start(ap, fmt);
        (void)vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        yew_log(level == FL_DIAG_ERROR ? YEW_LOG_ERROR : YEW_LOG_WARN,
                "%s", text);
    }
}

static MacroFile *file_push(MacroLib *lib)
{
    MacroFile *file;

    if (lib->nfiles == lib->file_cap) {
        lib->file_cap = lib->file_cap == 0U ? 16U : lib->file_cap * 2U;
        lib->files = yew_xreallocarray(lib->files, lib->file_cap,
                                       sizeof(*lib->files));
    }
    file = &lib->files[lib->nfiles++];
    (void)memset(file, 0, sizeof(*file));
    file->exports = FL_NIL_V;
    return file;
}

static MacroEntry *entry_push(MacroLib *lib)
{
    MacroEntry *entry;

    if (lib->nentries == lib->entry_cap) {
        lib->entry_cap = lib->entry_cap == 0U ? 32U : lib->entry_cap * 2U;
        lib->entries = yew_xreallocarray(lib->entries, lib->entry_cap,
                                         sizeof(*lib->entries));
    }
    entry = &lib->entries[lib->nentries++];
    (void)memset(entry, 0, sizeof(*entry));
    return entry;
}

static bool callable_info(FlValue value, u8 *arity)
{
    if (value.t == (u8)FL_CLOSURE) {
        *arity = ((FlClosure *)value.as.o)->fn->arity;
        return true;
    }
    if (value.t == (u8)FL_NATIVE) {
        FlNative *native = (FlNative *)value.as.o;

        if (native->min_ar != native->max_ar)
            return false;
        *arity = native->min_ar;
        return true;
    }
    return false;
}

static bool preflight_exports(MacroLib *candidate, u32 file_index)
{
    MacroFile *file = &candidate->files[file_index];
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlProgram program;
    u32 file_id;
    u32 i;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    file_id = fl_diag_add_file(&dc, file->path, file->source,
                               file->source_len);
    program = fl_parse(&arena, &dc, &in, file->source, file->source_len,
                       file_id);
    if (program.had_error || program.incomplete) {
        interner_free(&in);
        arena_free_all(&arena);
        return false;
    }
    for (i = 0U; i < program.n; i++) {
        const FlNode *node = program.stmts[i];
        u32 name_id;
        u32 arity;
        const char *binding;
        size_t bn;
        size_t sn;
        MacroEntry *entry;

        if (node->kind == (u8)FL_A_FN) {
            name_id = node->as.fn.name;
            arity = node->as.fn.nparams;
        } else if (node->kind == (u8)FL_A_MACRO) {
            name_id = node->as.macro.name;
            arity = 0U;
        } else {
            continue;
        }
        binding = yew_intern_str(&in, name_id);
        if (binding == NULL || binding[0] == '_')
            continue;
        bn = yew_intern_len(&in, name_id);
        sn = strlen(file->stem);
        entry = entry_push(candidate);
        entry->name = yew_xmalloc(sn + 1U + bn + 1U);
        (void)memcpy(entry->name, file->stem, sn);
        entry->name[sn] = '.';
        (void)memcpy(entry->name + sn + 1U, binding, bn);
        entry->name[sn + 1U + bn] = '\0';
        entry->binding = dup_n(binding, bn);
        if (bn == sn && memcmp(binding, file->stem, sn) == 0)
            entry->alias = dup_n(file->stem, sn);
        entry->file = file_index;
        entry->arity = arity > UINT8_MAX ? UINT8_MAX : (u8)arity;
        entry->replayable = arity == 0U;
    }
    interner_free(&in);
    arena_free_all(&arena);
    return true;
}

static bool bind_exports(MacroLib *candidate, u32 file_index, FlMap *exports)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    u32 matched = 0U;
    u32 expected = 0U;

    for (cursor = 0U; cursor < candidate->nentries; cursor++)
        if (candidate->entries[cursor].file == file_index)
            expected++;
    cursor = 0U;

    while (fl_map_iter(exports, &cursor, &key, &value)) {
        const FlStr *binding;
        u8 arity;
        u32 i;

        if (key.t != (u8)FL_STR || !callable_info(value, &arity))
            continue;
        binding = (const FlStr *)key.as.o;
        for (i = 0U; i < candidate->nentries; i++) {
            MacroEntry *entry = &candidate->entries[i];

            if (entry->file == file_index &&
                strlen(entry->binding) == (size_t)binding->len &&
                memcmp(entry->binding, binding->b, binding->len) == 0) {
                entry->callable = value;
                entry->arity = arity;
                entry->replayable = arity == 0U;
                matched++;
                break;
            }
        }
    }
    return matched == expected;
}

static bool recorded_major_mismatch(const YewMacroHeader *header)
{
    const char *s;
    u32 n;
    u32 major = 0U;
    bool digit = false;

    if (!header->recorded_with.present)
        return false;
    s = header->recorded_with.s;
    n = header->recorded_with.len;
    if (n >= 4U && memcmp(s, "yew ", 4U) == 0) {
        s += 4U;
        n -= 4U;
    }
    while (n != 0U && *s >= '0' && *s <= '9') {
        digit = true;
        major = major * 10U + (u32)(*s - '0');
        s++;
        n--;
    }
    return digit && major != 1U;
}

static bool same_public_name(const MacroEntry *a, const MacroEntry *b)
{
    if (strcmp(a->name, b->name) == 0)
        return true;
    if (a->alias != NULL && strcmp(a->alias, b->name) == 0)
        return true;
    if (b->alias != NULL && strcmp(a->name, b->alias) == 0)
        return true;
    return a->alias != NULL && b->alias != NULL &&
           strcmp(a->alias, b->alias) == 0;
}

static void reject_collisions(MacroLib *candidate, DiagCtx *dc, u32 diag_file)
{
    u32 i;
    u32 k;

    for (i = 0U; i < candidate->nentries; i++) {
        for (k = i + 1U; k < candidate->nentries; k++) {
            MacroEntry *a = &candidate->entries[i];
            MacroEntry *b = &candidate->entries[k];
            MacroFile *af;
            MacroFile *bf;

            if (a->file == b->file || !same_public_name(a, b))
                continue;
            af = &candidate->files[a->file];
            bf = &candidate->files[b->file];
            if (!af->rejected || !bf->rejected)
                report(dc, diag_file, FL_DIAG_ERROR,
                       "macro collision '%s': %s and %s; neither loaded",
                       strcmp(a->name, b->name) == 0 ? a->name :
                       (a->alias != NULL ? a->alias : b->alias),
                       af->path, bf->path);
            af->rejected = true;
            bf->rejected = true;
        }
    }
}

static void compact_entries(MacroLib *candidate)
{
    u32 read_at;
    u32 write_at = 0U;

    for (read_at = 0U; read_at < candidate->nentries; read_at++) {
        MacroEntry *entry = &candidate->entries[read_at];

        if (candidate->files[entry->file].rejected) {
            entry_free(entry);
            continue;
        }
        if (write_at != read_at) {
            candidate->entries[write_at] = *entry;
            (void)memset(entry, 0, sizeof(*entry));
        }
        write_at++;
    }
    candidate->nentries = write_at;
}

static void candidate_swap(MacroLib *live, MacroLib *candidate,
                           FlValue final_root)
{
    bool enabled = live->enabled;

    contents_free(live);
    live->files = candidate->files;
    live->nfiles = candidate->nfiles;
    live->file_cap = candidate->file_cap;
    live->entries = candidate->entries;
    live->nentries = candidate->nentries;
    live->entry_cap = candidate->entry_cap;
    live->dir = candidate->dir;
    live->ledger_ids = candidate->ledger_ids;
    live->nledger = candidate->nledger;
    live->ledger_cap = candidate->ledger_cap;
    live->root = final_root;
    live->enabled = enabled;
    candidate->files = NULL;
    candidate->entries = NULL;
    candidate->dir = NULL;
    candidate->ledger_ids = NULL;
    candidate->nledger = candidate->ledger_cap = 0U;
    candidate->nfiles = candidate->file_cap = 0U;
    candidate->nentries = candidate->entry_cap = 0U;
}

static void registration_remove(Ed *ed, u32 id)
{
    FlRegLedger *ledger = &ed->hooks.ledger;
    FlRegistration *reg;

    if (id == 0U || id > ledger->n)
        return;
    reg = &ledger->v[id - 1U];
    if (!reg->active)
        return;
    if (reg->kind == (u8)REG_HOOK)
        (void)fl_hook_remove(&ed->hooks, id);
    else if (reg->kind == (u8)REG_BIND)
        (void)yew_bind_remove(ed, id);
    else if (reg->kind == (u8)REG_OPTION)
        (void)yew_opt_remove(ed, id);
    else
        (void)fl_reg_remove(ledger, id);
}

static void registrations_remove(Ed *ed, MacroLib *lib)
{
    u32 i = lib->nledger;

    while (i != 0U)
        registration_remove(ed, lib->ledger_ids[--i]);
    lib->nledger = 0U;
}

static void registrations_capture(MacroLib *lib, const FlRegLedger *ledger,
                                  u32 before)
{
    u32 i;

    for (i = before; i < ledger->n; i++) {
        if (!ledger->v[i].active)
            continue;
        if (lib->nledger == lib->ledger_cap) {
            lib->ledger_cap = lib->ledger_cap == 0U ? 8U :
                                                    lib->ledger_cap * 2U;
            lib->ledger_ids = yew_xreallocarray(lib->ledger_ids,
                                                 lib->ledger_cap,
                                                 sizeof(*lib->ledger_ids));
        }
        lib->ledger_ids[lib->nledger++] = i + 1U;
    }
}

u32 yew_macrolib_scan(Ed *ed, DiagCtx *dc)
{
    MacroLib candidate = {0};
    MacroLib *live;
    FlVm *vm;
    NameVec names;
    FlList *all_exports;
    FlList *final_exports;
    u32 diag_file = 0U;
    u32 i;

    if (ed == NULL || (live = ed->macrolib) == NULL || live->scanning ||
        (vm = yew_fl_vm(ed)) == NULL)
        return 0U;
    live->scanning = true;
    candidate.dir = effective_dir(ed);
    if (dc != NULL)
        diag_file = fl_diag_add_file(dc, "<macro-library>", "", 0U);
    if (!names_read(candidate.dir, &names)) {
        report(dc, diag_file, FL_DIAG_ERROR,
               "cannot scan macro directory %s", candidate.dir);
        contents_free(&candidate);
        live->scanning = false;
        return live->nentries;
    }
    for (i = 0U; i < names.n; i++) {
        size_t nn = strlen(names.v[i]);
        size_t stem_len = nn - 3U;
        MacroFile *file;

        if (stem_len < 2U) {
            report(dc, diag_file, FL_DIAG_ERROR,
                   "macro file stem '%.*s' must contain at least 2 bytes",
                   (int)stem_len, names.v[i]);
            continue;
        }
        file = file_push(&candidate);
        file->stem = dup_n(names.v[i], stem_len);
        file->path = path_join(candidate.dir, names.v[i]);
        if (file->path == NULL ||
            !read_file(file->path, &file->source, &file->source_len)) {
            report(dc, diag_file, FL_DIAG_ERROR, "cannot read macro file %s",
                   file->path == NULL ? names.v[i] : file->path);
            file->rejected = true;
            continue;
        }
        if (yew_macro_header_parse(file->source, file->source_len,
                                   &file->header) ==
            YEW_MACRO_HEADER_UNSUPPORTED) {
            report(dc, diag_file, FL_DIAG_WARNING,
                   "%s: unsupported yew-macro schema %lu; skipped",
                   file->path, (unsigned long)file->header.schema);
            file->rejected = true;
            continue;
        }
        if (recorded_major_mismatch(&file->header))
            report(dc, diag_file, FL_DIAG_WARNING,
                   "%s: recorded-with %.*s has a different major version from yew %s; loading anyway",
                   file->path, (int)file->header.recorded_with.len,
                   file->header.recorded_with.s, YEW_VERSION);
        if (!preflight_exports(&candidate, candidate.nfiles - 1U)) {
            report(dc, diag_file, FL_DIAG_ERROR,
                   "%s: macro file did not parse; skipped", file->path);
            file->rejected = true;
        }
    }
    names_free(&names);
    reject_collisions(&candidate, dc, diag_file);
    compact_entries(&candidate);
    registrations_remove(ed, live);
    all_exports = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, all_exports));
    for (i = 0U; i < candidate.nfiles; i++) {
        MacroFile *file = &candidate.files[i];
        FlOrigin origin;
        FlValue exports;
        u32 ledger_before;

        if (file->rejected)
            continue;
        origin = (FlOrigin){(u8)FL_ORIGIN_CONFIG,
                  yew_intern(vm->in, file->path, strlen(file->path)),
                  FL_CAP_ALL};
        ledger_before = ed->hooks.ledger.n;
        if (!fl_module_eval_source(vm, file->path, file->source,
                                   file->source_len, origin, &exports)) {
            report(dc, diag_file, FL_DIAG_ERROR,
                   "%s: macro file did not load; skipped", file->path);
            file->rejected = true;
            while (ed->hooks.ledger.n > ledger_before) {
                registration_remove(ed, ed->hooks.ledger.n);
                if (ed->hooks.ledger.v[ed->hooks.ledger.n - 1U].active)
                    break;
                ed->hooks.ledger.n--;
            }
            vm->err = FL_NIL_V;
            continue;
        }
        registrations_capture(&candidate, &ed->hooks.ledger, ledger_before);
        file->exports = exports;
        (void)fl_list_push(vm, all_exports, exports);
        if (!bind_exports(&candidate, i, (FlMap *)exports.as.o)) {
            report(dc, diag_file, FL_DIAG_ERROR,
                   "%s: exported functions changed during load; skipped",
                   file->path);
            file->rejected = true;
        }
    }
    compact_entries(&candidate);
    final_exports = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, final_exports));
    for (i = 0U; i < candidate.nfiles; i++)
        if (!candidate.files[i].rejected &&
            candidate.files[i].exports.t == (u8)FL_MAP)
            (void)fl_list_push(vm, final_exports, candidate.files[i].exports);
    candidate_swap(live, &candidate, FL_OBJ_V(FL_LIST, final_exports));
    fl_gc_release(vm, 2U);
    contents_free(&candidate);
    fl_gc_collect(vm);
    live->scanning = false;
    return live->nentries;
}

u32 yew_macrolib_count(const Ed *ed)
{
    return ed == NULL || ed->macrolib == NULL ? 0U :
                                                ed->macrolib->nentries;
}

static void entry_view(const MacroLib *lib, const MacroEntry *entry,
                       YewMacroEntryView *out)
{
    const MacroFile *file = &lib->files[entry->file];

    *out = (YewMacroEntryView){entry->name, entry->alias, entry->binding,
           file->stem, file->path, file->source, file->source_len,
           file->header, 0U, entry->arity, entry->replayable};
}

bool yew_macrolib_at(const Ed *ed, u32 index, YewMacroEntryView *out)
{
    if (ed == NULL || ed->macrolib == NULL || out == NULL ||
        index >= ed->macrolib->nentries)
        return false;
    entry_view(ed->macrolib, &ed->macrolib->entries[index], out);
    return true;
}

static MacroEntry *find_entry(MacroLib *lib, const char *name)
{
    u32 i;

    if (lib == NULL || name == NULL)
        return NULL;
    for (i = 0U; i < lib->nentries; i++) {
        MacroEntry *entry = &lib->entries[i];

        if (strcmp(entry->name, name) == 0 ||
            (entry->alias != NULL && strcmp(entry->alias, name) == 0))
            return entry;
    }
    return NULL;
}

bool yew_macrolib_find(const Ed *ed, const char *name,
                       YewMacroEntryView *out)
{
    MacroEntry *entry;

    if (ed == NULL || out == NULL ||
        (entry = find_entry(ed->macrolib, name)) == NULL)
        return false;
    entry_view(ed->macrolib, entry, out);
    return true;
}

CmdStatus yew_macrolib_call(Ed *ed, const char *name)
{
    MacroEntry *entry;
    FlVm *vm;
    bool split_run;
    bool ok;

    if (ed == NULL || (entry = find_entry(ed->macrolib, name)) == NULL) {
        if (ed != NULL)
            yew_msg(ed, YEW_MSG_ERROR, "no macro named %s",
                    name == NULL ? "" : name);
        return YEW_CMD_ERR_ARG;
    }
    if (!entry->replayable) {
        yew_msg(ed, YEW_MSG_ERROR, "%s takes %u argument%s", entry->name,
                (unsigned)entry->arity, entry->arity == 1U ? "" : "s");
        return YEW_CMD_ERR_ARG;
    }
    vm = yew_fl_vm(ed);
    if (vm == NULL)
        return YEW_CMD_ERR_STATE;
    split_run = vm->txn.entry_active &&
                fl_runtime_cmd_source(vm) != YEW_SRC_REPLAY;
    if (split_run) {
        if (vm->txn.depth != 0U) {
            yew_msg(ed, YEW_MSG_ERROR,
                    "cannot replay a macro inside an edit transaction");
            return YEW_CMD_ERR_STATE;
        }
        if (!vm->host->edit_begin(vm))
            return YEW_CMD_ERR_STATE;
    }
    ok = fl_call_value(ed->fl, entry->callable, YEW_SRC_REPLAY);
    if (split_run && !vm->host->edit_end(vm, ok))
        ok = false;
    return ok ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

const char *yew_macrolib_dir(const Ed *ed)
{
    return ed == NULL || ed->macrolib == NULL ? NULL : ed->macrolib->dir;
}

void yew_macrolib_enable(Ed *ed)
{
    if (ed == NULL || ed->macrolib == NULL)
        return;
    ed->macrolib->enabled = true;
    (void)yew_macrolib_scan(ed, NULL);
}

void yew_macrolib_option_changed(Ed *ed)
{
    if (ed != NULL && ed->macrolib != NULL && ed->macrolib->enabled)
        (void)yew_macrolib_scan(ed, NULL);
}
