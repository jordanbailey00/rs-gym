/* PufferLib 5.0 environment header for RS-gym (ocean/rs_gym/rs_gym.h once
 * tools/puffer_overlay.sh links it into a PufferLib checkout). Declarations
 * only: the implementation is C, compiled from rc-env/puffer/binding.c into
 * the rs_gym_puffer library the trainer links. */
#ifndef RS_GYM_PUFFER_H
#define RS_GYM_PUFFER_H

typedef float obs_t;

#ifdef __cplusplus
extern "C" {
#endif
#include "pufferenv.h"
#include "env.h"

#define OBS_SIZE RC_ENV_OBS_SIZE
#define NUM_ATNS 1
#define ACT_SIZES { RC_ENV_NUM_ACTIONS }

struct Log {
    float perf;              /* task score / task target, 0-1 */
    float score;             /* task metric (XP, coins, kills ...) */
    float episode_return;
    float episode_length;
    float xp_gained;
    float levels_gained;
    float net_worth_gained;
    float kills;
    float deaths;
    float boss_kills;
    float n;                 /* PufferLib episode count; must be last */
};

struct Env {
    Log log;                         /* required by PufferLib */
    Agent agents[RC_ENV_MAX_AGENTS]; /* buffers wired by PufferLib after puf_init */
    int num_agents;                  /* players in this env's world */
    unsigned int rng;                /* per-env seed from PufferLib */
    int tag;
    int boundary_reached;
    RcEnv* env;                      /* heap world: Env stays relocatable */
    int task;
    float perf_target;
    unsigned int episode;
};

#ifdef __cplusplus
}
#endif
#endif /* RS_GYM_PUFFER_H */
