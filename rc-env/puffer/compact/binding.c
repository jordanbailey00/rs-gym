/* PufferLib 5.0 entry points for RS-gym's compact mode (schema compact-v2): one PufferLib env is
 * one world holding num_agents players; episodes autoreset in the step that ends them.
 * PufferLib 5.0 has a single done buffer, so terminals is 1 at either end; the observation's
 * episode section and the logs keep task completion (terminated) and the horizon (truncated)
 * apart. */
#include "rs_gym_compact.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t mix(uint32_t seed, uint32_t episode) {
    uint32_t x = seed + 0x9E3779B9u * (episode + 1u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x ? x : 0x12345678u;
}

static void write_agents(Env* env) {
    for (int i = 0; i < env->num_agents; i++) {
        Agent* a = &env->agents[i];
        if (a->observations) rc_cenv_observe(env->env, i, a->observations);
        if (a->action_mask) rc_cenv_action_mask(env->env, i, a->action_mask);
    }
}

static int profile_named(const char* name) {
    static const char* names[RC_CENV_PROFILE_COUNT] = {"free_play", "solo_boss", "development", "coop_boss"};
    for (int i = 0; name && i < RC_CENV_PROFILE_COUNT; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

static void log_episode(Env* env, int agent) {
    const RcCenvMetrics* m = rc_cenv_metrics(env->env, agent);
    Log* log = &env->log;
    int terminated = rc_cenv_terminated(env->env, agent);
    log->perf += terminated ? 1.0f : 0.0f;
    log->score += m->boss_credits;
    log->episode_return += env->returns[agent];
    log->episode_length += rc_cenv_episode_ticks(env->env);
    log->terminated += terminated;
    log->truncated += rc_cenv_truncated(env->env, agent);
    float xp = 0;
    for (int s = 0; s < 32; s++) xp += m->xp_gained[s];
    log->xp_gained += xp;
    log->deaths += m->deaths;
    log->trades += m->trades;
    log->produced += m->produced;
    log->invalid_actions += m->results[RC_CENV_MASKED] + m->results[RC_CENV_OUT_OF_RANGE]
                          + m->results[RC_CENV_STALE];
    log->n += 1.0f;
}

void puf_init(Env* env, Dict* kwargs) {
    DictItem* item = dict_find(kwargs, "num_agents");
    env->num_agents = item ? (int)item->value : 1;
    if (env->num_agents < 1) env->num_agents = 1;
    if (env->num_agents > RC_CENV_MAX_AGENTS) env->num_agents = RC_CENV_MAX_AGENTS;
    item = dict_find(kwargs, "profile");
    int profile = item && item->str ? profile_named(item->str) : item ? (int)item->value : RC_CENV_SOLO_BOSS;
    if (profile < 0 || profile >= RC_CENV_PROFILE_COUNT) {
        fprintf(stderr, "rs_gym_compact: unknown profile\n");
        exit(1);
    }
    DictItem* horizon = dict_find(kwargs, "horizon");
    DictItem* mode = dict_find(kwargs, "mode");
    DictItem* map = dict_find(kwargs, "public_map");
    RcCenvConfig cfg = {
        .mode = mode && mode->str && mode->str[0] ? mode->str : NULL,
        .num_agents = env->num_agents, .profile = profile,
        .horizon = horizon ? (int)horizon->value : 0, .seed = env->rng,
        .public_map = map ? map->value != 0 : true,
    };
    item = dict_find(kwargs, "record");
    if (item && item->str && item->str[0]) cfg.record_path = item->str;
    env->env = rc_cenv_create(&cfg);
    if (!env->env) {
        fprintf(stderr, "rs_gym_compact: could not create the world (is RUNEC_DATA_ROOT set to "
                        "RS-gym's data directory, after tools/build_mode.py?)\n");
        exit(1);
    }
    env->episode = 0;
    env->tag = 0;
    env->boundary_reached = 0;
    for (int i = 0; i < env->num_agents; i++) {
        env->agents[i].policy = 0;
        env->agents[i].action_mask = NULL;
        env->returns[i] = 0;
    }
}

void puf_reset(Env* env) {
    env->episode++;
    rc_cenv_reset(env->env, mix(env->rng, env->episode));
    for (int i = 0; i < env->num_agents; i++) env->returns[i] = 0;
    write_agents(env);
}

void puf_step(Env* env) {
    for (int i = 0; i < env->num_agents; i++) {
        float a = env->agents[i].actions ? env->agents[i].actions[0] : 0.0f;
        rc_cenv_set_action(env->env, i, (int)a);
    }
    int done = rc_cenv_step(env->env);
    for (int i = 0; i < env->num_agents; i++) {
        Agent* a = &env->agents[i];
        float r = rc_cenv_reward(env->env, i);
        if (!isfinite(r)) r = 0.0f;
        env->returns[i] += r;
        if (a->rewards) a->rewards[0] = r;
        int ended = rc_cenv_terminated(env->env, i) || rc_cenv_truncated(env->env, i);
        if (a->terminals) a->terminals[0] = done ? 1.0f : 0.0f;
        if (done && ended) log_episode(env, i);
    }
    if (done) puf_reset(env);  /* same-step autoreset */
    else write_agents(env);
}

/* rc-viewer replays recorded episodes (record = path in the ini). */
void puf_render(Env* env) {
    (void)env;
}

void puf_close(Env* env) {
    rc_cenv_destroy(env->env);
    env->env = NULL;
}

void puf_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "terminated", log->terminated);
    dict_set(out, "truncated", log->truncated);
    dict_set(out, "xp_gained", log->xp_gained);
    dict_set(out, "deaths", log->deaths);
    dict_set(out, "trades", log->trades);
    dict_set(out, "produced", log->produced);
    dict_set(out, "invalid_actions", log->invalid_actions);
}
