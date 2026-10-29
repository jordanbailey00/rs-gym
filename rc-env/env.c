#include "env_internal.h"

#include "../rc-content/content.h"
#include "../rc-core/api.h"
#include "../rc-core/events.h"
#include "../rc-core/game_data.h"
#include "../rc-core/items.h"
#include "../rc-core/npc.h"
#include "../rc-core/objects.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/players.h"
#include "../rc-core/skills.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define COINS 995

// ---- Tasks ------------------------------------------------------------------

enum { KIT_NONE = 0, KIT_STARTER, KIT_FIGHTER, KIT_KBD, KIT_CHAOS_ELEMENTAL };

#define COMBAT_XP(w) \
    [SKILL_ATTACK] = (w), [SKILL_STRENGTH] = (w), [SKILL_DEFENCE] = (w), \
    [SKILL_HITPOINTS] = (w), [SKILL_RANGED] = (w), [SKILL_MAGIC] = (w)
#define SKILLING_XP(w) \
    [SKILL_WOODCUTTING] = (w), [SKILL_MINING] = (w), [SKILL_FISHING] = (w), \
    [SKILL_FIREMAKING] = (w), [SKILL_COOKING] = (w), [SKILL_SMITHING] = (w), \
    [SKILL_PRAYER] = (w)

// Landmark indexes in maps/gym_v1.toml's [[landmark]] order.
enum { LM_KBD_LAIR = 21, LM_CHAOS_ELEMENTAL = 20 };

static const RcTask tasks[RC_TASK_COUNT] = {
    [RC_TASK_TOTAL] = {"total", 12000, {COMBAT_XP(0.0005f), SKILLING_XP(0.0005f)},
                       .level = 0.2f, .death = -0.5f, .kit = KIT_STARTER,
                       .start_landmark = -1, .boss_npc = -1},
    [RC_TASK_COMBAT] = {"combat", 6000, {COMBAT_XP(0.002f)}, .kill = 0.02f,
                        .death = -1.0f, .kit = KIT_FIGHTER, .start_landmark = -1,
                        .boss_npc = -1},
    [RC_TASK_WOODCUTTING] = {"woodcutting", 3000, {[SKILL_WOODCUTTING] = 0.004f},
                             .death = -1.0f, .kit = KIT_STARTER,
                             .start_landmark = -1, .boss_npc = -1},
    [RC_TASK_MINING] = {"mining", 3000, {[SKILL_MINING] = 0.004f}, .death = -1.0f,
                        .kit = KIT_STARTER, .start_landmark = -1, .boss_npc = -1},
    [RC_TASK_FISHING] = {"fishing", 3000, {[SKILL_FISHING] = 0.004f},
                         .death = -1.0f, .kit = KIT_STARTER, .start_landmark = -1,
                         .boss_npc = -1},
    [RC_TASK_COOKING] = {"cooking", 6000,
                         {[SKILL_COOKING] = 0.004f, [SKILL_FISHING] = 0.001f,
                          [SKILL_FIREMAKING] = 0.001f, [SKILL_WOODCUTTING] = 0.0005f},
                         .death = -1.0f, .kit = KIT_STARTER, .start_landmark = -1,
                         .boss_npc = -1},
    [RC_TASK_SMITHING] = {"smithing", 6000,
                          {[SKILL_SMITHING] = 0.004f, [SKILL_MINING] = 0.001f},
                          .death = -1.0f, .kit = KIT_STARTER, .start_landmark = -1,
                          .boss_npc = -1},
    [RC_TASK_WEALTH] = {"wealth", 6000, {0}, .net_worth = 0.001f, .death = -1.0f,
                        .kit = KIT_STARTER, .start_landmark = -1, .boss_npc = -1},
    [RC_TASK_KBD] = {"kbd", 2000, {0}, .boss_damage = 0.01f, .boss_kill = 1.0f,
                     .damage_taken = -0.002f, .death = -1.0f, .death_ends = true,
                     .kit = KIT_KBD, .start_landmark = LM_KBD_LAIR, .boss_npc = 239},
    [RC_TASK_CHAOS_ELEMENTAL] = {"chaos_elemental", 2000, {0}, .boss_damage = 0.01f,
                                 .boss_kill = 1.0f, .damage_taken = -0.002f,
                                 .death = -1.0f, .death_ends = true,
                                 .kit = KIT_CHAOS_ELEMENTAL,
                                 .start_landmark = LM_CHAOS_ELEMENTAL, .boss_npc = 2054},
};

