#ifndef RC_ENV_H
#define RC_ENV_H

// rc-env: the RL interface over an RS-gym world. One world holds 1-16 agents
// (players); a step is one game tick (600 ms) in which every agent acts. Each
// agent sees a flat float observation and picks one action from a flat
// discrete space with a validity mask. A task preset sets rewards, the
// starting kit and the episode length. Stepping is deterministic for a seed
// and an action sequence, which is what replays rely on.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RC_ENV_VERSION "v1"
#define RC_ENV_MAX_AGENTS 16

// ---- Observation (floats per agent) --------------------------------------
#define RC_OBS_ITEM 20         // per item: present, 16 categories, qty, value, tier
#define RC_OBS_NEAR_NPCS 8
#define RC_OBS_NPC 16
#define RC_OBS_NEAR_OBJECTS 8
#define RC_OBS_OBJECT 16
#define RC_OBS_NEAR_GROUND 6
#define RC_OBS_GROUND 13
#define RC_OBS_NEAR_PLAYERS 4
#define RC_OBS_PLAYER 8
#define RC_OBS_LANDMARKS 24
#define RC_OBS_MAP_RADIUS 5    // walkable tiles in an 11x11 square around the agent
#define RC_OBS_SHOP_SLOTS 32
#define RC_OBS_BANK_SLOTS 16
#define RC_OBS_SKILL_COUNT 13

enum {
    RC_OBS_SELF = 0,
    RC_OBS_SKILLS = RC_OBS_SELF + 48,
    RC_OBS_BONUSES = RC_OBS_SKILLS + RC_OBS_SKILL_COUNT * 3,
    RC_OBS_INVENTORY = RC_OBS_BONUSES + 14,
    RC_OBS_EQUIPMENT = RC_OBS_INVENTORY + 28 * RC_OBS_ITEM,
    RC_OBS_NPCS = RC_OBS_EQUIPMENT + 11 * 3,
    RC_OBS_OBJECTS = RC_OBS_NPCS + RC_OBS_NEAR_NPCS * RC_OBS_NPC,
    RC_OBS_GROUND_ITEMS = RC_OBS_OBJECTS + RC_OBS_NEAR_OBJECTS * RC_OBS_OBJECT,
    RC_OBS_PLAYERS = RC_OBS_GROUND_ITEMS + RC_OBS_NEAR_GROUND * RC_OBS_GROUND,
    RC_OBS_LANDMARK = RC_OBS_PLAYERS + RC_OBS_NEAR_PLAYERS * RC_OBS_PLAYER,
    RC_OBS_MAP = RC_OBS_LANDMARK + RC_OBS_LANDMARKS * 4,
    RC_OBS_SHOP = RC_OBS_MAP + (2 * RC_OBS_MAP_RADIUS + 1) * (2 * RC_OBS_MAP_RADIUS + 1),
    RC_OBS_BANK = RC_OBS_SHOP + RC_OBS_SHOP_SLOTS * 3 + 4,
    RC_OBS_TASK = RC_OBS_BANK + RC_OBS_BANK_SLOTS * 8,
    RC_ENV_OBS_SIZE = RC_OBS_TASK + 16,
};

// ---- Actions (one flat discrete space, masked) ----------------------------
#define RC_ACT_MOVE_DISTANCES 3   // 1, 5 and 15 tiles in each of 8 directions
#define RC_ACT_PRAYERS 8
#define RC_ACT_SPELLS 8

enum {
    RC_ACT_NOOP = 0,
    RC_ACT_MOVE = 1,                                        // dir * 3 + distance
    RC_ACT_TRAVEL = RC_ACT_MOVE + 8 * RC_ACT_MOVE_DISTANCES, // walk to landmark
    RC_ACT_ATTACK = RC_ACT_TRAVEL + RC_OBS_LANDMARKS,       // nearest NPC k
    RC_ACT_NPC_OP = RC_ACT_ATTACK + RC_OBS_NEAR_NPCS,       // NPC k, option 0 or 2
    RC_ACT_OBJECT_OP = RC_ACT_NPC_OP + RC_OBS_NEAR_NPCS * 2,  // object k, option 0-2
    RC_ACT_ITEM = RC_ACT_OBJECT_OP + RC_OBS_NEAR_OBJECTS * 3, // eat/drink/wield/wear/bury
    RC_ACT_DROP = RC_ACT_ITEM + 28,
    RC_ACT_LIGHT = RC_ACT_DROP + 28,                        // tinderbox on logs in slot
    RC_ACT_UNEQUIP = RC_ACT_LIGHT + 28,
    RC_ACT_PRAYER = RC_ACT_UNEQUIP + 11,                    // toggle
    RC_ACT_STYLE = RC_ACT_PRAYER + RC_ACT_PRAYERS,
    RC_ACT_RUN = RC_ACT_STYLE + 4,                          // toggle running
    RC_ACT_PICKUP = RC_ACT_RUN + 1,
    RC_ACT_MAKE = RC_ACT_PICKUP + RC_OBS_NEAR_GROUND,       // furnace/anvil product, make all
    RC_ACT_SHOP_BUY = RC_ACT_MAKE + 96,
    RC_ACT_SHOP_BUY10 = RC_ACT_SHOP_BUY + RC_OBS_SHOP_SLOTS,
    RC_ACT_SHOP_SELL = RC_ACT_SHOP_BUY10 + RC_OBS_SHOP_SLOTS, // all of the slot's item
    RC_ACT_DEPOSIT = RC_ACT_SHOP_SELL + 28,                 // all of the slot's item
    RC_ACT_DEPOSIT_ALL = RC_ACT_DEPOSIT + 28,
    RC_ACT_WITHDRAW = RC_ACT_DEPOSIT_ALL + 1,               // all of a bank slot
    RC_ACT_CLOSE = RC_ACT_WITHDRAW + RC_OBS_BANK_SLOTS,     // bank, shop or make menu
    RC_ACT_AUTOCAST = RC_ACT_CLOSE + 1,
    RC_ENV_NUM_ACTIONS = RC_ACT_AUTOCAST + RC_ACT_SPELLS,
};

