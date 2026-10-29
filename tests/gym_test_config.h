// World config for headless gym tests: rc_gym_world_config with the gym layout (start tile,
// respawn point, pinned area) from data/regions/gym.cfg.
#ifndef GYM_TEST_CONFIG_H
#define GYM_TEST_CONFIG_H

#include "../rc-core/config.h"
#include "../rc-core/layout.h"

static RcGymLayout gym_layout;
#define GYM_START_X (gym_layout.start_x)
#define GYM_START_Y (gym_layout.start_y)

static RcWorldConfig gym_test_config(int players) {
    int have_layout = rc_gym_layout_load(RC_GYM_LAYOUT_PATH, &gym_layout);
    return rc_gym_world_config(have_layout ? &gym_layout : NULL, players);
}

#endif