const RcTask *rc_env_task(int task) {
    return task >= 0 && task < RC_TASK_COUNT ? &tasks[task] : NULL;
}

int rc_env_task_find(const char *name) {
    for (int i = 0; name && i < RC_TASK_COUNT; i++)
        if (strcmp(tasks[i].name, name) == 0) return i;
    return -1;
}

// ---- Shared game data ---------------------------------------------------------
// Every env in a process reads the same immutable game data.

static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static RcGameData *shared_data;

static RcGameData *acquire_data(const RcWorldConfig *cfg) {
    pthread_mutex_lock(&data_lock);
    if (!shared_data) {
        RcGameDataLoadReport report;
        shared_data = rc_game_data_load(cfg, &report);
        if (!shared_data) fprintf(stderr, "rc-env: %s\n", report.message);
    }
    RcGameData *data = shared_data;
    if (data) rc_game_data_retain(data);
    pthread_mutex_unlock(&data_lock);
    return data;
}

// The object placement cache is shared by every world and is not thread safe
// while it loads and evicts pages. All gym pages are resident once one world
// has loaded the gym, so it is frozen then and worlds may step on any thread.
static void freeze_shared_caches(const RcEnv *env) {
    pthread_mutex_lock(&data_lock);
    (void)rc_object_placements_prefetch_rect(
        env->layout.min_rx * 64, env->layout.min_ry * 64,
        env->layout.max_rx * 64 + 63, env->layout.max_ry * 64 + 63, NULL);
    (void)rc_object_placements_freeze();
    pthread_mutex_unlock(&data_lock);
}

// ---- Kits -------------------------------------------------------------------------

static void set_level(RcPlayer *p, int skill, int level) {
    p->skills.base_level[skill] = p->skills.boosted_level[skill] = level;
    p->skills.xp[skill] = RC_XP_TABLE[level - 1];
    p->skills.xp_hundredths[skill] = 0;
    if (skill == SKILL_HITPOINTS) p->max_hp = p->current_hp = level * 10;
    if (skill == SKILL_PRAYER) p->current_prayer_points = level * 10;
}

static void clear_items(RcPlayer *p) {
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        p->inventory[i] = (RcInvSlot){.item_id = -1, .generation = p->inventory[i].generation + 1};
    for (int i = 0; i < RC_EQUIP_COUNT; i++)
        p->equipment[i] = (RcInvSlot){.item_id = -1, .generation = p->equipment[i].generation + 1};
    for (int i = 0; i < RC_BANK_SIZE; i++) p->bank[i] = (RcInvSlot){.item_id = -1};
}

static void wear(RcPlayer *p, int item_id) {
    const RcItemDef *def = rc_item_def_get(item_id);
    if (def && def->equippable && def->equip_slot >= 0 && def->equip_slot < RC_EQUIP_COUNT)
        p->equipment[def->equip_slot] = (RcInvSlot){.item_id = item_id, .quantity = 1};
}

static void give(RcWorld *world, int item_id, int quantity) {
    const RcItemDef *def = rc_item_def_get(item_id);
    if (def && def->stackable) {
        (void)rc_player_inventory_add(world, item_id, quantity, 0);
        return;
    }
    for (int i = 0; i < quantity; i++) (void)rc_player_inventory_add(world, item_id, 1, 0);
}

