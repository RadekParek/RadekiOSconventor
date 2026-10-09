#define _POSIX_C_SOURCE 200809L
#include "rt_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s (errno=%d)\n", \
                __FILE__, __LINE__, #condition, errno); \
        return 1; \
    } \
} while (0)

static int resolves(const char *guest, const char *mode, const char *expected) {
    char actual[512];
    if (!rt_guest_host_path(guest, mode, actual, sizeof actual)) return 0;
    return strcmp(actual, expected) == 0;
}

int main(void) {
    char actual[8];
    CHECK(setenv("RADEK_GAME_DATA", "/private/game-data", 1) == 0);
    CHECK(setenv("RADEK_GAME_BUNDLE", "/private/bundle/App.app", 1) == 0);
    CHECK(setenv("RADEK_GAME_OBB", "/private/obb", 1) == 0);

    /* NSHomeDirectory's / and NSTemporaryDirectory's /tmp both stay in the
     * app-private writable root; the specific tmp path takes precedence. */
    CHECK(resolves("/", "r", "/private/game-data"));
    CHECK(resolves("/Documents/save.dat", "w", "/private/game-data/Documents/save.dat"));
    CHECK(resolves("/tmp/cache.bin", "w", "/private/game-data/tmp/cache.bin"));
    CHECK(resolves("./settings.lua", "w", "/private/game-data/settings.lua"));

    /* Bundle files are a more-specific, read-only mount, whether addressed
     * through Foundation's bundle path or the fixture's relative data path. */
    CHECK(resolves("/radek-bundle/App.app/data/level.dat", "r",
                   "/private/bundle/App.app/data/level.dat"));
    CHECK(resolves("./data/level.dat", "r", "/private/bundle/App.app/data/level.dat"));
    errno = 0;
    CHECK(!rt_guest_host_path("data/level.dat", "w", actual, sizeof actual));
    CHECK(errno == EACCES);
    errno = 0;
    CHECK(!rt_guest_host_path("/radek-bundle/App.app/user.dat", "r+", actual, sizeof actual));
    CHECK(errno == EACCES);

    /* OBB is optional and read-only; absent mounts fail closed, not through to
     * the host's real /Android/obb directory. */
    CHECK(resolves("/Android/obb/main.obb", "r", "/private/obb/main.obb"));
    errno = 0;
    CHECK(!rt_guest_host_path("/Android/obb/main.obb", "w", actual, sizeof actual));
    CHECK(errno == EACCES);
    CHECK(setenv("RADEK_GAME_OBB", "", 1) == 0);
    errno = 0;
    CHECK(!rt_guest_host_path("/Android/obb/main.obb", "r", actual, sizeof actual));
    CHECK(errno == EACCES);

    /* Mount boundaries and traversal are checked before constructing a host
     * path; relative user files remain writable under the app-private root. */
    CHECK(setenv("RADEK_GAME_OBB", "/private/obb", 1) == 0);
    CHECK(resolves("/Android/obb-extra/file", "r",
                   "/private/game-data/Android/obb-extra/file"));
    errno = 0;
    CHECK(!rt_guest_host_path("/tmp/../escape", "w", actual, sizeof actual));
    CHECK(errno == EACCES);
    errno = 0;
    CHECK(!rt_guest_host_path("data/../../escape", "r", actual, sizeof actual));
    CHECK(errno == EACCES);
    errno = 0;
    CHECK(!rt_guest_host_path("/tmp/long", "r", actual, sizeof actual));
    CHECK(errno == ENAMETOOLONG);

    puts("translated-runtime app-private, bundle, OBB, and traversal path tests passed");
    return 0;
}
