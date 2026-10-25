#include "collision.h"
#include "io.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTPI_MAGIC 0x49505443u
#define CTPI_VERSION 1u

enum {
    CTPI_HEADER_U32S = 9,
    CTPI_HEADER_BYTES = CTPI_HEADER_U32S * 4,
    CTPI_INDEX_ENTRY_BYTES = 8,
    CTPI_RECORD_BYTES = 8,
    CTPI_MAX_PAGE_ROWS = RC_MAX_PLANES * RC_REGION_SIZE * RC_REGION_SIZE,
};

typedef struct {
    RcCollisionRegion *region;
    uint32_t row_count;
    uint64_t last_used;
    uint16_t mapsquare;
    uint8_t loaded;
} RcCollisionPage;

typedef struct {
    uint32_t first, count;
} RcCollisionRange;

struct RcCollisionStore {
    char *path;
    RcCollisionRange *index;
    RcCollisionPage *pages;
    uint32_t *cache_slots;
    uint32_t cache_capacity;
    uint32_t total_rows;
    uint32_t occupied_pages;
    uint32_t plane_counts[RC_MAX_PLANES];
    uint32_t resident_pages;
    uint32_t resident_rows;
    uint64_t use_clock;
};

RcCollisionRegion *g_rc_collision_regions = NULL;
int g_rc_collision_region_count = 0;

static const RcCollisionData *g_active_collision_data = NULL;
static RcCollisionStore *g_global_collision_store = NULL;

_Static_assert(sizeof(RcTile) == sizeof(uint32_t),
               "collision page copies require one uint32_t per core tile");

static uint32_t read_u32_le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char *copy_string(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, s, size);
    return copy;
}

static void reset_cache_slots(uint32_t *slots) {
    if (slots) {
        memset(slots, 0xff,
               (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*slots));
    }
}

static void collision_store_free(RcCollisionStore *store) {
    if (!store) return;
    if (store->pages) {
        for (uint32_t i = 0; i < store->cache_capacity; i++)
            free(store->pages[i].region);
    }
    free(store->pages);
    free(store->cache_slots);
    free(store->index);
    free(store->path);
    free(store);
}

static RcCollisionStore *collision_store_clone(
    const RcCollisionStore *source) {
    if (!source) return NULL;
    RcCollisionStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->path = copy_string(source->path);
    store->index = malloc(
        (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*store->index));
    store->cache_capacity = source->cache_capacity;
    store->pages = calloc(store->cache_capacity, sizeof(*store->pages));
    store->cache_slots = malloc(
        (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*store->cache_slots));
    if (!store->path || !store->index || !store->pages
            || !store->cache_slots) {
        collision_store_free(store);
        return NULL;
    }
    memcpy(store->index, source->index,
           (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*store->index));
    reset_cache_slots(store->cache_slots);
    store->total_rows = source->total_rows;
    store->occupied_pages = source->occupied_pages;
    memcpy(store->plane_counts, source->plane_counts,
           sizeof(store->plane_counts));
    return store;
}