static void apply_kit(RcWorld *world, int kit) {
    RcPlayer *p = world->player;
    clear_items(p);
    for (int s = 0; s < SKILL_COUNT; s++) set_level(p, s, s == SKILL_HITPOINTS ? 10 : 1);
    switch (kit) {
    case KIT_STARTER:
        give(world, 1351, 1);   // bronze axe
        give(world, 1265, 1);   // bronze pickaxe
        give(world, 303, 1);    // small fishing net
        give(world, 590, 1);    // tinderbox
        give(world, 2347, 1);   // hammer
        give(world, COINS, 25);
        wear(p, 1277);          // bronze sword
        break;
    case KIT_FIGHTER:
        wear(p, 1323);          // iron scimitar
        wear(p, 1137);          // iron med helm
        wear(p, 1101);          // iron chainbody
        wear(p, 1191);          // iron kiteshield
        give(world, 315, 5);    // shrimps
        give(world, COINS, 25);
        break;
    case KIT_KBD:
    case KIT_CHAOS_ELEMENTAL:
        for (int s = SKILL_ATTACK; s <= SKILL_MAGIC; s++) set_level(p, s, 70);
        set_level(p, SKILL_PRAYER, 43);
        wear(p, 1333);          // rune scimitar
        wear(p, 1163);          // rune full helm
        wear(p, 1127);          // rune platebody
        wear(p, 1079);          // rune platelegs
        wear(p, 1731);          // amulet of power
        wear(p, kit == KIT_KBD ? 1540 : 1201);  // anti-dragon shield, rune kiteshield
        give(world, 379, 20);   // lobsters
        break;
    default:
        break;
    }
    p->run_energy = 10000;
    p->special_energy = 10000;
    p->active_prayers = 0;
    rc_recalc_bonuses(p);
}

// ---- Tracking -----------------------------------------------------------------------

static int64_t item_stack_value(const RcInvSlot *s) {
    if (s->item_id < 0 || s->quantity <= 0) return 0;
    if (s->item_id == COINS) return s->quantity;
    const RcItemDef *def = rc_item_def_get(s->item_id);
    return def ? (int64_t)def->value * s->quantity : 0;
}

int rc_env_net_worth_item_value(int item_id) {
    const RcItemDef *def = item_id >= 0 ? rc_item_def_get(item_id) : NULL;
    return item_id == COINS ? 1 : def ? def->value : 0;
}

static int64_t net_worth(const RcPlayer *p) {
    int64_t total = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) total += item_stack_value(&p->inventory[i]);
    for (int i = 0; i < RC_EQUIP_COUNT; i++) total += item_stack_value(&p->equipment[i]);
    for (int i = 0; i < RC_BANK_SIZE; i++) total += item_stack_value(&p->bank[i]);
    return total;
}

static int level_total(const RcPlayer *p) {
    int total = 0;
    for (int s = 0; s < SKILL_COUNT; s++) total += p->skills.base_level[s];
    return total;
}

static const RcNpc *find_boss(const RcWorld *world, int def_id) {
    int count = 0;
    const RcNpc *npcs = rc_get_npcs(world, &count);
    for (int i = 0; i < count; i++) {
        const RcNpcDef *def = rc_npc_base_def_for_npc(&npcs[i]);
        if (npcs[i].active && def && def->id == def_id) return &npcs[i];
    }
    return NULL;
}

static void begin_track(RcEnv *env, int agent) {
    const RcPlayer *p = &env->world->players[agent];
    RcAgentTrack *t = &env->track[agent];
    memset(t, 0, sizeof(*t));
    for (int s = 0; s < SKILL_COUNT; s++) t->xp[s] = t->start_xp[s] = p->skills.xp[s];
    t->level_total = t->start_levels = level_total(p);
    t->net_worth = t->start_net_worth = net_worth(p);
    t->hp = p->current_hp;
    t->boss_uid = -1;
}

static void finish_stats(RcEnv *env, int agent) {
    const RcPlayer *p = &env->world->players[agent];
    const RcAgentTrack *t = &env->track[agent];
    RcEnvEpisodeStats *s = &env->stats[agent];
    memset(s, 0, sizeof(*s));
    int task_xp = 0, all_xp = 0;
    for (int k = 0; k < SKILL_COUNT; k++) {
        int gained = p->skills.xp[k] - t->start_xp[k];
        all_xp += gained;
        if (env->task->xp[k] > 0) task_xp += gained;
    }
    s->episode_return = t->episode_return;
    s->episode_length = (float)t->episode_length;
    s->xp_gained = (float)all_xp;
    s->levels_gained = (float)(level_total(p) - t->start_levels);
    s->net_worth_gained = (float)(net_worth(p) - t->start_net_worth);
    s->kills = t->kills;
    s->deaths = t->deaths;
    s->boss_kills = t->boss_kills;
    switch (env->cfg.task) {
    case RC_TASK_TOTAL: s->score = s->levels_gained; break;
    case RC_TASK_WEALTH: s->score = s->net_worth_gained; break;
    case RC_TASK_KBD:
    case RC_TASK_CHAOS_ELEMENTAL: s->score = s->boss_kills; break;
    default: s->score = (float)task_xp; break;
    }
    env->stats_ready[agent] = true;
}

