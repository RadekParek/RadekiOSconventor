#ifndef RADEK_GAME_RT_ENTRY_H
#define RADEK_GAME_RT_ENTRY_H

#include <stddef.h>

/* Single-use, fail-closed entry for an NDK-linked portable-C translation. */
int radek_translated_game_run(const char *memory_image_path,
                              const char *guest_data_root,
                              const char *guest_bundle_root,
                              const char *guest_obb_root,
                              char *report_json,
                              size_t report_capacity);

#endif
