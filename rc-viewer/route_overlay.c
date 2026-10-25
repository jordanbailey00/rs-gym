#include "route_overlay.h"

static int direction(int delta) {
    return (delta > 0) - (delta < 0);
}

void runec_route_overlay_begin(RuneCRouteOverlayCursor *cursor,
                               int start_x, int start_y,
                               const int *waypoint_x,
                               const int *waypoint_y,
                               int waypoint_index, int waypoint_count) {
    if (!cursor) return;
    cursor->x = start_x;
    cursor->y = start_y;
    cursor->waypoint_x = waypoint_x;
    cursor->waypoint_y = waypoint_y;
    cursor->waypoint_index = waypoint_index < 0 ? 0 : waypoint_index;
    cursor->waypoint_count = waypoint_count < 0 ? 0 : waypoint_count;
}

int runec_route_overlay_next(RuneCRouteOverlayCursor *cursor,
                             int *out_x, int *out_y) {
    if (!cursor || !out_x || !out_y || !cursor->waypoint_x
            || !cursor->waypoint_y) {
        return 0;
    }
    while (cursor->waypoint_index < cursor->waypoint_count) {
        int target_x = cursor->waypoint_x[cursor->waypoint_index];
        int target_y = cursor->waypoint_y[cursor->waypoint_index];
        int dx = target_x - cursor->x;
        int dy = target_y - cursor->y;
        if (!dx && !dy) {
            cursor->waypoint_index++;
            continue;
        }
        cursor->x += direction(dx);
        cursor->y += direction(dy);
        *out_x = cursor->x;
        *out_y = cursor->y;
        return 1;
    }
    return 0;
}
