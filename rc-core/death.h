#ifndef RC_DEATH_H
#define RC_DEATH_H

#include "types.h"

// Gym death rules, installed by content (world->death_rules). A player who
// dies keeps their `keep_items` most valuable unstackable items (one more
// under Protect Item). Everything else goes to a gravestone only they can see
// for `grave_ticks` outside the wilderness, or to the floor for everyone for
// `wild_drop_ticks` inside it. `respawn_delay` ticks later they respawn at
// the world's respawn point with full hitpoints, prayer, run and special
// energy.
typedef struct RcDeathRules {
    uint8_t keep_items;
    uint16_t respawn_delay;
    uint16_t grave_ticks;
    uint16_t wild_drop_ticks;
    bool grave_to_bank;  // a gravestone's remaining items move to the bank instead of vanishing
} RcDeathRules;

// Acting player: rc_death_begin right after they die; rc_death_tick each tick
// (respawns them when due).
void rc_death_begin(RcWorld *world);
void rc_death_tick(RcWorld *world);

// Which inventory (first RC_INVENTORY_SIZE) and equipment (next
// RC_EQUIP_COUNT) slots the acting player would keep if they died now.
void rc_death_kept_slots(const RcWorld *world, bool kept[]);

#endif
