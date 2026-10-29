// rs-gym-bench: agent steps per second for masked random play, including the
// observation and mask every agent gets each step. One env per thread.
//
//   rs-gym-bench [--envs N] [--agents M] [--threads T] [--steps S] [--task NAME]
#include "../env.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    RcEnv *env;
    int steps;
    uint32_t rng;
    long agent_steps;
} Worker;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint32_t next(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void *run(void *arg) {
    Worker *wk = arg;
    int agents = rc_env_num_agents(wk->env);
    float *obs = malloc(RC_ENV_OBS_SIZE * sizeof(float));
    uint8_t mask[RC_ENV_NUM_ACTIONS];
    int valid[RC_ENV_NUM_ACTIONS];
    for (int t = 0; t < wk->steps; t++) {
        for (int i = 0; i < agents; i++) {
            rc_env_observe(wk->env, i, obs);
            rc_env_action_mask(wk->env, i, mask);
            int n = 0;
            for (int a = 0; a < RC_ENV_NUM_ACTIONS; a++)
                if (mask[a]) valid[n++] = a;
            rc_env_set_action(wk->env, i, n ? valid[next(&wk->rng) % (uint32_t)n] : 0);
        }
        if (rc_env_step(wk->env)) rc_env_reset(wk->env, next(&wk->rng));
        wk->agent_steps += agents;
    }
    free(obs);
    return NULL;
}

int main(int argc, char **argv) {
    int envs = 1, agents = 1, threads = 1, steps = 5000;
    const char *task = "total";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--envs")) envs = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--agents")) agents = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) threads = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--steps")) steps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--task")) task = argv[i + 1];
    }
    if (threads > envs) threads = envs;
    int task_id = rc_env_task_find(task);
    if (task_id < 0 || envs < 1 || threads < 1) {
        fprintf(stderr, "usage: rs-gym-bench [--envs N] [--agents M] [--threads T] [--steps S] [--task NAME]\n");
        return 2;
    }
    Worker *workers = calloc((size_t)envs, sizeof(*workers));
    for (int i = 0; i < envs; i++) {
        RcEnvConfig cfg = {.num_agents = agents, .task = task_id, .seed = (uint32_t)i + 1};
        workers[i].env = rc_env_create(&cfg);
        if (!workers[i].env) return 1;
        workers[i].steps = steps;
        workers[i].rng = 2654435761u * (uint32_t)(i + 1);
    }
    double start = now();
    // Threads take envs round-robin in batches of `threads`.
    for (int base = 0; base < envs; base += threads) {
        pthread_t tid[256];
        int n = envs - base < threads ? envs - base : threads;
        for (int t = 0; t < n; t++) pthread_create(&tid[t], NULL, run, &workers[base + t]);
        for (int t = 0; t < n; t++) pthread_join(tid[t], NULL);
    }
    double seconds = now() - start;
    long total = 0;
    for (int i = 0; i < envs; i++) total += workers[i].agent_steps;
    printf("task %s: %d envs x %d agents, %d threads: %.0f agent steps/s (%.0f per thread)\n",
           task, envs, agents, threads, total / seconds, total / seconds / threads);
    for (int i = 0; i < envs; i++) rc_env_destroy(workers[i].env);
    free(workers);
    return 0;
}
