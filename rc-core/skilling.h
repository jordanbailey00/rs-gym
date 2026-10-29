#ifndef RC_SKILLING_H
#define RC_SKILLING_H

#include "types.h"

// Gym skilling: woodcutting and mining at gathering nodes, fishing at spots,
// firemaking, cooking, and furnace/anvil production. Content installs the
// rule tables (rc-content/skilling.c); each player runs at most one job and
// rolls the OSRS success curve once per attempt.
//
// Success chances are out of 256, interpolated from `low` at level 1 to
// `high` at level 99 (both in tenths, so 12.5 is 125), plus one, capped at
// 256 (always succeeds).

typedef struct {
    uint32_t item_id;
    uint8_t level;   // level in the tool's skill to use it
    uint8_t ticks;   // pickaxes: ticks between swings
    uint16_t scale;  // axes: success multiplier in hundredths
} RcSkillTool;

typedef struct {
    uint32_t obj_id, product;
    uint8_t skill, level;
    uint16_t xp_tenths, low, high;
    uint8_t deplete_one_in;  // a success depletes the node with chance 1/N
} RcGatherRule;

typedef struct {
    uint32_t product;
    uint8_t level;
    uint16_t xp_tenths, low, high;
} RcFishCatch;

#define RC_FISH_MAX_CATCHES 2

typedef struct {
    uint32_t npc_id;
    uint8_t option;       // NPC option index (0-4)
    uint8_t catch_count;
    uint32_t tool, bait;  // bait 0: nothing consumed
    RcFishCatch catches[RC_FISH_MAX_CATCHES];  // highest level first
} RcFishRule;

typedef struct {
    uint32_t logs;
    uint8_t level;
    uint16_t xp_tenths;
} RcFireRule;

typedef struct {
    uint32_t raw, cooked, burnt;
    uint8_t level;
    uint16_t xp_tenths, low, high;
} RcCookRule;

typedef struct RcSkillingRules {
    const RcSkillTool *axes, *pickaxes;  // best first
    int axe_count, pickaxe_count;
    const RcGatherRule *gather;
    int gather_count;
    const RcFishRule *fish;
    int fish_count;
    const RcFireRule *fires;
    int fire_count;
    const RcCookRule *cook;
    int cook_count;
    const uint32_t *cook_objects;  // fires and ranges with a Cook option
    int cook_object_count;
    uint32_t tinderbox, hammer, fire_obj_id;
    uint8_t woodcut_ticks, fish_ticks, fire_ticks, cook_ticks;
    uint16_t fire_low, fire_high;
    uint16_t fire_min_ticks, fire_max_ticks;  // how long a lit fire burns
} RcSkillingRules;

int rc_skill_chance_256(int level, int low_tenths, int high_tenths,
                        int scale_pct);

// Content installs its rules once per world, then adds the furnace/anvil
// products (indexes into the active recipes.bin rows).
void rc_skilling_install(RcWorld *world, const RcSkillingRules *rules);
int  rc_skilling_add_make_option(RcWorld *world, int recipe,
                                 RcMakeMenu facility, int success_pct);

// Job starts act for world->player. Each checks tool, level, inventory and
// target, publishes the OSRS message and returns 0 when one fails, or starts
// the job and returns 1. A running job is replaced.
int rc_skilling_start_gather(RcWorld *world, int obj_id, int x, int y,
                             int plane);
int rc_skilling_start_fish(RcWorld *world, int npc_uid, int option);
int rc_skilling_start_firemaking(RcWorld *world, int inv_slot);
int rc_skilling_start_cook(RcWorld *world, int obj_id, int x, int y,
                           int plane, uint64_t placement_key, int raw_item);
int rc_skilling_open_make(RcWorld *world, RcMakeMenu menu);
int rc_skilling_make(RcWorld *world, int option, int count);
void rc_skilling_close_make(RcPlayer *player);
void rc_skilling_stop(RcPlayer *player);

// Runs world->player's job when its attempt is due (per-player tick phase).
void rc_skilling_tick(RcWorld *world);

// Queries for content, the viewer and the agent interface.
int rc_skilling_make_option_ready(const RcWorld *world, int option);
const RcGatherRule *rc_skilling_gather_rule(const RcWorld *world, int obj_id);
const RcFishRule *rc_skilling_fish_rule(const RcWorld *world, int npc_id,
                                        int option);
const RcFireRule *rc_skilling_fire_rule(const RcWorld *world, int logs);
const RcCookRule *rc_skilling_cook_rule(const RcWorld *world, int raw_item);
int rc_skilling_is_cook_object(const RcWorld *world, int obj_id);
// Best tool the player can use for a gathering skill (inventory or wielded).
const RcSkillTool *rc_skilling_best_tool(const RcWorld *world,
                                         const RcPlayer *player, int skill);

#endif
