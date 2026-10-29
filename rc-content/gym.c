// Gym world rules: the town's shops (stock, prices, Trade handlers), burying
// bones, and what happens on death. Engines: rc-core/shops.c, rc-core/death.c.
#include "content.h"
#include "config.h"
#include "death.h"
#include "interaction.h"
#include "items.h"
#include "mode.h"
#include "shops.h"
#include "skills.h"

#include <stdint.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// {item, base stock, ticks per restock step}
static const RcShopItemDef general_items[] = {
    {590, 2, 100},   // tinderbox
    {2347, 5, 100},  // hammer
    {1925, 3, 100},  // bucket
    {1931, 5, 100},  // pot
    {1935, 2, 100},  // jug
};

static const RcShopItemDef tool_items[] = {
    {1351, 10, 100}, {1349, 5, 200}, {1353, 3, 400}, {1355, 2, 600},  // axes
    {1357, 1, 1000}, {1359, 1, 3000},
    {1265, 5, 100}, {1267, 5, 200}, {1269, 3, 400}, {1273, 2, 600},   // pickaxes
    {1271, 1, 1000}, {1275, 1, 3000},
    {303, 5, 100}, {307, 5, 100}, {309, 5, 100}, {311, 2, 400},       // fishing gear
    {301, 2, 400}, {313, 1500, 10}, {314, 1000, 10},
    {590, 5, 100}, {2347, 5, 100},                                    // tinderbox, hammer
};

static const RcShopItemDef combat_items[] = {
    {556, 5000, 10}, {555, 5000, 10}, {557, 5000, 10}, {554, 5000, 10},  // elemental runes
    {558, 5000, 10}, {562, 250, 10}, {560, 250, 15},                    // mind, chaos, death
    {882, 2000, 10}, {884, 1500, 15}, {886, 1000, 20}, {888, 800, 15},  // arrows
    {841, 4, 100}, {839, 4, 100}, {843, 3, 200}, {845, 3, 200},         // bows
    {849, 2, 200}, {847, 2, 200}, {853, 1, 100},
    {1379, 5, 100}, {1381, 2, 400}, {1387, 2, 400},                     // staffs
    {1129, 5, 100}, {1095, 5, 100}, {1063, 5, 100}, {1169, 3, 200},     // leather, coif
    {579, 3, 100}, {577, 3, 100},                                       // wizard hat, robe
    {1540, 10, 100},                                                    // anti-dragon shield
};

static const RcShopDef shops[] = {
    {"General Store", 2813, 400, 1300, 30, true, general_items, COUNT(general_items)},
    {"Bob's Brilliant Tools", 10619, 600, 1000, 30, false, tool_items, COUNT(tool_items)},
    {"Lowe's Combat Supplies", 2883, 550, 1000, 30, false, combat_items, COUNT(combat_items)},
};

// OSRS keeps 3 items (4 with Protect Item); a gravestone lasts 15 minutes.
static const RcDeathRules death_rules = {
    .keep_items = 3, .respawn_delay = 4, .grave_ticks = 1500, .wild_drop_ticks = 500,
};

// Burying bones: Prayer XP in tenths.
static const struct { int item, xp_tenths; } bones[] = {
    {526, 45}, {532, 150}, {536, 720},  // bones, big bones, dragon bones
};

static RcInteractionHandlerResult bury_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    int slot = pending->target.inventory_slot;
    if (slot < 0 || slot >= RC_INVENTORY_SIZE
            || player->inventory[slot].item_id != pending->target.definition_id)
        return rc_interaction_result_cancel(RC_INTERACTION_FAIL_TARGET_MISSING);
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, world).code != RC_ITEM_RESULT_OK
            || rc_item_tx_remove_slot(&tx, slot, 1, player->inventory[slot].generation).code
                != RC_ITEM_RESULT_OK
            || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK)
        return rc_interaction_result_cancel(RC_INTERACTION_FAIL_TARGET_MISSING);
    rc_add_xp_hundredths(&player->skills, SKILL_PRAYER, (int64_t)(intptr_t)ctx * 10);
    return rc_interaction_result_message("You bury the bones.");
}

static RcInteractionHandlerResult trade_handler(
    RcWorld *world, RcPlayer *player, const RcPendingInteraction *pending,
    void *ctx) {
    (void)player;
    (void)pending;
    if (!rc_shop_open(world, (int)(intptr_t)ctx))
        return rc_interaction_result_failure(RC_INTERACTION_FAIL_INVALID_TARGET,
                                             "The shop is closed.");
    return rc_interaction_result_complete();
}

int rc_content_gym_register(RcWorld *world) {
    if (!world) return 0;
    if (rc_mode_active()) return 1;  // a gym mode brings its own rules (compact.c)
    world->death_rules = &death_rules;
    if (!(world->enabled & RC_SUB_INVENTORY)) return 1;
    world->shop_count = 0;
    int ok = 1;
    for (int i = 0; i < COUNT(bones); i++) {
        RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
        key.kind = RC_INTERACTION_INVENTORY_ITEM;
        key.op = RC_INTERACTION_OP1;  // Bury
        key.definition_id = bones[i].item;
        ok &= rc_interaction_register_world_handler(world, &key, bury_handler,
                                                    (void *)(intptr_t)bones[i].xp_tenths);
    }
    for (int i = 0; i < COUNT(shops); i++) {
        int idx = rc_shop_install(world, &shops[i]);
        RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
        key.kind = RC_INTERACTION_NPC;
        key.op = RC_INTERACTION_OP3;  // Trade
        key.definition_id = shops[i].npc_id;
        ok &= idx >= 0 && rc_interaction_register_world_handler(
            world, &key, trade_handler, (void *)(intptr_t)idx);
    }
    return ok;
}
