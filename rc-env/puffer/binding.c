/* PufferLib 5.0 entry points for RS-gym: puf_init/reset/step/render/close/log
 * over rc-env. One PufferLib env is one gym world holding num_agents players;
 * episodes autoreset in the step that ends them. */
#include "rs_gym.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* What "perf" divides the task score by. */
static float task_target(int task) {
    switch (task) {
    case RC_TASK_TOTAL: return 30.0f;            /* levels gained */
    case RC_TASK_WEALTH: return 5000.0f;         /* coins of net worth */
    case RC_TASK_KBD:
    case RC_TASK_CHAOS_ELEMENTAL: return 1.0f;   /* boss kills */
    case RC_TASK_SMITHING: return 3000.0f;       /* XP */
    default: return 5000.0f;                     /* XP */
    }
}

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
        if (a->observations) rc_env_observe(env->env, i, a->observations);
        if (a->action_mask) rc_env_action_mask(env->env, i, a->action_mask);
    }
}

static void log_episode(Env* env, int agent) {
    RcEnvEpisodeStats s;
    if (!rc_env_episode_stats(env->env, agent, &s)) return;
    Log* log = &env->log;
    float perf = env->perf_target > 0 ? s.score / env->perf_target : 0.0f;
    log->perf += perf < 0 ? 0 : perf > 1 ? 1 : perf;
    log->score += s.score;
    log->episode_return += s.episode_return;
    log->episode_length += s.episode_length;
    log->xp_gained += s.xp_gained;
    log->levels_gained += s.levels_gained;
    log->net_worth_gained += s.net_worth_gained;
    log->kills += s.kills;
    log->deaths += s.deaths;
    log->boss_kills += s.boss_kills;
    log->n += 1.0f;
}

void puf_init(Env* env, Dict* kwargs) {
    DictItem* item = dict_find(kwargs, "num_agents");
    env->num_agents = item ? (int)item->value : 1;
    if (env->num_agents < 1) env->num_agents = 1;
    if (env->num_agents > RC_ENV_MAX_AGENTS) env->num_agents = RC_ENV_MAX_AGENTS;
    item = dict_find(kwargs, "task");
    env->task = item && item->str ? rc_env_task_find(item->str) : item ? (int)item->value : 0;
    if (env->task < 0 || env->task >= RC_TASK_COUNT) {
        fprintf(stderr, "rs_gym: unknown task\n");
        exit(1);
    }
    item = dict_find(kwargs, "episode_ticks");
    RcEnvConfig cfg = {
        .num_agents = env->num_agents, .task = env->task,
        .episode_ticks = item ? (int)item->value : 0, .seed = env->rng,
    };
    item = dict_find(kwargs, "record");
    if (item && item->str && item->str[0]) cfg.record_path = item->str;
    env->env = rc_env_create(&cfg);
    if (!env->env) {
        fprintf(stderr, "rs_gym: could not create the gym world (is RUNEC_DATA_ROOT set "
                        "to RS-gym's data directory, after tools/build_gym.py?)\n");
        exit(1);
    }
    env->perf_target = task_target(env->task);
    env->episode = 0;
    env->tag = 0;
    env->boundary_reached = 0;
    for (int i = 0; i < env->num_agents; i++) {
        env->agents[i].policy = 0;
        env->agents[i].action_mask = NULL;
    }
}

void puf_reset(Env* env) {
    env->episode++;
    rc_env_reset(env->env, mix(env->rng, env->episode));
    write_agents(env);
}

void puf_step(Env* env) {
    for (int i = 0; i < env->num_agents; i++) {
        float a = env->agents[i].actions ? env->agents[i].actions[0] : 0.0f;
        rc_env_set_action(env->env, i, (int)a);
    }
    int done = rc_env_step(env->env);
    for (int i = 0; i < env->num_agents; i++) {
        Agent* a = &env->agents[i];
        float r = rc_env_reward(env->env, i);
        if (!isfinite(r)) r = 0.0f;
        if (a->rewards) a->rewards[0] = r < -1.0f ? -1.0f : r > 1.0f ? 1.0f : r;
        int terminal = rc_env_terminal(env->env, i);
        if (a->terminals) a->terminals[0] = (float)terminal;
        if (terminal) log_episode(env, i);
    }
    if (done) puf_reset(env);  /* same-step autoreset */
    else write_agents(env);
}

/* The RS-gym viewer replays recorded episodes (record = path in the ini). */
void puf_render(Env* env) {
    (void)env;
}

void puf_close(Env* env) {
    rc_env_destroy(env->env);
    env->env = NULL;
}

void puf_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "xp_gained", log->xp_gained);
    dict_set(out, "levels_gained", log->levels_gained);
    dict_set(out, "net_worth_gained", log->net_worth_gained);
    dict_set(out, "kills", log->kills);
    dict_set(out, "deaths", log->deaths);
    dict_set(out, "boss_kills", log->boss_kills);
}
