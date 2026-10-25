#include "world_transform.h"

#include <stdint.h>

int viewer_world_transform_set(ViewerWorldTransform *transform,
                               int origin_x, int origin_y) {
    if (!transform || !rc_world_coord_valid(origin_x)
            || !rc_world_coord_valid(origin_y)) {
        return 0;
    }
    *transform = (ViewerWorldTransform){origin_x, origin_y};
    return 1;
}

int viewer_world_to_local_tile(const ViewerWorldTransform *transform,
                               int world_x, int world_y,
                               int *local_x, int *local_y) {
    if (!transform || !local_x || !local_y
            || !rc_world_coord_valid(transform->origin_x)
            || !rc_world_coord_valid(transform->origin_y)
            || !rc_world_coord_valid(world_x)
            || !rc_world_coord_valid(world_y)) {
        return 0;
    }
    *local_x = world_x - transform->origin_x;
    *local_y = world_y - transform->origin_y;
    return 1;
}

int viewer_local_to_world_tile(const ViewerWorldTransform *transform,
                               int local_x, int local_y,
                               int *world_x, int *world_y) {
    if (!transform || !world_x || !world_y
            || !rc_world_coord_valid(transform->origin_x)
            || !rc_world_coord_valid(transform->origin_y)) {
        return 0;
    }
    int64_t x = (int64_t)transform->origin_x + local_x;
    int64_t y = (int64_t)transform->origin_y + local_y;
    if (x < 0 || x > RC_WORLD_MAX || y < 0 || y > RC_WORLD_MAX)
        return 0;
    *world_x = (int)x;
    *world_y = (int)y;
    return 1;
}

int viewer_world_transform_rebase_delta(
    const ViewerWorldTransform *current,
    const ViewerWorldTransform *target,
    int *delta_x, int *delta_y) {
    if (!current || !target || !delta_x || !delta_y
            || !rc_world_coord_valid(current->origin_x)
            || !rc_world_coord_valid(current->origin_y)
            || !rc_world_coord_valid(target->origin_x)
            || !rc_world_coord_valid(target->origin_y)) {
        return 0;
    }
    *delta_x = target->origin_x - current->origin_x;
    *delta_y = target->origin_y - current->origin_y;
    return 1;
}
