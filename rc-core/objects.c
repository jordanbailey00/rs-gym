#include "objects.h"
#include "coordinates.h"
#include "io.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODEF_MAGIC 0x4645444Fu
#define OPLI_MAGIC 0x494C504Fu
#define OBHV_MAGIC 0x5648424Fu
#define OBJ_VERSION 1u
#define OPLI_VERSION 1u
#define ODEF_VERSION_MIN 1u
#define ODEF_VERSION_MAX 2u
#define OBHV_VERSION_MIN 1u
#define OBHV_VERSION_MAX 2u

enum {
    OPLI_HEADER_U32S = 9,
    OPLI_HEADER_BYTES = OPLI_HEADER_U32S * 4,
    OPLI_INDEX_ENTRY_BYTES = 8,
    OPLI_RECORD_BYTES = 22,
};

typedef struct {
    RcObjectPlacement *rows;
    uint32_t count;
    uint64_t last_used;
    uint16_t mapsquare;
    uint8_t loaded;
} RcObjectPlacementPage;

struct RcObjectPlacementStore {
    char *path;
    RcObjectRange *index;
    RcObjectPlacementPage *pages;
    uint32_t *cache_slots;
    uint32_t cache_capacity;
    uint32_t total_rows;
    uint32_t occupied_pages;
    uint32_t source_plane_counts[4];
    uint32_t resident_pages;
    uint32_t resident_rows;
    uint64_t use_clock;
};

RcObjectDef g_rc_object_defs[RC_MAX_OBJECT_ID];
RcObjectBehavior g_rc_object_behaviors[RC_MAX_OBJECT_ID];
RcObjectPlacement *g_rc_object_placements = NULL;
RcObjectParam *g_rc_object_params = NULL;
int32_t *g_rc_object_transforms = NULL;
int g_rc_object_def_count = 0;
int g_rc_object_behavior_count = 0;
int g_rc_object_placement_count = 0;
int g_rc_object_param_count = 0;
int g_rc_object_transform_count = 0;

static RcObjectRange g_region_index[RC_MAX_OBJECT_ID];
static const RcObjectDef *g_active_object_defs = g_rc_object_defs;
static const RcObjectBehavior *g_active_object_behaviors =
    g_rc_object_behaviors;
static const RcObjectPlacement *g_active_object_placements = NULL;
static const RcObjectParam *g_active_object_params = NULL;
static const int32_t *g_active_object_transforms = NULL;
static const RcObjectRange *g_active_region_index = g_region_index;
static int g_active_object_def_count = 0;
static int g_active_object_behavior_count = 0;
static int g_active_object_placement_count = 0;
static int g_active_object_param_count = 0;
static int g_active_object_transform_count = 0;
static const RcObjectData *g_active_object_data = NULL;
static RcObjectPlacementStore *g_global_placement_store = NULL;

static uint16_t read_u16_le(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *p) {
    return (uint64_t)read_u32_le(p)
         | ((uint64_t)read_u32_le(p + 4) << 32);
}

static char *copy_string(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, s, size);
    return copy;
}

static void object_placement_store_free(RcObjectPlacementStore *store) {
    if (!store) return;
    if (store->pages) {
        for (uint32_t i = 0; i < store->cache_capacity; i++)
            free(store->pages[i].rows);
    }
    free(store->pages);
    free(store->cache_slots);
    free(store->index);
    free(store->path);
    free(store);
}

static RcObjectPlacementStore *object_placement_store_clone(
    const RcObjectPlacementStore *source) {
    if (!source) return NULL;
    RcObjectPlacementStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->path = copy_string(source->path);
    store->index = malloc((size_t)RC_MAX_OBJECT_ID * sizeof(*store->index));
    store->cache_capacity = source->cache_capacity;
    store->pages = calloc(store->cache_capacity, sizeof(*store->pages));
    store->cache_slots = malloc(
        (size_t)RC_MAX_OBJECT_ID * sizeof(*store->cache_slots));
    if (!store->path || !store->index || !store->pages
            || !store->cache_slots) {
        object_placement_store_free(store);
        return NULL;
    }
    memset(store->cache_slots, 0xff,
           (size_t)RC_MAX_OBJECT_ID * sizeof(*store->cache_slots));
    memcpy(store->index, source->index,
           (size_t)RC_MAX_OBJECT_ID * sizeof(*store->index));
    store->total_rows = source->total_rows;
    store->occupied_pages = source->occupied_pages;
    memcpy(store->source_plane_counts, source->source_plane_counts,
           sizeof(store->source_plane_counts));
    return store;
}

static void reset_object_range_index(RcObjectRange *index) {
    if (!index) return;
    for (int i = 0; i < RC_MAX_OBJECT_ID; i++) {
        index[i].first = UINT32_MAX;
        index[i].count = 0;
    }
}

void rc_object_data_init(RcObjectData *data) {
    if (!data) return;
    memset(data->defs, 0, sizeof(data->defs));
    memset(data->behaviors, 0, sizeof(data->behaviors));
    data->placements = NULL;
    data->placement_store = NULL;
    data->params = NULL;
    data->transforms = NULL;
    data->def_count = 0;
    data->behavior_count = 0;
    data->placement_count = 0;
    data->param_count = 0;
    data->transform_count = 0;
    reset_object_range_index(data->region_index);
}

