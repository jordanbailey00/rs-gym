// rc-env (run tools/build_mode.py island first): masked random rollouts stay valid and finite, episodes
// replay exactly from a seed, a scripted woodcutter earns reward, multi-agent episodes end
// together, and death ends a boss episode.
#include "../rc-core/types.h"
#include "../rc-env/env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { SKIP = 77 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static float obs[RC_ENV_MAX_AGENTS][RC_ENV_OBS_SIZE];
static uint8_t mask[RC_ENV_MAX_AGENTS][RC_ENV_NUM_ACTIONS];

static uint32_t rng_next(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// A random valid action, preferring to keep doing things (NOOP rarely).
static int random_action(const uint8_t *m, uint32_t *rng) {
    int valid[RC_ENV_NUM_ACTIONS], n = 0;
    for (int a = 1; a < RC_ENV_NUM_ACTIONS; a++)
        if (m[a]) valid[n++] = a;
    if (!n || rng_next(rng) % 8 == 0) return RC_ACT_NOOP;
    return valid[rng_next(rng) % (uint32_t)n];
}

static uint64_t hash_obs(const float *o, uint64_t h) {
    const uint8_t *b = (const uint8_t *)o;
    for (size_t i = 0; i < RC_ENV_OBS_SIZE * sizeof(float); i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// Steps `steps` ticks of masked random play; returns a hash of every observation and reward.
static uint64_t rollout(RcEnv *env, uint32_t seed, int steps, int *invalid, int *nonfinite) {
    uint32_t rng = seed * 2654435761u + 1;
    uint64_t h = 1469598103934665603ull;
    int agents = rc_env_num_agents(env);
    rc_env_reset(env, seed);
    for (int t = 0; t < steps; t++) {
        for (int i = 0; i < agents; i++) {
            rc_env_observe(env, i, obs[i]);
            rc_env_action_mask(env, i, mask[i]);
            if (!mask[i][RC_ACT_NOOP]) (*invalid)++;
            int a = random_action(mask[i], &rng);
            if (!mask[i][a]) (*invalid)++;
            rc_env_set_action(env, i, a);
            h = hash_obs(obs[i], h);
        }
        int done = rc_env_step(env);
        for (int i = 0; i < agents; i++) {
            float r = rc_env_reward(env, i);
            if (!isfinite(r)) (*nonfinite)++;
            uint32_t bits;
            memcpy(&bits, &r, sizeof(bits));
            h = (h ^ bits) * 1099511628211ull;
        }
        for (int i = 0; i < agents; i++)
            for (int k = 0; k < RC_ENV_OBS_SIZE; k++)
                if (!isfinite(obs[i][k])) (*nonfinite)++;
        if (done) rc_env_reset(env, seed + (uint32_t)t);
    }
    return h;
}

static void random_play_and_determinism(void) {
    RcEnvConfig cfg = {.mode = "island", .num_agents = 2, .task = RC_TASK_TOTAL, .seed = 11};
    RcEnv *a = rc_env_create(&cfg), *b = rc_env_create(&cfg);
    CHECK(a && b, "env creation");
    if (!a || !b) return;
    int invalid = 0, nonfinite = 0;
    uint64_t h1 = rollout(a, 5, 1500, &invalid, &nonfinite);
    uint64_t h2 = rollout(b, 5, 1500, &invalid, &nonfinite);
    uint64_t h3 = rollout(a, 5, 1500, &invalid, &nonfinite);  // same env, after other episodes
    CHECK(invalid == 0, "%d invalid masked choices", invalid);
    CHECK(nonfinite == 0, "%d non-finite values", nonfinite);
    CHECK(h1 == h2 && h1 == h3, "rollouts differ: %llx %llx %llx",
          (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)h3);
    uint64_t other = rollout(b, 6, 300, &invalid, &nonfinite);
    CHECK(other != h1, "a different seed gave the same rollout");
    rc_env_destroy(a);
    rc_env_destroy(b);
}

static int nearest_object_of_kind(const float *o, int kind) {
    for (int k = 0; k < RC_OBS_NEAR_OBJECTS; k++) {
        const float *f = o + RC_OBS_OBJECTS + k * RC_OBS_OBJECT;
        if (f[0] > 0 && f[4 + kind] > 0 && f[13] > 0) return k;
    }
    return -1;
}

static void scripted_woodcutter(void) {
    RcEnvConfig cfg = {.mode = "island", .num_agents = 1, .task = RC_TASK_WOODCUTTING, .seed = 3};
    RcEnv *env = rc_env_create(&cfg);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    float total = 0;
    int travel_trees = RC_ACT_TRAVEL + 10;  // landmark "trees"
    for (int t = 0; t < 600; t++) {
        rc_env_observe(env, 0, obs[0]);
        rc_env_action_mask(env, 0, mask[0]);
        int busy = obs[0][RC_OBS_SELF + 24] == 0;  // a skilling job is running
        int tree = nearest_object_of_kind(obs[0], 0);
        int action = RC_ACT_NOOP;
        if (!busy && tree >= 0 && mask[0][RC_ACT_OBJECT_OP + tree * 3])
            action = RC_ACT_OBJECT_OP + tree * 3;
        else if (!busy && tree < 0 && obs[0][RC_OBS_SELF + 41] == 0 && mask[0][travel_trees])
            action = travel_trees;
        rc_env_set_action(env, 0, action);
        rc_env_step(env);
        total += rc_env_reward(env, 0);
    }
    const RcWorld *w = rc_env_world(env);
    CHECK(w->players[0].skills.xp[SKILL_WOODCUTTING] > 0, "no woodcutting xp after 600 ticks");
    CHECK(total > 0.05f, "woodcutting return %.3f", total);
    rc_env_destroy(env);
}

static void multi_agent_episode(void) {
    RcEnvConfig cfg = {.mode = "island", .num_agents = 4, .task = RC_TASK_TOTAL, .episode_ticks = 200, .seed = 9};
    RcEnv *env = rc_env_create(&cfg);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    uint32_t rng = 77;
    int done = 0, t = 0;
    for (; t < 400 && !done; t++) {
        for (int i = 0; i < 4; i++) {
            rc_env_action_mask(env, i, mask[i]);
            rc_env_set_action(env, i, random_action(mask[i], &rng));
        }
        done = rc_env_step(env);
    }
    CHECK(done && t == 200, "episode ended at tick %d (done %d)", t, done);
    for (int i = 0; i < 4; i++) {
        RcEnvEpisodeStats stats;
        CHECK(rc_env_terminal(env, i) && rc_env_episode_stats(env, i, &stats), "agent %d not terminal", i);
        CHECK(stats.episode_length == 200, "agent %d length %.0f", i, stats.episode_length);
    }
    rc_env_observe(env, 0, obs[0]);
    rc_env_observe(env, 1, obs[1]);
    CHECK(memcmp(obs[0], obs[1], sizeof(obs[0])) != 0, "agents see the same observation");
    rc_env_destroy(env);
}

static void boss_death_ends_episode(void) {
    RcEnvConfig cfg = {.mode = "island", .num_agents = 1, .task = RC_TASK_KBD, .seed = 1};
    RcEnv *env = rc_env_create(&cfg);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    RcWorld *w = rc_env_world(env);
    int terminal_at = -1;
    for (int t = 0; t < 1500 && terminal_at < 0; t++) {
        w->players[0].current_hp = w->players[0].current_hp > 40 ? 40 : w->players[0].current_hp;
        rc_env_observe(env, 0, obs[0]);
        rc_env_action_mask(env, 0, mask[0]);
        int action = RC_ACT_NOOP;
        for (int k = 0; k < RC_OBS_NEAR_NPCS && !action; k++)  // fight the dragon at 4 hitpoints
            if (obs[0][RC_OBS_NPCS + k * RC_OBS_NPC + 9] > 0 && mask[0][RC_ACT_ATTACK + k])
                action = RC_ACT_ATTACK + k;
        rc_env_set_action(env, 0, action);
        rc_env_step(env);
        if (rc_env_terminal(env, 0)) terminal_at = t;
    }
    RcEnvEpisodeStats stats;
    CHECK(terminal_at >= 0, "agent never died in the KBD lair");
    CHECK(rc_env_episode_stats(env, 0, &stats) && stats.deaths == 1, "death not counted");
    CHECK(!w->players[0].is_dead && w->players[0].current_hp == w->players[0].max_hp,
          "agent not back for a new life");
    rc_env_destroy(env);
}

// A recorded episode replays tick for tick in a fresh env.
static void record_and_replay(void) {
    char path[512];
    const char *tmp = getenv("RS_GYM_TEST_TMP");  // ctest: the build directory
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/test_env.rgr", tmp && tmp[0] ? tmp : "/tmp");
    RcEnvConfig cfg = {.mode = "island", .num_agents = 3, .task = RC_TASK_TOTAL, .seed = 21, .record_path = path};
    RcEnv *env = rc_env_create(&cfg);
    CHECK(env != NULL, "env creation");
    if (!env) return;
    rc_env_reset(env, 1234);  // the recording is of this episode
    uint32_t rng = 99;
    int xs[400][3], ys[400][3];
    const RcWorld *w = rc_env_world(env);
    for (int t = 0; t < 400; t++) {
        for (int i = 0; i < 3; i++) {
            rc_env_action_mask(env, i, mask[i]);
            rc_env_set_action(env, i, random_action(mask[i], &rng));
        }
        rc_env_step(env);
        for (int i = 0; i < 3; i++) {
            xs[t][i] = w->players[i].x;
            ys[t][i] = w->players[i].y;
        }
    }
    rc_env_destroy(env);  // flushes the recording

    RcReplayHeader header;
    FILE *f = rc_env_replay_open(path, &header);
    CHECK(f && header.num_agents == 3 && header.seed == 1234 && header.ticks == 400,
          "replay header: agents %d seed %u ticks %u", header.num_agents, header.seed, header.ticks);
    if (!f) return;
    RcEnvConfig replay_cfg = {.mode = header.mode, .num_agents = header.num_agents, .task = header.task,
                              .episode_ticks = header.episode_ticks, .seed = header.seed};
    RcEnv *replay = rc_env_create(&replay_cfg);
    const RcWorld *rw = rc_env_world(replay);
    int actions[RC_ENV_MAX_AGENTS], t = 0, diverged = -1;
    while (rc_env_replay_next(f, &header, actions)) {
        for (int i = 0; i < header.num_agents; i++) rc_env_set_action(replay, i, actions[i]);
        rc_env_step(replay);
        for (int i = 0; i < 3 && diverged < 0; i++)
            if (rw->players[i].x != xs[t][i] || rw->players[i].y != ys[t][i]) diverged = t;
        t++;
    }
    fclose(f);
    CHECK(t == 400, "replayed %d of 400 ticks", t);
    CHECK(diverged < 0, "replay diverged at tick %d", diverged);
    rc_env_destroy(replay);
}

int main(void) {
    struct stat st;
    if (stat("data/modes/island/gym.cfg", &st) != 0) {
        fprintf(stderr, "gym data not built; run tools/build_mode.py island\n");
        return SKIP;
    }
    printf("obs %d floats, %d actions\n", RC_ENV_OBS_SIZE, RC_ENV_NUM_ACTIONS);
    random_play_and_determinism();
    scripted_woodcutter();
    multi_agent_episode();
    boss_death_ends_episode();
    record_and_replay();
    if (failures) return 1;
    printf("env: PASS\n");
    return 0;
}
