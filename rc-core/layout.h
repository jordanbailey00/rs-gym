#ifndef RC_LAYOUT_H
#define RC_LAYOUT_H

// Gym modes and their layout. tools/build_mode.py writes each mode's generated data to
// data/modes/<mode>/; gym.cfg there names the map, start and respawn tiles, the mapsquare rect
// the engine keeps loaded, and named landmarks.

#include "config.h"

#define RC_GYM_DEFAULT_MODE "compact_osrs_v1"
#define RC_GYM_MAX_LANDMARKS 32
#define RC_GYM_PATH_LEN 320

typedef struct {
    char name[24];
    int x, y, plane;
} RcGymLandmark;

typedef struct {
    char name[64];             // map name
    char mode[64];             // mode name
    char dir[RC_GYM_PATH_LEN]; // data/modes/<mode>
    int start_x, start_y, start_plane;
    int respawn_x, respawn_y, respawn_plane;
    int min_rx, min_ry, max_rx, max_ry;  // pinned active area, inclusive mapsquares
    RcGymLandmark landmarks[RC_GYM_MAX_LANDMARKS];  // named places, spec order
    int landmark_count;
    bool has_rules;            // the mode ships a rules file (mode.rules)
    // Data paths under dir; rc_gym_world_config points the config at these.
    char placements[RC_GYM_PATH_LEN], collision[RC_GYM_PATH_LEN], npc_spawns[RC_GYM_PATH_LEN],
         ground_items[RC_GYM_PATH_LEN], area_flags[RC_GYM_PATH_LEN], traversal[RC_GYM_PATH_LEN],
         nodes[RC_GYM_PATH_LEN], drops[RC_GYM_PATH_LEN], rules[RC_GYM_PATH_LEN],
         regions[RC_GYM_PATH_LEN], spawn_objects[RC_GYM_PATH_LEN];
} RcGymLayout;

// The mode to use when the caller names none: $RS_GYM_MODE, else RC_GYM_DEFAULT_MODE.
const char *rc_gym_mode_name(const char *mode);

// Loads data/modes/<mode>/gym.cfg (mode NULL: rc_gym_mode_name(NULL)); 1 on success.
int rc_gym_layout_load_mode(const char *mode, RcGymLayout *out);

// Pins the config's active area to the gym and sets its respawn point.
void rc_gym_layout_apply(const RcGymLayout *layout, RcWorldConfig *cfg);

// The gym's world config for `players` players: subsystems, shared definitions, the mode's data
// (paths into `layout`, which must outlive the config) and the layout applied. The viewer, tests
// and rc-env all start from it.
RcWorldConfig rc_gym_world_config(const RcGymLayout *layout, int players);

#endif