void rc_object_data_free(RcObjectData *data) {
    if (!data) return;
    free(data->placements);
    object_placement_store_free(data->placement_store);
    free(data->params);
    free(data->transforms);
    rc_object_data_init(data);
}

void rc_objects_use_data(const RcObjectData *data) {
    g_active_object_data = data;
    if (!data) {
        g_active_object_defs = g_rc_object_defs;
        g_active_object_behaviors = g_rc_object_behaviors;
        g_active_object_placements = g_rc_object_placements;
        g_active_object_params = g_rc_object_params;
        g_active_object_transforms = g_rc_object_transforms;
        g_active_region_index = g_region_index;
        g_active_object_def_count = g_rc_object_def_count;
        g_active_object_behavior_count = g_rc_object_behavior_count;
        g_active_object_placement_count = g_rc_object_placement_count;
        g_active_object_param_count = g_rc_object_param_count;
        g_active_object_transform_count = g_rc_object_transform_count;
        return;
    }
    g_active_object_defs = data->defs;
    g_active_object_behaviors = data->behaviors;
    g_active_object_placements = data->placements;
    g_active_object_params = data->params;
    g_active_object_transforms = data->transforms;
    g_active_region_index = data->region_index;
    g_active_object_def_count = data->def_count;
    g_active_object_behavior_count = data->behavior_count;
    g_active_object_placement_count = data->placement_count;
    g_active_object_param_count = data->param_count;
    g_active_object_transform_count = data->transform_count;
}

void rc_objects_reset_data_if_active(const RcObjectData *data) {
    if (!data) return;
    if (g_active_object_defs == data->defs
            || g_active_object_behaviors == data->behaviors
            || g_active_object_placements == data->placements
            || g_active_object_data == data
            || g_active_object_params == data->params
            || g_active_object_transforms == data->transforms) {
        rc_objects_use_data(NULL);
    }
}

static int read_pstr(FILE *f, char *out, int cap,
                     const char *path, const char *what) {
    uint16_t len;
    if (!rc_read_exact(f, &len, sizeof(len), 1, path, what)) return 0;
    int keep = len < (uint16_t)(cap - 1) ? (int)len : cap - 1;
    if (keep && !rc_read_exact(f, out, 1, (size_t)keep, path, what)) return 0;
    out[keep] = '\0';
    if (len > (uint16_t)keep &&
            !rc_seek(f, (long)(len - (uint16_t)keep), SEEK_CUR, path, what)) {
        return 0;
    }
    return 1;
}

static int read_header(FILE *f, const char *path, uint32_t expect_magic,
                       uint32_t *count) {
    uint32_t magic, version;
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path, "version")
            || !rc_read_exact(f, count, sizeof(*count), 1, path, "count")) {
        return 0;
    }
    return magic == expect_magic && version == OBJ_VERSION;
}

static int read_header_version(FILE *f, const char *path,
                               uint32_t expect_magic, uint32_t min_version,
                               uint32_t max_version, uint32_t *version_out,
                               uint32_t *count) {
    uint32_t magic, version;
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path, "version")
            || !rc_read_exact(f, count, sizeof(*count), 1, path, "count")) {
        return 0;
    }
    if (version_out) *version_out = version;
    return magic == expect_magic
        && version >= min_version
        && version <= max_version;
}

static int append_object_param_into(RcObjectParam **params, int *count,
                                    uint32_t obj_id, uint32_t key,
                                    int32_t value) {
    if (!params || !count || *count < 0) return 0;
    size_t next_count = (size_t)*count + 1u;
    RcObjectParam *next = realloc(*params,
                                  next_count * sizeof(*next));
    if (!next) return 0;
    *params = next;
    (*params)[*count] = (RcObjectParam){
        .obj_id = obj_id,
        .key = key,
        .value = value,
    };
    (*count)++;
    return 1;
}

static int append_object_transform_into(int32_t **transforms, int *count,
                                        int *capacity, int32_t value) {
    if (!transforms || !count || !capacity || *count < 0 || *capacity < 0)
        return 0;
    if (*count == *capacity) {
        int next_capacity = *capacity > 0 ? *capacity * 2 : 1024;
        if (next_capacity <= *count) return 0;
        int32_t *next = realloc(*transforms,
            (size_t)next_capacity * sizeof(*next));
        if (!next) return 0;
        *transforms = next;
        *capacity = next_capacity;
    }
    (*transforms)[(*count)++] = value;
    return 1;
}

