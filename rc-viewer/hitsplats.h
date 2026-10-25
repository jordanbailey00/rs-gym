#ifndef RUNEC_VIEWER_HITSPLATS_H
#define RUNEC_VIEWER_HITSPLATS_H

#include "../rc-core/types.h"

#include <stdint.h>

#define RUNEC_HITSPLAT_MAX_DEFS 128
#define RUNEC_HITSPLAT_SLOT_COUNT 4
#define RUNEC_CLIENT_CYCLES_PER_SECOND 50.0f

typedef struct {
    int loaded;
    int id;
    int sprite_id;
    uint32_t text_color;
    int scroll_x;
    int scroll_y;
    int text_offset_y;
    int fade_start_cycle;
    int replacement_mode;
    int shows_amount;
    int display_cycles;
} RuneCHitsplatDef;

typedef struct {
    RuneCHitsplatDef defs[RUNEC_HITSPLAT_MAX_DEFS];
    int loaded_count;
} RuneCHitsplatCatalog;

typedef struct {
    int active;
    int definition_id;
    int amount;
    uint64_t sequence;
    float remaining_cycles;
    float total_cycles;
} RuneCHitsplatSlot;

typedef struct {
    RuneCHitsplatSlot slots[RUNEC_HITSPLAT_SLOT_COUNT];
    int cursor;
    uint64_t last_sequence;
} RuneCHitsplatState;

int runec_hitsplat_catalog_load(RuneCHitsplatCatalog *catalog,
                                const char *path);
int runec_hitsplat_catalog_validate_assets(
    const RuneCHitsplatCatalog *catalog, const char *sprite_directory);
const RuneCHitsplatDef *runec_hitsplat_def(
    const RuneCHitsplatCatalog *catalog, int definition_id);
int runec_hitsplat_basic_definition(const RcCombatRecentHit *hit,
                                    int local_target);

void runec_hitsplat_state_reset(RuneCHitsplatState *state);
void runec_hitsplat_state_advance(RuneCHitsplatState *state,
                                  float client_cycles);
int runec_hitsplat_state_add(RuneCHitsplatState *state,
                             const RuneCHitsplatDef *definition,
                             int amount, uint64_t sequence);
void runec_hitsplat_state_sync(RuneCHitsplatState *state,
                               const RuneCHitsplatCatalog *catalog,
                               const RcCombatRecentHit *hits, int hit_count,
                               int local_target);
void runec_hitsplat_slot_offset(int slot, int *x, int *y);
void runec_hitsplat_visual_offset(const RuneCHitsplatDef *definition,
                                  const RuneCHitsplatSlot *slot,
                                  float *x, float *y,
                                  unsigned char *alpha);

#endif
