// compact-v2 lifecycle: worlds, profiles (wrappers), rewards, metrics, the event log, snapshots,
// state hashes and replays (rc-env/compact_env.h).
#include "compact_internal.h"

#include "../rc-content/content.h"
#include "../rc-core/api.h"
#include "../rc-core/events.h"
#include "../rc-core/game_data.h"
#include "../rc-core/objects.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/players.h"
#include "../rc-core/snapshot.h"
#include "../rc-core/storage.h"
#include "../rc-core/trade.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// rc-env/travel.c
void rc_env_build_travel(const RcGymLayout *layout, const RcWorldMap *map);

enum { COINS = 995, REPLAY_MAGIC = 0x32434752u /* RGC2 */, REPLAY_VERSION = 1 };

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t mix_seed(uint32_t seed) {
    uint32_t x = seed + 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x ? x : 0x12345678u;
}

static int64_t coins_held(const RcPlayer *p) {
    int64_t n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) if (p->inventory[i].item_id == COINS) n += p->inventory[i].quantity;
    for (int i = 0; i < RC_BANK_SIZE; i++) if (p->bank[i].item_id == COINS) n += p->bank[i].quantity;
    for (int i = 0; i < p->trade.offer_count; i++) if (p->trade.offer[i].item_id == COINS) n += p->trade.offer[i].quantity;
    return n;
}

