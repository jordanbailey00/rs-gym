#ifndef RUNEC_VIEWER_ROUTE_OVERLAY_H
#define RUNEC_VIEWER_ROUTE_OVERLAY_H

typedef struct {
    int x;
    int y;
    const int *waypoint_x;
    const int *waypoint_y;
    int waypoint_index;
    int waypoint_count;
} RuneCRouteOverlayCursor;

void runec_route_overlay_begin(RuneCRouteOverlayCursor *cursor,
                               int start_x, int start_y,
                               const int *waypoint_x,
                               const int *waypoint_y,
                               int waypoint_index, int waypoint_count);
int runec_route_overlay_next(RuneCRouteOverlayCursor *cursor,
                             int *out_x, int *out_y);

#endif