// Kill credit goes where the loot goes: the player who dealt the most damage.
static void on_npc_died(RcWorld *world, int evt, const void *payload, void *ctx) {
    (void)evt;
    RcEnv *env = ctx;
    const RcPayloadNpcEvent *e = payload;
    const RcNpc *npc = rc_npc_resolve_const(world, (RcNpcId)e->npc_id);
    if (!npc) return;
    int owner = -1;
    for (int i = 0; i < world->player_count && i < RC_ENV_MAX_AGENTS; i++)
        if (npc->player_damage[i] && (owner < 0 || npc->player_damage[i] > npc->player_damage[owner]))
            owner = i;
    if (owner < 0) return;
    env->track[owner].kills += 1.0f;
    if (env->task->boss_npc >= 0 && (int)e->def_id == env->task->boss_npc)
        env->track[owner].boss_kills += 1.0f;
}

// ---- Episodes ----------------------------------------------------------------------

static uint32_t mix_seed(uint32_t seed) {
    uint32_t x = seed + 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x ? x : 0x12345678u;
}

static void start_tile(const RcEnv *env, int agent, int *x, int *y, int *plane) {
    const RcTask *task = env->task;
    *x = env->layout.start_x;
    *y = env->layout.start_y;
    *plane = env->layout.start_plane;
    if (task->start_landmark >= 0 && task->start_landmark < env->layout.landmark_count) {
        const RcGymLandmark *lm = &env->layout.landmarks[task->start_landmark];
        *x = lm->x;
        *y = lm->y;
        *plane = lm->plane;
    }
    // Spread agents over a few tiles around the start.
    int dx = agent % 4 - 1, dy = agent / 4 - 1;
    if (!rc_tile_blocked(&env->world->map, *x + dx, *y + dy, *plane)) {
        *x += dx;
        *y += dy;
    }
}

static void place_agent(RcEnv *env, int agent) {
    RcWorld *world = env->world;
    (void)rc_world_select_player(world, agent);
    int x, y, plane;
    start_tile(env, agent, &x, &y, &plane);
    RcPlayer *p = world->player;
    if (p->is_dead) (void)rc_world_respawn_player(world, x, y, plane);
    p->respawn_tick = 0;
    apply_kit(world, env->task->kit);
    if (!rc_world_relocate_player(world, x, y, plane))
        (void)rc_world_relocate_player(world, env->layout.start_x, env->layout.start_y,
                                       env->layout.start_plane);
    begin_track(env, agent);
}

static void record_begin(RcEnv *env) {
    if (!env->cfg.record_path) return;
    if (env->record) fclose(env->record);
    env->record = fopen(env->cfg.record_path, "wb");
    env->recorded_ticks = 0;
    if (!env->record) {
        fprintf(stderr, "rc-env: cannot record to %s\n", env->cfg.record_path);
        return;
    }
    // Header: magic, version, agents, task, episode ticks, seed, tick count (patched as it
    // grows), then the 64-byte mode name.
    uint32_t header[7] = {0x50524752u /* RGRP */, 2, (uint32_t)env->cfg.num_agents,
                          (uint32_t)env->cfg.task, (uint32_t)env->episode_ticks,
                          env->seed, 0};
    char mode[64] = {0};
    snprintf(mode, sizeof(mode), "%s", env->layout.mode);
    fwrite(header, sizeof(header), 1, env->record);
    fwrite(mode, sizeof(mode), 1, env->record);
}

static void record_tick(RcEnv *env) {
    if (!env->record) return;
    uint16_t actions[RC_ENV_MAX_AGENTS];
    for (int i = 0; i < env->cfg.num_agents; i++) actions[i] = (uint16_t)env->pending_action[i];
    fwrite(actions, sizeof(actions[0]), (size_t)env->cfg.num_agents, env->record);
    env->recorded_ticks++;
    long end = ftell(env->record);
    fseek(env->record, 6 * (long)sizeof(uint32_t), SEEK_SET);
    fwrite(&env->recorded_ticks, sizeof(env->recorded_ticks), 1, env->record);
    fseek(env->record, end, SEEK_SET);
}