static void log_event(RcCompactEnv *env, int agent, const char *kind, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void log_event(RcCompactEnv *env, int agent, const char *kind, const char *fmt, ...) {
    if (!env->events) return;
    fprintf(env->events, "%u %d %s ", (unsigned)(env->world->tick - env->start_tick), agent, kind);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(env->events, fmt, ap);
    va_end(ap);
    fputc('\n', env->events);
}

// ---- Engine events ---------------------------------------------------------------------------------
// During hit resolution the acting player is the hitter, so it owns the damage and the kill.

static void on_npc_damaged(RcWorld *world, int evt, const void *payload, void *ctx) {
    (void)evt;
    RcCompactEnv *env = ctx;
    const RcPayloadNpcDamaged *d = payload;
    if (d->source_npc_id != UINT32_MAX || !world->player) return;
    int agent = world->player->index;
    if (agent < 0 || agent >= env->num_agents) return;
    int style = d->style == COMBAT_RANGED ? 1 : d->style == COMBAT_MAGIC ? 2
              : d->style == COMBAT_NONE ? 3 : 0;
    env->agents[agent].m.damage_dealt[style] += d->damage;
}

static void on_npc_died(RcWorld *world, int evt, const void *payload, void *ctx) {
    (void)evt;
    RcCompactEnv *env = ctx;
    const RcPayloadNpcEvent *d = payload;
    if (!world->player) return;
    int agent = world->player->index;
    if (agent < 0 || agent >= env->num_agents) return;
    env->agents[agent].m.kills++;
    log_event(env, agent, "kill", "npc=%u", d->def_id);
}

// ---- Episodes --------------------------------------------------------------------------------------

static void place_agent(RcCompactEnv *env, int agent) {
    RcWorld *w = env->world;
    rc_world_select_player(w, agent);
    int x = env->layout.start_x + agent % 4 - 1, y = env->layout.start_y + agent / 4;
    if (rc_tile_blocked(&w->map, x, y, env->layout.start_plane)) {
        x = env->layout.start_x;
        y = env->layout.start_y;
    }
    (void)rc_world_relocate_player(w, x, y, env->layout.start_plane);
    rc_content_compact_new_character(w);
    RcCenvAgent *a = &env->agents[agent];
    memset(a, 0, sizeof(*a));
    a->m.first_product_tick = a->m.first_band15_tick = a->m.first_band30_tick = -1;
    for (int s = 0; s < SKILL_COUNT; s++) a->xp[s] = w->player->skills.xp[s];
    a->coins = coins_held(w->player);
}

static void record_begin(RcCompactEnv *env) {
    if (!env->record_path[0]) return;
    if (env->record) fclose(env->record);
    env->record = fopen(env->record_path, "wb");
    env->recorded = 0;
    if (!env->record) {
        fprintf(stderr, "rc-env: cannot record to %s\n", env->record_path);
        return;
    }
    uint32_t header[8] = {REPLAY_MAGIC, REPLAY_VERSION, (uint32_t)env->num_agents,
                          (uint32_t)env->profile, (uint32_t)env->horizon, env->seed, 0, 0};
    char schema[16] = {0}, mode[64] = {0};
    snprintf(schema, sizeof(schema), "%s", RC_CENV_SCHEMA);
    snprintf(mode, sizeof(mode), "%s", env->layout.mode);
    fwrite(header, sizeof(header), 1, env->record);
    fwrite(schema, sizeof(schema), 1, env->record);
    fwrite(mode, sizeof(mode), 1, env->record);
}

static void record_tick(RcCompactEnv *env) {
    if (!env->record) return;
    uint16_t actions[RC_CENV_MAX_AGENTS];
    for (int i = 0; i < env->num_agents; i++) actions[i] = (uint16_t)env->agents[i].action;
    fwrite(actions, sizeof(actions[0]), (size_t)env->num_agents, env->record);
    env->recorded++;
    long end = ftell(env->record);
    fseek(env->record, 6 * (long)sizeof(uint32_t), SEEK_SET);
    fwrite(&env->recorded, sizeof(env->recorded), 1, env->record);
    fseek(env->record, end, SEEK_SET);
}

void rc_cenv_reset(RcCompactEnv *env, uint32_t seed) {
    if (!env) return;
    RcWorld *w = env->world;
    env->seed = seed;
    (void)rc_world_reset_seeded(w, mix_seed(seed));
    (void)rc_event_subscribe(w, RC_EVT_NPC_DAMAGED, on_npc_damaged, env);
    (void)rc_event_subscribe(w, RC_EVT_NPC_DIED, on_npc_died, env);
    env->start_tick = w->tick;
    env->success = false;
    env->success_tick = 0;
    env->boss_kills_seen = 0;
    for (int i = 0; i < env->num_agents; i++) place_agent(env, i);
    rc_world_select_player(w, 0);
    record_begin(env);
    if (env->events)
        fprintf(env->events, "# episode schema=%s mode=%s mode_version=%d profile=%d agents=%d seed=%u "
                "horizon=%d start=fresh public_map=%d\n", RC_CENV_SCHEMA, env->rules->name,
                env->rules->version, env->profile, env->num_agents, seed, env->horizon,
                env->cfg.public_map ? 1 : 0);
    for (int i = 0; i < env->num_agents; i++) env->views[i].valid = 0;
}

RcCompactEnv *rc_cenv_create(const RcCenvConfig *cfg) {
    if (!cfg || cfg->num_agents < 1 || cfg->num_agents > RC_CENV_MAX_AGENTS
            || cfg->profile < 0 || cfg->profile >= RC_CENV_PROFILE_COUNT) {
        fprintf(stderr, "rc-env: bad compact config (agents 1-%d, profile 0-%d)\n",
                RC_CENV_MAX_AGENTS, RC_CENV_PROFILE_COUNT - 1);
        return NULL;
    }
    RcCompactEnv *env = calloc(1, sizeof(*env));
    if (!env) return NULL;
    env->cfg = *cfg;
    if (cfg->record_path) snprintf(env->record_path, sizeof(env->record_path), "%s", cfg->record_path);
    if (cfg->event_log) {
        snprintf(env->event_path, sizeof(env->event_path), "%s", cfg->event_log);
        env->events = fopen(env->event_path, "a");
    }
    env->cfg.record_path = env->cfg.event_log = NULL;
    if (!rc_gym_layout_load_mode(cfg->mode, &env->layout) || !env->layout.has_rules) {
        fprintf(stderr, "rc-env: mode %s is not built or has no rules\n", rc_gym_mode_name(cfg->mode));
        rc_cenv_destroy(env);
        return NULL;
    }
    snprintf(env->mode, sizeof(env->mode), "%s", env->layout.mode);
    RcWorldConfig wc = rc_gym_world_config(&env->layout, cfg->num_agents);
    RcGameDataLoadReport report;
    RcGameData *data = rc_game_data_load(&wc, &report);
    if (!data) fprintf(stderr, "rc-env: %s\n", report.message);
    env->world = data ? rc_world_create_with_data(data, &wc) : NULL;
    if (data) rc_game_data_release(data);
    env->rules = rc_mode_active();
    if (!env->world || !env->rules || !rc_world_install_content(env->world, rc_content_register_all)
            || cfg->num_agents > env->rules->max_players) {
        fprintf(stderr, "rc-env: the %s world did not start (at most %d agents)\n", env->mode,
                env->rules ? env->rules->max_players : 0);
        rc_cenv_destroy(env);
        return NULL;
    }
    env->num_agents = cfg->num_agents;
    env->profile = cfg->profile;
    env->horizon = cfg->horizon > 0 ? cfg->horizon : env->rules->horizon > 0 ? env->rules->horizon : 12000;
    rc_cenv_build_tables(env);
    rc_cenv_reset(env, cfg->seed);
    pthread_mutex_lock(&cache_lock);  // every gym page resident, then read-only for all threads
    (void)rc_object_placements_prefetch_rect(env->layout.min_rx * 64, env->layout.min_ry * 64,
                                             env->layout.max_rx * 64 + 63, env->layout.max_ry * 64 + 63,
                                             NULL);
    (void)rc_object_placements_freeze();
    pthread_mutex_unlock(&cache_lock);
    rc_env_build_travel(&env->layout, &env->world->map);
    rc_cenv_index_locs(env);
    return env;
}

void rc_cenv_destroy(RcCompactEnv *env) {
    if (!env) return;
    if (env->record) fclose(env->record);
    if (env->events) fclose(env->events);
    if (env->world) rc_world_destroy(env->world);
    rc_cenv_free_tables(env);
    free(env);
}

int rc_cenv_num_agents(const RcCompactEnv *env) { return env ? env->num_agents : 0; }
struct RcWorld *rc_cenv_world(RcCompactEnv *env) { return env ? env->world : NULL; }
int rc_cenv_episode_ticks(const RcCompactEnv *env) {
    return env ? (int)(env->world->tick - env->start_tick) : 0;
}

void rc_cenv_set_action(RcCompactEnv *env, int agent, int action) {
    if (env && agent >= 0 && agent < env->num_agents) env->agents[agent].action = action;
}

// ---- Metrics ---------------------------------------------------------------------------------------

static int item_tier(const RcCompactEnv *env, int item) {
    return item >= 0 && item < RC_MAX_ITEM_DEFS ? env->tier[item] : 0;
}

static int is_product(const RcModeRules *m, int item) {
    for (int i = 0; i < m->recipe_count; i++) if ((int)m->recipes[i].recipe.output_item == item) return 1;
    for (int i = 0; i < m->cook_count; i++) if ((int)m->cook[i].cooked == item) return 1;
    return 0;
}

static void track_agent(RcCompactEnv *env, int agent, int completed_trade) {
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    RcCenvAgent *a = &env->agents[agent];
    RcCenvMetrics *m = &a->m;
    int tick = (int)(w->tick - env->start_tick);
    for (int s = 0; s < SKILL_COUNT && s < 32; s++) {
        int gain = p->skills.xp[s] - a->xp[s];
        if (gain > 0) {
            m->xp_gained[s] += gain;
            log_event(env, agent, "xp", "skill=%d xp=%d", s, gain);
        }
        a->xp[s] = p->skills.xp[s];
    }
    if (p->is_dead && !a->was_dead) {
        m->deaths++;
        log_event(env, agent, "death", "x=%d y=%d", p->x, p->y);
    }
    if (p->is_dead) m->ticks_dead++;
    a->was_dead = p->is_dead;
    int inside = rc_cenv_in_arena(env, p);
    if (inside) m->ticks_in_arena++;
    if (inside && !a->in_arena) m->boss_attempts++;
    a->in_arena = inside;
    int64_t coins = coins_held(p), delta = coins - a->coins;
    if (delta) {
        int service = p->make_menu == RC_MAKE_MENU_TANNER || p->make_menu == RC_MAKE_MENU_ALDO;
        if (completed_trade) {
            if (delta > 0) m->coins_traded_in += delta;
            else m->coins_traded_out -= delta;
        } else if (p->shop_open >= 0) {
            if (delta > 0) m->coins_from_sales += delta;
            else m->coins_spent_shops -= delta;
        } else if (service && delta < 0) {
            m->coins_spent_services -= delta;
        } else if (delta > 0) {
            m->coins_from_loot += delta;
        }
        log_event(env, agent, "coins", "delta=%lld", (long long)delta);
    }
    a->coins = coins;
    if (completed_trade) {
        m->trades++;
        log_event(env, agent, "trade", "completed=1");
    }
    for (int i = 0; i < RC_EQUIP_COUNT; i++) {
        int tier = item_tier(env, p->equipment[i].item_id);
        if (tier >= 15 && m->first_band15_tick < 0) m->first_band15_tick = tick;
        if (tier >= 30 && m->first_band30_tick < 0) m->first_band30_tick = tick;
    }
}

// Production and consumption from what a step changed in the agent's inventory.
static void track_items(RcCompactEnv *env, int agent, const RcInvSlot *before, int job) {
    const RcPlayer *p = &env->world->players[agent];
    RcCenvMetrics *m = &env->agents[agent].m;
    int seen[RC_INVENTORY_SIZE * 2], n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        if (before[i].item_id >= 0) seen[n++] = before[i].item_id;
        if (p->inventory[i].item_id >= 0) seen[n++] = p->inventory[i].item_id;
    }
    for (int i = 0; i < n; i++) {
        int item = seen[i], dup = 0;
        for (int j = 0; j < i; j++) dup |= seen[j] == item;
        if (dup) continue;
        int was = 0, now = 0;
        for (int s = 0; s < RC_INVENTORY_SIZE; s++) {
            if (before[s].item_id == item) was += before[s].quantity;
            if (p->inventory[s].item_id == item) now += p->inventory[s].quantity;
        }
        if (now > was && job && is_product(env->rules, item)) {
            m->produced += now - was;
            if (m->first_product_tick < 0)
                m->first_product_tick = (int)(env->world->tick - env->start_tick);
            log_event(env, agent, "produce", "item=%d qty=%d", item, now - was);
        }
        int cat = item < RC_MAX_ITEM_DEFS ? env->category[item] : 13;
        if (now < was && (cat == 0 || cat == 1 || cat == 6 || cat == 7) && p->shop_open < 0
                && p->storage_kind == RC_STORAGE_NONE && p->trade.stage != RC_TRADE_STAGE_OPEN
                && !p->is_dead) {
            m->consumed += was - now;
            log_event(env, agent, "consume", "item=%d qty=%d", item, was - now);
        }
    }
}

