#ifndef RUNEC_VIEWER_WORLD_TRANSFORM_H
#define RUNEC_VIEWER_WORLD_TRANSFORM_H

#include "../rc-core/coordinates.h"

typedef struct ViewerWorldTransform {
    int origin_x;
    int origin_y;
} ViewerWorldTransform;

int viewer_world_transform_set(ViewerWorldTransform *transform,
                               int origin_x, int origin_y);
int viewer_world_to_local_tile(const ViewerWorldTransform *transform,
                               int world_x, int world_y,
                               int *local_x, int *local_y);
int viewer_local_to_world_tile(const ViewerWorldTransform *transform,
                               int local_x, int local_y,
                               int *world_x, int *world_y);
int viewer_world_transform_rebase_delta(
    const ViewerWorldTransform *current,
    const ViewerWorldTransform *target,
    int *delta_x, int *delta_y);

static inline int viewer_world_local_x(
    const ViewerWorldTransform *transform, int world_x) {
    return world_x - transform->origin_x;
}

static inline int viewer_world_local_y(
    const ViewerWorldTransform *transform, int world_y) {
    return world_y - transform->origin_y;
}

#endif
