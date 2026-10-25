#ifndef RC_SPAWN_INDEX_H
#define RC_SPAWN_INDEX_H

#include "coordinates.h"

#include <stdint.h>

enum {
    RC_SPAWN_INDEX_VERSION = 1,
    RC_SPAWN_INDEX_MAPSQUARE_COUNT = RC_MAPSQUARE_COUNT,
};

typedef struct {
    unsigned char *records;
    uint32_t record_count;
    uint32_t total_rows;
    uint32_t occupied_pages;
    uint32_t pages_loaded;
    uint32_t record_size;
    uint32_t source_plane_counts[RC_MAX_PLANES];
} RcSpawnIndexSlice;

int rc_spawn_index_read(const char *path, uint32_t expected_magic,
                        uint32_t expected_record_size, int use_filter,
                        int min_x, int min_y, int max_x, int max_y,
                        RcSpawnIndexSlice *out);
int rc_spawn_index_sort_source_order(RcSpawnIndexSlice *slice);
uint64_t rc_spawn_index_record_key(const char *path, uint32_t source_order,
                                   uint32_t ordinal);
void rc_spawn_index_slice_free(RcSpawnIndexSlice *slice);

#endif