// ---- Step ------------------------------------------------------------------------------------------

static int done(const RcCenvAgent *a) { return a->terminated || a->truncated; }

int rc_cenv_step(RcCompactEnv *env) {
    if (!env) return 0;
    RcWorld *w = env->world;
    record_tick(env);
    RcInvSlot before[RC_CENV_MAX_AGENTS][RC_INVENTORY_SIZE];
    int making[RC_CENV_MAX_AGENTS];
    uint8_t trade_results[RC_CENV_MAX_AGENTS];
    for (int i = 0; i < env->num_agents; i++) {
        RcCenvAgent *a = &env->agents[i];
        a->reward = 0;
        memcpy(before[i], w->players[i].inventory, sizeof(before[i]));
        making[i] = w->players[i].skill_job.kind == RC_SKILL_JOB_MAKE
                 || w->players[i].skill_job.kind == RC_SKILL_JOB_COOK;
        RcCenvResult r = done(a) ? (a->action == RC_CACT_WAIT ? RC_CENV_OK : RC_CENV_DONE)
                                 : rc_cenv_execute(env, i, a->action);
        a->last = r;
        a->m.results[r]++;
        a->action = RC_CACT_WAIT;
        trade_results[i] = w->players[i].trade.last_result;
    }
    rc_world_select_player(w, 0);
    rc_world_tick(w);
    for (int i = 0; i < env->num_agents; i++) {
        const RcPlayer *p = &w->players[i];
        // A job that ran this tick may also have finished in it.
        int job = making[i] || p->skill_job.kind == RC_SKILL_JOB_MAKE
               || p->skill_job.kind == RC_SKILL_JOB_COOK;
        int completed = p->trade.last_result == RC_TRADE_COMPLETED && trade_results[i] != RC_TRADE_COMPLETED;
        track_agent(env, i, completed);
        track_items(env, i, before[i], job);
        env->views[i].valid = 0;
    }
    // Boss kills: credit, and the solo and cooperative tasks.
    const RcModeBossState *b = &w->mode_boss;
    if (b->kills != env->boss_kills_seen) {
        env->boss_kills_seen = b->kills;
        uint16_t team = (uint16_t)((1u << env->num_agents) - 1);
        for (int i = 0; i < env->num_agents; i++) {
            RcCenvAgent *a = &env->agents[i];
            if (b->last_qualified >> i & 1) {
                a->m.boss_credits++;
                a->m.boss_kills++;
                log_event(env, i, "boss_kill", "credit=1 owner=%d", b->last_owner);
            }
            if (env->profile == RC_CENV_SOLO_BOSS && (b->last_qualified >> i & 1) && !done(a)) {
                a->reward = 1.0f;
                a->terminated = true;
                env->success = true;
                env->success_tick = w->tick;
            }
        }
        if (env->profile == RC_CENV_COOP_BOSS && (b->last_qualified & team) && !env->success) {
            env->success = true;
            env->success_tick = w->tick;
            for (int i = 0; i < env->num_agents; i++) {
                if (done(&env->agents[i])) continue;
                env->agents[i].reward = 1.0f;
                env->agents[i].terminated = true;
            }
        }
    }
    int all_done = 1;
    int elapsed = (int)(w->tick - env->start_tick);
    for (int i = 0; i < env->num_agents; i++) {
        RcCenvAgent *a = &env->agents[i];
        if (!done(a) && elapsed >= env->horizon) a->truncated = true;
        all_done &= done(a);
    }
    if (all_done) log_event(env, -1, "end", "ticks=%d success=%d", elapsed, env->success);
    if (env->events) fflush(env->events);
    return all_done;
}