int rc_load_object_defs_into(const char *path, RcObjectData *data) {
    if (!path || !data) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;
    uint32_t version, count;
    if (!read_header_version(f, path, ODEF_MAGIC, ODEF_VERSION_MIN,
                             ODEF_VERSION_MAX, &version, &count)) {
        rc_asset_close(f);
        return -1;
    }
    memset(data->defs, 0, sizeof(data->defs));
    free(data->params);
    data->params = NULL;
    data->param_count = 0;
    free(data->transforms);
    data->transforms = NULL;
    data->transform_count = 0;
    data->def_count = 0;
    int transform_capacity = 0;
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t obj_id, flags;
        uint16_t width, length;
        uint8_t interact_type, action_count, model_count, transform_count;
        uint8_t force_approach;
        uint8_t supports_items = 255, clip_flags = 0;
        uint16_t param_count = 0;
        uint16_t ambient_sound_distance = 0, ambient_sound_retain = 0;
        int32_t varbit, varp, animation_id;
        int32_t ambient_sound_id = -1;
        uint32_t map_icon;
        if (!rc_read_exact(f, &obj_id, sizeof(obj_id), 1, path, "object id")
                || !rc_read_exact(f, &width, sizeof(width), 1, path, "width")
                || !rc_read_exact(f, &length, sizeof(length), 1, path, "length")
                || !rc_read_exact(f, &interact_type, sizeof(interact_type), 1,
                                  path, "interact type")
                || !rc_read_exact(f, &action_count, sizeof(action_count), 1,
                                  path, "action count")
                || !rc_read_exact(f, &model_count, sizeof(model_count), 1,
                                  path, "model count")
                || !rc_read_exact(f, &transform_count, sizeof(transform_count),
                                  1, path, "transform count")
                || !rc_read_exact(f, &force_approach,
                                  sizeof(force_approach), 1, path,
                                  "force approach")
                || !rc_read_exact(f, &varbit, sizeof(varbit), 1, path, "varbit")
                || !rc_read_exact(f, &varp, sizeof(varp), 1, path, "varp")
                || !rc_read_exact(f, &animation_id, sizeof(animation_id), 1,
                                  path, "animation")
                || !rc_read_exact(f, &map_icon, sizeof(map_icon), 1,
                                  path, "map icon")
                || !rc_read_exact(f, &flags, sizeof(flags), 1, path, "flags")) {
            rc_asset_close(f);
            return -1;
        }
        if (version >= 2u) {
            if (!rc_read_exact(f, &supports_items, sizeof(supports_items), 1,
                               path, "supports items")
                    || !rc_read_exact(f, &clip_flags, sizeof(clip_flags), 1,
                                      path, "clip flags")
                    || !rc_read_exact(f, &param_count, sizeof(param_count), 1,
                                      path, "param count")
                    || !rc_read_exact(f, &ambient_sound_id,
                                      sizeof(ambient_sound_id), 1, path,
                                      "ambient sound")
                    || !rc_read_exact(f, &ambient_sound_distance,
                                      sizeof(ambient_sound_distance), 1, path,
                                      "ambient sound distance")
                    || !rc_read_exact(f, &ambient_sound_retain,
                                      sizeof(ambient_sound_retain), 1, path,
                                      "ambient sound retain")) {
                rc_asset_close(f);
                return -1;
            }
        }
        RcObjectDef row;
        memset(&row, 0, sizeof(row));
        row.id = (int)obj_id;
        row.width = width;
        row.length = length;
        row.interact_type = interact_type;
        row.action_count = action_count;
        row.transform_offset = (uint32_t)data->transform_count;
        row.transform_count = transform_count;
        row.force_approach = force_approach;
        row.varbit = varbit;
        row.varp = varp;
        row.flags = flags;
        row.supports_items = supports_items;
        row.clip_flags = clip_flags;
        if (!read_pstr(f, row.name, sizeof(row.name), path, "name")) {
            rc_asset_close(f);
            return -1;
        }
        for (int a = 0; a < RC_OBJECT_ACTIONS; a++) {
            if (!read_pstr(f, row.actions[a], sizeof(row.actions[a]),
                           path, "action")) {
                rc_asset_close(f);
                return -1;
            }
        }
        long model_skip = (long)model_count * 4L;
        if (model_skip && !rc_seek(f, model_skip, SEEK_CUR, path, "models")) {
            rc_asset_close(f);
            return -1;
        }
        for (int t = 0; t < transform_count; t++) {
            int32_t transform;
            if (!rc_read_exact(f, &transform, sizeof(transform), 1, path,
                               "transform")) {
                rc_asset_close(f);
                return -1;
            }
            if (!append_object_transform_into(
                    &data->transforms, &data->transform_count,
                    &transform_capacity, transform)) {
                rc_asset_close(f);
                return -1;
            }
        }
        row.param_first = (uint32_t)data->param_count;
        row.param_count = param_count;
        for (uint16_t p = 0; p < param_count; p++) {
            uint32_t key;
            int32_t value;
            if (!rc_read_exact(f, &key, sizeof(key), 1, path, "param key")
                    || !rc_read_exact(f, &value, sizeof(value), 1, path,
                                      "param value")) {
                rc_asset_close(f);
                return -1;
            }
            if (obj_id < RC_MAX_OBJECT_ID
                    && !append_object_param_into(&data->params,
                                                 &data->param_count,
                                                 obj_id, key, value)) {
                rc_asset_close(f);
                return -1;
            }
        }
        if (obj_id >= RC_MAX_OBJECT_ID) {
            row.param_count = 0;
        }
        if (obj_id < RC_MAX_OBJECT_ID) {
            row.loaded = 1;
            data->defs[obj_id] = row;
            loaded++;
        }
    }
    rc_asset_close(f);
    data->def_count = loaded;
    return loaded;
}

