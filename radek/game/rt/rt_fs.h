#ifndef RADEK_GAME_RT_FS_H
#define RADEK_GAME_RT_FS_H

#include <stddef.h>

/* Resolve guest-visible filesystem names into explicit app-private mounts.
 * Returns 1 on success and 0 for an unmapped, unsafe, or read-only write path. */
int rt_guest_host_path(const char *guest_path, const char *mode,
                       char *host_path, size_t host_capacity);

#endif
