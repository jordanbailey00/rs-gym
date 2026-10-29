#ifndef RC_COMPACT_ENV_H
#define RC_COMPACT_ENV_H

// rc-env's interface to a gym mode with rules (compact_osrs_v1), schema "compact-v2"
// (rc-env/README.md documents every field). One world holds 1-8 agents and a step is one game
// tick: every agent's action is collected against the same pre-tick state, the world ticks once,
// and each agent observes the result. An agent sees its own state, a 25x25-tile window around it
// (terrain, NPCs, locs, ground items, other players, falling-brick shadows), the interface it has
// open (bank, shop, production menu, trade) and nothing else: no other player's inventory or
// bank, no remote stock, no future rolls. It picks one action from a flat discrete space with a
// validity mask built from that same information. A profile (wrapper) decides when the task ends
// and what earns reward; metrics and an optional event log record what happened. Snapshots
// restore exactly and state hashes check determinism.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RC_CENV_SCHEMA "compact-v2"
#define RC_CENV_MAX_AGENTS 8
#define RC_CENV_RADIUS 12          // tiles each way: a 25x25 window
#define RC_CENV_SIDE (2 * RC_CENV_RADIUS + 1)
#define RC_CENV_TILES (RC_CENV_SIDE * RC_CENV_SIDE)

// ---- Observation (floats per agent) ----------------------------------------------------------------
#define RC_CENV_ITEM 20            // item features: present, 14 categories, tier, quantity, value,
                                   // charges, vocabulary index
#define RC_CENV_SKILLS 15
#define RC_CENV_PRAYERS 18
#define RC_CENV_SPELLS 6
#define RC_CENV_NPCS 12
#define RC_CENV_NPC 28
#define RC_CENV_LOCS 12
#define RC_CENV_LOC 30
#define RC_CENV_GROUND 8
#define RC_CENV_GROUND_ITEM 24
#define RC_CENV_PLAYERS 7
#define RC_CENV_PLAYER 10
#define RC_CENV_HAZARDS 8
#define RC_CENV_LANDMARKS 32
#define RC_CENV_SHOP_SLOTS 64
#define RC_CENV_BANK_SLOTS 48
#define RC_CENV_MAKE_SLOTS 16
#define RC_CENV_OFFER_SLOTS 8
#define RC_CENV_TILE_CHANNELS 3    // blocked, falling-brick shadow, occupied

enum {
    RC_COBS_SELF = 0,
    RC_COBS_SKILLS = RC_COBS_SELF + 96,
    RC_COBS_INVENTORY = RC_COBS_SKILLS + RC_CENV_SKILLS * 3,
    RC_COBS_EQUIPMENT = RC_COBS_INVENTORY + 28 * RC_CENV_ITEM,
    RC_COBS_TILES = RC_COBS_EQUIPMENT + 11 * RC_CENV_ITEM,
    RC_COBS_NPCS = RC_COBS_TILES + RC_CENV_TILES * RC_CENV_TILE_CHANNELS,
    RC_COBS_LOCS = RC_COBS_NPCS + RC_CENV_NPCS * RC_CENV_NPC,
    RC_COBS_GROUND = RC_COBS_LOCS + RC_CENV_LOCS * RC_CENV_LOC,
    RC_COBS_PLAYERS = RC_COBS_GROUND + RC_CENV_GROUND * RC_CENV_GROUND_ITEM,
    RC_COBS_HAZARDS = RC_COBS_PLAYERS + RC_CENV_PLAYERS * RC_CENV_PLAYER,
    RC_COBS_LANDMARKS = RC_COBS_HAZARDS + RC_CENV_HAZARDS * 4,
    RC_COBS_SHOP = RC_COBS_LANDMARKS + RC_CENV_LANDMARKS * 4,
    RC_COBS_BANK = RC_COBS_SHOP + 4 + RC_CENV_SHOP_SLOTS * 5,
    RC_COBS_MAKE = RC_COBS_BANK + 2 + RC_CENV_BANK_SLOTS * 4,
    RC_COBS_TRADE = RC_COBS_MAKE + 12 + RC_CENV_MAKE_SLOTS * 4,
    RC_COBS_EPISODE = RC_COBS_TRADE + 10 + 2 * RC_CENV_OFFER_SLOTS * 4,
    RC_CENV_OBS_SIZE = RC_COBS_EPISODE + 16,
};