int rc_load_object_behaviors_into(const char *path, RcObjectData *data) {
    if (!path || !data) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;
    uint32_t version, count;
    if (!read_header_version(f, path, OBHV_MAGIC, OBHV_VERSION_MIN,
                             OBHV_VERSION_MAX, &version, &count)) {
        rc_asset_close(f);
        return -1;
    }
    memset(data->behaviors, 0, sizeof(data->behaviors));
    data->behavior_count = 0;
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t obj_id, flags;
        int32_t next_loc_stage = -1;
        int32_t open_sound = -1;
        int32_t close_sound = -1;
        int32_t climb_anim = -1;
        uint8_t action_mask, skill;
        uint16_t pad;
        if (!rc_read_exact(f, &obj_id, sizeof(obj_id), 1, path, "object id")
                || !rc_read_exact(f, &flags, sizeof(flags), 1, path, "flags")) {
            rc_asset_close(f);
            return -1;
        }
        if (version >= 2u) {
            if (!rc_read_exact(f, &next_loc_stage, sizeof(next_loc_stage), 1,
                               path, "next loc stage")
                    || !rc_read_exact(f, &open_sound, sizeof(open_sound), 1,
                                      path, "open sound")
                    || !rc_read_exact(f, &close_sound, sizeof(close_sound), 1,
                                      path, "close sound")
                    || !rc_read_exact(f, &climb_anim, sizeof(climb_anim), 1,
                                      path, "climb animation")) {
                rc_asset_close(f);
                return -1;
            }
        }
        if (!rc_read_exact(f, &action_mask, sizeof(action_mask), 1,
                           path, "action mask")
                || !rc_read_exact(f, &skill, sizeof(skill), 1, path, "skill")
                || !rc_read_exact(f, &pad, sizeof(pad), 1, path, "pad")) {
            rc_asset_close(f);
            return -1;
        }
        if (obj_id < RC_MAX_OBJECT_ID) {
            data->behaviors[obj_id] = (RcObjectBehavior){
                .flags = flags,
                .next_loc_stage = next_loc_stage,
                .action_mask = action_mask,
                .skill = skill,
                .loaded = 1,
            };
            loaded++;
        }
    }
    rc_asset_close(f);
    data->behavior_count = loaded;
    return loaded;
}

static RcObjectPlacementStore *object_placement_store_load(const char *path) {
    if (!path || !path[0]) return NULL;
    RcAssetReader *reader = rc_asset_reader_open(path);
    if (!reader) return NULL;

    unsigned char raw_header[OPLI_HEADER_BYTES];
    uint32_t header[OPLI_HEADER_U32S];
    if (!rc_asset_reader_read_at(reader, 0, raw_header, sizeof(raw_header))) {
        rc_asset_reader_close(reader);
        return NULL;
    }
    for (int i = 0; i < OPLI_HEADER_U32S; i++)
        header[i] = read_u32_le(raw_header + i * 4);
    uint64_t records_offset = OPLI_HEADER_BYTES
        + (uint64_t)RC_MAX_OBJECT_ID * OPLI_INDEX_ENTRY_BYTES;
    uint64_t records_size = (uint64_t)header[2] * header[3];
    if (header[0] != OPLI_MAGIC || header[1] != OPLI_VERSION
            || header[3] != OPLI_RECORD_BYTES
            || header[2] > INT_MAX
            || header[4] > RC_MAX_OBJECT_ID
            || (uint64_t)header[5] + header[6] + header[7] + header[8]
                   != header[2]
            || records_size > SIZE_MAX
            || rc_asset_reader_size(reader) != records_offset + records_size) {
        rc_asset_reader_close(reader);
        return NULL;
    }

    RcObjectPlacementStore *store = calloc(1, sizeof(*store));
    unsigned char *raw_index = malloc(
        (size_t)RC_MAX_OBJECT_ID * OPLI_INDEX_ENTRY_BYTES);
    if (!store || !raw_index) {
        free(raw_index);
        object_placement_store_free(store);
        rc_asset_reader_close(reader);
        return NULL;
    }
    store->path = copy_string(path);
    store->index = malloc((size_t)RC_MAX_OBJECT_ID * sizeof(*store->index));
    store->cache_capacity = RC_OBJECT_PLACEMENT_DEFAULT_CACHE_PAGES;
    store->pages = calloc(store->cache_capacity, sizeof(*store->pages));
    store->cache_slots = malloc(
        (size_t)RC_MAX_OBJECT_ID * sizeof(*store->cache_slots));
    if (!store->path || !store->index || !store->pages
            || !store->cache_slots
            || !rc_asset_reader_read_at(
                reader, OPLI_HEADER_BYTES, raw_index,
                (size_t)RC_MAX_OBJECT_ID * OPLI_INDEX_ENTRY_BYTES)) {
        free(raw_index);
        object_placement_store_free(store);
        rc_asset_reader_close(reader);
        return NULL;
    }
    memset(store->cache_slots, 0xff,
           (size_t)RC_MAX_OBJECT_ID * sizeof(*store->cache_slots));

    uint32_t expected_first = 0;
    uint32_t occupied_pages = 0;
    for (uint32_t i = 0; i < RC_MAX_OBJECT_ID; i++) {
        const unsigned char *raw = raw_index
            + (size_t)i * OPLI_INDEX_ENTRY_BYTES;
        RcObjectRange page = {
            .first = read_u32_le(raw),
            .count = read_u32_le(raw + 4),
        };
        if (page.first != expected_first
                || page.count > header[2] - expected_first) {
            free(raw_index);
            object_placement_store_free(store);
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
        object_placement_store_free(store);
        return NULL;
    }
    store->total_rows = header[2];
    store->occupied_pages = header[4];
    memcpy(store->source_plane_counts, &header[5],
           sizeof(store->source_plane_counts));
    return store;
}

static int parse_placement_page(const unsigned char *raw, uint32_t count,
                                uint16_t mapsquare,
                                RcObjectPlacement *rows) {
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *src = raw + (size_t)i * OPLI_RECORD_BYTES;
        RcObjectPlacement row = {
            .obj_id = read_u32_le(src),
            .key = read_u64_le(src + 4),
            .x = read_u16_le(src + 12),
            .y = read_u16_le(src + 14),
            .mapsquare = read_u16_le(src + 16),
            .plane = src[18],
            .type = src[19],
            .rotation = src[20],
            .flags = src[21],
        };
        uint16_t coordinate_mapsquare;
        if (row.key == 0
                || !rc_world_to_mapsquare(row.x, row.y,
                                          &coordinate_mapsquare, NULL, NULL)
                || row.mapsquare != mapsquare
                || coordinate_mapsquare != mapsquare
                || !rc_plane_valid(row.plane)
                || row.type >= 64 || row.rotation >= 4) {
            return 0;
        }
        rows[i] = row;
    }
    return 1;
}

