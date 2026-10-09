#include "rt_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rt_path_prefix(const char *path, const char *prefix) {
    size_t length = strlen(prefix);
    return strncmp(path, prefix, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static int rt_path_has_parent_component(const char *path) {
    const char *cursor = path;
    while (*cursor) {
        const char *start;
        size_t length;
        while (*cursor == '/') cursor++;
        start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        length = (size_t)(cursor - start);
        if (length == 2 && start[0] == '.' && start[1] == '.')
            return 1;
    }
    return 0;
}

static int rt_mode_writes(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

/* Absolute guest paths never fall through to the Android process filesystem.
 * The longest/specific guest mount wins before the app-private guest root. */
int rt_guest_host_path(const char *guest_path, const char *mode,
                       char *host_path, size_t host_capacity) {
    const char *root = NULL;
    const char *relative = guest_path;
    int writable = 1;
    int written;
    if (!guest_path || !*guest_path || !host_path || host_capacity == 0)
        return 0;

    if (rt_path_prefix(guest_path, "/radek-bundle/App.app")) {
        root = getenv("RADEK_GAME_BUNDLE");
        relative = guest_path + strlen("/radek-bundle/App.app");
        writable = 0;
    } else if (rt_path_prefix(guest_path, "/Android/obb")) {
        root = getenv("RADEK_GAME_OBB");
        relative = guest_path + strlen("/Android/obb");
        writable = 0;
    } else if (guest_path[0] == '/') {
        root = getenv("RADEK_GAME_DATA");
        relative = guest_path + 1;
    } else {
        while (relative[0] == '.' && relative[1] == '/')
            relative += 2;
        if ((strncmp(relative, "data/", 5) == 0 || strcmp(relative, "data") == 0) &&
            getenv("RADEK_GAME_BUNDLE") && *getenv("RADEK_GAME_BUNDLE")) {
            root = getenv("RADEK_GAME_BUNDLE");
            writable = 0;
        } else {
            root = getenv("RADEK_GAME_DATA");
        }
    }
    if (!root || !*root || !relative || rt_path_has_parent_component(relative)) {
        errno = EACCES;
        return 0;
    }
    if (rt_mode_writes(mode) && !writable) {
        errno = EACCES;
        return 0;
    }
    if (*relative == '/')
        while (*relative == '/') relative++;
    if (*relative == '\0')
        written = snprintf(host_path, host_capacity, "%s", root);
    else if (root[strlen(root) - 1] == '/')
        written = snprintf(host_path, host_capacity, "%s%s", root, relative);
    else
        written = snprintf(host_path, host_capacity, "%s/%s", root, relative);
    if (written < 0 || (size_t)written >= host_capacity) {
        errno = ENAMETOOLONG;
        return 0;
    }
    return 1;
}
