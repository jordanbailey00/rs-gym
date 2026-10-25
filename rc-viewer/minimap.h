#ifndef RUNEC_VIEWER_MINIMAP_H
#define RUNEC_VIEWER_MINIMAP_H

#include "raylib.h"

#define RUNEC_MINIMAP_DISPLAY_SIZE 152
#define RUNEC_MINIMAP_DISPLAY_CENTER 76.0f
#define RUNEC_MINIMAP_DISPLAY_RADIUS 75.0f
#define RUNEC_MINIMAP_DISPLAY_PIXELS_PER_TILE 3.5f
#define RUNEC_MINIMAP_MAPSQUARE_SIZE 64
#define RUNEC_MINIMAP_SOURCE_SIZE 512
#define RUNEC_MINIMAP_SOURCE_PIXELS_PER_TILE 4.0f
#define RUNEC_MINIMAP_SOURCE_BORDER_TILES 32.0f

typedef struct {
    int region_x;
    int region_y;
    int plane;
    const Color *pixels;
    int width;
    int height;
} RuneCMinimapChunk;

typedef struct {
    const RuneCMinimapChunk *chunks;
    int chunk_count;
    int plane;
} RuneCMinimapScene;

void runec_minimap_render(const RuneCMinimapScene *scene, float player_x,
                          float player_y, float camera_yaw, Color *output);
Vector2 runec_minimap_rotate_offset(float dx, float dy, float camera_yaw);
int runec_minimap_click_to_world(float map_x, float map_y, float player_x,
                                 float player_y, float camera_yaw,
                                 int *tile_x, int *tile_y);

#endif