// ---- Actions (one flat discrete space, masked) -----------------------------------------------------
#define RC_CENV_COMBOS 32          // the mode's item-on-item pairs that open a menu or charge
enum {
    RC_CACT_WAIT = 0,                                         // keep whatever is under way
    RC_CACT_STOP,                                             // stop moving, fighting, making
    RC_CACT_RUN,                                              // toggle running
    RC_CACT_MOVE,                                             // + window tile (row-major)
    RC_CACT_TRAVEL = RC_CACT_MOVE + RC_CENV_TILES,            // + landmark (public map)
    RC_CACT_ATTACK = RC_CACT_TRAVEL + RC_CENV_LANDMARKS,       // + listed NPC
    RC_CACT_NPC_OP = RC_CACT_ATTACK + RC_CENV_NPCS,           // + NPC * 2 + its 1st/2nd other option
    RC_CACT_LOC_OP = RC_CACT_NPC_OP + RC_CENV_NPCS * 2,       // + loc * 3 + its 1st-3rd option
    RC_CACT_ITEM = RC_CACT_LOC_OP + RC_CENV_LOCS * 3,         // + slot: first option (eat, wield ...)
    RC_CACT_ITEM_ALT = RC_CACT_ITEM + 28,                     // + slot: last option (uncharge ...)
    RC_CACT_DROP = RC_CACT_ITEM_ALT + 28,                     // + slot
    RC_CACT_UNEQUIP = RC_CACT_DROP + 28,                      // + equipment slot
    RC_CACT_COMBINE = RC_CACT_UNEQUIP + 11,                   // + item pair
    RC_CACT_PICKUP = RC_CACT_COMBINE + RC_CENV_COMBOS,        // + listed ground item
    RC_CACT_PRAYER = RC_CACT_PICKUP + RC_CENV_GROUND,         // + enabled prayer (toggle)
    RC_CACT_STYLE = RC_CACT_PRAYER + RC_CENV_PRAYERS,         // + attack style
    RC_CACT_AUTOCAST = RC_CACT_STYLE + 4,                     // + enabled spell
    RC_CACT_MAKE_ONE = RC_CACT_AUTOCAST + RC_CENV_SPELLS,     // + shown menu option
    RC_CACT_MAKE_ALL = RC_CACT_MAKE_ONE + RC_CENV_MAKE_SLOTS, // + shown menu option
    RC_CACT_DEPOSIT = RC_CACT_MAKE_ALL + RC_CENV_MAKE_SLOTS,  // + inventory slot (all of it)
    RC_CACT_DEPOSIT_INVENTORY = RC_CACT_DEPOSIT + 28,
    RC_CACT_DEPOSIT_EQUIPMENT,
    RC_CACT_WITHDRAW_ONE,                                     // + shown bank slot
    RC_CACT_WITHDRAW_ALL = RC_CACT_WITHDRAW_ONE + RC_CENV_BANK_SLOTS,
    RC_CACT_BUY_ONE = RC_CACT_WITHDRAW_ALL + RC_CENV_BANK_SLOTS,  // + shop slot
    RC_CACT_BUY_TEN = RC_CACT_BUY_ONE + RC_CENV_SHOP_SLOTS,
    RC_CACT_SELL = RC_CACT_BUY_TEN + RC_CENV_SHOP_SLOTS,       // + inventory slot (all of it)
    RC_CACT_CLOSE = RC_CACT_SELL + 28,                        // bank, shop or menu
    RC_CACT_TRADE_REQUEST,                                    // + listed player
    RC_CACT_TRADE_OFFER = RC_CACT_TRADE_REQUEST + RC_CENV_PLAYERS,  // + inventory slot (all)
    RC_CACT_TRADE_WITHDRAW = RC_CACT_TRADE_OFFER + 28,        // + own offer slot
    RC_CACT_TRADE_ACCEPT = RC_CACT_TRADE_WITHDRAW + RC_CENV_OFFER_SLOTS,
    RC_CACT_TRADE_DECLINE,
    RC_CENV_NUM_ACTIONS,
};

// Why an action did not happen (the diagnostics channel; not part of the observation).
typedef enum {
    RC_CENV_OK = 0,
    RC_CENV_OUT_OF_RANGE,      // not an action id
    RC_CENV_MASKED,            // invalid under the mask the agent was given
    RC_CENV_STALE,             // its target is gone since the observation
    RC_CENV_REJECTED,          // the engine refused the command
    RC_CENV_DEAD,              // the agent is dead
    RC_CENV_DONE,              // the agent's episode is over
    RC_CENV_RESULT_COUNT,
} RcCenvResult;

