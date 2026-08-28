#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/buf.h"
#include "util/runtime_asset.h"

#if YEW_EMBED_RUNTIME
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} PathList;

static char *path_join(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    char *joined = yew_xmalloc(left_len + right_len + 2U);

    (void)memcpy(joined, left, left_len);
    joined[left_len] = '/';
    (void)memcpy(joined + left_len + 1U, right, right_len + 1U);
    return joined;
}

static void path_list_add(PathList *paths, const char *path)
{
    if (paths->len == paths->cap) {
        size_t cap = paths->cap == 0U ? 32U : paths->cap * 2U;

        paths->items = yew_xreallocarray(paths->items, cap,
                                         sizeof(*paths->items));
        paths->cap = cap;
    }
    paths->items[paths->len++] = yew_xstrdup(path);
}

static const char *runtime_root(void)
{
    const char *root = getenv("YEW_RUNTIME_DIR");

    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT(root[0] != '\0');
    return root;
}

static void collect_runtime_files(PathList *paths, const char *root,
                                  const char *relative)
{
    char *directory = relative[0] == '\0'
                          ? yew_xstrdup(root)
                          : path_join(root, relative);
    DIR *dir = opendir(directory);
    struct dirent *entry;

    YEW_ASSERT_NOT_NULL(dir);
    while ((entry = readdir(dir)) != NULL) {
        char *child_relative;
        char *disk_path;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        child_relative = relative[0] == '\0'
                             ? yew_xstrdup(entry->d_name)
                             : path_join(relative, entry->d_name);
        disk_path = path_join(root, child_relative);
        YEW_ASSERT_EQ_I64(lstat(disk_path, &st), 0);
        if (S_ISDIR(st.st_mode))
            collect_runtime_files(paths, root, child_relative);
        else if (S_ISREG(st.st_mode))
            path_list_add(paths, child_relative);
        yew_xfree(disk_path);
        yew_xfree(child_relative);
    }
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    yew_xfree(directory);
}

static void path_list_sort(PathList *paths)
{
    size_t i;

    for (i = 1U; i < paths->len; i++) {
        char *item = paths->items[i];
        size_t j = i;

        while (j > 0U && strcmp(paths->items[j - 1U], item) > 0) {
            paths->items[j] = paths->items[j - 1U];
            j--;
        }
        paths->items[j] = item;
    }
}

static void path_list_free(PathList *paths)
{
    size_t i;

    for (i = 0U; i < paths->len; i++)
        yew_xfree(paths->items[i]);
    yew_xfree(paths->items);
}

static void read_file(const char *path, Bytebuf *out)
{
    FILE *fp = fopen(path, "rb");
    u8 chunk[4096];
    size_t n;

    YEW_ASSERT_NOT_NULL(fp);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) != 0U)
        bytebuf_append(out, chunk, n);
    YEW_ASSERT_EQ_I64(ferror(fp), 0);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}
#endif

void test_runtime_asset_disabled_is_empty(void)
{
#if YEW_EMBED_RUNTIME
    YEW_ASSERT(yew_runtime_asset_count() > 0U);
#else
    Bytebuf out;
    static const u8 sentinel[] = {0xa5U, 0x00U, 0x5aU};

    bytebuf_init(&out);
    bytebuf_append(&out, sentinel, sizeof(sentinel));
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), 0U);
    YEW_ASSERT_NULL(yew_runtime_asset_name(0U));
    YEW_ASSERT(!yew_runtime_asset_has("init.fl"));
    YEW_ASSERT(!yew_runtime_asset_has(NULL));
    YEW_ASSERT(!yew_runtime_asset_read("init.fl", &out));
    YEW_ASSERT_EQ_U64(out.len, sizeof(sentinel));
    YEW_ASSERT_EQ_MEM(out.data, sentinel, sizeof(sentinel));
    YEW_ASSERT_NULL(yew_runtime_asset_resolve("init.fl"));
    bytebuf_free(&out);
#endif
}

