#ifndef RC_RNG_H
#define RC_RNG_H

#include <stdint.h>

#define RC_DEFAULT_SEED UINT32_C(0x6d2b79f5)

// XORshift32 — deterministic, fast, single uint32 state
static inline uint32_t rc_rng_next(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

// Uniform random integer in [0, bound). A zero bound has no valid member and
// returns zero. Rejection removes modulo bias for non-power-of-two bounds.
static inline uint32_t rc_rng_bounded(uint32_t *state, uint32_t bound) {
    if (!state || bound == 0) return 0;
    uint32_t threshold = (uint32_t)(-bound) % bound;
    for (;;) {
        uint32_t value = rc_rng_next(state);
        if (value >= threshold) return value % bound;
    }
}

// Random int in [0, max] inclusive
static inline int rc_rng_range(uint32_t *state, int max) {
    if (!state || max <= 0) return 0;
    uint32_t bound = (uint32_t)max + 1u;
    return (int)rc_rng_bounded(state, bound);
}

#endif
