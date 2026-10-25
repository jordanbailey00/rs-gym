#ifndef RUNEC_PRAYER_UI_H
#define RUNEC_PRAYER_UI_H

#include "../rc-core/types.h"

typedef struct {
    const char *name, *available_asset, *locked_asset, *overhead_asset;
} RuneCPrayerUiRef;

typedef struct {
    uint32_t active, quick, available;
    bool quick_active;
    int count, ids[31], levels[32];
    const char *reasons[32];
} RuneCPrayerUiState;

const RuneCPrayerUiRef *runec_prayer_ui_ref(int id);
const char *runec_prayer_overhead_asset(uint32_t active);
int runec_prayer_ui_sync(RuneCPrayerUiState *state, const RcWorld *world);

#endif