void rc_env_reset(RcEnv *env, uint32_t seed) {
    if (!env) return;
    env->seed = seed;
    RcWorld *world = env->world;
    (void)rc_world_reset_seeded(world, mix_seed(seed));
    (void)rc_event_subscribe(world, RC_EVT_NPC_DIED, on_npc_died, env);
    for (int i = 0; i < env->cfg.num_agents; i++) {
        place_agent(env, i);
        env->pending_action[i] = RC_ACT_NOOP;
        env->reward[i] = 0.0f;
        env->terminal[i] = 0;
        env->stats_ready[i] = false;
    }
    (void)rc_world_select_player(world, 0);
    record_begin(env);
    env->view_fresh = false;
    rc_env_refresh_views(env);
}

RcEnv *rc_env_create(const RcEnvConfig *cfg) {
    if (!cfg || cfg->num_agents < 1 || cfg->num_agents > RC_ENV_MAX_AGENTS
            || !rc_env_task(cfg->task)) {
        fprintf(stderr, "rc-env: bad config (agents 1-%d, task 0-%d)\n",
                RC_ENV_MAX_AGENTS, RC_TASK_COUNT - 1);
        return NULL;
    }
    RcEnv *env = calloc(1, sizeof(*env));
    if (!env) return NULL;
    env->cfg = *cfg;
    env->task = rc_env_task(cfg->task);
    env->episode_ticks = cfg->episode_ticks > 0 ? cfg->episode_ticks : env->task->episode_ticks;
    if (!rc_gym_layout_load_mode(cfg->mode, &env->layout)) {
        free(env);
        return NULL;
    }
    RcWorldConfig wc = rc_gym_world_config(&env->layout, cfg->num_agents);
    RcGameData *data = acquire_data(&wc);
    env->world = data ? rc_world_create_with_data(data, &wc) : NULL;
    if (data) rc_game_data_release(data);
    if (!env->world || !rc_world_install_content(env->world, rc_content_register_all)) {
        rc_env_destroy(env);
        return NULL;
    }
    rc_env_reset(env, cfg->seed);
    freeze_shared_caches(env);
    rc_env_build_travel(&env->layout, &env->world->map);
    if (!rc_env_index_objects(env)) {
        rc_env_destroy(env);
        return NULL;
    }
    env->view_fresh = false;
    rc_env_refresh_views(env);
    return env;
}

void rc_env_destroy(RcEnv *env) {
    if (!env) return;
    if (env->record) fclose(env->record);
    if (env->world) rc_world_destroy(env->world);
    free(env->objects);
    free(env);
}

int rc_env_num_agents(const RcEnv *env) {
    return env ? env->cfg.num_agents : 0;
}

struct RcWorld *rc_env_world(RcEnv *env) {
    return env ? env->world : NULL;
}

void rc_env_set_action(RcEnv *env, int agent, int action) {
    if (env && agent >= 0 && agent < env->cfg.num_agents)
        env->pending_action[agent] = action >= 0 && action < RC_ENV_NUM_ACTIONS
                                   ? action : RC_ACT_NOOP;
}

// ---- Step ---------------------------------------------------------------------------

static float score_agent(RcEnv *env, int agent) {
    const RcTask *task = env->task;
    RcWorld *world = env->world;
    const RcPlayer *p = &world->players[agent];
    RcAgentTrack *t = &env->track[agent];
    float reward = 0.0f;
    for (int s = 0; s < SKILL_COUNT; s++) {
        int gained = p->skills.xp[s] - t->xp[s];
        if (gained > 0) reward += gained * task->xp[s];
        t->xp[s] = p->skills.xp[s];
    }
    int levels = level_total(p);
    if (levels > t->level_total) reward += (levels - t->level_total) * task->level;
    t->level_total = levels;
    if (task->net_worth != 0.0f) {
        int64_t worth = net_worth(p);
        reward += (float)(worth - t->net_worth) * task->net_worth;
        t->net_worth = worth;
    }
    if (task->boss_npc >= 0) {
        const RcNpc *boss = find_boss(world, task->boss_npc);
        int damage = boss ? boss->player_damage[agent] : 0;
        if (boss && boss->uid == t->boss_uid && damage > t->boss_damage)
            reward += (damage - t->boss_damage) / 10.0f * task->boss_damage;
        t->boss_uid = boss ? boss->uid : -1;
        t->boss_damage = damage;
    }
    if (!p->is_dead && p->current_hp < t->hp)
        reward += (t->hp - p->current_hp) / 10.0f * task->damage_taken;
    t->hp = p->current_hp;
    if (p->is_dead && !t->dead) {
        reward += task->death;
        t->deaths += 1.0f;
    }
    t->dead = p->is_dead;
    return reward;
}