static RcCollisionStore *collision_store_load(const char *path) {
    if (!path || !path[0]) return NULL;
    RcAssetReader *reader = rc_asset_reader_open(path);
    if (!reader) return NULL;

    unsigned char raw_header[CTPI_HEADER_BYTES];
    uint32_t header[CTPI_HEADER_U32S];
    if (!rc_asset_reader_read_at(reader, 0, raw_header, sizeof(raw_header))) {
        rc_asset_reader_close(reader);
        return NULL;
    }
    for (int i = 0; i < CTPI_HEADER_U32S; i++)
        header[i] = read_u32_le(raw_header + i * 4);

    uint64_t records_offset = CTPI_HEADER_BYTES
        + (uint64_t)RC_COLLISION_MAPSQUARE_COUNT * CTPI_INDEX_ENTRY_BYTES;
    uint64_t records_size = (uint64_t)header[2] * header[3];
    uint64_t plane_total = (uint64_t)header[5] + header[6]
                         + header[7] + header[8];
    if (header[0] != CTPI_MAGIC || header[1] != CTPI_VERSION
            || header[3] != CTPI_RECORD_BYTES
            || header[4] > RC_COLLISION_MAPSQUARE_COUNT
            || plane_total != header[2]
            || rc_asset_reader_size(reader) != records_offset + records_size) {
        rc_asset_reader_close(reader);
        return NULL;
    }

    RcCollisionStore *store = calloc(1, sizeof(*store));
    unsigned char *raw_index = malloc(
        (size_t)RC_COLLISION_MAPSQUARE_COUNT * CTPI_INDEX_ENTRY_BYTES);
    if (!store || !raw_index) {
        free(raw_index);
        collision_store_free(store);
        rc_asset_reader_close(reader);
        return NULL;
    }
    store->path = copy_string(path);
    store->index = malloc(
        (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*store->index));
    store->cache_capacity = RC_COLLISION_DEFAULT_CACHE_PAGES;
    store->pages = calloc(store->cache_capacity, sizeof(*store->pages));
    store->cache_slots = malloc(
        (size_t)RC_COLLISION_MAPSQUARE_COUNT * sizeof(*store->cache_slots));
    if (!store->path || !store->index || !store->pages
            || !store->cache_slots
            || !rc_asset_reader_read_at(
                reader, CTPI_HEADER_BYTES, raw_index,
                (size_t)RC_COLLISION_MAPSQUARE_COUNT
                    * CTPI_INDEX_ENTRY_BYTES)) {
        free(raw_index);
        collision_store_free(store);
        rc_asset_reader_close(reader);
        return NULL;
    }
    reset_cache_slots(store->cache_slots);

    uint32_t expected_first = 0;
    uint32_t occupied_pages = 0;
    for (uint32_t i = 0; i < RC_COLLISION_MAPSQUARE_COUNT; i++) {
        const unsigned char *raw = raw_index
            + (size_t)i * CTPI_INDEX_ENTRY_BYTES;
        RcCollisionRange page = {
            .first = read_u32_le(raw),
            .count = read_u32_le(raw + 4),
        };
        if (page.first != expected_first
                || page.count > header[2] - expected_first
                || page.count > CTPI_MAX_PAGE_ROWS) {
            free(raw_index);
            collision_store_free(store);
            rc_asset_reader_close(reader);
            return NULL;
        }
        store->index[i] = page;
        expected_first += page.count;
        occupied_pages += page.count > 0;
    }
    free(raw_index);
    rc_asset_reader_close(reader);
    if (expected_first != header[2] || occupied_pages != header[4]) {
        collision_store_free(store);
        return NULL;
    }
    store->total_rows = header[2];
    store->occupied_pages = header[4];
    memcpy(store->plane_counts, &header[5], sizeof(store->plane_counts));
    return store;
}

static int parse_collision_page(const unsigned char *raw, uint32_t count,
                                uint16_t mapsquare,
                                RcCollisionRegion *region) {
    uint32_t last_key = 0;
    region->mapsquare = mapsquare;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *row = raw + (size_t)i * CTPI_RECORD_BYTES;
        uint8_t local_x = row[0];
        uint8_t local_y = row[1];
        uint8_t plane = row[2];
        uint8_t pad = row[3];
        uint32_t flags = read_u32_le(row + 4);
        uint32_t key = (uint32_t)plane * RC_REGION_SIZE * RC_REGION_SIZE
                     + (uint32_t)local_x * RC_REGION_SIZE + local_y;
        if (local_x >= RC_REGION_SIZE || local_y >= RC_REGION_SIZE
                || plane >= RC_MAX_PLANES || pad != 0 || flags == 0
                || (i > 0 && key <= last_key)) {
            return 0;
        }
        region->flags[plane][local_x][local_y] = flags;
        last_key = key;
    }
    return 1;
}

static RcCollisionPage *cached_collision_page(RcCollisionStore *store,
                                               uint16_t mapsquare) {
    uint32_t slot = store->cache_slots[mapsquare];
    if (slot >= store->cache_capacity) return NULL;
    RcCollisionPage *page = &store->pages[slot];
    return page->loaded && page->mapsquare == mapsquare ? page : NULL;
}

static RcCollisionPage *collision_cache_slot(RcCollisionStore *store) {
    RcCollisionPage *oldest = &store->pages[0];
    for (uint32_t i = 0; i < store->cache_capacity; i++) {
        RcCollisionPage *page = &store->pages[i];
        if (!page->loaded) return page;
        if (page->last_used < oldest->last_used) oldest = page;
    }
    return oldest;
}

