// The PufferLib 5.0 binding (rc-env/puffer) against a stand-in pufferenv.h: buffers wired the way
// PufferLib's CPU path wires them, masked actions, clamped rewards, same-step autoreset, episode
// logs, and an Env that keeps working after it is moved in memory.
#include "rs_gym.h"

#include <math.h>
#include <sys/stat.h>

enum { SKIP = 77, AGENTS = 3, EPISODE = 120 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void wire(Env *env, obs_t *obs, float *actions, float *rewards, float *terminals,
                 unsigned char *masks) {
    int act_sizes[] = ACT_SIZES, act_n = 0;
    for (int i = 0; i < NUM_ATNS; i++) act_n += act_sizes[i];
    for (int i = 0; i < env->num_agents; i++) {
        env->agents[i].observations = obs + i * OBS_SIZE;
        env->agents[i].actions = actions + i * NUM_ATNS;
        env->agents[i].rewards = rewards + i;
        env->agents[i].terminals = terminals + i;
        env->agents[i].action_mask = masks + i * act_n;
    }
}

int main(void) {
    struct stat st;
    if (stat("data/regions/gym.cfg", &st) != 0) {
        fprintf(stderr, "gym data not built; run tools/build_gym.py\n");
        return SKIP;
    }
    Dict kwargs = {0};
    dict_set(&kwargs, "num_agents", AGENTS);
    dict_set(&kwargs, "episode_ticks", EPISODE);
    DictItem *task = &kwargs.items[kwargs.size++];
    snprintf(task->key, sizeof(task->key), "task");
    task->str = "woodcutting";

    Env *env = calloc(1, sizeof(Env));
    env->rng = 42;
    puf_init(env, &kwargs);
    CHECK(env->num_agents == AGENTS && env->task == RC_TASK_WOODCUTTING, "init: %d agents task %d",
          env->num_agents, env->task);

    static obs_t obs[AGENTS * OBS_SIZE];
    static float actions[AGENTS * NUM_ATNS], rewards[AGENTS], terminals[AGENTS];
    static unsigned char masks[AGENTS * RC_ENV_NUM_ACTIONS];
    wire(env, obs, actions, rewards, terminals, masks);
    puf_reset(env);

    uint32_t rng = 7;
    int terminal_steps = 0, bad = 0;
    for (int t = 0; t < 3 * EPISODE; t++) {
        if (t == EPISODE + 7) {
            // PufferLib may reallocate its Env array: the world lives on the heap, so a copy works.
            Env *moved = malloc(sizeof(Env));
            memcpy(moved, env, sizeof(Env));
            memset(env, 0xAB, sizeof(Env));
            free(env);
            env = moved;
        }
        for (int i = 0; i < AGENTS; i++) {
            unsigned char *m = masks + i * RC_ENV_NUM_ACTIONS;
            int valid[RC_ENV_NUM_ACTIONS], n = 0;
            for (int a = 0; a < RC_ENV_NUM_ACTIONS; a++)
                if (m[a]) valid[n++] = a;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            actions[i] = n ? (float)valid[rng % (uint32_t)n] : 0.0f;
        }
        puf_step(env);
        for (int i = 0; i < AGENTS; i++) {
            if (!(rewards[i] >= -1.0f && rewards[i] <= 1.0f)) bad++;
            if (terminals[i] != 0.0f && terminals[i] != 1.0f) bad++;
            for (int k = 0; k < OBS_SIZE; k++)
                if (!isfinite(obs[i * OBS_SIZE + k])) bad++;
        }
        if (terminals[0] == 1.0f) {
            terminal_steps++;
            CHECK((t + 1) % EPISODE == 0, "episode ended at step %d", t + 1);
            CHECK(terminals[1] == 1.0f && terminals[2] == 1.0f, "agents ended apart");
            CHECK(obs[OBS_SIZE * 0 + RC_OBS_TASK + 10] == 0.0f, "no autoreset after the episode");
        }
    }
    CHECK(bad == 0, "%d out-of-range rewards, terminals or observations", bad);
    CHECK(terminal_steps == 3, "%d episode ends in %d steps", terminal_steps, 3 * EPISODE);
    CHECK(env->log.n == 3 * AGENTS, "log counted %.0f agent episodes", env->log.n);
    CHECK(env->log.episode_length == 3 * AGENTS * EPISODE, "logged length %.0f", env->log.episode_length);
    Dict out = {0};
    puf_log(&env->log, &out);
    CHECK(dict_find(&out, "perf") && dict_find(&out, "score") && dict_find(&out, "xp_gained"),
          "missing log keys");
    puf_render(env);
    puf_close(env);
    free(env);
    if (failures) return 1;
    printf("puffer binding: PASS\n");
    return 0;
}
