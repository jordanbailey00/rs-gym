// World config for headless gym tests: rc_gym_world_config for a mode, with its layout (start
// tile, respawn point, pinned area) from data/modes/<mode>/gym.cfg.
#ifndef GYM_TEST_CONFIG_H
#define GYM_TEST_CONFIG_H

#include "../rc-core/config.h"
#include "../rc-core/layout.h"

#include <stdio.h>
#include <sys/stat.h>

static RcGymLayout gym_layout;
#define GYM_START_X (gym_layout.start_x)
#define GYM_START_Y (gym_layout.start_y)

// 1 when tools/build_mode.py has built the mode.
static int gym_mode_built(const char *mode) {
    char path[RC_GYM_PATH_LEN];
    struct stat st;
    snprintf(path, sizeof(path), "data/modes/%s/regions/mapsquare.catalog", mode);
    return stat(path, &st) == 0;
}

static RcWorldConfig gym_test_config_mode(const char *mode, int players) {
    int have_layout = rc_gym_layout_load_mode(mode, &gym_layout);
    return rc_gym_world_config(have_layout ? &gym_layout : NULL, players);
}

#endif