static int collision_page_get(RcCollisionStore *store, RcAssetReader *reader,
                              uint16_t mapsquare, RcCollisionPage **out,
                              int *loaded) {
    if (out) *out = NULL;
    if (loaded) *loaded = 0;
    if (!store) return 0;
    RcCollisionRange source = store->index[mapsquare];
    if (source.count == 0) return 1;

    RcCollisionPage *page = cached_collision_page(store, mapsquare);
    if (page) {
        page->last_used = ++store->use_clock;
        if (out) *out = page;
        return 1;
    }

    int close_reader = 0;
    if (!reader) {
        reader = rc_asset_reader_open(store->path);
        close_reader = 1;
    }
    if (!reader) return 0;
    size_t raw_size = (size_t)source.count * CTPI_RECORD_BYTES;
    unsigned char *raw = malloc(raw_size);
    RcCollisionRegion *region = calloc(1, sizeof(*region));
    uint64_t records_offset = CTPI_HEADER_BYTES
        + (uint64_t)RC_COLLISION_MAPSQUARE_COUNT * CTPI_INDEX_ENTRY_BYTES;
    int ok = raw && region
        && rc_asset_reader_read_at(
            reader, records_offset + (uint64_t)source.first * CTPI_RECORD_BYTES,
            raw, raw_size)
        && parse_collision_page(raw, source.count, mapsquare, region);
    free(raw);
    if (close_reader) rc_asset_reader_close(reader);
    if (!ok) {
        free(region);
        return 0;
    }

    page = collision_cache_slot(store);
    if (page->loaded) {
        store->cache_slots[page->mapsquare] = UINT32_MAX;
        store->resident_rows -= page->row_count;
        free(page->region);
    } else {
        store->resident_pages++;
    }
    *page = (RcCollisionPage){
        .region = region,
        .row_count = source.count,
        .last_used = ++store->use_clock,
        .mapsquare = mapsquare,
        .loaded = 1,
    };
    store->cache_slots[mapsquare] = (uint32_t)(page - store->pages);
    store->resident_rows += source.count;
    if (out) *out = page;
    if (loaded) *loaded = 1;
    return 1;
}

static RcCollisionStore *active_collision_store(void) {
    if (g_active_collision_data) return g_active_collision_data->store;
    return g_global_collision_store;
}

void rc_collision_data_init(RcCollisionData *data) {
    if (!data) return;
    data->store = NULL;
    data->region_count = 0;
}

void rc_collision_data_free(RcCollisionData *data) {
    if (!data) return;
    collision_store_free(data->store);
    rc_collision_data_init(data);
}

void rc_collision_use_data(const RcCollisionData *data) {
    g_active_collision_data = data;
}

void rc_collision_reset_data_if_active(const RcCollisionData *data) {
    if (g_active_collision_data == data) rc_collision_use_data(NULL);
}

int rc_load_collision_tiles_into(const char *path, RcCollisionData *data) {
    if (!path || !data) return -1;
    RcCollisionStore *store = collision_store_load(path);
    if (!store) return -1;
    collision_store_free(data->store);
    data->store = store;
    data->region_count = (int)store->occupied_pages;
    return data->region_count;
}

int rc_collision_mirror_to_globals(const RcCollisionData *data) {
    if (!data) return 0;
    RcCollisionStore *store = collision_store_clone(data->store);
    if (data->store && !store) return 0;
    collision_store_free(g_global_collision_store);
    free(g_rc_collision_regions);
    g_global_collision_store = store;
    g_rc_collision_regions = NULL;
    g_rc_collision_region_count = data->region_count;
    return 1;
}

int rc_load_collision_tiles(const char *path) {
    RcCollisionData data;
    rc_collision_data_init(&data);
    int loaded = rc_load_collision_tiles_into(path, &data);
    if (loaded >= 0 && !rc_collision_mirror_to_globals(&data)) loaded = -1;
    rc_collision_data_free(&data);
    if (loaded >= 0) rc_collision_use_data(NULL);
    return loaded;
}

int rc_collision_is_loaded(void) {
    if (g_active_collision_data) {
        return g_active_collision_data->store
            && g_active_collision_data->region_count > 0;
    }
    return g_global_collision_store && g_rc_collision_region_count > 0;
}

int rc_collision_set_cache_limit(int max_pages) {
    RcCollisionStore *store = active_collision_store();
    if (!store) return 0;
    if (max_pages <= 0) return -1;
    if (max_pages > RC_COLLISION_MAPSQUARE_COUNT)
        max_pages = RC_COLLISION_MAPSQUARE_COUNT;
    if ((uint32_t)max_pages == store->cache_capacity) return 0;
    RcCollisionPage *pages = calloc((size_t)max_pages, sizeof(*pages));
    if (!pages) return -1;
    for (uint32_t i = 0; i < store->cache_capacity; i++)
        free(store->pages[i].region);
    free(store->pages);
    store->pages = pages;
    reset_cache_slots(store->cache_slots);
    store->cache_capacity = (uint32_t)max_pages;
    store->resident_pages = 0;
    store->resident_rows = 0;
    store->use_clock = 0;
    return 1;
}