// ---- Tasks ------------------------------------------------------------------
typedef enum {
    RC_TASK_TOTAL = 0,     // all-round: total level and XP everywhere
    RC_TASK_COMBAT,        // melee XP and kills
    RC_TASK_WOODCUTTING,
    RC_TASK_MINING,
    RC_TASK_FISHING,
    RC_TASK_COOKING,       // catch, light a fire, cook
    RC_TASK_SMITHING,      // mine, smelt, smith
    RC_TASK_WEALTH,        // grow net worth (coins plus item value)
    RC_TASK_KBD,           // kill the King Black Dragon
    RC_TASK_CHAOS_ELEMENTAL,
    RC_TASK_COUNT,
} RcTaskId;

typedef struct {
    const char *name;
    int episode_ticks;
    float xp[32];             // reward per XP point, by skill (RcSkill)
    float level;              // per level gained, any skill
    float net_worth;          // per coin of net worth gained
    float kill;               // per NPC killed (loot credit)
    float boss_damage;        // per hitpoint of damage to the task's boss
    float boss_kill;
    float damage_taken;       // per hitpoint lost (use a negative weight)
    float death;              // on death (negative)
    bool death_ends;          // death is terminal for that agent
    int kit;                  // starting items and stats (rc-env/env.c)
    int start_landmark;       // landmark index agents start at, -1: gym start
    int boss_npc;             // the task's boss NPC definition, or -1
} RcTask;

const RcTask *rc_env_task(int task);
int rc_env_task_find(const char *name);

// ---- Environment --------------------------------------------------------------
typedef struct {
    const char *mode;   // gym mode (data/modes/<mode>); NULL: $RS_GYM_MODE or the default
    int num_agents;     // players in the world, 1-16
    int task;           // RcTaskId
    int episode_ticks;  // 0: the task's default
    uint32_t seed;
    const char *record_path;  // write each episode's actions here (replay), or NULL
} RcEnvConfig;

// Per-agent episode summary, filled when the agent's episode ends.
typedef struct {
    float score;            // task metric (XP, coins, kills ... see rc-env/README.md)
    float episode_return;
    float episode_length;
    float xp_gained;
    float levels_gained;
    float net_worth_gained;
    float kills;
    float deaths;
    float boss_kills;
} RcEnvEpisodeStats;

typedef struct RcEnv RcEnv;

// Loads the gym's game data once per process; every env shares it.
RcEnv *rc_env_create(const RcEnvConfig *cfg);
void rc_env_destroy(RcEnv *env);
int rc_env_num_agents(const RcEnv *env);
struct RcWorld *rc_env_world(RcEnv *env);

// Starts an episode from `seed`: the world, agents and RNG start over.
void rc_env_reset(RcEnv *env, uint32_t seed);
// Queues one action for `agent` this step (unset agents wait).
void rc_env_set_action(RcEnv *env, int agent, int action);
// Advances one tick: applies the actions, ticks the world, scores rewards.
// Returns 1 when the episode's time ran out (every agent is terminal; reset
// before stepping again).
int rc_env_step(RcEnv *env);

float rc_env_reward(const RcEnv *env, int agent);
int rc_env_terminal(const RcEnv *env, int agent);
// Fills the agent's summary after a terminal step; 0 when it has none.
int rc_env_episode_stats(const RcEnv *env, int agent, RcEnvEpisodeStats *out);
void rc_env_observe(RcEnv *env, int agent, float *obs);      // RC_ENV_OBS_SIZE
void rc_env_action_mask(RcEnv *env, int agent, uint8_t *mask); // RC_ENV_NUM_ACTIONS
// Short label for an action id ("travel bank", "attack 2" ...), for logs and tools.
const char *rc_env_action_name(int action, char *buf, int cap);

// Gives world->player the task's starting stats and items (the viewer's
// default player uses RC_TASK_TOTAL's kit).
void rc_env_give_task_kit(struct RcWorld *world, int task);

// Replay files (RcEnvConfig.record_path): a header, then one action per agent
// per tick. rc_env_replay_open reads the header and returns the file at the
// first tick, or NULL.
typedef struct {
    char mode[64];
    int num_agents, task, episode_ticks;
    uint32_t seed, ticks;
} RcReplayHeader;
FILE *rc_env_replay_open(const char *path, RcReplayHeader *out);
// Reads the next tick's actions (num_agents of them); 0 at the end.
int rc_env_replay_next(FILE *f, const RcReplayHeader *header, int *actions);

#endif
