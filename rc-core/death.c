#include "death.h"

#include "api.h"
#include "area_flags.h"
#include "interaction.h"
#include "items.h"
#include "prayer.h"

#include <string.h>

enum { DEATH_SLOTS = RC_INVENTORY_SIZE + RC_EQUIP_COUNT };

static const RcInvSlot *death_slot(const RcPlayer *p, int i) {
    return i < RC_INVENTORY_SIZE ? &p->inventory[i]
                                 : &p->equipment[i - RC_INVENTORY_SIZE];
}

void rc_death_kept_slots(const RcWorld *world, bool kept[]) {
    memset(kept, 0, DEATH_SLOTS * sizeof(kept[0]));
    const RcDeathRules *rules = world ? world->death_rules : NULL;
    if (!rules) return;
    const RcPlayer *p = world->player;
    int keep = rules->keep_items
             + ((p->active_prayers >> RC_PRAYER_PROTECT_ITEM) & 1);
    // Unstackable items by value, highest first; ties keep slot order.
    int order[DEATH_SLOTS], values[DEATH_SLOTS], n = 0;
    for (int i = 0; i < DEATH_SLOTS; i++) {
        const RcInvSlot *s = death_slot(p, i);
        const RcItemDef *def = s->item_id >= 0 && s->quantity > 0
            ? rc_item_def_get(s->item_id) : NULL;
        if (!def || def->stackable) continue;
        int j = n++;
        while (j > 0 && values[j - 1] < def->value) {
            order[j] = order[j - 1];
            values[j] = values[j - 1];
            j--;
        }
        order[j] = i;
        values[j] = def->value;
    }
    for (int i = 0; i < n && i < keep; i++) kept[order[i]] = true;
}

void rc_death_begin(RcWorld *world) {
    const RcDeathRules *rules = world ? world->death_rules : NULL;
    if (!rules) return;
    RcPlayer *p = world->player;
    p->respawn_tick = world->tick + (rules->respawn_delay ? rules->respawn_delay : 1);
    bool kept[DEATH_SLOTS];
    rc_death_kept_slots(world, kept);
    RcGroundGrant grants[DEATH_SLOTS];
    int count = 0;
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code != RC_ITEM_RESULT_OK) return;
    for (int i = 0; i < DEATH_SLOTS; i++) {
        const RcInvSlot *s = death_slot(p, i);
        if (kept[i] || s->item_id < 0 || s->quantity <= 0) continue;
        RcItemActionResult r = i < RC_INVENTORY_SIZE
            ? rc_item_tx_remove_slot(&tx, i, s->quantity, s->generation)
            : rc_item_tx_remove_equipment(&tx, i - RC_INVENTORY_SIZE,
                                          s->quantity, s->generation);
        if (r.code != RC_ITEM_RESULT_OK) return;
        grants[count++] = (RcGroundGrant){s->item_id, s->quantity, s->state_id};
    }
    int wild = rc_wilderness_level_at(p->x, p->y, p->plane) > 0;
    int ticks = wild ? rules->wild_drop_ticks : rules->grave_ticks;
    RcGroundPolicy policy = {
        .reveal_ticks = wild ? 0 : ticks, .lifetime_ticks = ticks,
        .delay_ticks = 0, .untradeable_lifetime_ticks = ticks,
    };
    (void)rc_item_tx_commit_ground(&tx, grants, count, p->x, p->y, p->plane,
                                   wild ? RC_GROUND_OWNER_NONE : p->index,
                                   policy);
    rc_recalc_bonuses(p);
    rc_interaction_publish_message(p, "Oh dear, you are dead!");
}

void rc_death_tick(RcWorld *world) {
    RcPlayer *p = world ? world->player : NULL;
    if (!p || !world->death_rules || !p->is_dead || !p->respawn_tick
            || world->tick < p->respawn_tick)
        return;
    p->respawn_tick = 0;
    p->special_energy = 10000;
    if (!rc_world_respawn_player(world, world->respawn_x, world->respawn_y,
                                 world->respawn_plane))
        return;
    p->run_energy = 10000;
    p->toxin_severity = 0;
    p->toxin_venom = false;
}
