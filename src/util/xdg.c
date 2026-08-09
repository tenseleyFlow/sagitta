#define _POSIX_C_SOURCE 200809L

#include "util/xdg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/log.h"

char *sag_xdg_state_dir(void)
{
    const char *root = getenv("XDG_STATE_HOME");
    const char *suffix = "/sagitta";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.local/state/sagitta";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = sag_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

char *sag_xdg_config_dir(void)
{
    const char *root = getenv("XDG_CONFIG_HOME");
    const char *suffix = "/sagitta";
    size_t len;
    char *path;

    if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.config/sagitta";
    }
    if (root == NULL || root[0] == '\0')
        return NULL;
    len = strlen(root) + strlen(suffix);
    path = sag_xmalloc(len + 1U);
    (void)memcpy(path, root, strlen(root));
    (void)memcpy(path + strlen(root), suffix, strlen(suffix) + 1U);
    return path;
}

static bool sag_mkdir_one(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0)
        return true;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool sag_mkdirs(const char *path, unsigned int mode)
{
    char *copy;
    char *p;
    bool ok = true;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    copy = sag_xmalloc(strlen(path) + 1U);
    (void)memcpy(copy, path, strlen(path) + 1U);
    for (p = copy + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!sag_mkdir_one(copy, (mode_t)mode)) {
            ok = false;
            break;
        }
        *p = '/';
    }
    if (ok)
        ok = sag_mkdir_one(copy, (mode_t)mode);
    free(copy);
    return ok;
}
