#include "streaming.h"

#include "../rc-core/io.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

ViewerStreamingConfig viewer_streaming_config_default(void) {
    return (ViewerStreamingConfig){
        .scene_radius_regions = VIEWER_STREAMING_DEFAULT_SCENE_RADIUS,
        .preload_radius_regions = VIEWER_STREAMING_DEFAULT_PRELOAD_RADIUS,
        .max_gpu_chunks = VIEWER_STREAMING_DEFAULT_MAX_GPU_CHUNKS,
        .max_cpu_chunks = VIEWER_STREAMING_DEFAULT_MAX_CPU_CHUNKS,
        .upload_budget_mb_per_frame =
            VIEWER_STREAMING_DEFAULT_UPLOAD_BUDGET_MB,
    };
}

void viewer_streaming_config_sanitize(ViewerStreamingConfig *config) {
    if (!config) return;
    ViewerStreamingConfig defaults = viewer_streaming_config_default();
    if (config->scene_radius_regions < 0)
        config->scene_radius_regions = defaults.scene_radius_regions;
    if (config->preload_radius_regions < 0)
        config->preload_radius_regions = defaults.preload_radius_regions;
    if (config->max_gpu_chunks <= 0
            || config->max_gpu_chunks > VIEWER_STREAMING_CHUNK_CAPACITY) {
        config->max_gpu_chunks = defaults.max_gpu_chunks;
    }
    if (config->max_cpu_chunks <= 0
            || config->max_cpu_chunks > VIEWER_STREAMING_CHUNK_CAPACITY) {
        config->max_cpu_chunks = defaults.max_cpu_chunks;
    }
    if (config->upload_budget_mb_per_frame <= 0) {
        config->upload_budget_mb_per_frame =
            defaults.upload_budget_mb_per_frame;
    }
}

double viewer_streaming_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