void test_runtime_asset_inventory_roundtrips(void)
{
#if YEW_EMBED_RUNTIME
    PathList paths = {0};
    const char *root = runtime_root();
    size_t i;

    collect_runtime_files(&paths, root, "");
    path_list_sort(&paths);
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), paths.len);
    YEW_ASSERT_NULL(yew_runtime_asset_name(paths.len));
    for (i = 0U; i < paths.len; i++) {
        Bytebuf disk;
        Bytebuf embedded;
        char *disk_path;

        YEW_ASSERT_EQ_STR(yew_runtime_asset_name(i), paths.items[i]);
        if (i != 0U)
            YEW_ASSERT(strcmp(yew_runtime_asset_name(i - 1U),
                              yew_runtime_asset_name(i)) < 0);
        YEW_ASSERT(yew_runtime_asset_has(paths.items[i]));
        bytebuf_init(&disk);
        bytebuf_init(&embedded);
        disk_path = path_join(root, paths.items[i]);
        read_file(disk_path, &disk);
        YEW_ASSERT(yew_runtime_asset_read(paths.items[i], &embedded));
        YEW_ASSERT_EQ_U64(embedded.len, disk.len);
        YEW_ASSERT_EQ_MEM(embedded.data, disk.data, disk.len);
        yew_xfree(disk_path);
        bytebuf_free(&embedded);
        bytebuf_free(&disk);
    }
    path_list_free(&paths);
#else
    YEW_ASSERT_EQ_U64(yew_runtime_asset_count(), 0U);
#endif
}

void test_runtime_asset_paths_are_canonical_and_confined(void)
{
#if YEW_EMBED_RUNTIME
    const char *name = yew_runtime_asset_name(0U);
    const char *invalid[] = {
        NULL, "", "/init.fl", "../init.fl", "a/../../init.fl",
        "runtime/../init.fl", "runtime/", "runtime////"
    };
    Bytebuf out;
    char *alias;
    char *resolved;
    size_t i;
    size_t name_len;
    static const u8 sentinel[] = {0xdeU, 0xadU, 0xbeU, 0xefU};

    YEW_ASSERT_NOT_NULL(name);
    name_len = strlen(name);
    alias = yew_xmalloc(name_len + sizeof("runtime//./"));
    (void)memcpy(alias, "runtime//./", sizeof("runtime//./") - 1U);
    (void)memcpy(alias + sizeof("runtime//./") - 1U, name, name_len + 1U);
    YEW_ASSERT(yew_runtime_asset_has(alias));
    resolved = yew_runtime_asset_resolve(alias);
    YEW_ASSERT_NOT_NULL(resolved);
    YEW_ASSERT_EQ_U64(strlen(resolved), name_len + 8U);
    YEW_ASSERT_EQ_MEM(resolved, "runtime/", 8U);
    YEW_ASSERT_EQ_STR(resolved + 8U, name);
    yew_xfree(resolved);
    yew_xfree(alias);

    alias = yew_xmalloc(name_len + 12U);
    (void)memcpy(alias, "unused/../", 10U);
    (void)memcpy(alias + 10U, name, name_len + 1U);
    YEW_ASSERT(yew_runtime_asset_has(alias));
    yew_xfree(alias);

    bytebuf_init(&out);
    bytebuf_append(&out, sentinel, sizeof(sentinel));
    for (i = 0U; i < YEW_ARRAY_LEN(invalid); i++) {
        YEW_ASSERT(!yew_runtime_asset_has(invalid[i]));
        YEW_ASSERT_NULL(yew_runtime_asset_resolve(invalid[i]));
        YEW_ASSERT(!yew_runtime_asset_read(invalid[i], &out));
        YEW_ASSERT_EQ_U64(out.len, sizeof(sentinel));
        YEW_ASSERT_EQ_MEM(out.data, sentinel, sizeof(sentinel));
    }
    YEW_ASSERT(!yew_runtime_asset_read(name, NULL));
    YEW_ASSERT(!yew_runtime_asset_read("does/not/exist", &out));
    YEW_ASSERT_EQ_U64(out.len, sizeof(sentinel));
    YEW_ASSERT_EQ_MEM(out.data, sentinel, sizeof(sentinel));
    bytebuf_free(&out);
#else
    YEW_ASSERT_NULL(yew_runtime_asset_resolve("runtime/init.fl"));
#endif
}
