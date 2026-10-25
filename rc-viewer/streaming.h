#ifndef RUNEC_VIEWER_STREAMING_H
#define RUNEC_VIEWER_STREAMING_H

#include <stddef.h>
#include <stdint.h>

#include "../rc-core/coordinates.h"

enum {
    VIEWER_STREAMING_CHUNK_CAPACITY = 128,
    VIEWER_STREAMING_DEFAULT_SCENE_RADIUS = 1,
    VIEWER_STREAMING_DEFAULT_PRELOAD_RADIUS = 0,
    VIEWER_STREAMING_DEFAULT_MAX_GPU_CHUNKS = 128,
    VIEWER_STREAMING_DEFAULT_MAX_CPU_CHUNKS = 128,
    VIEWER_STREAMING_DEFAULT_UPLOAD_BUDGET_MB = 16,
    VIEWER_STREAMING_MAPSQUARE_SIZE = RC_MAPSQUARE_SIZE,
    VIEWER_STREAMING_MAX_MAPSQUARE_COORD = RC_MAPSQUARE_AXIS - 1,
    VIEWER_STREAMING_MAPSQUARE_CATALOG_BYTES = RC_MAPSQUARE_COUNT / 8,
};

#define VIEWER_STREAMING_MAPSQUARE_CATALOG_MAGIC 0x3151534du

typedef struct {
    int region_x;
    int region_y;
} ViewerMapsquareCoord;

typedef struct {
    uint8_t present[VIEWER_STREAMING_MAPSQUARE_CATALOG_BYTES];
    uint32_t count;
    int loaded;
} ViewerMapsquareCatalog;

typedef struct {
    int scene_radius_regions;
    int preload_radius_regions;
    int max_gpu_chunks;
    int max_cpu_chunks;
    int upload_budget_mb_per_frame;
} ViewerStreamingConfig;

typedef struct {
    int active;
    uint64_t traversal_generation;
    int source_x, source_y, source_plane;
    int destination_x, destination_y, destination_plane;
} ViewerStreamingPlayerTransition;

typedef enum {
    VIEWER_STREAMING_REQUEST_ACTIVATE = 0,
    VIEWER_STREAMING_REQUEST_MOVEMENT_PREFETCH,
    VIEWER_STREAMING_REQUEST_TRANSPORT_PREFETCH,
    VIEWER_STREAMING_REQUEST_PLANE,
} ViewerStreamingRequestKind;

typedef struct {
    uint64_t scene_load_count;
    double startup_ms;
    double scene_or_chunk_load_ms;
    double scene_or_chunk_load_total_ms;
    double cpu_decode_ms;
    double cpu_decode_total_ms;
    double gpu_upload_ms;
    double gpu_upload_total_ms;
    int terrain_chunks_cpu;
    int terrain_chunks_gpu;
    int object_chunks_cpu;
    int object_chunks_gpu;
    int async_jobs_pending;
    int cpu_chunks_staged;
    uint64_t gpu_upload_bytes_frame;
    uint64_t gpu_upload_bytes_peak;
    uint64_t gpu_upload_budget_bytes;
    uint64_t async_frames;
    double main_thread_streaming_work_max_ms;
    uint64_t terrain_vertices_resident;
    uint64_t object_vertices_resident;
    double texture_cache_mb;
    double model_cache_mb;
    int active_npcs;
    int active_ground_items;
    int backend_pages_loaded;
    double backend_page_load_ms;
    double backend_active_area_load_ms;
    int draw_calls_estimate;
} ViewerStreamingTelemetry;

ViewerStreamingConfig viewer_streaming_config_default(void);
void viewer_streaming_config_sanitize(ViewerStreamingConfig *config);
double viewer_streaming_now_ms(void);
void viewer_streaming_telemetry_record_load(ViewerStreamingTelemetry *telemetry,
                                            double load_ms,
                                            double cpu_decode_ms,
                                            double gpu_upload_ms);
int viewer_streaming_plan_mapsquares(
    const ViewerStreamingConfig *config,
    int tile_x,
    int tile_y,
    ViewerMapsquareCoord *out,
    int capacity);
int viewer_streaming_mapsquare_in_plan(const ViewerMapsquareCoord *plan,
                                       int count, int region_x, int region_y);
int viewer_streaming_catalog_load(ViewerMapsquareCatalog *catalog,
                                  const char *path);
int viewer_streaming_catalog_contains(const ViewerMapsquareCatalog *catalog,
                                      int region_x, int region_y);
int viewer_streaming_filter_mapsquares(
    const ViewerMapsquareCatalog *catalog,
    ViewerMapsquareCoord *plan,
    int count);
int viewer_streaming_mapsquare_path(char *out, size_t capacity,
                                    const char *directory, int region_x,
                                    int region_y, int plane,
                                    const char *suffix);
size_t viewer_streaming_upload_budget_bytes(
    const ViewerStreamingConfig *config);
int viewer_streaming_upload_budget_admit(size_t used_bytes,
                                         size_t upload_bytes,
                                         size_t budget_bytes);
int viewer_streaming_chunk_retained(const ViewerMapsquareCoord *plan,
                                    int plan_count, int region_x,
                                    int region_y, int chunk_plane,
                                    int scene_plane, int player_plane);
int viewer_streaming_same_window(int first_x, int first_y, int first_plane,
                                 int second_x, int second_y,
                                 int second_plane);
int viewer_streaming_predict_prefetch_center(
    int active_region_x, int active_region_y,
    int player_x, int player_y, int target_x, int target_y,
    int edge_distance, int *center_x, int *center_y);
void viewer_streaming_player_transition_begin(
    ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int source_x, int source_y, int source_plane,
    int destination_x, int destination_y, int destination_plane);
int viewer_streaming_player_transition_source(
    const ViewerStreamingPlayerTransition *transition,
    int *x, int *y, int *plane);
int viewer_streaming_player_transition_current(
    const ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int authoritative_x, int authoritative_y, int authoritative_plane);
int viewer_streaming_player_transition_commit(
    ViewerStreamingPlayerTransition *transition,
    uint64_t traversal_generation,
    int authoritative_x, int authoritative_y, int authoritative_plane,
    int *x, int *y, int *plane);

#endif
