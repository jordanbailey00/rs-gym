#include "hitsplats.h"

#include "../rc-core/assets.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HITSPLAT_MAGIC = 0x54505348,
    HITSPLAT_VERSION = 1,
    HITSPLAT_RECORD_SIZE = 20,
    HITSPLAT_ZERO_LIT = 26,
    HITSPLAT_ZERO_TINT = 27,
    HITSPLAT_DAMAGE_LIT = 28,
    HITSPLAT_DAMAGE_TINT = 29,
    HITSPLAT_MAX_LIT = 48,
    HITSPLAT_POISON_LIT = 68,
};

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t read_i16(const unsigned char *p) {
    return (int16_t)read_u16(p);
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int runec_hitsplat_catalog_load(RuneCHitsplatCatalog *catalog,
                                const char *path) {
    if (!catalog || !path) return 0;
    memset(catalog, 0, sizeof(*catalog));
    RcAssetBytes bytes = rc_asset_read_all(path);
    if (!bytes.data || bytes.size < 12) {
        fprintf(stderr, "hitsplats: can't read %s\n", path);
        rc_asset_bytes_free(&bytes);
        return 0;
    }

    uint32_t magic = read_u32(bytes.data);
    uint32_t version = read_u32(bytes.data + 4);
    uint32_t count = read_u32(bytes.data + 8);
    size_t expected = 12u + (size_t)count * HITSPLAT_RECORD_SIZE;
    if (magic != HITSPLAT_MAGIC || version != HITSPLAT_VERSION
            || count > RUNEC_HITSPLAT_MAX_DEFS || bytes.size != expected) {
        fprintf(stderr, "hitsplats: invalid catalog %s\n", path);
        rc_asset_bytes_free(&bytes);
        return 0;
    }

    const unsigned char *p = bytes.data + 12;
    for (uint32_t i = 0; i < count; i++, p += HITSPLAT_RECORD_SIZE) {
        int id = read_u16(p);
        if (id < 0 || id >= RUNEC_HITSPLAT_MAX_DEFS
                || catalog->defs[id].loaded) {
            fprintf(stderr, "hitsplats: invalid or duplicate id %d\n", id);
            rc_asset_bytes_free(&bytes);
            memset(catalog, 0, sizeof(*catalog));
            return 0;
        }
        RuneCHitsplatDef *definition = &catalog->defs[id];
        definition->loaded = 1;
        definition->id = id;
        definition->sprite_id = read_i16(p + 2);
        definition->text_color = read_u32(p + 4);
        definition->scroll_x = read_i16(p + 8);
        definition->scroll_y = read_i16(p + 10);
        definition->text_offset_y = read_i16(p + 12);
        definition->fade_start_cycle = read_i16(p + 14);
        definition->replacement_mode = (int8_t)p[16];
        definition->shows_amount = (p[17] & 1u) != 0;
        definition->display_cycles = read_u16(p + 18);
        if (definition->sprite_id < 0 || definition->display_cycles <= 0
                || definition->fade_start_cycle
                    >= definition->display_cycles) {
            fprintf(stderr, "hitsplats: invalid definition %d\n", id);
            rc_asset_bytes_free(&bytes);
            memset(catalog, 0, sizeof(*catalog));
            return 0;
        }
        catalog->loaded_count++;
    }
    rc_asset_bytes_free(&bytes);

    const int required[] = {
        HITSPLAT_ZERO_LIT, HITSPLAT_ZERO_TINT,
        HITSPLAT_DAMAGE_LIT, HITSPLAT_DAMAGE_TINT,
        HITSPLAT_MAX_LIT, HITSPLAT_POISON_LIT,
    };
    for (int i = 0; i < (int)(sizeof(required) / sizeof(required[0])); i++) {
        if (!catalog->defs[required[i]].loaded) {
            fprintf(stderr, "hitsplats: missing basic definition %d\n",
                    required[i]);
            memset(catalog, 0, sizeof(*catalog));
            return 0;
        }
    }
    fprintf(stderr, "hitsplats: loaded %d B237 definitions from %s\n",
            catalog->loaded_count, path);
    return catalog->loaded_count;
}

int runec_hitsplat_catalog_validate_assets(
    const RuneCHitsplatCatalog *catalog, const char *sprite_directory) {
    if (!catalog || !sprite_directory || catalog->loaded_count <= 0) return 0;
    int found = 0;
    for (int id = 0; id < RUNEC_HITSPLAT_MAX_DEFS; id++) {
        if (!catalog->defs[id].loaded) continue;
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/hitsplat_%d.png",
                         sprite_directory, id);
        if (n < 0 || n >= (int)sizeof(path) || !rc_asset_exists(path)) {
            fprintf(stderr, "hitsplats: missing required sprite for id %d\n",
                    id);
            return 0;
        }
        found++;
    }
    return found == catalog->loaded_count;
}

const RuneCHitsplatDef *runec_hitsplat_def(
    const RuneCHitsplatCatalog *catalog, int definition_id) {
    if (!catalog || definition_id < 0
            || definition_id >= RUNEC_HITSPLAT_MAX_DEFS
            || !catalog->defs[definition_id].loaded) {
        return NULL;
    }
    return &catalog->defs[definition_id];
}

int runec_hitsplat_basic_definition(const RcCombatRecentHit *hit,
                                    int local_target) {
    if (!hit) return -1;
    if (hit->source_uid == RC_HIT_SOURCE_STATUS && hit->damage > 0)
        return HITSPLAT_POISON_LIT;
    int lit = local_target || hit->source_uid == RC_HIT_SOURCE_PLAYER;
    if (hit->hit_type == RC_HIT_TYPE_MISS || hit->damage <= 0)
        return lit ? HITSPLAT_ZERO_LIT : HITSPLAT_ZERO_TINT;
    if (lit && hit->hit_type == RC_HIT_TYPE_MAX)
        return HITSPLAT_MAX_LIT;
    return lit ? HITSPLAT_DAMAGE_LIT : HITSPLAT_DAMAGE_TINT;
}