void viewer_streaming_telemetry_record_load(ViewerStreamingTelemetry *telemetry,
                                            double load_ms,
                                            double cpu_decode_ms,
                                            double gpu_upload_ms) {
    if (!telemetry) return;
    if (load_ms < 0.0) load_ms = 0.0;
    if (cpu_decode_ms < 0.0) cpu_decode_ms = 0.0;
    if (gpu_upload_ms < 0.0) gpu_upload_ms = 0.0;
    telemetry->scene_load_count++;
    telemetry->scene_or_chunk_load_ms = load_ms;
    telemetry->scene_or_chunk_load_total_ms += load_ms;
    telemetry->cpu_decode_ms = cpu_decode_ms;
    telemetry->cpu_decode_total_ms += cpu_decode_ms;
    telemetry->gpu_upload_ms = gpu_upload_ms;
    telemetry->gpu_upload_total_ms += gpu_upload_ms;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

int viewer_streaming_plan_mapsquares(
    const ViewerStreamingConfig *config,
    int tile_x,
    int tile_y,
    ViewerMapsquareCoord *out,
    int capacity) {
    if (!config || !out || capacity <= 0
            || !rc_world_coord_valid(tile_x)
            || !rc_world_coord_valid(tile_y))
        return -1;

    int center_x = tile_x / VIEWER_STREAMING_MAPSQUARE_SIZE;
    int center_y = tile_y / VIEWER_STREAMING_MAPSQUARE_SIZE;
    if (center_x > VIEWER_STREAMING_MAX_MAPSQUARE_COORD
            || center_y > VIEWER_STREAMING_MAX_MAPSQUARE_COORD)
        return -1;

    int radius = max_int(config->scene_radius_regions,
                         config->preload_radius_regions);
    if (radius >= RC_MAPSQUARE_AXIS) return -1;
    int min_x = max_int(0, center_x - radius);
    int min_y = max_int(0, center_y - radius);
    int max_x = min_int(VIEWER_STREAMING_MAX_MAPSQUARE_COORD,
                        center_x + radius);
    int max_y = min_int(VIEWER_STREAMING_MAX_MAPSQUARE_COORD,
                        center_y + radius);
    int count = (max_x - min_x + 1) * (max_y - min_y + 1);
    int limit = min_int(capacity, config->max_cpu_chunks);
    limit = min_int(limit, config->max_gpu_chunks);
    if (count > limit)
        return -1;

    int written = 0;
    for (int distance = 0; distance <= radius; distance++) {
        for (int region_y = min_y; region_y <= max_y; region_y++) {
            for (int region_x = min_x; region_x <= max_x; region_x++) {
                int dx = region_x - center_x;
                int dy = region_y - center_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (max_int(dx, dy) != distance)
                    continue;
                out[written++] = (ViewerMapsquareCoord){region_x, region_y};
            }
        }
    }
    return written == count ? written : -1;
}

int viewer_streaming_mapsquare_in_plan(const ViewerMapsquareCoord *plan,
                                       int count, int region_x, int region_y) {
    if (!plan || count <= 0)
        return 0;
    for (int i = 0; i < count; i++) {
        if (plan[i].region_x == region_x && plan[i].region_y == region_y)
            return 1;
    }
    return 0;
}

static int mapsquare_catalog_index(int region_x, int region_y) {
    if (region_x < 0 || region_y < 0
            || region_x > VIEWER_STREAMING_MAX_MAPSQUARE_COORD
            || region_y > VIEWER_STREAMING_MAX_MAPSQUARE_COORD) {
        return -1;
    }
    return region_y * (VIEWER_STREAMING_MAX_MAPSQUARE_COORD + 1) + region_x;
}

int viewer_streaming_catalog_load(ViewerMapsquareCatalog *catalog,
                                  const char *path) {
    if (!catalog || !path || !path[0])
        return 0;
    memset(catalog, 0, sizeof(*catalog));
    FILE *file = rc_asset_fopen(path, "rb");
    if (!file)
        return 0;

    uint32_t magic = 0;
    uint32_t count = 0;
    int valid = rc_read_exact(file, &magic, sizeof(magic), 1, path,
                              "mapsquare catalog magic")
        && rc_read_exact(file, &count, sizeof(count), 1, path,
                         "mapsquare catalog count")
        && magic == VIEWER_STREAMING_MAPSQUARE_CATALOG_MAGIC
        && count <= 65536u;
    uint32_t previous_key = 0;
    for (uint32_t i = 0; valid && i < count; i++) {
        uint8_t coordinate[2] = {0};
        valid = rc_read_exact(file, coordinate, sizeof(coordinate), 1, path,
                              "mapsquare catalog entry");
        uint32_t key = ((uint32_t)coordinate[1] << 8) | coordinate[0];
        if (valid && i > 0 && key <= previous_key)
            valid = 0;
        if (valid) {
            int index = mapsquare_catalog_index(coordinate[0], coordinate[1]);
            catalog->present[index >> 3] |= (uint8_t)(1u << (index & 7));
            previous_key = key;
        }
    }
    if (valid && fgetc(file) != EOF)
        valid = 0;
    rc_asset_close(file);
    if (!valid) {
        memset(catalog, 0, sizeof(*catalog));
        fprintf(stderr, "viewer mapsquare: invalid catalog %s\n", path);
        return 0;
    }
    catalog->count = count;
    catalog->loaded = 1;
    return 1;
}

int viewer_streaming_catalog_contains(const ViewerMapsquareCatalog *catalog,
                                      int region_x, int region_y) {
    if (!catalog || !catalog->loaded)
        return 0;
    int index = mapsquare_catalog_index(region_x, region_y);
    return index >= 0
        && (catalog->present[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

int viewer_streaming_filter_mapsquares(
    const ViewerMapsquareCatalog *catalog,
    ViewerMapsquareCoord *plan,
    int count) {
    if (!plan || count < 0)
        return -1;
    if (!catalog || !catalog->loaded)
        return count;
    int written = 0;
    for (int i = 0; i < count; i++) {
        if (viewer_streaming_catalog_contains(
                catalog, plan[i].region_x, plan[i].region_y)) {
            plan[written++] = plan[i];
        }
    }
    return written;
}

int viewer_streaming_mapsquare_path(char *out, size_t capacity,
                                    const char *directory, int region_x,
                                    int region_y, int plane,
                                    const char *suffix) {
    if (!out || capacity == 0 || !directory || !directory[0] || !suffix
            || !suffix[0] || !rc_mapsquare_coord_valid(region_x)
            || !rc_mapsquare_coord_valid(region_y)
            || !rc_plane_valid(plane))
        return 0;
    int n = snprintf(out, capacity, "%s/%d_%d.p%d%s", directory,
                     region_x, region_y, plane, suffix);
    return n > 0 && (size_t)n < capacity;
}

size_t viewer_streaming_upload_budget_bytes(
    const ViewerStreamingConfig *config) {
    if (!config || config->upload_budget_mb_per_frame <= 0)
        return 0;
    size_t mb = (size_t)config->upload_budget_mb_per_frame;
    if (mb > SIZE_MAX / (1024u * 1024u))
        return SIZE_MAX;
    return mb * 1024u * 1024u;
}

int viewer_streaming_upload_budget_admit(size_t used_bytes,
                                         size_t upload_bytes,
                                         size_t budget_bytes) {
    if (upload_bytes == 0)
        return 1;
    if (budget_bytes == 0 || used_bytes > budget_bytes)
        return 0;
    if (upload_bytes <= budget_bytes - used_bytes)
        return 1;
    // An indivisible mesh larger than the budget is admitted alone.
    return used_bytes == 0;
}

int viewer_streaming_chunk_retained(const ViewerMapsquareCoord *plan,
                                    int plan_count, int region_x,
                                    int region_y, int chunk_plane,
                                    int scene_plane, int player_plane) {
    return (chunk_plane == scene_plane || chunk_plane == player_plane)
        && viewer_streaming_mapsquare_in_plan(
            plan, plan_count, region_x, region_y);
}

int viewer_streaming_same_window(int first_x, int first_y, int first_plane,
                                 int second_x, int second_y,
                                 int second_plane) {
    if (!rc_world_tile_valid(first_x, first_y, first_plane)
            || !rc_world_tile_valid(second_x, second_y, second_plane))
        return 0;
    return first_plane == second_plane
        && first_x / VIEWER_STREAMING_MAPSQUARE_SIZE
            == second_x / VIEWER_STREAMING_MAPSQUARE_SIZE
        && first_y / VIEWER_STREAMING_MAPSQUARE_SIZE
            == second_y / VIEWER_STREAMING_MAPSQUARE_SIZE;
}

static int direction(int value) {
    return (value > 0) - (value < 0);
}

int viewer_streaming_predict_prefetch_center(
    int active_region_x, int active_region_y,
    int player_x, int player_y, int target_x, int target_y,
    int edge_distance, int *center_x, int *center_y) {
    if (!center_x || !center_y
            || !rc_mapsquare_coord_valid(active_region_x)
            || !rc_mapsquare_coord_valid(active_region_y)
            || !rc_world_coord_valid(player_x)
            || !rc_world_coord_valid(player_y)
            || !rc_world_coord_valid(target_x)
            || !rc_world_coord_valid(target_y)
            || edge_distance < 0) {
        return 0;
    }
    if (edge_distance > VIEWER_STREAMING_MAPSQUARE_SIZE / 2)
        edge_distance = VIEWER_STREAMING_MAPSQUARE_SIZE / 2;

    int next_region_x = active_region_x;
    int next_region_y = active_region_y;
    int target_region_x = target_x / VIEWER_STREAMING_MAPSQUARE_SIZE;
    int target_region_y = target_y / VIEWER_STREAMING_MAPSQUARE_SIZE;
    int local_x = player_x
                - active_region_x * VIEWER_STREAMING_MAPSQUARE_SIZE;
    int local_y = player_y
                - active_region_y * VIEWER_STREAMING_MAPSQUARE_SIZE;

    if (target_region_x != active_region_x) {
        next_region_x += direction(target_region_x - active_region_x);
    } else if (target_x > player_x
            && local_x >= VIEWER_STREAMING_MAPSQUARE_SIZE - edge_distance) {
        next_region_x++;
    } else if (target_x < player_x && local_x < edge_distance) {
        next_region_x--;
    }

    if (target_region_y != active_region_y) {
        next_region_y += direction(target_region_y - active_region_y);
    } else if (target_y > player_y
            && local_y >= VIEWER_STREAMING_MAPSQUARE_SIZE - edge_distance) {
        next_region_y++;
    } else if (target_y < player_y && local_y < edge_distance) {
        next_region_y--;
    }

    if (next_region_x == active_region_x
            && next_region_y == active_region_y) {
        return 0;
    }
    if (!rc_mapsquare_coord_valid(next_region_x)
            || !rc_mapsquare_coord_valid(next_region_y))
        return 0;
    *center_x = next_region_x * VIEWER_STREAMING_MAPSQUARE_SIZE
              + VIEWER_STREAMING_MAPSQUARE_SIZE / 2;
    *center_y = next_region_y * VIEWER_STREAMING_MAPSQUARE_SIZE
              + VIEWER_STREAMING_MAPSQUARE_SIZE / 2;
    return 1;
}

void viewer_streaming_player_transition_begin(
    ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int source_x, int source_y, int source_plane,
    int destination_x, int destination_y, int destination_plane) {
    if (!transition)
        return;
    memset(transition, 0, sizeof(*transition));
    if (!rc_world_tile_valid(source_x, source_y, source_plane)
            || !rc_world_tile_valid(destination_x, destination_y,
                                    destination_plane)) {
        return;
    }
    *transition = (ViewerStreamingPlayerTransition){
        .active = 1,
        .traversal_generation = traversal_generation,
        .source_x = source_x,
        .source_y = source_y,
        .source_plane = source_plane,
        .destination_x = destination_x,
        .destination_y = destination_y,
        .destination_plane = destination_plane,
    };
}

int viewer_streaming_player_transition_source(
    const ViewerStreamingPlayerTransition *transition,
    int *x, int *y, int *plane) {
    if (!transition || !transition->active || !x || !y || !plane
            || !rc_world_tile_valid(transition->source_x,
                                    transition->source_y,
                                    transition->source_plane))
        return 0;
    *x = transition->source_x;
    *y = transition->source_y;
    *plane = transition->source_plane;
    return 1;
}

int viewer_streaming_player_transition_current(
    const ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int authoritative_x, int authoritative_y, int authoritative_plane) {
    return transition && transition->active
        && rc_world_tile_valid(transition->destination_x,
                               transition->destination_y,
                               transition->destination_plane)
        && (transition->traversal_generation == 0
            || transition->traversal_generation == traversal_generation)
        && transition->destination_x == authoritative_x
        && transition->destination_y == authoritative_y
        && transition->destination_plane == authoritative_plane;
}

int viewer_streaming_player_transition_commit(
    ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int authoritative_x, int authoritative_y, int authoritative_plane,
    int *x, int *y, int *plane) {
    if (!x || !y || !plane
            || !viewer_streaming_player_transition_current(
                transition, traversal_generation, authoritative_x,
                authoritative_y, authoritative_plane))
        return 0;
    *x = transition->destination_x;
    *y = transition->destination_y;
    *plane = transition->destination_plane;
    memset(transition, 0, sizeof(*transition));
    return 1;
}
