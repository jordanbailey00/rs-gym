// The compact-v2 PufferLib 5.0 binding (rc-env/puffer/compact) against the stand-in pufferenv.h:
// buffers wired as PufferLib's CPU path wires them, masked random actions, the development
// profile's horizon ending every agent's episode with an autoreset in the same step, the logs,
// and an Env that keeps working after it moves in memory.
#include "rs_gym_compact.h"

#include <math.h>
#include <sys/stat.h>

enum { SKIP = 77, AGENTS = 2, HORIZON = 80 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static obs_t obs[AGENTS * OBS_SIZE];
static float actions[AGENTS], rewards[AGENTS], terminals[AGENTS];
static unsigned char masks[AGENTS * RC_CENV_NUM_ACTIONS];

int main(void) {
    struct stat st;
    if (stat("data/modes/compact_osrs_v1/gym.cfg", &st) != 0) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    Dict kwargs = {0};
    dict_set(&kwargs, "num_agents", AGENTS);
    dict_set(&kwargs, "horizon", HORIZON);
    DictItem *profile = &kwargs.items[kwargs.size++];
    snprintf(profile->key, sizeof(profile->key), "profile");
    profile->str = "development";
    Env *env = calloc(1, sizeof(Env));
    env->rng = 9;
    puf_init(env, &kwargs);
    for (int i = 0; i < AGENTS; i++) {
        env->agents[i].observations = obs + i * OBS_SIZE;
        env->agents[i].actions = actions + i;
        env->agents[i].rewards = rewards + i;
        env->agents[i].terminals = terminals + i;
        env->agents[i].action_mask = masks + i * RC_CENV_NUM_ACTIONS;
    }
    puf_reset(env);
    uint32_t rng = 3;
    int ends = 0, bad = 0;
    for (int t = 0; t < 3 * HORIZON; t++) {
        if (t == HORIZON + 5) {  // PufferLib may move its Env array: the world lives on the heap
            Env *moved = malloc(sizeof(Env));
            memcpy(moved, env, sizeof(Env));
            memset(env, 0xAB, sizeof(Env));
            free(env);
            env = moved;
        }
        for (int i = 0; i < AGENTS; i++) {
            unsigned char *m = masks + i * RC_CENV_NUM_ACTIONS;
            int valid[RC_CENV_NUM_ACTIONS], n = 0;
            for (int a = 0; a < RC_CENV_NUM_ACTIONS; a++) if (m[a]) valid[n++] = a;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            actions[i] = n ? (float)valid[rng % (uint32_t)n] : 0.0f;
        }
        puf_step(env);
        for (int i = 0; i < AGENTS * OBS_SIZE; i++) bad += !isfinite(obs[i]);
        if (terminals[0] == 1.0f) {
            ends++;
            CHECK((t + 1) % HORIZON == 0, "episode ended at step %d", t + 1);
            CHECK(terminals[1] == 1.0f, "agents ended apart");
        }
    }
    CHECK(bad == 0, "%d non-finite observation values", bad);
    CHECK(ends == 3 && env->log.n == 3 * AGENTS && env->log.truncated == 3 * AGENTS
          && env->log.terminated == 0, "ends %d, logged %.0f episodes (%.0f truncated)", ends,
          env->log.n, env->log.truncated);
    Dict out = {0};
    puf_log(&env->log, &out);
    CHECK(dict_find(&out, "terminated") && dict_find(&out, "truncated") && dict_find(&out, "invalid_actions"),
          "missing log keys");
    puf_close(env);
    free(env);
    if (failures) return 1;
    printf("puffer compact binding: PASS\n");
    return 0;
}