void runec_hitsplat_state_reset(RuneCHitsplatState *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void runec_hitsplat_state_advance(RuneCHitsplatState *state,
                                  float client_cycles) {
    if (!state || client_cycles <= 0.0f) return;
    for (int i = 0; i < RUNEC_HITSPLAT_SLOT_COUNT; i++) {
        RuneCHitsplatSlot *slot = &state->slots[i];
        if (!slot->active) continue;
        slot->remaining_cycles -= client_cycles;
        if (slot->remaining_cycles <= 0.0f)
            memset(slot, 0, sizeof(*slot));
    }
}

static int replacement_slot(const RuneCHitsplatState *state,
                            const RuneCHitsplatDef *definition, int amount) {
    if (definition->replacement_mode < 0) return -1;
    int slot = 0;
    if (definition->replacement_mode == 0) {
        float remaining = state->slots[0].remaining_cycles;
        for (int i = 1; i < RUNEC_HITSPLAT_SLOT_COUNT; i++) {
            if (state->slots[i].remaining_cycles < remaining) {
                remaining = state->slots[i].remaining_cycles;
                slot = i;
            }
        }
        return slot;
    }
    if (definition->replacement_mode == 1) {
        int lowest = state->slots[0].amount;
        for (int i = 1; i < RUNEC_HITSPLAT_SLOT_COUNT; i++) {
            if (state->slots[i].amount < lowest) {
                lowest = state->slots[i].amount;
                slot = i;
            }
        }
        return amount > lowest ? slot : -1;
    }
    return -1;
}

int runec_hitsplat_state_add(RuneCHitsplatState *state,
                             const RuneCHitsplatDef *definition,
                             int amount, uint64_t sequence) {
    if (!state || !definition || !definition->loaded
            || definition->display_cycles <= 0 || sequence == 0) {
        return -1;
    }
    int slot = -1;
    int active_count = 0;
    for (int i = 0; i < RUNEC_HITSPLAT_SLOT_COUNT; i++)
        active_count += state->slots[i].active ? 1 : 0;
    if (active_count == 0)
        state->cursor = 0;
    for (int checked = 0; checked < RUNEC_HITSPLAT_SLOT_COUNT; checked++) {
        int candidate = state->cursor++ & (RUNEC_HITSPLAT_SLOT_COUNT - 1);
        if (!state->slots[candidate].active) {
            slot = candidate;
            break;
        }
    }
    if (slot < 0)
        slot = replacement_slot(state, definition, amount);
    if (slot < 0) return -1;

    state->slots[slot] = (RuneCHitsplatSlot){
        .active = 1,
        .definition_id = definition->id,
        .amount = amount,
        .sequence = sequence,
        .remaining_cycles = (float)definition->display_cycles,
        .total_cycles = (float)definition->display_cycles,
    };
    return slot;
}

void runec_hitsplat_state_sync(RuneCHitsplatState *state,
                               const RuneCHitsplatCatalog *catalog,
                               const RcCombatRecentHit *hits, int hit_count,
                               int local_target) {
    if (!state || !catalog || !hits || hit_count <= 0) return;
    if (hit_count > RUNEC_HITSPLAT_SLOT_COUNT)
        hit_count = RUNEC_HITSPLAT_SLOT_COUNT;
    for (int i = 0; i < hit_count; i++) {
        const RcCombatRecentHit *hit = &hits[i];
        if (hit->sequence == 0 || hit->sequence <= state->last_sequence)
            continue;
        state->last_sequence = hit->sequence;
        int definition_id = runec_hitsplat_basic_definition(hit, local_target);
        const RuneCHitsplatDef *definition =
            runec_hitsplat_def(catalog, definition_id);
        if (definition)
            runec_hitsplat_state_add(state, definition, hit->damage,
                                     hit->sequence);
    }
}

void runec_hitsplat_slot_offset(int slot, int *x, int *y) {
    static const int offsets[RUNEC_HITSPLAT_SLOT_COUNT][2] = {
        {0, 0}, {0, -20}, {-15, -10}, {15, -10},
    };
    if (!x || !y) return;
    if (slot < 0 || slot >= RUNEC_HITSPLAT_SLOT_COUNT) {
        *x = 0;
        *y = 0;
        return;
    }
    *x = offsets[slot][0];
    *y = offsets[slot][1];
}

void runec_hitsplat_visual_offset(const RuneCHitsplatDef *definition,
                                  const RuneCHitsplatSlot *slot,
                                  float *x, float *y,
                                  unsigned char *alpha) {
    if (!x || !y || !alpha) return;
    *x = 0.0f;
    *y = 0.0f;
    *alpha = 255;
    if (!definition || !slot || slot->total_cycles <= 0.0f) return;

    float progress = 1.0f - slot->remaining_cycles / slot->total_cycles;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    *x = (float)definition->scroll_x * progress;
    *y = -(float)definition->scroll_y * progress;

    if (definition->fade_start_cycle >= 0) {
        float fade_cycles = slot->total_cycles
                          - (float)definition->fade_start_cycle;
        float opacity = fade_cycles > 0.0f
            ? slot->remaining_cycles / fade_cycles : 0.0f;
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        *alpha = (unsigned char)(opacity * 255.0f);
    }
}
