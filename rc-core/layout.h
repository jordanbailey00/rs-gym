#ifndef RC_LAYOUT_H
#define RC_LAYOUT_H

// Gym layout written by tools/build_gym.py (data/regions/gym.cfg): where players start and
// respawn, the mapsquare rect the engine keeps loaded, and named landmarks.

#include "config.h"

#define RC_GYM_MAX_LANDMARKS 24

typedef struct {
    char name[24];
    int x, y, plane;
} RcGymLandmark;

typedef struct {
    char name[64];
    int start_x, start_y, start_plane;
    int respawn_x, respawn_y, respawn_plane;
    int min_rx, min_ry, max_rx, max_ry;  // pinned active area, inclusive mapsquares
    RcGymLandmark landmarks[RC_GYM_MAX_LANDMARKS];  // named places, spec order
    int landmark_count;
} RcGymLayout;

#define RC_GYM_LAYOUT_PATH "data/regions/gym.cfg"

// Reads a layout file (relative paths resolve under the data root); 1 on success.
int rc_gym_layout_load(const char *path, RcGymLayout *out);

// Pins the config's active area to the gym and sets its respawn point.
void rc_gym_layout_apply(const RcGymLayout *layout, RcWorldConfig *cfg);

// The gym's world config for `players` players: subsystems and installed data
// (paths relative to the data root), with the layout applied when given. The
// viewer, tests and the agent interface all start from it.
RcWorldConfig rc_gym_world_config(const RcGymLayout *layout, int players);

#endif
