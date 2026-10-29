// Private state shared by rc-env/compact_env.c (lifecycle, wrappers, metrics, snapshots, replays)
// and rc-env/compact_obs.c (what agents see and can do).
#ifndef RC_COMPACT_INTERNAL_H
#define RC_COMPACT_INTERNAL_H

#include "compact_env.h"

#include "../rc-core/items.h"
#include "../rc-core/layout.h"
#include "../rc-core/mode.h"
#include "../rc-core/types.h"

typedef struct {
    int id, x, y, plane;
    int8_t ops[3];             // the loc's option slots that work here, -1 none
} RcCenvLoc;

// What an agent saw at its last observation; actions index into these lists.
typedef struct {
    int npc_uid[RC_CENV_NPCS];
    int npc_count;
    RcCenvLoc locs[RC_CENV_LOCS];
    int loc_count;
    int ground[RC_CENV_GROUND], ground_uid[RC_CENV_GROUND], ground_version[RC_CENV_GROUND];
    int ground_count;
    int players[RC_CENV_PLAYERS];
    int player_count;
    int bank_slot[RC_CENV_BANK_SLOTS];
    int bank_count;
    int make_option[RC_CENV_MAKE_SLOTS];
    int make_count;
    int x, y, plane;           // the agent's tile when it observed
    int valid;
} RcCenvView;

typedef struct {
    RcCenvMetrics m;
    int xp[32];
    int was_dead, in_arena, credits_seen;
    int64_t coins;             // inventory + bank + offered, for coin flows
    float reward;
    bool terminated, truncated;
    RcCenvResult last;
    int action;
} RcCenvAgent;

enum { RC_CENV_CATEGORIES = 14 };

struct RcCompactEnv {
    RcCenvConfig cfg;
    char mode[64];
    char record_path[512], event_path[512];
    RcGymLayout layout;
    struct RcGameData *data;
    struct RcWorld *world;
    const RcModeRules *rules;
    int num_agents, profile, horizon;
    uint32_t seed;
    RcTick start_tick;
    RcCenvAgent agents[RC_CENV_MAX_AGENTS];
    RcCenvView views[RC_CENV_MAX_AGENTS];
    // Item vocabulary and categories (from the mode's rules), indexed by item id.
    RcCenvLoc *locs;           // every loc with a working option in the mode's area (static)
    int loc_count;
    int16_t *vocab;            // 1-based index into the mode's items, 0 unknown
    int vocab_size;
    uint8_t *category;         // 0-13
    uint8_t *tier;             // highest use requirement
    struct { int a, b; } combos[RC_CENV_COMBOS];
    int combo_count;
    int prayer_ids[RC_CENV_PRAYERS];
    int spell_ids[RC_CENV_SPELLS];
    bool success;              // the profile's task completed this episode
    RcTick success_tick;
    uint32_t boss_kills_seen;
    FILE *record, *events;
    uint32_t recorded;
};

// compact_obs.c
void rc_cenv_build_tables(RcCompactEnv *env);
void rc_cenv_free_tables(RcCompactEnv *env);
void rc_cenv_encode(RcCompactEnv *env, int agent, float *obs);
void rc_cenv_mask(RcCompactEnv *env, int agent, uint8_t *mask);
RcCenvResult rc_cenv_execute(RcCompactEnv *env, int agent, int action);
void rc_cenv_refresh_view(RcCompactEnv *env, int agent);
void rc_cenv_index_locs(RcCompactEnv *env);
int rc_cenv_in_arena(const RcCompactEnv *env, const RcPlayer *p);

#endif
