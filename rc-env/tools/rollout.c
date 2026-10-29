// rs-gym-rollout: plays one episode of a task with a random or scripted policy,
// prints each agent's episode summary, and can record it for rc-viewer --replay.
//
//   rs-gym-rollout [--task NAME] [--agents N] [--ticks T] [--seed S]
//                  [--policy random|scripted] [--record FILE]
//   rs-gym-rollout --mode compact_osrs_v1 [--agents N] [--ticks T] [--seed S] [--record FILE]
//
// --mode plays the compact-v2 interface (rc-env/compact_env.h) with random masked actions in the
// development profile and prints each agent's metrics.
// The scripted policy power-skills for woodcutting, mining and fishing: it walks
// to the resource, gathers until the inventory is full, drops what it gathered
// and goes again. Other tasks fall back to random play.
#include "../compact_env.h"
#include "../env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LM_MINE = 9, LM_TREES = 10, LM_NET_FISHING = 13 };

static float obs[RC_ENV_OBS_SIZE];
static uint8_t mask[RC_ENV_NUM_ACTIONS];

static uint32_t next(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static int random_action(uint32_t *rng) {
    int valid[RC_ENV_NUM_ACTIONS], n = 0;
    for (int a = 1; a < RC_ENV_NUM_ACTIONS; a++)
        if (mask[a]) valid[n++] = a;
    return n && next(rng) % 8 ? valid[next(rng) % (uint32_t)n] : RC_ACT_NOOP;
}

// Inventory item category (rc-env item features: index 1 + category).
static int slot_category(int slot) {
    const float *f = obs + RC_OBS_INVENTORY + slot * RC_OBS_ITEM;
    if (f[0] <= 0) return -1;
    for (int c = 0; c < 16; c++)
        if (f[1 + c] > 0) return c;
    return -1;
}

static int scripted_action(int task, uint32_t *rng) {
    // What to gather, where, and what to drop when full (item categories: raw food 2, logs 3, ore 4).
    int object_kind = -1, landmark, drop_category;
    if (task == RC_TASK_WOODCUTTING) object_kind = 0, landmark = LM_TREES, drop_category = 3;
    else if (task == RC_TASK_MINING) object_kind = 1, landmark = LM_MINE, drop_category = 4;
    else if (task == RC_TASK_FISHING) landmark = LM_NET_FISHING, drop_category = 2;
    else return random_action(rng);

    int busy = obs[RC_OBS_SELF + 24] == 0 || obs[RC_OBS_SELF + 41] > 0;  // skilling or walking
    if (obs[RC_OBS_SELF + 35] == 0) {  // inventory full: drop the catch
        for (int i = 0; i < 28; i++)
            if (slot_category(i) == drop_category && mask[RC_ACT_DROP + i]) return RC_ACT_DROP + i;
    }
    if (busy) return RC_ACT_NOOP;
    if (object_kind >= 0) {
        for (int k = 0; k < RC_OBS_NEAR_OBJECTS; k++) {
            const float *f = obs + RC_OBS_OBJECTS + k * RC_OBS_OBJECT;
            if (f[0] > 0 && f[4 + object_kind] > 0 && mask[RC_ACT_OBJECT_OP + k * 3])
                return RC_ACT_OBJECT_OP + k * 3;
        }
    } else {
        for (int k = 0; k < RC_OBS_NEAR_NPCS; k++) {
            const float *f = obs + RC_OBS_NPCS + k * RC_OBS_NPC;
            if (f[0] > 0 && f[10] > 0 && mask[RC_ACT_NPC_OP + k * 2]) return RC_ACT_NPC_OP + k * 2;
        }
    }
    return mask[RC_ACT_TRAVEL + landmark] ? RC_ACT_TRAVEL + landmark : random_action(rng);
}

static int compact_rollout(const char *mode, int agents, int ticks, uint32_t seed, const char *record) {
    static float cobs[RC_CENV_OBS_SIZE];
    static uint8_t cmask[RC_CENV_NUM_ACTIONS];
    RcCenvConfig cfg = {.mode = mode, .num_agents = agents, .profile = RC_CENV_DEVELOPMENT,
                        .horizon = ticks, .seed = seed, .public_map = true, .record_path = record};
    RcCompactEnv *env = rc_cenv_create(&cfg);
    if (!env) return 1;
    uint32_t rng = seed * 2654435761u + 7;
    int done = 0;
    while (!done) {
        for (int i = 0; i < agents; i++) {
            rc_cenv_observe(env, i, cobs);
            rc_cenv_action_mask(env, i, cmask);
            int valid[RC_CENV_NUM_ACTIONS], n = 0;
            for (int k = 0; k < RC_CENV_NUM_ACTIONS; k++) if (cmask[k]) valid[n++] = k;
            rc_cenv_set_action(env, i, n ? valid[next(&rng) % (uint32_t)n] : 0);
        }
        rc_cenv_step(env);
        done = 1;
        for (int i = 0; i < agents; i++) done &= rc_cenv_terminated(env, i) || rc_cenv_truncated(env, i);
    }
    for (int i = 0; i < agents; i++) {
        const RcCenvMetrics *m = rc_cenv_metrics(env, i);
        int xp = 0;
        for (int k = 0; k < 32; k++) xp += m->xp_gained[k];
        printf("agent %d: xp %d kills %d deaths %d produced %d consumed %d loot %lld spent %lld\n", i, xp,
               m->kills, m->deaths, m->produced, m->consumed, (long long)m->coins_from_loot,
               (long long)m->coins_spent_shops);
    }
    printf("ticks %d hash %016llx\n", rc_cenv_episode_ticks(env), (unsigned long long)rc_cenv_state_hash(env));
    rc_cenv_destroy(env);
    if (record) printf("recorded %s (view: ./build/cmake/rc-viewer --replay %s)\n", record, record);
    return 0;
}

int main(int argc, char **argv) {
    const char *task_name = "woodcutting", *policy = "scripted", *record = NULL, *mode = NULL;
    int agents = 1, ticks = 0;
    uint32_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--task")) task_name = argv[i + 1];
        else if (!strcmp(argv[i], "--agents")) agents = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--ticks")) ticks = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seed")) seed = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--policy")) policy = argv[i + 1];
        else if (!strcmp(argv[i], "--record")) record = argv[i + 1];
        else if (!strcmp(argv[i], "--mode")) mode = argv[i + 1];
    }
    if (mode) {
        if (agents < 1 || agents > RC_CENV_MAX_AGENTS) return 2;
        return compact_rollout(mode, agents, ticks ? ticks : 3000, seed, record);
    }
    int task = rc_env_task_find(task_name);
    if (task < 0 || agents < 1 || agents > RC_ENV_MAX_AGENTS) {
        fprintf(stderr, "usage: rs-gym-rollout [--task NAME] [--agents N] [--ticks T] [--seed S] "
                        "[--policy random|scripted] [--record FILE]\n");
        return 2;
    }
    RcEnvConfig cfg = {.num_agents = agents, .task = task, .episode_ticks = ticks,
                       .seed = seed, .record_path = record};
    RcEnv *env = rc_env_create(&cfg);
    if (!env) return 1;
    int scripted = !strcmp(policy, "scripted");
    uint32_t rng = seed * 2654435761u + 7;
    RcEnvEpisodeStats stats[RC_ENV_MAX_AGENTS];
    int done = 0;
    while (!done) {
        for (int i = 0; i < agents; i++) {
            rc_env_observe(env, i, obs);
            rc_env_action_mask(env, i, mask);
            rc_env_set_action(env, i, scripted ? scripted_action(task, &rng) : random_action(&rng));
        }
        done = rc_env_step(env);
    }
    for (int i = 0; i < agents; i++) {
        if (!rc_env_episode_stats(env, i, &stats[i])) continue;
        printf("agent %d: score %.1f return %.2f length %.0f xp %.0f levels %.0f worth %+.0f "
               "kills %.0f deaths %.0f\n", i, stats[i].score, stats[i].episode_return,
               stats[i].episode_length, stats[i].xp_gained, stats[i].levels_gained,
               stats[i].net_worth_gained, stats[i].kills, stats[i].deaths);
    }
    rc_env_destroy(env);
    if (record) printf("recorded %s (view: ./build/cmake/rc-viewer --replay %s)\n", record, record);
    return 0;
}
