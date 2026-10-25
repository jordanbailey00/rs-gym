#include "minimap.h"

#include <math.h>
#include <stddef.h>

static const Color MINIMAP_BLANK = {0, 0, 0, 0};

Vector2 runec_minimap_rotate_offset(float dx, float dy, float camera_yaw) {
    float sine = sinf(camera_yaw);
    float cosine = cosf(camera_yaw);
    return (Vector2){
        dx * cosine + dy * sine,
        -dx * sine + dy * cosine,
    };
}

static const RuneCMinimapChunk *find_chunk(const RuneCMinimapScene *scene,
                                           int region_x, int region_y) {
    if (!scene || !scene->chunks)
        return NULL;
    for (int i = 0; i < scene->chunk_count; i++) {
        const RuneCMinimapChunk *chunk = &scene->chunks[i];
        if (chunk->region_x == region_x && chunk->region_y == region_y
                && chunk->plane == scene->plane && chunk->pixels
                && chunk->width == RUNEC_MINIMAP_SOURCE_SIZE
                && chunk->height == RUNEC_MINIMAP_SOURCE_SIZE) {
            return chunk;
        }
    }
    return NULL;
}

static Color sample_scene(const RuneCMinimapScene *scene, float world_x,
                          float world_y) {
    int region_x = (int)floorf(
        world_x / (float)RUNEC_MINIMAP_MAPSQUARE_SIZE);
    int region_y = (int)floorf(
        world_y / (float)RUNEC_MINIMAP_MAPSQUARE_SIZE);
    const RuneCMinimapChunk *chunk = find_chunk(scene, region_x, region_y);
    if (!chunk)
        return MINIMAP_BLANK;

    float local_x = world_x
        - (float)(region_x * RUNEC_MINIMAP_MAPSQUARE_SIZE);
    float local_y = world_y
        - (float)(region_y * RUNEC_MINIMAP_MAPSQUARE_SIZE);
    float source_x = (RUNEC_MINIMAP_SOURCE_BORDER_TILES + local_x)
                   * RUNEC_MINIMAP_SOURCE_PIXELS_PER_TILE;
    float source_y = (float)RUNEC_MINIMAP_SOURCE_SIZE
                   - (RUNEC_MINIMAP_SOURCE_BORDER_TILES + local_y)
                   * RUNEC_MINIMAP_SOURCE_PIXELS_PER_TILE;
    if (source_x < 0.0f || source_y < 0.0f
            || source_x >= (float)chunk->width
            || source_y >= (float)chunk->height) {
        return MINIMAP_BLANK;
    }
    int x = (int)floorf(source_x);
    int y = (int)floorf(source_y);
    return chunk->pixels[x + y * chunk->width];
}

void runec_minimap_render(const RuneCMinimapScene *scene, float player_x,
                          float player_y, float camera_yaw, Color *output) {
    if (!output)
        return;
    float sine = sinf(camera_yaw);
    float cosine = cosf(camera_yaw);
    float source_scale = 1.0f / RUNEC_MINIMAP_DISPLAY_PIXELS_PER_TILE;

    for (int y = 0; y < RUNEC_MINIMAP_DISPLAY_SIZE; y++) {
        for (int x = 0; x < RUNEC_MINIMAP_DISPLAY_SIZE; x++) {
            float screen_x = (float)x - RUNEC_MINIMAP_DISPLAY_CENTER;
            float screen_y = (float)y - RUNEC_MINIMAP_DISPLAY_CENTER;
            int index = x + y * RUNEC_MINIMAP_DISPLAY_SIZE;
            if (screen_x * screen_x + screen_y * screen_y
                    > RUNEC_MINIMAP_DISPLAY_RADIUS
                    * RUNEC_MINIMAP_DISPLAY_RADIUS) {
                output[index] = MINIMAP_BLANK;
                continue;
            }
            float dx = (screen_x * cosine + screen_y * sine) * source_scale;
            float dy = (-screen_x * sine + screen_y * cosine) * source_scale;
            output[index] = sample_scene(scene, player_x + dx, player_y - dy);
        }
    }
}

int runec_minimap_click_to_world(float map_x, float map_y, float player_x,
                                 float player_y, float camera_yaw,
                                 int *tile_x, int *tile_y) {
    if (!tile_x || !tile_y)
        return 0;
    float screen_x = map_x - RUNEC_MINIMAP_DISPLAY_CENTER;
    float screen_y = map_y - RUNEC_MINIMAP_DISPLAY_CENTER;
    if (screen_x * screen_x + screen_y * screen_y
            > RUNEC_MINIMAP_DISPLAY_RADIUS * RUNEC_MINIMAP_DISPLAY_RADIUS) {
        return 0;
    }

    float sine = sinf(camera_yaw);
    float cosine = cosf(camera_yaw);
    float dx = (screen_x * cosine + screen_y * sine)
             / RUNEC_MINIMAP_DISPLAY_PIXELS_PER_TILE;
    float dy = (screen_x * sine - screen_y * cosine)
             / RUNEC_MINIMAP_DISPLAY_PIXELS_PER_TILE;
    *tile_x = (int)floorf(player_x + dx);
    *tile_y = (int)floorf(player_y + dy);
    return 1;
}
