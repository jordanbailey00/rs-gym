#include "prayer_ui.h"
#include "../rc-core/prayer.h"

#include <stdio.h>
#include <string.h>

static const RuneCPrayerUiRef refs[] = {
#define PRAYER_ICON(id, name, on, off, overhead) [id] = {name, #on, #off, overhead},
#include "prayer_icons.inc"
#undef PRAYER_ICON
};

const RuneCPrayerUiRef *runec_prayer_ui_ref(int id) {
    return id >= 0 && id < (int)(sizeof(refs) / sizeof(refs[0])) ? &refs[id] : NULL;
}

const char *runec_prayer_overhead_asset(uint32_t active) {
    for (int i = 0; active && i < (int)(sizeof(refs) / sizeof(refs[0])); i++) {
        if ((active & (1u << i)) && refs[i].overhead_asset[0]) return refs[i].overhead_asset;
    }
    return NULL;
}

int runec_prayer_ui_sync(RuneCPrayerUiState *state, const RcWorld *world) {
    if (!state || !world) return 0;
    state->count = 0;
    state->available = 0;
    state->active = world->player.active_prayers;
    state->quick = world->player.quick_prayers;
    state->quick_active = world->player.quick_prayers_active;
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if (!def) continue;
        const RuneCPrayerUiRef *ref = runec_prayer_ui_ref(i);
        if (!ref || strcmp(def->name, ref->name)) {
            fprintf(stderr, "prayer UI: missing/mismatched mapping for prayer %d\n", i);
            return 0;
        }
        RcPrayerResult result = rc_prayer_available(world, i);
        state->reasons[i] = rc_prayer_result_message(result);
        state->levels[i] = def->level;
        if (result == RC_PRAYER_OK) state->available |= 1u << i;
    }
    for (int i = 0; i < RC_MAX_PRAYER_DEFS; i++) {
        const RcPrayerDef *def = rc_prayer_def_get(i);
        if (!def || def->replaces_id != 255) continue;
        int id = rc_prayer_resolve(world, i);
        int slot = state->count++;
        while (slot > 0 && state->levels[state->ids[slot - 1]] > state->levels[id]) {
            state->ids[slot] = state->ids[slot - 1];
            slot--;
        }
        state->ids[slot] = id;
    }
    return state->count > 0;
}
