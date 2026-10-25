#include "health_bars.h"

#include <stdint.h>
#include <string.h>

void runec_health_bar_reset(RuneCHealthBarState *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void runec_health_bar_advance(RuneCHealthBarState *state,
                              float client_cycles) {
    if (!state || client_cycles <= 0.0f || state->remaining_cycles <= 0.0f)
        return;
    state->remaining_cycles -= client_cycles;
    if (state->remaining_cycles < 0.0f)
        state->remaining_cycles = 0.0f;
}

void runec_health_bar_sync(RuneCHealthBarState *state,
                           const RcCombatRecentHit *hits, int hit_count,
                           int current_hp, int maximum_hp) {
    if (!state) return;
    if (!hits) hit_count = 0;
    if (hit_count > 4) hit_count = 4;

    int received_update = 0;
    for (int i = 0; i < hit_count; i++) {
        uint64_t sequence = hits[i].sequence;
        if (sequence == 0 || sequence <= state->last_sequence) continue;
        state->last_sequence = sequence;
        received_update = 1;
    }
    if (maximum_hp <= 0 || (!received_update && !runec_health_bar_visible(state))) return;

    if (current_hp < 0) current_hp = 0;
    if (current_hp > maximum_hp) current_hp = maximum_hp;
    state->current_hp = current_hp;
    state->maximum_hp = maximum_hp;
    if (received_update) state->remaining_cycles = RUNEC_HEALTH_BAR_DISPLAY_CYCLES;
}

int runec_health_bar_visible(const RuneCHealthBarState *state) {
    return state && state->remaining_cycles > 0.0f && state->maximum_hp > 0;
}

int runec_health_bar_fill_width(const RuneCHealthBarState *state) {
    if (!state || state->maximum_hp <= 0 || state->current_hp <= 0) return 0;
    if (state->current_hp >= state->maximum_hp)
        return RUNEC_HEALTH_BAR_WIDTH;
    return (int)((int64_t)state->current_hp * RUNEC_HEALTH_BAR_WIDTH /
                 state->maximum_hp);
}

float runec_health_bar_model_anchor(float min_y, float max_y, float scale) {
    float scaled_min = min_y * scale;
    float scaled_max = max_y * scale;
    float top = scaled_min > scaled_max ? scaled_min : scaled_max;
    return top + RUNEC_HEALTH_BAR_HEIGHT_OFFSET;
}