// ---- Profiles (benchmark wrappers) -------------------------------------------------------------------
typedef enum {
    RC_CENV_FREE_PLAY = 0,     // no task: stops only at the safety horizon; no reward
    RC_CENV_SOLO_BOSS,         // first qualifying Scurrius kill (+1, terminated) or the horizon
    RC_CENV_DEVELOPMENT,       // fixed horizon; metrics only, no reward
    RC_CENV_COOP_BOSS,         // first kill with a qualified team member: +1 for every member
    RC_CENV_PROFILE_COUNT,
} RcCenvProfile;

typedef struct {
    const char *mode;          // NULL: $RS_GYM_MODE or the default (compact_osrs_v1)
    int num_agents;            // 1 to the mode's maximum (8)
    int profile;               // RcCenvProfile
    int horizon;               // ticks; 0: the mode's benchmark horizon (12000)
    uint32_t seed;
    bool public_map;           // landmark bearings (the same named places the viewer shows); default on
    const char *record_path;   // write each episode's actions here, or NULL
    const char *event_log;     // append authoritative events (one line each) here, or NULL
} RcCenvConfig;

// Per-agent diagnostics; never part of the actor's input.
typedef struct {
    int xp_gained[32];
    int deaths, ticks_dead, kills, boss_kills, boss_credits, boss_attempts;
    int trades;
    int produced, consumed;            // supplies made (food, arrows, runes ...) / used up
    int64_t coins_from_sales, coins_from_loot, coins_spent_shops, coins_spent_services;
    int64_t coins_traded_in, coins_traded_out;
    int damage_dealt[4];               // melee, ranged, magic, other
    int first_product_tick;            // -1 until the first production
    int first_band15_tick, first_band30_tick;  // first time wielding/wearing band equipment
    int ticks_in_arena;
    int results[RC_CENV_RESULT_COUNT];
} RcCenvMetrics;

typedef struct RcCompactEnv RcCompactEnv;
typedef struct RcCompactSnapshot RcCompactSnapshot;

RcCompactEnv *rc_cenv_create(const RcCenvConfig *cfg);
void rc_cenv_destroy(RcCompactEnv *env);
int  rc_cenv_num_agents(const RcCompactEnv *env);
struct RcWorld *rc_cenv_world(RcCompactEnv *env);

// A full reset: a fresh world from `seed`, fresh characters with the starter kit.
void rc_cenv_reset(RcCompactEnv *env, uint32_t seed);
void rc_cenv_set_action(RcCompactEnv *env, int agent, int action);
// One tick. Returns 1 when every agent's episode has ended (reset before stepping again).
int  rc_cenv_step(RcCompactEnv *env);

float rc_cenv_reward(const RcCompactEnv *env, int agent);
int  rc_cenv_terminated(const RcCompactEnv *env, int agent);  // the task completed
int  rc_cenv_truncated(const RcCompactEnv *env, int agent);   // the horizon ran out
int  rc_cenv_episode_ticks(const RcCompactEnv *env);
void rc_cenv_observe(RcCompactEnv *env, int agent, float *obs);       // RC_CENV_OBS_SIZE
void rc_cenv_action_mask(RcCompactEnv *env, int agent, uint8_t *mask);  // RC_CENV_NUM_ACTIONS
RcCenvResult rc_cenv_last_result(const RcCompactEnv *env, int agent);
const RcCenvMetrics *rc_cenv_metrics(const RcCompactEnv *env, int agent);
const char *rc_cenv_action_name(int action, char *buf, int cap);

// Determinism: a hash of the world's canonical state and the wrapper's state.
uint64_t rc_cenv_state_hash(const RcCompactEnv *env);
RcCompactSnapshot *rc_cenv_snapshot(const RcCompactEnv *env);
int  rc_cenv_restore(RcCompactEnv *env, const RcCompactSnapshot *snapshot);
void rc_cenv_snapshot_free(RcCompactSnapshot *snapshot);

// Replays (RcCenvConfig.record_path): a header, then each agent's action per tick.
typedef struct {
    char schema[16], mode[64];
    int num_agents, profile, horizon;
    uint32_t seed, ticks;
} RcCenvReplayHeader;
FILE *rc_cenv_replay_open(const char *path, RcCenvReplayHeader *out);
int  rc_cenv_replay_next(FILE *f, const RcCenvReplayHeader *header, int *actions);

#endif