float rc_cenv_reward(const RcCompactEnv *env, int agent) {
    return env && agent >= 0 && agent < env->num_agents ? env->agents[agent].reward : 0.0f;
}

int rc_cenv_terminated(const RcCompactEnv *env, int agent) {
    return env && agent >= 0 && agent < env->num_agents && env->agents[agent].terminated;
}

int rc_cenv_truncated(const RcCompactEnv *env, int agent) {
    return env && agent >= 0 && agent < env->num_agents && env->agents[agent].truncated;
}

void rc_cenv_observe(RcCompactEnv *env, int agent, float *obs) {
    if (env && agent >= 0 && agent < env->num_agents && obs) rc_cenv_encode(env, agent, obs);
}

void rc_cenv_action_mask(RcCompactEnv *env, int agent, uint8_t *mask) {
    if (env && agent >= 0 && agent < env->num_agents && mask) rc_cenv_mask(env, agent, mask);
}

RcCenvResult rc_cenv_last_result(const RcCompactEnv *env, int agent) {
    return env && agent >= 0 && agent < env->num_agents ? env->agents[agent].last : RC_CENV_OK;
}

const RcCenvMetrics *rc_cenv_metrics(const RcCompactEnv *env, int agent) {
    return env && agent >= 0 && agent < env->num_agents ? &env->agents[agent].m : NULL;
}

