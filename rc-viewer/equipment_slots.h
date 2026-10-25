#ifndef RUNEC_VIEWER_EQUIPMENT_SLOTS_H
#define RUNEC_VIEWER_EQUIPMENT_SLOTS_H

#include "../rc-core/types.h"

// OSRS worn slots contain gaps. Slot 13 is ammunition; slot 6 is not an alias.
static inline int ui_equip_slot_to_core(int slot) {
    static const int slots[] = {
        EQUIP_HEAD, EQUIP_CAPE, EQUIP_AMULET, EQUIP_WEAPON,
        EQUIP_BODY, EQUIP_SHIELD, -1, EQUIP_LEGS,
        -1, EQUIP_GLOVES, EQUIP_BOOTS, -1, EQUIP_RING, EQUIP_AMMO,
    };
    return slot >= 0 && slot < (int)(sizeof(slots) / sizeof(slots[0]))
         ? slots[slot] : -1;
}

static inline int core_equip_slot_to_ui(int slot) {
    static const int slots[RC_EQUIP_COUNT] = {
        0, 1, 2, 3, 4, 5, 7, 9, 10, 12, 13,
    };
    return slot >= 0 && slot < RC_EQUIP_COUNT ? slots[slot] : -1;
}

#endif
