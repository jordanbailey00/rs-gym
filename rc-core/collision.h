#ifndef RC_COLLISION_H
#define RC_COLLISION_H

#include "types.h"

#define RC_COLLISION_MAPSQUARE_COUNT RC_MAPSQUARE_COUNT
#define RC_COLLISION_DEFAULT_CACHE_PAGES 64

typedef struct RcCollisionStore RcCollisionStore;

typedef struct {
    uint16_t mapsquare;
    uint32_t flags[RC_MAX_PLANES][RC_REGION_SIZE][RC_REGION_SIZE];
} RcCollisionRegion;

typedef struct {
    uint32_t total_rows;
    uint32_t occupied_pages;
    uint32_t pages_requested;
    uint32_t pages_loaded;
    uint32_t rows_loaded;
    uint32_t pages_resident;
    uint32_t rows_resident;
} RcCollisionLoadStats;

typedef struct {
    RcCollisionStore *store;
    int region_count;
} RcCollisionData;

extern RcCollisionRegion *g_rc_collision_regions;
extern int g_rc_collision_region_count;

int rc_load_collision_tiles(const char *path);
void rc_collision_data_init(RcCollisionData *data);
void rc_collision_data_free(RcCollisionData *data);
int rc_load_collision_tiles_into(const char *path, RcCollisionData *data);
int rc_collision_mirror_to_globals(const RcCollisionData *data);
void rc_collision_use_data(const RcCollisionData *data);
void rc_collision_reset_data_if_active(const RcCollisionData *data);
int rc_collision_is_loaded(void);
uint32_t rc_collision_flags_at(int x, int y, int plane, int *found);
int rc_collision_set_cache_limit(int max_pages);
int rc_collision_populate_map_rect_stats(RcWorldMap *map, int min_x, int min_y,
                                         int max_x, int max_y,
                                         RcCollisionLoadStats *stats);
int rc_collision_populate_map_rect(RcWorldMap *map, int min_x, int min_y,
                                   int max_x, int max_y);

#endif
