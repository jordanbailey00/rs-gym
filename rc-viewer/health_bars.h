#ifndef RUNEC_VIEWER_HEALTH_BARS_H
#define RUNEC_VIEWER_HEALTH_BARS_H

#include "../rc-core/types.h"

#include <stdint.h>

// B237 ordinary actor health-bar definition 0.
#define RUNEC_HEALTH_BAR_WIDTH 30
#define RUNEC_HEALTH_BAR_HEIGHT 5
#define RUNEC_HEALTH_BAR_DISPLAY_CYCLES 300.0f
#define RUNEC_HEALTH_BAR_HEIGHT_OFFSET (15.0f / 128.0f)

typedef struct {
    uint64_t last_sequence;
    float remaining_cycles;
    int current_hp;
    int maximum_hp;
} RuneCHealthBarState;

void runec_health_bar_reset(RuneCHealthBarState *state);
void runec_health_bar_advance(RuneCHealthBarState *state,
                              float client_cycles);
void runec_health_bar_sync(RuneCHealthBarState *state,
                           const RcCombatRecentHit *hits, int hit_count,
                           int current_hp, int maximum_hp);
int runec_health_bar_visible(const RuneCHealthBarState *state);
int runec_health_bar_fill_width(const RuneCHealthBarState *state);
float runec_health_bar_model_anchor(float min_y, float max_y, float scale);

#endif