static int collision_mapsquare_bounds(int min_x, int min_y,
                                      int max_x, int max_y,
                                      int *min_rx, int *min_ry,
                                      int *max_rx, int *max_ry) {
    if (min_x > max_x || min_y > max_y) return -1;
    RcTileRect bounds;
    if (!rc_tile_rect_intersect_world(min_x, min_y, max_x, max_y, &bounds))
        return 0;
    *min_rx = bounds.min_x / RC_MAPSQUARE_SIZE;
    *min_ry = bounds.min_y / RC_MAPSQUARE_SIZE;
    *max_rx = bounds.max_x / RC_MAPSQUARE_SIZE;
    *max_ry = bounds.max_y / RC_MAPSQUARE_SIZE;
    return 1;
}

int rc_collision_populate_map_rect_stats(RcWorldMap *map, int min_x, int min_y,
                                         int max_x, int max_y,
                                         RcCollisionLoadStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    RcCollisionStore *store = active_collision_store();
    if (!map || !store || !rc_collision_is_loaded()) return -1;
    if (stats) {
        stats->total_rows = store->total_rows;
        stats->occupied_pages = store->occupied_pages;
    }

    int min_rx, min_ry, max_rx, max_ry;
    int bounds = collision_mapsquare_bounds(
        min_x, min_y, max_x, max_y, &min_rx, &min_ry, &max_rx, &max_ry);
    if (bounds < 0) return -1;
    memset(map, 0, sizeof(*map));
    if (bounds == 0) {
        if (stats) {
            stats->pages_resident = store->resident_pages;
            stats->rows_resident = store->resident_rows;
        }
        return 0;
    }
    uint64_t geometric_pages = (uint64_t)(max_rx - min_rx + 1)
                             * (uint64_t)(max_ry - min_ry + 1);
    if (geometric_pages > RC_MAX_REGIONS) return -1;
    map->base_region_x = min_rx;
    map->base_region_y = min_ry;
    map->region_width = max_rx - min_rx + 1;
    map->region_height = max_ry - min_ry + 1;

    uint32_t missing_pages = 0;
    for (int rx = min_rx; rx <= max_rx; rx++) {
        for (int ry = min_ry; ry <= max_ry; ry++) {
            uint16_t mapsquare;
            if (!rc_mapsquare_key(rx, ry, &mapsquare)) return -1;
            if (store->index[mapsquare].count == 0) continue;
            if (stats) stats->pages_requested++;
            if (!cached_collision_page(store, mapsquare)) missing_pages++;
        }
    }

    RcAssetReader *reader = missing_pages
        ? rc_asset_reader_open(store->path) : NULL;
    if (missing_pages && !reader) return -1;
    int result = 0;
    for (int rx = min_rx; rx <= max_rx && result >= 0; rx++) {
        for (int ry = min_ry; ry <= max_ry; ry++) {
            uint16_t mapsquare;
            if (!rc_mapsquare_key(rx, ry, &mapsquare)) {
                result = -1;
                break;
            }
            if (store->index[mapsquare].count == 0) continue;
            RcCollisionPage *page = NULL;
            int loaded = 0;
            if (!collision_page_get(store, reader, mapsquare, &page, &loaded)
                    || !page || map->region_count >= RC_MAX_REGIONS) {
                result = -1;
                break;
            }
            RcRegion *dst = &map->regions[map->region_count++];
            dst->region_x = rx;
            dst->region_y = ry;
            dst->loaded = 1;
            memcpy(dst->tiles, page->region->flags, sizeof(dst->tiles));
            if (loaded && stats) {
                stats->pages_loaded++;
                stats->rows_loaded += page->row_count;
            }
        }
    }
    if (reader) rc_asset_reader_close(reader);
    if (stats) {
        stats->pages_resident = store->resident_pages;
        stats->rows_resident = store->resident_rows;
    }
    return result < 0 ? -1 : map->region_count;
}

int rc_collision_populate_map_rect(RcWorldMap *map, int min_x, int min_y,
                                   int max_x, int max_y) {
    return rc_collision_populate_map_rect_stats(
        map, min_x, min_y, max_x, max_y, NULL);
}

uint32_t rc_collision_flags_at(int x, int y, int plane, int *found) {
    if (found) *found = 0;
    uint16_t mapsquare;
    int local_x, local_y;
    if (!rc_collision_is_loaded() || !rc_world_tile_valid(x, y, plane)
            || !rc_world_to_mapsquare(x, y, &mapsquare,
                                      &local_x, &local_y)) {
        return 0;
    }
    RcCollisionStore *store = active_collision_store();
    RcCollisionPage *page = NULL;
    if (!collision_page_get(store, NULL, mapsquare, &page, NULL) || !page)
        return 0;
    if (found) *found = 1;
    return page->region->flags[plane][local_x][local_y];
}
