#define _POSIX_C_SOURCE 200809L

#include "util/xdg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/log.h"

char *yew_xdg_state_dir(void)
{
    const char *root = getenv("XDG_STATE_HOME");
    const char *suffix = "/yew";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.local/state/yew";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = yew_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

char *yew_xdg_data_dir(void)
{
    const char *root = getenv("XDG_DATA_HOME");
    const char *suffix = "/yew";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.local/share/yew";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = yew_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

char *yew_xdg_config_dir(void)
{
    const char *root = getenv("XDG_CONFIG_HOME");
    const char *suffix = "/yew";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.config/yew";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = yew_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

char *yew_xdg_cache_dir(void)
{
    const char *root = getenv("XDG_CACHE_HOME");
    const char *suffix = "/yew";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.cache/yew";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = yew_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

static bool yew_mkdir_one(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0)
        return true;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool yew_mkdirs(const char *path, unsigned int mode)
{
    char *copy;
    char *p;
    bool ok = true;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    copy = yew_xmalloc(strlen(path) + 1U);
    (void)memcpy(copy, path, strlen(path) + 1U);
    for (p = copy + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!yew_mkdir_one(copy, (mode_t)mode)) {
            ok = false;
            break;
        }
        *p = '/';
    }
    if (ok)
        ok = yew_mkdir_one(copy, (mode_t)mode);
    yew_xfree(copy);
    return ok;
}
