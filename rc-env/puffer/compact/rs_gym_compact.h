/* PufferLib 5.0 environment header for RS-gym's compact_osrs_v1 mode, schema compact-v2
 * (ocean/rs_gym_compact/rs_gym_compact.h once tools/puffer_overlay.sh links it into a PufferLib
 * checkout). Declarations only: the implementation is rc-env/puffer/compact/binding.c, compiled
 * into the rs_gym_compact_puffer library the trainer links. */
#ifndef RS_GYM_COMPACT_PUFFER_H
#define RS_GYM_COMPACT_PUFFER_H

typedef float obs_t;

#ifdef __cplusplus
extern "C" {
#endif
#include "pufferenv.h"
#include "compact_env.h"

#define OBS_SIZE RC_CENV_OBS_SIZE
#define NUM_ATNS 1
#define ACT_SIZES { RC_CENV_NUM_ACTIONS }

struct Log {
    float perf;              /* task success rate (boss profiles), else 0 */
    float score;             /* boss kill credits */
    float episode_return;
    float episode_length;
    float terminated;        /* episodes that ended by task completion */
    float truncated;         /* episodes that ended at the horizon */
    float xp_gained;
    float deaths;
    float trades;
    float produced;
    float invalid_actions;   /* masked, out-of-range or stale actions */
    float n;                 /* PufferLib episode count; must be last */
};

struct Env {
    Log log;                             /* required by PufferLib */
    Agent agents[RC_CENV_MAX_AGENTS];    /* buffers wired by PufferLib after puf_init */
    int num_agents;
    unsigned int rng;                    /* per-env seed from PufferLib */
    int tag;
    int boundary_reached;
    RcCompactEnv* env;                   /* heap world: Env stays relocatable */
    float returns[RC_CENV_MAX_AGENTS];
    unsigned int episode;
};

#ifdef __cplusplus
}
#endif
#endif /* RS_GYM_COMPACT_PUFFER_H */