int rc_env_step(RcEnv *env) {
    if (!env) return 0;
    RcWorld *world = env->world;
    record_tick(env);
    float kills[RC_ENV_MAX_AGENTS], boss_kills[RC_ENV_MAX_AGENTS];
    for (int i = 0; i < env->cfg.num_agents; i++) {
        kills[i] = env->track[i].kills;
        boss_kills[i] = env->track[i].boss_kills;
        env->terminal[i] = 0;
        env->stats_ready[i] = false;
        if (!world->players[i].is_dead && env->pending_action[i] != RC_ACT_NOOP) {
            (void)rc_world_select_player(world, i);
            rc_env_apply_action(env, i, env->pending_action[i]);
        }
        env->pending_action[i] = RC_ACT_NOOP;
    }
    (void)rc_world_select_player(world, 0);
    rc_world_tick(world);
    int time_up = (int)world->tick >= env->episode_ticks;
    for (int i = 0; i < env->cfg.num_agents; i++) {
        RcAgentTrack *t = &env->track[i];
        float reward = score_agent(env, i);
        reward += (t->kills - kills[i]) * env->task->kill;
        reward += (t->boss_kills - boss_kills[i]) * env->task->boss_kill;
        env->reward[i] = reward;
        t->episode_return += reward;
        t->episode_length++;
        int died = env->task->death_ends && world->players[i].is_dead;
        if (died || time_up) {
            env->terminal[i] = 1;
            finish_stats(env, i);
            if (died && !time_up) {
                // A fresh life in the same world.
                place_agent(env, i);
                env->track[i].dead = false;
            }
        }
    }
    (void)rc_world_select_player(world, 0);
    env->view_fresh = false;
    rc_env_refresh_views(env);
    return time_up;
}

float rc_env_reward(const RcEnv *env, int agent) {
    return env && agent >= 0 && agent < env->cfg.num_agents ? env->reward[agent] : 0.0f;
}

int rc_env_terminal(const RcEnv *env, int agent) {
    return env && agent >= 0 && agent < env->cfg.num_agents ? env->terminal[agent] : 0;
}

int rc_env_episode_stats(const RcEnv *env, int agent, RcEnvEpisodeStats *out) {
    if (!env || agent < 0 || agent >= env->cfg.num_agents || !env->stats_ready[agent])
        return 0;
    if (out) *out = env->stats[agent];
    return 1;
}

void rc_env_give_task_kit(struct RcWorld *world, int task) {
    const RcTask *t = rc_env_task(task);
    if (world && t) apply_kit(world, t->kit);
}

FILE *rc_env_replay_open(const char *path, RcReplayHeader *out) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    uint32_t header[7];
    char mode[64];
    if (!f || fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x50524752u
            || header[1] != 2 || header[2] < 1 || header[2] > RC_ENV_MAX_AGENTS
            || header[3] >= RC_TASK_COUNT || fread(mode, sizeof(mode), 1, f) != 1) {
        if (f) fclose(f);
        return NULL;
    }
    mode[sizeof(mode) - 1] = '\0';
    *out = (RcReplayHeader){{0}, (int)header[2], (int)header[3], (int)header[4], header[5], header[6]};
    snprintf(out->mode, sizeof(out->mode), "%s", mode);
    return f;
}

int rc_env_replay_next(FILE *f, const RcReplayHeader *header, int *actions) {
    uint16_t raw[RC_ENV_MAX_AGENTS];
    if (!f || fread(raw, sizeof(raw[0]), (size_t)header->num_agents, f)
            != (size_t)header->num_agents)
        return 0;
    for (int i = 0; i < header->num_agents; i++) actions[i] = raw[i];
    return 1;
}