static RcObjectPlacementPage *cached_placement_page(
    RcObjectPlacementStore *store, uint16_t mapsquare) {
    uint32_t slot = store->cache_slots[mapsquare];
    if (slot >= store->cache_capacity) return NULL;
    RcObjectPlacementPage *page = &store->pages[slot];
    return page->loaded && page->mapsquare == mapsquare ? page : NULL;
}

static RcObjectPlacementPage *placement_cache_slot(
    RcObjectPlacementStore *store) {
    RcObjectPlacementPage *oldest = &store->pages[0];
    for (uint32_t i = 0; i < store->cache_capacity; i++) {
        RcObjectPlacementPage *page = &store->pages[i];
        if (!page->loaded) return page;
        if (page->last_used < oldest->last_used) oldest = page;
    }
    return oldest;
}

static int object_placement_page_get(RcObjectPlacementStore *store,
                                     RcAssetReader *reader,
                                     uint16_t mapsquare,
                                     RcObjectPlacementPage **out,
                                     int *loaded) {
    if (out) *out = NULL;
    if (loaded) *loaded = 0;
    if (!store) return 0;
    RcObjectRange source = store->index[mapsquare];
    if (source.count == 0) return 1;

    RcObjectPlacementPage *page = cached_placement_page(store, mapsquare);
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
    size_t raw_size = (size_t)source.count * OPLI_RECORD_BYTES;
    unsigned char *raw = malloc(raw_size);
    RcObjectPlacement *rows = malloc(
        (size_t)source.count * sizeof(*rows));
    uint64_t records_offset = OPLI_HEADER_BYTES
        + (uint64_t)RC_MAX_OBJECT_ID * OPLI_INDEX_ENTRY_BYTES;
    int ok = raw && rows
        && rc_asset_reader_read_at(
            reader, records_offset + (uint64_t)source.first * OPLI_RECORD_BYTES,
            raw, raw_size)
        && parse_placement_page(raw, source.count, mapsquare, rows);
    free(raw);
    if (close_reader) rc_asset_reader_close(reader);
    if (!ok) {
        free(rows);
        return 0;
    }

    page = placement_cache_slot(store);
    if (page->loaded) {
        store->cache_slots[page->mapsquare] = UINT32_MAX;
        store->resident_rows -= page->count;
        free(page->rows);
    } else {
        store->resident_pages++;
    }
    *page = (RcObjectPlacementPage){
        .rows = rows,
        .count = source.count,
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

int rc_load_object_placements_into(const char *path, RcObjectData *data) {
    if (!path || !data) return -1;
    RcObjectPlacementStore *store = object_placement_store_load(path);
    if (!store) return -1;
    free(data->placements);
    object_placement_store_free(data->placement_store);
    data->placements = NULL;
    data->placement_store = store;
    data->placement_count = (int)store->total_rows;
    reset_object_range_index(data->region_index);
    return data->placement_count;
}

int rc_object_data_import_globals(RcObjectData *data) {
    if (!data) return 0;
    memcpy(data->defs, g_rc_object_defs, sizeof(data->defs));
    memcpy(data->behaviors, g_rc_object_behaviors, sizeof(data->behaviors));
    data->def_count = g_rc_object_def_count;
    data->behavior_count = g_rc_object_behavior_count;
    data->param_count = g_rc_object_param_count;
    data->transform_count = g_rc_object_transform_count;
    data->placement_count = g_rc_object_placements
        ? g_rc_object_placement_count
        : g_global_placement_store
            ? (int)g_global_placement_store->total_rows : 0;
    memcpy(data->region_index, g_region_index, sizeof(data->region_index));

    if (data->param_count > 0) {
        if (!g_rc_object_params) return 0;
        data->params = malloc((size_t)data->param_count * sizeof(*data->params));
        if (!data->params) return 0;
        memcpy(data->params, g_rc_object_params,
               (size_t)data->param_count * sizeof(*data->params));
    }
    if (data->transform_count > 0) {
        if (!g_rc_object_transforms) return 0;
        data->transforms = malloc(
            (size_t)data->transform_count * sizeof(*data->transforms));
        if (!data->transforms) return 0;
        memcpy(data->transforms, g_rc_object_transforms,
               (size_t)data->transform_count * sizeof(*data->transforms));
    }
    if (data->placement_count > 0 && g_rc_object_placements) {
        data->placements =
            malloc((size_t)data->placement_count * sizeof(*data->placements));
        if (!data->placements) return 0;
        memcpy(data->placements, g_rc_object_placements,
               (size_t)data->placement_count * sizeof(*data->placements));
    } else if (data->placement_count > 0 && g_global_placement_store) {
        data->placement_store =
            object_placement_store_clone(g_global_placement_store);
        if (!data->placement_store) return 0;
    }
    return 1;
}

static int mirror_object_defs_to_globals(const RcObjectData *data) {
    if (!data) return 0;
    RcObjectParam *params = NULL;
    int32_t *transforms = NULL;
    if (data->param_count > 0) {
        if (!data->params) return 0;
        params = malloc((size_t)data->param_count * sizeof(*params));
        if (!params) return 0;
        memcpy(params, data->params,
               (size_t)data->param_count * sizeof(*params));
    }
    if (data->transform_count > 0) {
        if (!data->transforms) {
            free(params);
            return 0;
        }
        transforms = malloc(
            (size_t)data->transform_count * sizeof(*transforms));
        if (!transforms) {
            free(params);
            return 0;
        }
        memcpy(transforms, data->transforms,
               (size_t)data->transform_count * sizeof(*transforms));
    }
    memcpy(g_rc_object_defs, data->defs, sizeof(g_rc_object_defs));
    free(g_rc_object_params);
    free(g_rc_object_transforms);
    g_rc_object_params = params;
    g_rc_object_transforms = transforms;
    g_rc_object_def_count = data->def_count;
    g_rc_object_param_count = data->param_count;
    g_rc_object_transform_count = data->transform_count;
    return 1;
}

static int mirror_object_behaviors_to_globals(const RcObjectData *data) {
    if (!data) return 0;
    memcpy(g_rc_object_behaviors, data->behaviors,
           sizeof(g_rc_object_behaviors));
    g_rc_object_behavior_count = data->behavior_count;
    return 1;
}

static int mirror_object_placements_to_globals(const RcObjectData *data) {
    if (!data) return 0;
    if (data->placement_store) {
        RcObjectPlacementStore *store =
            object_placement_store_clone(data->placement_store);
        if (!store) return 0;
        free(g_rc_object_placements);
        object_placement_store_free(g_global_placement_store);
        g_rc_object_placements = NULL;
        g_global_placement_store = store;
        g_rc_object_placement_count = data->placement_count;
        reset_object_range_index(g_region_index);
        return 1;
    }
    if (data->placement_count > 0 && !data->placements)
        return g_global_placement_store != NULL;
    RcObjectPlacement *rows = NULL;
    if (data->placement_count > 0) {
        if (!data->placements) return 0;
        rows = malloc((size_t)data->placement_count * sizeof(*rows));
        if (!rows) return 0;
        memcpy(rows, data->placements,
               (size_t)data->placement_count * sizeof(*rows));
    }
    free(g_rc_object_placements);
    object_placement_store_free(g_global_placement_store);
    g_rc_object_placements = rows;
    g_global_placement_store = NULL;
    g_rc_object_placement_count = data->placement_count;
    memcpy(g_region_index, data->region_index, sizeof(g_region_index));
    return 1;
}

int rc_objects_mirror_to_globals(const RcObjectData *data) {
    if (!mirror_object_defs_to_globals(data)) return 0;
    if (!mirror_object_behaviors_to_globals(data)) return 0;
    if (!mirror_object_placements_to_globals(data)) return 0;
    return 1;
}

int rc_load_object_defs(const char *path) {
    RcObjectData *data = calloc(1, sizeof(*data));
    if (!data) return -1;
    rc_object_data_init(data);
    int loaded = rc_load_object_defs_into(path, data);
    if (loaded >= 0 && !mirror_object_defs_to_globals(data)) loaded = -1;
    rc_object_data_free(data);
    free(data);
    if (loaded >= 0) rc_objects_use_data(NULL);
    return loaded;
}

int rc_load_object_behaviors(const char *path) {
    RcObjectData *data = calloc(1, sizeof(*data));
    if (!data) return -1;
    rc_object_data_init(data);
    int loaded = rc_load_object_behaviors_into(path, data);
    if (loaded >= 0 && !mirror_object_behaviors_to_globals(data)) loaded = -1;
    rc_object_data_free(data);
    free(data);
    if (loaded >= 0) rc_objects_use_data(NULL);
    return loaded;
}

int rc_load_object_placements(const char *path) {
    RcObjectPlacementStore *store = object_placement_store_load(path);
    if (!store) return -1;
    free(g_rc_object_placements);
    object_placement_store_free(g_global_placement_store);
    g_rc_object_placements = NULL;
    g_global_placement_store = store;
    g_rc_object_placement_count = (int)store->total_rows;
    reset_object_range_index(g_region_index);
    rc_objects_use_data(NULL);
    return g_rc_object_placement_count;
}

const RcObjectDef *rc_object_def_get(int obj_id) {
    if (obj_id < 0 || obj_id >= RC_MAX_OBJECT_ID) return NULL;
    const RcObjectDef *defs = g_active_object_defs ? g_active_object_defs
                                                   : g_rc_object_defs;
    const RcObjectDef *def = &defs[obj_id];
    if (def->loaded) return def;
    return NULL;
}

int rc_object_def_transform_at(const RcObjectDef *def, int index,
                               int *out_obj_id) {
    if (!def || !out_obj_id || index < 0 || index >= def->transform_count)
        return 0;
    uint64_t offset = (uint64_t)def->transform_offset + (uint32_t)index;
    if (!g_active_object_transforms
            || offset >= (uint64_t)g_active_object_transform_count) {
        return 0;
    }
    *out_obj_id = g_active_object_transforms[offset];
    return 1;
}

int rc_object_def_param_int(int obj_id, int key, int default_value) {
    const RcObjectDef *def = rc_object_def_get(obj_id);
    const RcObjectParam *params = g_active_object_params;
    int param_count = g_active_object_param_count;
    if (def == &g_rc_object_defs[obj_id]) {
        params = g_rc_object_params;
        param_count = g_rc_object_param_count;
    }
    if (!def || key < 0 || !params) return default_value;
    uint32_t first = def->param_first;
    uint32_t end = first + def->param_count;
    if (first >= (uint32_t)param_count
            || end > (uint32_t)param_count
            || end < first) {
        return default_value;
    }
    for (uint32_t i = first; i < end; i++) {
        const RcObjectParam *param = &params[i];
        if ((int)param->key == key) return param->value;
    }
    return default_value;
}

const RcObjectBehavior *rc_object_behavior_get(int obj_id) {
    if (obj_id < 0 || obj_id >= RC_MAX_OBJECT_ID) return NULL;
    const RcObjectBehavior *behaviors = g_active_object_behaviors
                                      ? g_active_object_behaviors
                                      : g_rc_object_behaviors;
    const RcObjectBehavior *behavior = &behaviors[obj_id];
    if (behavior->loaded) return behavior;
    return NULL;
}

static RcObjectPlacementStore *active_placement_store(void) {
    if (g_active_object_data && g_active_object_data->placement_store)
        return g_active_object_data->placement_store;
    return g_global_placement_store;
}

int rc_object_placements_set_cache_limit(int max_pages) {
    RcObjectPlacementStore *store = active_placement_store();
    if (!store) return 0;
    if (max_pages <= 0) return -1;
    if (max_pages > RC_MAX_OBJECT_ID) max_pages = RC_MAX_OBJECT_ID;
    if ((uint32_t)max_pages == store->cache_capacity) return 0;
    RcObjectPlacementPage *pages = calloc(
        (size_t)max_pages, sizeof(*pages));
    if (!pages) return -1;
    for (uint32_t i = 0; i < store->cache_capacity; i++)
        free(store->pages[i].rows);
    free(store->pages);
    store->pages = pages;
    memset(store->cache_slots, 0xff,
           (size_t)RC_MAX_OBJECT_ID * sizeof(*store->cache_slots));
    store->cache_capacity = (uint32_t)max_pages;
    store->resident_pages = 0;
    store->resident_rows = 0;
    store->use_clock = 0;
    return 1;
}

const RcObjectPlacement *rc_object_region_placements(uint16_t mapsquare,
                                                     int *count) {
    RcObjectPlacementStore *store = active_placement_store();
    if (store) {
        RcObjectPlacementPage *page = NULL;
        if (!object_placement_page_get(store, NULL, mapsquare, &page, NULL)) {
            if (count) *count = 0;
            return NULL;
        }
        if (count) *count = page ? (int)page->count : 0;
        return page ? page->rows : NULL;
    }
    const RcObjectRange *index = g_active_region_index ? g_active_region_index
                                                       : g_region_index;
    const RcObjectPlacement *placements = g_active_object_placements
                                        ? g_active_object_placements
                                        : g_rc_object_placements;
    RcObjectRange idx = index[mapsquare];
    if (count) *count = idx.first == UINT32_MAX ? 0 : (int)idx.count;
    if (idx.first == UINT32_MAX || !placements) return NULL;
    return &placements[idx.first];
}

int rc_object_placement_count(void) {
    if (g_active_object_data) return g_active_object_data->placement_count;
    if (g_global_placement_store)
        return (int)g_global_placement_store->total_rows;
    return g_rc_object_placements ? g_rc_object_placement_count : 0;
}

int rc_object_has_placements(void) {
    return rc_object_placement_count() > 0;
}

static int placement_mapsquare_bounds(int min_x, int min_y,
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

int rc_object_placements_prefetch_rect(int min_x, int min_y,
                                       int max_x, int max_y,
                                       RcObjectPlacementLoadStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    RcObjectPlacementStore *store = active_placement_store();
    if (!store) return 0;
    if (stats) {
        stats->total_rows = store->total_rows;
        stats->occupied_pages = store->occupied_pages;
    }

    int min_rx, min_ry, max_rx, max_ry;
    int bounds = placement_mapsquare_bounds(
        min_x, min_y, max_x, max_y, &min_rx, &min_ry, &max_rx, &max_ry);
    if (bounds < 0) return -1;
    if (bounds == 0) {
        if (stats) {
            stats->pages_resident = store->resident_pages;
            stats->rows_resident = store->resident_rows;
        }
        return 0;
    }

    uint32_t missing_pages = 0;
    for (int rx = min_rx; rx <= max_rx; rx++) {
        for (int ry = min_ry; ry <= max_ry; ry++) {
            uint16_t mapsquare;
            if (!rc_mapsquare_key(rx, ry, &mapsquare)) return -1;
            if (store->index[mapsquare].count == 0) continue;
            if (stats) stats->pages_requested++;
            if (!cached_placement_page(store, mapsquare)) missing_pages++;
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
            RcObjectPlacementPage *page = NULL;
            int loaded = 0;
            if (!object_placement_page_get(store, reader, mapsquare,
                                           &page, &loaded)) {
                result = -1;
                break;
            }
            if (loaded && stats) {
                stats->pages_loaded++;
                stats->rows_loaded += page->count;
            }
        }
    }
    if (reader) rc_asset_reader_close(reader);
    if (stats) {
        stats->pages_resident = store->resident_pages;
        stats->rows_resident = store->resident_rows;
    }
    return result < 0 ? -1 : (int)(stats ? stats->pages_loaded
                                         : missing_pages);
}

int rc_object_placements_at(int x, int y, int plane,
                            RcObjectPlacement *out, int max_out) {
    uint16_t ms;
    if (!rc_world_tile_valid(x, y, plane)
            || !rc_world_to_mapsquare(x, y, &ms, NULL, NULL)
            || !out || max_out <= 0) return 0;
    int count = 0;
    const RcObjectPlacement *rows = rc_object_region_placements(ms, &count);
    int n = 0;
    for (int i = 0; rows && i < count && n < max_out; i++) {
        if (rows[i].x == x && rows[i].y == y && rows[i].plane == plane) {
            out[n++] = rows[i];
        }
    }
    return n;
}

int rc_object_layer_for_type(int type) {
    if (type >= 0 && type <= 3) return 0;
    if (type >= 4 && type <= 8) return 1;
    if (type == 22) return 3;
    return type >= 0 ? 2 : -1;
}

int rc_object_placement_find_key(uint64_t key, int x, int y, int plane,
                                 RcObjectPlacement *out) {
    uint16_t mapsquare;
    if (!key || !out || !rc_world_tile_valid(x, y, plane)
            || !rc_world_to_mapsquare(x, y, &mapsquare, NULL, NULL)) {
        return 0;
    }
    int count = 0;
    const RcObjectPlacement *rows = rc_object_region_placements(
        mapsquare, &count);
    for (int i = 0; rows && i < count; i++) {
        if (rows[i].key == key && rows[i].x == x && rows[i].y == y
                && rows[i].plane == plane) {
            *out = rows[i];
            return 1;
        }
    }
    return 0;
}

int rc_object_placement_find_layer(int x, int y, int plane, int layer,
                                   RcObjectPlacement *out) {
    uint16_t mapsquare;
    if (!out || layer < 0 || layer > 3
            || !rc_world_tile_valid(x, y, plane)
            || !rc_world_to_mapsquare(x, y, &mapsquare, NULL, NULL)) {
        return 0;
    }
    int count = 0;
    int matches = 0;
    const RcObjectPlacement *rows = rc_object_region_placements(
        mapsquare, &count);
    for (int i = 0; rows && i < count; i++) {
        if (rows[i].x != x || rows[i].y != y || rows[i].plane != plane
                || rc_object_layer_for_type(rows[i].type) != layer) {
            continue;
        }
        *out = rows[i];
        matches++;
    }
    return matches == 1 ? 1 : matches == 0 ? 0 : -1;
}
