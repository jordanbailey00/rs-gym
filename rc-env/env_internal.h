// Private state shared by rc-env/env.c (lifecycle, tasks, rewards, replays)
// and rc-env/view.c (what agents see and can do).
#ifndef RC_ENV_INTERNAL_H
#define RC_ENV_INTERNAL_H

#include "env.h"

#include "../rc-core/layout.h"
#include "../rc-core/types.h"

#include <stdio.h>

// An object agents can use: tree, rock, range, furnace, anvil, bank booth,
// stairs/ladder/trapdoor/lever, altar. Indexed once per env from the gym's
// placements; fires players light are found among the world's object states.
typedef enum {
    RC_INTERACT_KIND_TREE = 0,
    RC_INTERACT_KIND_ROCK,
    RC_INTERACT_KIND_COOK,
    RC_INTERACT_KIND_FURNACE,
    RC_INTERACT_KIND_ANVIL,
    RC_INTERACT_KIND_BANK,
    RC_INTERACT_KIND_TRAVERSAL,
    RC_INTERACT_KIND_ALTAR,
    RC_INTERACT_KIND_COUNT,
} RcInteractKind;

typedef struct {
    int obj_id;
    int x, y, plane;
    uint64_t key;
    uint8_t kind;      // RcInteractKind
    uint8_t ops;       // bit per supported option (0-2)
} RcInteractable;

// What an agent saw at its last observation; actions index into these lists.
typedef struct {
    int npc_uid[RC_OBS_NEAR_NPCS];
    int npc_count;
    RcInteractable objects[RC_OBS_NEAR_OBJECTS];
    int object_count;
    int ground[RC_OBS_NEAR_GROUND];      // ground item index
    int ground_uid[RC_OBS_NEAR_GROUND];
    int ground_count;
    int players[RC_OBS_NEAR_PLAYERS];    // player slot
    int player_count;
    int bank_slot[RC_OBS_BANK_SLOTS];    // first occupied bank slots
    int bank_count;
} RcAgentView;

// Running totals used for rewards and episode summaries.
typedef struct {
    int xp[32];
    int level_total;
    int64_t net_worth;
    int hp;
    bool dead;
    int boss_uid;
    int boss_damage;
    float episode_return;
    int episode_length;
    float kills, deaths, boss_kills;
    int start_xp[32];
    int start_levels;
    int64_t start_net_worth;
} RcAgentTrack;

struct RcEnv {
    struct RcWorld *world;
    RcEnvConfig cfg;
    const RcTask *task;
    int episode_ticks;
    uint32_t seed;
    RcGymLayout layout;
    RcInteractable *objects;
    int object_count;
    int pending_action[RC_ENV_MAX_AGENTS];
    float reward[RC_ENV_MAX_AGENTS];
    uint8_t terminal[RC_ENV_MAX_AGENTS];
    RcAgentTrack track[RC_ENV_MAX_AGENTS];
    RcEnvEpisodeStats stats[RC_ENV_MAX_AGENTS];
    bool stats_ready[RC_ENV_MAX_AGENTS];
    RcAgentView view[RC_ENV_MAX_AGENTS];
    bool view_fresh;
    FILE *record;
    uint32_t recorded_ticks;
};

// rc-env/travel.c
void rc_env_build_travel(const RcGymLayout *layout, const RcWorldMap *map);
int rc_env_travel_distance(int landmark, int x, int y, int plane);  // -1: unreachable
int rc_env_travel_waypoint(const RcWorldMap *map, int landmark, int x, int y,
                           int plane, int max_steps, int *wx, int *wy);

// rc-env/view.c
int rc_env_index_objects(RcEnv *env);
void rc_env_refresh_views(RcEnv *env);
void rc_env_apply_action(RcEnv *env, int agent, int action);
int rc_env_net_worth_item_value(int item_id);

#endif