// ---- Determinism -----------------------------------------------------------------------------------

uint64_t rc_cenv_state_hash(const RcCompactEnv *env) {
    if (!env) return 0;
    uint64_t h = rc_world_state_hash(env->world);
    uint64_t extra[4] = {env->start_tick, env->success, env->success_tick, env->boss_kills_seen};
    for (int i = 0; i < env->num_agents; i++)
        extra[3] = extra[3] * 31 + (uint64_t)env->agents[i].terminated * 2 + env->agents[i].truncated;
    for (int i = 0; i < 4; i++) {
        h ^= extra[i] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

struct RcCompactSnapshot {
    RcWorldSnapshot *world;
    RcCenvAgent agents[RC_CENV_MAX_AGENTS];
    RcTick start_tick, success_tick;
    bool success;
    uint32_t boss_kills_seen, seed;
};

RcCompactSnapshot *rc_cenv_snapshot(const RcCompactEnv *env) {
    if (!env) return NULL;
    RcCompactSnapshot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->world = rc_world_snapshot(env->world);
    if (!s->world) {
        free(s);
        return NULL;
    }
    memcpy(s->agents, env->agents, sizeof(s->agents));
    s->start_tick = env->start_tick;
    s->success_tick = env->success_tick;
    s->success = env->success;
    s->boss_kills_seen = env->boss_kills_seen;
    s->seed = env->seed;
    return s;
}

int rc_cenv_restore(RcCompactEnv *env, const RcCompactSnapshot *s) {
    if (!env || !s || !rc_world_restore(env->world, s->world)) return 0;
    memcpy(env->agents, s->agents, sizeof(env->agents));
    env->start_tick = s->start_tick;
    env->success_tick = s->success_tick;
    env->success = s->success;
    env->boss_kills_seen = s->boss_kills_seen;
    env->seed = s->seed;
    // The event subscriptions in the restored world name this env.
    (void)rc_event_unsubscribe(env->world, RC_EVT_NPC_DAMAGED, on_npc_damaged, env);
    (void)rc_event_unsubscribe(env->world, RC_EVT_NPC_DIED, on_npc_died, env);
    (void)rc_event_subscribe(env->world, RC_EVT_NPC_DAMAGED, on_npc_damaged, env);
    (void)rc_event_subscribe(env->world, RC_EVT_NPC_DIED, on_npc_died, env);
    for (int i = 0; i < env->num_agents; i++) env->views[i].valid = 0;
    return 1;
}

void rc_cenv_snapshot_free(RcCompactSnapshot *s) {
    if (!s) return;
    rc_world_snapshot_free(s->world);
    free(s);
}

// ---- Replays ---------------------------------------------------------------------------------------

FILE *rc_cenv_replay_open(const char *path, RcCenvReplayHeader *out) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    uint32_t header[8];
    char schema[16], mode[64];
    if (!f || fread(header, sizeof(header), 1, f) != 1 || header[0] != REPLAY_MAGIC
            || header[1] != REPLAY_VERSION || header[2] < 1 || header[2] > RC_CENV_MAX_AGENTS
            || fread(schema, sizeof(schema), 1, f) != 1 || fread(mode, sizeof(mode), 1, f) != 1) {
        if (f) fclose(f);
        return NULL;
    }
    schema[sizeof(schema) - 1] = mode[sizeof(mode) - 1] = '\0';
    memset(out, 0, sizeof(*out));
    snprintf(out->schema, sizeof(out->schema), "%s", schema);
    snprintf(out->mode, sizeof(out->mode), "%s", mode);
    out->num_agents = (int)header[2];
    out->profile = (int)header[3];
    out->horizon = (int)header[4];
    out->seed = header[5];
    out->ticks = header[6];
    return f;
}

int rc_cenv_replay_next(FILE *f, const RcCenvReplayHeader *header, int *actions) {
    uint16_t raw[RC_CENV_MAX_AGENTS];
    if (!f || fread(raw, sizeof(raw[0]), (size_t)header->num_agents, f) != (size_t)header->num_agents)
        return 0;
    for (int i = 0; i < header->num_agents; i++) actions[i] = raw[i];
    return 1;
}
